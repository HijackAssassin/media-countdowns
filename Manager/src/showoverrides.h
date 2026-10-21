#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QTime>
#include "tiledata.h"
#include "relayconfig.h"

// =============================================================================
//  ShowOverrides — V5.
//
//  Hand-entered corrections published from the relay dashboard and applied by
//  every installation.
//
//  They exist because some services aren't covered by any data source at all:
//  TVmaze records no air time for Disney+, so a show that genuinely drops at
//  9 PM Eastern falls back to the app's generic default and there is no API
//  anywhere that knows better. A person supplies the answer once and everyone
//  gets it.
//
//  Precedence is deliberate and narrow:
//
//      customAirTime (the user's own, per tile)   ← always wins
//        └─ override airTime (from the relay)     ← this
//             └─ source airTime (TVmaze)
//                  └─ TimeZoneUtil default
//
//  A correction must never overrule a time the user set themselves on their
//  own tile — that would be a remote server quietly undoing a local decision.
//  Since overrides only ever write TileData::airTime, and customAirTime is
//  checked first in effectiveTime(), that ordering falls out for free.
// =============================================================================
class ShowOverrides : public QObject
{
    Q_OBJECT
public:
    struct Entry {
        QTime airTime;      // invalid when only a day shift is set
        // V5.4.28 — the zone airTime was ENTERED in, on the dashboard.
        //
        // Without it a correction was ambiguous the moment two users were in
        // different zones: "3 AM" typed by someone in Eastern was applied
        // verbatim, so a Pacific user got 3 AM Pacific — three hours out — and
        // nothing recorded what had been meant. The relay always sends one and
        // defaults it to Eastern, so this is only empty talking to a relay
        // older than V5.4.28, where assuming Eastern is exactly right: Eastern
        // is what every correction entered before this meant.
        QString airTimeZone;
        int   dayShift = 0; // -1, 0 or +1
        // V5.4.5 — an announced release window. year 0 means none set; month 0
        // with a year means the whole year ("returns some time in 2027"),
        // which is what the dashboard sends when its month box is left blank.
        int   windowMonth = 0;
        int   windowYear  = 0;
        bool hasWindow() const { return windowYear > 0; }
    };

    static ShowOverrides& instance()
    {
        static ShowOverrides inst;
        return inst;
    }

    // Pulled once per refresh cycle. Tiny (a few dozen rows) and rarely
    // changing, but never cached to disk: a stale correction is precisely
    // what this mechanism exists to fix.
    void refresh()
    {
        if (!RelayConfig::shouldUseRelayForTmdb() && !RelayConfig::shouldUseRelayForIgdb())
            return;   // relay disabled entirely — nothing to ask
        // (shouldUseRelayFor* already answer false when no relay is configured
        // at all, which is how a build from the public source starts out.)
        QNetworkRequest req{QUrl(RelayConfig::baseUrl() + "/overrides")};
        req.setRawHeader("Authorization", ("Bearer " + RelayConfig::sharedSecret()).toUtf8());
        req.setRawHeader("X-Client-ID", RelayConfig::installationId().toUtf8());

        QNetworkReply* r = m_nam->get(req);
        connect(r, &QNetworkReply::finished, this, [r, this]() {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) return;   // keep what we have
            QJsonArray arr = QJsonDocument::fromJson(r->readAll())
                                 .object()["overrides"].toArray();
            QHash<QString, Entry> next;
            for (const QJsonValue& v : arr) {
                QJsonObject o = v.toObject();
                QString key = o["show_key"].toString();
                if (key.isEmpty()) continue;
                Entry e;
                QString t = o["air_time"].toString();
                if (!t.isEmpty()) e.airTime = QTime::fromString(t, "HH:mm");
                e.airTimeZone = o["air_time_zone"].toString();
                e.dayShift = qBound(-1, o["day_shift"].toInt(0), 1);
                e.windowMonth = qBound(0, o["window_month"].toInt(0), 12);
                e.windowYear  = o["window_year"].toInt(0);
                if (e.windowYear < 1900 || e.windowYear > 2999) {
                    e.windowYear  = 0;   // nonsense year — treat as no window
                    e.windowMonth = 0;
                }
                next.insert(key, e);
            }
            m_entries = next;
            emit updated();
        });
    }

    // Every key form a tile could be corrected under. A tile can be keyed by
    // either provider, and the dashboard records whichever source it looked
    // the show up through — so try each form rather than assuming one.
    static QStringList keysFor(const TileData& td)
    {
        QStringList keys;
        if (td.tvmazeId > 0) keys << QString("tvmaze:%1").arg(td.tvmazeId);
        if (td.tmdbId > 0) {
            keys << QString("tmdb:%1").arg(td.tmdbId);
            keys << QString("tmdb_tv:%1").arg(td.tmdbId);
            keys << QString("tmdb_movie:%1").arg(td.tmdbId);
            keys << QString("igdb_game:%1").arg(td.tmdbId);   // games reuse tmdbId
        }
        return keys;
    }

    // Bring a tile in line with the corrections currently published, whatever
    // it was carrying before. Returns true when something changed, so the
    // caller knows to save.
    //
    // V5.4 — this UNDOES whatever it previously applied before applying
    // what's in force now, rather than only ever adding corrections on top.
    // Two bugs came from the old one-way version:
    //
    //  • Deleting a correction on the dashboard changed nothing for anyone
    //    who already had it. The row vanished from /overrides, so this
    //    function simply stopped finding an entry — and left the value it had
    //    already written into the tile sitting there permanently. For a game
    //    it never came back at all, because IgdbScraper deliberately carries
    //    airTime across refreshes.
    //  • A dayShift was re-applied on every pass. The old guard was
    //    `if (shifted != targetDate)`, which is true every single time when
    //    the shift is non-zero, so a corrected tile's date crept forward a
    //    day on each refresh cycle.
    //
    // Undo-then-reapply also means an EDITED correction (9 PM changed to
    // 10 PM) lands correctly, which the old version got right only by
    // accident for airTime and got wrong for dayShift.
    bool apply(TileData& td) const
    {
        const QTime   origAir  = td.airTime;
        const QDate   origDate = td.targetDate;
        const QString origKey  = td.overrideKey;

        // ── Undo ─────────────────────────────────────────────────────────
        if (!td.overrideKey.isEmpty()) {
            td.airTime = td.overrideBaseAirTime;
            // A window replaced the date outright, so put the whole thing
            // back — including which kind of window (if any) it had been.
            if (td.overrideBaseWindowKind > 0 || td.overrideBaseTargetDate.isValid()) {
                td.targetDate      = td.overrideBaseTargetDate;
                td.dateDisplay     = td.overrideBaseDateDisplay;
                td.isMonthOnlyDate = (td.overrideBaseWindowKind == 1);
                td.isYearOnlyDate  = (td.overrideBaseWindowKind == 2);
            } else if (td.overrideDayShift != 0 && td.targetDate.isValid()) {
                td.targetDate = td.targetDate.addDays(-td.overrideDayShift);
            }
            td.clearOverrideBase();
        }

        // ── Re-apply whatever is published now ───────────────────────────
        const Entry* entry = nullptr;
        QString key;
        for (const QString& k : keysFor(td)) {
            auto it = m_entries.constFind(k);
            if (it != m_entries.constEnd()) { entry = &it.value(); key = k; break; }
        }

        // V5.4.6 — precedence between a window correction and the source.
        //
        // The undo above has already put the tile back to the source's own
        // values, so what it holds right now IS what the source says. That's
        // what decides whether the window still applies:
        //
        //   • a window, an estimate, or no date at all → the source doesn't
        //     actually know, so a hand-set window is better information and
        //     replaces it. Replacing one window with a better one is the
        //     whole point.
        //   • a real published date → the source now knows, so it wins. A
        //     guess must never override a fact, and the correction is also
        //     reported spent so it stops applying for everyone: the NEXT
        //     season may return at a completely different time of year, and
        //     a window left in place would quietly assert this year's month
        //     against it.
        //
        // Air time is the opposite and is never affected by any of this — a
        // slot a show airs in doesn't change because a season did.
        const bool hasWindow = entry && entry->hasWindow();
        const bool sourceHasRealDate = td.targetDate.isValid()
                                    && !td.isMonthOnlyDate
                                    && !td.isYearOnlyDate
                                    && !td.isEstimatedDate;

        // Spent, not ignored: the window stops applying AND is reported so it
        // stops applying for everyone. The rest of the same correction still
        // applies — a correction that sets both a window and an air time
        // keeps the air time.
        const bool windowSpent = hasWindow && sourceHasRealDate;
        if (windowSpent) reportWindowSuperseded(key, td);
        const bool windowCorrection = hasWindow && !windowSpent;

        if (entry && (windowCorrection || !td.isWindowDate())) {
            td.overrideKey             = key;
            td.overrideBaseAirTime     = td.airTime;
            td.overrideDayShift        = 0;
            td.overrideBaseTargetDate  = QDate();
            td.overrideBaseDateDisplay.clear();
            td.overrideBaseWindowKind  = 0;

            if (windowCorrection) {
                // Remember the whole date, not a delta — this replaces it.
                td.overrideBaseTargetDate  = td.targetDate;
                td.overrideBaseDateDisplay = td.dateDisplay;
                td.overrideBaseWindowKind  = td.isMonthOnlyDate ? 1
                                           : td.isYearOnlyDate  ? 2 : 0;

                const int y = entry->windowYear;
                const int m = entry->windowMonth;
                if (m >= 1 && m <= 12) {
                    // Last day of that month, the same maximum-countdown rule
                    // TvmazeScraper uses for an announced month.
                    QDate first(y, m, 1);
                    td.targetDate      = QDate(y, m, first.daysInMonth());
                    td.dateDisplay     = first.toString("MMMM yyyy");
                    td.isMonthOnlyDate = true;
                    td.isYearOnlyDate  = false;
                } else {
                    td.targetDate      = QDate(y, 12, 31);
                    td.dateDisplay     = QString::number(y);
                    td.isMonthOnlyDate = false;
                    td.isYearOnlyDate  = true;
                }
                // A window has no time of day, whatever else was set.
                td.airTime = QTime();
            } else {
                if (entry->airTime.isValid()) {
                    // Convert from the zone the correction was written in to
                    // the one this user reads times in. Both are one of the
                    // four US zones the app offers, which all shift for DST
                    // together, so the difference is a whole number of hours
                    // and TimeZoneUtil's own table is enough — no calendar
                    // arithmetic, and it stays consistent with how every other
                    // time in the app is derived.
                    const int fromPT = TimeZoneUtil::hoursAheadOfPacific(
                        entry->airTimeZone.isEmpty() ? QStringLiteral("America/New_York")
                                                     : entry->airTimeZone);
                    const int toPT   = TimeZoneUtil::hoursAheadOfPacific(
                        TimeZoneUtil::currentZoneId());
                    const int deltaHours = toPT - fromPT;

                    // addSecs wraps at midnight, which loses the day it wrapped
                    // into. 1 AM Eastern is 10 PM the PREVIOUS day in Pacific,
                    // so without this a corrected tile would sit a full day
                    // late — the same class of off-by-a-day the airdate/
                    // airstamp handling in TvmazeScraper exists to avoid.
                    const QDateTime fromMoment(td.targetDate.isValid() ? td.targetDate
                                                                       : QDate::currentDate(),
                                               entry->airTime);
                    const QDateTime shifted = fromMoment.addSecs(deltaHours * 3600);
                    td.airTime = shifted.time();
                    if (td.targetDate.isValid() && shifted.date() != fromMoment.date()) {
                        const int roll = fromMoment.date().daysTo(shifted.date());
                        td.targetDate = td.targetDate.addDays(roll);
                        // RECORD it, or the undo above cannot reverse it. Missing
                        // this meant the date crept a day further on every refresh
                        // and never came back when the correction was deleted --
                        // exactly the two bugs the one-way version of this
                        // function had, arriving by a different route.
                        td.overrideDayShift += roll;
                    }
                }

                // A shift moves the date itself — for a listing filed under
                // the wrong day, which happens with late-night broadcasts
                // recorded under the previous evening.
                if (entry->dayShift != 0 && td.targetDate.isValid()) {
                    td.targetDate        = td.targetDate.addDays(entry->dayShift);
                    // += not =, so a correction that both shifts the day AND
                    // rolls one from the zone conversion undoes BOTH. Assigning
                    // here would silently discard the roll recorded above.
                    td.overrideDayShift += entry->dayShift;
                }
            }
        }

        if (td.targetDate != origDate && td.targetDate.isValid() && !td.isWindowDate())
            td.dateDisplay = td.targetDate.toString("MMMM d, yyyy");

        return td.airTime != origAir || td.targetDate != origDate
            || td.overrideKey != origKey;
    }

    // V5.4.6 — "a real date exists now, that window is spent."
    //
    // Sent once per show per run of the app. The relay clears only the window
    // and keeps any air time, and clearing an already-cleared one is a no-op,
    // so a second report would be harmless — but apply() runs on every refresh
    // cycle and there is no reason to say the same thing every six hours.
    //
    // mutable because apply() is const: this reports a fact about the tile
    // rather than changing this object's meaning.
    void reportWindowSuperseded(const QString& key, const TileData& td) const
    {
        if (!RelayConfig::isConfigured()) return;   // V5.4.26 — nobody to tell
        if (key.isEmpty() || m_supersededReported.contains(key)) return;
        if (!td.targetDate.isValid()) return;
        m_supersededReported.insert(key);

        QNetworkRequest req{QUrl(RelayConfig::baseUrl() + "/window-superseded")};
        req.setRawHeader("Authorization", ("Bearer " + RelayConfig::sharedSecret()).toUtf8());
        req.setRawHeader("X-Client-ID", RelayConfig::installationId().toUtf8());
        req.setRawHeader("Content-Type", "application/json");

        QJsonObject body;
        body["show_key"]  = key;
        body["real_date"] = td.targetDate.toString(Qt::ISODate);
        body["title"]     = td.displayTitle();

        QNetworkReply* r = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
    }

    // One-click "this air time looks wrong". The relay counts one report per
    // installation per show, so this can't be used to spam.
    void report(const TileData& td)
    {
        if (!RelayConfig::isConfigured()) return;   // V5.4.26 — nobody to tell
        QString key;
        if (td.tvmazeId > 0)   key = QString("tvmaze:%1").arg(td.tvmazeId);
        else if (td.tmdbId > 0) key = QString("tmdb:%1").arg(td.tmdbId);
        if (key.isEmpty()) return;

        QNetworkRequest req{QUrl(RelayConfig::baseUrl() + "/report-airtime")};
        req.setRawHeader("Authorization", ("Bearer " + RelayConfig::sharedSecret()).toUtf8());
        req.setRawHeader("X-Client-ID", RelayConfig::installationId().toUtf8());
        req.setRawHeader("Content-Type", "application/json");

        QJsonObject body;
        body["show_key"] = key;
        body["title"]    = td.displayTitle();
        body["source"]   = td.tvmazeId > 0 ? "tvmaze" : "tmdb";

        QNetworkReply* r = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
    }

signals:
    void updated();

private:
    ShowOverrides() : m_nam(new QNetworkAccessManager(this)) {}
    Q_DISABLE_COPY(ShowOverrides)

    QNetworkAccessManager*  m_nam;
    QHash<QString, Entry>   m_entries;
    // Show keys already reported as having outgrown their window this run.
    mutable QSet<QString>   m_supersededReported;
};
