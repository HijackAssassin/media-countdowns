#pragma once
#include <QString>
#include <QStringList>
#include <QDate>
#include <QTime>
#include <QDateTime>
#include <QRegularExpression>
#include <QColor>
#include "timezoneutil.h"

// Notification lifecycle:
//   Active   — tile counting down, notification not yet sent
//   Ready    — tile just expired; notifier will fire then flip to Inactive
//   Inactive — notification sent; never re-fires
enum class NotifStatus { Active, Ready, Inactive };

// V5 — one place that understands a status label's shape.
//
// Labels come in two forms: a single episode ("S04E01") and a multi-episode
// drop where several aired the same day ("S04E01+E02+E03", built by
// TmdbScraper::fetchSeasonForMultiEp). Three separate copies of a
// `^S(\d+)E(\d+)$` regex used to parse these, and every one of them silently
// failed on the multi-episode form — which is why a tile like
// "Reacher S04E01+E02+E03" would never advance to E04 and just sat in
// Released.
//
// lastEpisode is the meaningful one for "what aired most recently" — for a
// triple drop, episode 3 is where the season actually stands.
struct EpisodeLabel {
    bool valid        = false;
    int  season       = 0;
    int  firstEpisode = 0;
    int  lastEpisode  = 0;
};

inline EpisodeLabel parseEpisodeLabel(const QString& label)
{
    EpisodeLabel out;
    static const QRegularExpression shape("^S(\\d+)((?:\\+?E\\d+)+)$");
    QRegularExpressionMatch m = shape.match(label);
    if (!m.hasMatch()) return out;

    out.season = m.captured(1).toInt();
    static const QRegularExpression epRe("E(\\d+)");
    QRegularExpressionMatchIterator it = epRe.globalMatch(m.captured(2));
    while (it.hasNext()) {
        int n = it.next().captured(1).toInt();
        if (out.firstEpisode == 0) out.firstEpisode = n;
        out.lastEpisode = n;
    }
    out.valid = (out.season > 0 && out.lastEpisode > 0);
    return out;
}

struct TileData
{
    QString id;
    QString title;
    QString customTitle;
    int     tmdbId    = 0;
    // V5 — TVmaze's own show id, cached once resolved so the title-matching
    // migration only ever runs once per tile. 0 = not yet resolved. Kept
    // alongside tmdbId rather than replacing it, so a tile can still fall
    // back to TMDB if the user supplies their own key.
    int     tvmazeId  = 0;
    QString mediaType;
    QString tmdbUrl;
    // V5 — TVmaze's own page for this show, kept separate rather than
    // sharing tmdbUrl. They're links to two different sites and a tile can
    // legitimately have both; overwriting one with the other loses a real
    // link and makes a "view on X" action open somewhere else entirely.
    QString tvmazeUrl;
    QString statusLabel;
    QString rawDateText;
    QString dateDisplay;
    QString customDateStr;
    QDate   targetDate;
    QDate   customDate;
    QTime   airTime;
    QTime   customAirTime;

    // V5.4.28 — the ABSOLUTE INSTANT this episode airs, as TVmaze published
    // it, kept alongside the wall-clock airTime above.
    //
    // airTime and targetDate are wall-clock values in whatever Time Zone was
    // set when the tile was last refreshed. That is fine until the setting
    // changes, at which point they are simply wrong and nothing notices:
    // switching Eastern to Pacific left every show still reading 3 AM instead
    // of midnight, and a tile refreshed under one zone sat beside a tile
    // refreshed under another, which is why the same show could recap as
    // "9 PM" once and "3 AM" the next time. 9 PM Pacific and midnight Eastern
    // are the same instant; so are midnight Pacific and 3 AM Eastern.
    //
    // Storing the instant makes the zone a DISPLAY choice rather than baked
    // data: changing it recomputes date and time locally and exactly, with no
    // network round trip and no arithmetic drift from shifting an already
    // shifted value. See MainWindow::retimeTilesForZone().
    //
    // Only set when TVmaze published a real air time. Streaming services
    // publish none, so those tiles keep an invalid airTime and fall through to
    // TimeZoneUtil's default, which already follows the setting live.
    QDateTime airStampUtc;
    QString imagePath;              // the CURRENTLY ACTIVE/displayed image
    QString fetchedImagePath;       // v3.1.0 — the TMDB-fetched backdrop slot; empty if none

    // V5 — when the fetched backdrop was downloaded, and which season it
    // belonged to. Two reasons this is tracked rather than downloading once
    // and keeping it forever:
    //
    //  1. Show artwork genuinely changes between seasons — a season 2 tile
    //     still displaying season 1's backdrop looks stale and wrong. If the
    //     tile has moved to a new season since the image was fetched, the
    //     artwork is refetched.
    //  2. TMDB's API terms forbid caching anything obtained from them for
    //     longer than 6 months, and a permanently-kept backdrop would breach
    //     that. Re-fetching on that schedule keeps it compliant and happens
    //     to keep the art current too.
    //
    // artworkSeason is 0 for movies and games, which have no seasons — those
    // refresh on the age rule alone.
    int     artworkSeason  = 0;
    QDate   artworkFetchedOn;
    QStringList customImagePaths;   // v3.1.0 — user-selected images, oldest → newest
    bool    notified    = false;
    NotifStatus notifStatus = NotifStatus::Active;
    bool    isLooped       = false;
    bool    noDateOverride = false;   // user explicitly removed the date — survives TMDB refresh
    QString presetType;

    // V5.4.3 — "a looped occurrence arrived and still owes a notification".
    //
    // Looped tiles are the one kind this app advances ITSELF, immediately, on
    // the tick that they expire. That left no window for the notifier to
    // notice — so while this app was open, a birthday or holiday arriving was
    // never announced at all.
    //
    // Rather than have this app fire its own notification (two programs
    // racing to announce the same instant, which is how duplicates happen),
    // it advances the tile and leaves this flag. The notifier is still the
    // only thing that ever notifies; it announces the flagged tile and clears
    // it. When this app ISN'T running the notifier does both jobs itself and
    // the flag never gets set, so exactly one notification happens either way.
    bool    pendingLoopNotice = false;

    // V5.4.26 — the looped occurrence this tile has most recently advanced
    // PAST. Invalid until one has.
    //
    // A looped tile is the one kind whose date moves the moment it arrives:
    // this app advances it on the tick that it expires, and the notifier does
    // the same when it is the only thing running. Either way the occurrence
    // that actually happened is gone from the tile within a second of
    // happening, which is why a birthday could be announced by the notifier
    // and then appear in neither the startup recap nor Recap/History — by the
    // time anything looked, the tile was already counting down to next year.
    //
    // This is the record of it, written at both advance sites. See
    // recapCandidateDate() for the single place that decides which of the two
    // dates a recap should be reading.
    QDate   loopLastOccurrence;
    // V4.7 — invalid QColor() means "not set", for both of these. Purely
    // visual, and named for what the user sees: tagColor draws the tile's
    // OUTLINE, textColor recolours its countdown and title. tagColor keeps its
    // original field name on purpose — renaming it would rewrite the key in
    // every existing tiles.json for a cosmetic gain, and the notifier reads
    // that file too.
    QColor  tagColor;
    QColor  textColor;   // V5.4.15 — the tile's text; invalid = the default white

    // ── Purely the user's choices — no scraper ever produces these ─────────
    //
    // V5.4.16. onTileRefreshed() replaces a tile wholesale with what the
    // scraper returned (`td = updated`) and then restores, by name, everything
    // the scraper doesn't know about. Three of these were missing from that
    // list, so every single refresh silently wiped them: both colours and the
    // favourite star. A colour tag survived only until the next refresh,
    // which is roughly every six hours or any time you press refresh.
    //
    // Adding three more names to that list would have fixed it until the
    // fourth was forgotten. This is the one place they are listed instead —
    // if you add a field the user sets and a scraper never returns, add it
    // HERE and every path that carries a tile across a refresh gets it free.
    void carryUserSettingsFrom(const TileData& previous)
    {
        tagColor   = previous.tagColor;
        textColor  = previous.textColor;
        isFavorite = previous.isFavorite;
    }
    bool    isFavorite = false;   // V4.11 — purely visual, a gold star in the tile's top-right corner
    bool    isTheatrical   = false;   // movies only — true = theatrical (US) release date
    QString loopInterval;      // "Yearly" | "Monthly" | "Weekly" | "Daily"
    int     loopWeekday    = 1; // 1=Mon..7=Sun (Qt::DayOfWeek), used when Weekly
    int     loopDayOfMonth = 1; // 1-31, used when Monthly
    int     releaseYear        = 0;   // v3.0.2 — original release/premiere year, for the "Year" tile display option
    int     seasonEpisodeCount = 0;   // v3.0.2 — total episodes in the relevant season, for "Total Episodes"

    // v3.1.0 Feature 3 — estimated (unverified) episode dates. When TMDB
    // hasn't confirmed a next-episode date yet but the season clearly has
    // more episodes coming, we guess a weekly-cadence date rather than
    // showing the show as "released." lastVerifiedDate/StatusLabel are the
    // anchor to revert to if that guess passes without ever being verified.
    bool    isEstimatedDate      = false;
    bool    inMidSeasonBreak     = false;
    QDate   lastVerifiedDate;
    QString lastVerifiedStatusLabel;

    // V5 — when the FIRST estimate lapsed without ever being confirmed.
    // A single unconfirmed guess means nothing (TMDB routinely fills an
    // episode's date in only a couple of days ahead), so a lapsed estimate
    // now rolls forward another week instead of declaring a hiatus. This
    // is the anchor for how long that has been going on: once it's been
    // 5 days with still nothing verified — comfortably past the ~2-day
    // lead time a real date normally appears with — the show genuinely is
    // on a break. Invalid whenever the tile is sitting on a verified date.
    QDate   unverifiedSince;

    // V5 — WHICH release of this tile has already been shown in a startup
    // recap. Deliberately not a plain "seen" flag: a show is one tile across
    // its whole run, so a boolean would mark the tile seen at episode 2 and
    // then never recap episodes 3, 4, 5… again. Recording the specific
    // release means each new episode counts as its own missed event.
    //
    // The notifier runs constantly and fires as things release, but the main
    // app doesn't, so anything that came out while it was closed is simply
    // missed — that's what the recap catches up on. Separate from
    // notifStatus, which tracks the notifier's own lifecycle: a tile can be
    // notified without ever having been recapped, and vice versa.
    QDate   recappedDate;
    QString recappedLabel;

    // V5 — the user typed their own season/episode/total in the Edit
    // dialog. Without this flag the override lasted only until the next
    // refresh: statusLabel is scraped data, so any refresh replaced it —
    // and moving a tile between Countdowns and Released triggers exactly
    // such a refresh, which is why the numbers appeared to "snap back" the
    // moment the date was edited.
    //
    // While set, refreshes leave the episode label alone. It clears the
    // instant a source reports a genuinely DIFFERENT date, since that means
    // real data has caught up and should win — which is the behaviour
    // originally asked for: "those custom numbers and date will be replaced
    // the next time a new date is fetched."
    bool    episodeOverride = false;

    // V5 — the last season/episode/total a DATA SOURCE reported, kept
    // alongside the possibly-overridden live values above.
    //
    // Without these there is nothing to revert to: once a tile is
    // overridden, statusLabel and seasonEpisodeCount ARE the override, and
    // the real numbers exist nowhere. Storing them means Reset restores
    // instantly (no refetch round-trip) and the Edit dialog can tell
    // "matches the source" from "hand-typed" to grey its Reset correctly.
    QString officialStatusLabel;
    int     officialSeasonEpisodeCount = 0;

    // V5 — the only thing known about this release is a month ("March
    // 2027"), not a day. targetDate holds the LAST day of that month so the
    // countdown is a maximum: it can't reach zero before the show could
    // possibly air, and by the time the month arrives a real date is nearly
    // always published and replaces this outright.
    //
    // The tile deliberately renders just "March 2027" — showing a specific
    // day the source never stated would be inventing precision.
    bool    isMonthOnlyDate = false;

    // V5.4 — the same idea one step coarser: the only thing announced is a
    // YEAR. TVmaze writes this as "returning 2026" with no month at all
    // (Dexter: Resurrection is the case this was built for), and until that
    // fix the missing month simply failed the parse, so the tile sat on "No
    // Release Date Yet" while the source did in fact say something useful.
    //
    // Same contract as isMonthOnlyDate: targetDate holds the LAST day of the
    // window — December 31 — so the countdown is the maximum possible wait
    // and cannot reach zero before the show could plausibly air. The tile
    // renders just "2026".
    //
    // Kept as its own flag rather than folded into isMonthOnlyDate because
    // the two differ in precision and a future change might want to treat
    // them differently; isWindowDate() below is what code should test when
    // it only cares that the date is a window rather than a real day.
    bool    isYearOnlyDate = false;

    // True when the date this tile counts down to is a WINDOW BOUND rather
    // than a real release day. Anything that would state or derive day-level
    // precision — a weekday name, an air time, a weekly cadence roll-forward,
    // an override's dayShift — must not act on such a date.
    //
    // A customDate ends that: the user typing an exact day IS a real day, and
    // it's what effectiveDate() counts down to, so suppressing the weekday or
    // sending the lapsed tile to Other would be second-guessing a date they
    // deliberately entered.
    bool isWindowDate() const {
        return (isMonthOnlyDate || isYearOnlyDate) && !customDate.isValid();
    }

    // V5.4 — has anything of this title ever actually come out? It decides
    // what happens when an announced window ("2026") runs out without a real
    // date ever being published, because the honest answer differs:
    //
    //   already aired  → the last episode that DID air is still the most
    //                    recent real thing, so the tile goes back to showing
    //                    that, in Released
    //   never aired    → there is nothing to fall back to, so the tile has no
    //                    date at all and belongs in Other
    //
    // lastVerifiedDate is the test because it is only ever set from a date a
    // source actually published for an episode (TvmazeScraper::applyEpisodes),
    // so it cannot be true of a show that has never had one. The premiere
    // year corroborates it for tiles that predate that field being tracked.
    bool hasAiredBefore() const {
        if (lastVerifiedDate.isValid()) return true;
        if (releaseYear > 0 && releaseYear <= QDate::currentDate().year()) return true;
        // "returning for season 2" is itself a statement that season 1 exists.
        EpisodeLabel el = parseEpisodeLabel(statusLabel);
        return el.valid && el.season > 1;
    }

    // Where a lapsed window belongs, asked in one place so the tab filter and
    // the transition that rewrites the tile cannot disagree with each other
    // (which would show up as a tile flicking between tabs for a tick).
    // Released needs a real past date to show; without one there is nothing
    // to display and Other is the only honest answer.
    bool lapsedWindowGoesToReleased() const {
        return hasAiredBefore() && lastVerifiedDate.isValid();
    }

    // V5.4 — which relay correction is currently BAKED INTO this tile, and
    // what it replaced.
    //
    // Corrections have to be written into airTime/targetDate rather than
    // consulted at display time, because the notifier reads tiles.json
    // directly and knows nothing about the relay — an unbaked correction
    // would mean the app and the notification disagreed about the time.
    //
    // Baking is destructive, though, and that made deleting a correction on
    // the dashboard do nothing for anyone who had already received it: the
    // client just stopped being told about a correction whose effects it had
    // already applied, with no memory of what the value had been. Recording
    // the replaced value is what makes the correction reversible, so a
    // deletion actually reverts everyone instead of only affecting installs
    // that had never seen it.
    //
    // overrideKey empty means nothing is applied and the other two are
    // meaningless. It also makes apply() idempotent, which fixes a second
    // bug: a dayShift used to be re-added on every single pass, so a
    // corrected tile's date crept forward a day at a time.
    QString overrideKey;
    QTime   overrideBaseAirTime;   // airTime before the correction was applied
    int     overrideDayShift = 0;  // shift currently baked into targetDate

    // V5.4.5 — a correction can also state a release WINDOW ("returns March
    // 2027"), for a show that comes back the same rough time each year before
    // any source says so. That replaces the date outright rather than nudging
    // it, so undoing needs the whole previous date, not a delta.
    //
    // overrideBaseWindowKind records which flag was set before: 0 none,
    // 1 month, 2 year. A plain bool pair would do, but one number keeps the
    // JSON to a single extra key and can't be left in a half-restored state.
    QDate   overrideBaseTargetDate;
    QString overrideBaseDateDisplay;
    int     overrideBaseWindowKind = 0;

    // Called by a scraper whenever it writes genuinely fresh source values,
    // since those replace whatever a correction had baked in and become the
    // new base to measure against.
    void clearOverrideBase() {
        overrideKey.clear();
        overrideBaseAirTime = QTime();
        overrideDayShift    = 0;
        overrideBaseTargetDate  = QDate();
        overrideBaseDateDisplay.clear();
        overrideBaseWindowKind  = 0;
    }

    // V5.4.26 — WHICH release a recap should be talking about for this tile.
    //
    // Normally that is simply the date the tile is showing. Looped tiles are
    // the exception, because they advance the instant their occurrence
    // arrives: what actually happened is loopLastOccurrence, and what the tile
    // now shows is next year's. Asking effectiveDate() there gets a future
    // date and the occurrence is silently dropped — which is exactly why
    // birthdays and holidays never appeared in the startup recap.
    //
    // One function so the popup, the permanent history and "have we already
    // shown this?" cannot disagree about what a looped tile's last release
    // was. A tile still sitting on its own past occurrence answers with that,
    // so a loop the notifier hasn't rolled yet works the same way.
    QDate recapCandidateDate() const {
        const QDate d = effectiveDate();
        if (isLooped && loopLastOccurrence.isValid()
            && (!d.isValid() || d > loopLastOccurrence))
            return loopLastOccurrence;
        return d;
    }

    // Has that release actually happened? An occurrence already advanced past
    // is in the past by definition; anything else is the tile's own expiry
    // test, which accounts for the air time on the day itself.
    bool recapCandidateArrived() const {
        const QDate when = recapCandidateDate();
        if (!when.isValid()) return false;
        if (when == effectiveDate()) return isExpired();
        return true;
    }

    // Has the release this tile is CURRENTLY sitting on already been
    // recapped? False for a tile that has since advanced to a new episode —
    // or, for a looped tile, to a new occurrence.
    bool alreadyRecapped() const {
        return recappedDate.isValid()
            && recappedDate  == recapCandidateDate()
            && recappedLabel == statusLabel;
    }

    QString displayTitle()  const { return customTitle.isEmpty() ? title : customTitle; }
    QString displayDate()   const { return customDateStr.isEmpty() ? dateDisplay : customDateStr; }
    QDate   effectiveDate() const { return customDate.isValid() ? customDate : targetDate; }

    // v3.0.0 — when no scraped/custom time is set, the default air time now
    // depends on the Settings → Time Zone preference: theatrical movies use a
    // fixed noon local time, digital movies/shows shift with the selected
    // zone, and games/custom/special tiles are unaffected (still midnight).
    // Always returns a valid QTime now (previously could return invalid).
    QTime   effectiveTime() const {
        if (customAirTime.isValid()) return customAirTime;
        if (airTime.isValid())       return airTime;
        if (mediaType == "movie")
            return isTheatrical ? TimeZoneUtil::defaultTheatricalTime()
                                 : TimeZoneUtil::defaultDigitalTime();
        if (mediaType == "tv")
            return TimeZoneUtil::defaultDigitalTime();
        return QTime(0, 0, 0);   // games / custom / special
    }

    bool isExpired() const {
        QDate d = effectiveDate();
        if (!d.isValid()) return false;
        if (d < QDate::currentDate()) return true;
        if (d > QDate::currentDate()) return false;
        // d == today: use midnight as default when no specific air time is set.
        // This matches the countdown widget which also defaults to QTime(0,0,0).
        // Without this fix, tiles with "today" + no time show countdown=0 but
        // never move to the Released tab because isExpired() returned false.
        QTime t = effectiveTime().isValid() ? effectiveTime() : QTime(0, 0, 0);
        return QTime::currentTime() >= t;
    }
    bool hasDate()       const { return !noDateOverride && effectiveDate().isValid(); }
    int  daysRemaining() const { return QDate::currentDate().daysTo(effectiveDate()); }
    bool isValid()       const { return !id.isEmpty() && !title.isEmpty(); }

    // v3.1.0 — every known image for this tile, in display order: the
    // fetched backdrop (if any) always first, then custom images oldest to
    // newest. Used for the hover-to-cycle preview in the Edit dialog.
    // V5 — should the fetched backdrop be downloaded again? See the
    // artworkSeason/artworkFetchedOn comments above for the reasoning.
    // 150 days rather than a full 180 leaves a month of slack, so a user
    // who opens the app irregularly can't drift past TMDB's 6-month bound
    // simply by not launching it for a few weeks.
    static constexpr int kArtworkMaxAgeDays = 150;

    bool artworkNeedsRefresh(int currentSeason) const {
        if (fetchedImagePath.isEmpty()) return false;   // nothing cached yet — handled separately
        if (currentSeason > 0 && artworkSeason > 0 && currentSeason != artworkSeason)
            return true;                                 // new season, new artwork
        if (!artworkFetchedOn.isValid()) return true;    // pre-V5 tile, age unknown
        return artworkFetchedOn.daysTo(QDate::currentDate()) >= kArtworkMaxAgeDays;
    }

    QStringList allImagePaths() const {
        QStringList list;
        if (!fetchedImagePath.isEmpty()) list << fetchedImagePath;
        list += customImagePaths;
        return list;
    }
};
