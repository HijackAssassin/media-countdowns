#pragma once
#include <QDate>
#include <QTime>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include "tiledata.h"

// =============================================================================
//  ReleaseHistory — V5.4.22. What has actually come out, kept permanently.
//
//  The app never recorded this. A tile only holds its NEXT release: when
//  Reacher moves from S04E02 to S04E03, the fact that S04E02 aired on a
//  particular day is simply gone. So a recap could never have been built from
//  the tiles alone — it needed somewhere to write things down.
//
//  How entries get here, and the one thing to understand about the design:
//  nothing calls "record this release" at the moment a release happens. There
//  are too many paths that move a tile past a date — the global tick, a
//  refresh, the local episode advance, the startup catch-up — and a list of
//  call sites is exactly the pattern that has caused five bugs in this
//  codebase already. Instead sweep() looks at every tile and records any date
//  that is now in the past and isn't recorded yet. It runs on every save, and
//  is idempotent: seeing the same episode a hundred times adds one entry.
//
//  What that buys: it is self-healing. A path that moves a tile without
//  telling anyone still gets picked up on the next sweep, and the very first
//  sweep on an existing installation back-fills whatever the tiles currently
//  know, so the recap isn't empty on the day it ships.
//
//  What it costs, stated plainly: the app can only record what it can see.
//  Episodes that aired and were advanced past while a previous version was
//  running were never written down and cannot be recovered. History starts
//  from the first sweep, plus whatever each tile was holding at that moment.
// =============================================================================
class ReleaseHistory
{
public:
    struct Entry {
        QDate   date;
        // V5.4.26 — the time it came out, so the list can say "August 4
        // @4:00PM" like the startup recap does. Invalid on entries written
        // before this existed; sweep() fills those in where it still can, and
        // a row without one simply shows the date.
        QTime   time;
        QString title;
        QString statusLabel;      // "S04E02" when there is one, else empty
        int     episodeTotal = 0; // the season's episode count, 0 when unknown
        QString mediaType;
        QString tileId;

        // What makes two records the same release. Deliberately NOT the tile
        // id alone: a tile is one show across its whole run, and every episode
        // of it is a separate release.
        //
        // The time is deliberately NOT part of it: deletions are remembered by
        // key, so adding a field to this would orphan every key already
        // written down as removed and bring those entries back.
        QString key() const {
            return tileId + "|" + date.toString(Qt::ISODate) + "|" + statusLabel;
        }
    };

    static ReleaseHistory& instance()
    {
        static ReleaseHistory inst;
        return inst;
    }

    // Newest first, which is the order the recap reads in.
    //
    // Every reader loads first. The load used to happen only in sweep(), which
    // worked purely because the recap sweeps before it reads — a harness that
    // just asked for the years got an empty list from a file with five entries
    // in it. Anything that depends on being called in the right order will
    // eventually be called in the wrong one.
    QList<Entry> all() const { ensureLoaded(); return m_entries; }

    QList<int> years() const
    {
        ensureLoaded();
        QList<int> out;
        for (const Entry& e : m_entries)
            if (!out.contains(e.date.year())) out << e.date.year();
        std::sort(out.begin(), out.end(), std::greater<int>());
        return out;
    }

    QList<Entry> forYear(int year) const
    {
        ensureLoaded();
        QList<Entry> out;
        for (const Entry& e : m_entries)
            if (e.date.year() == year) out << e;
        return out;
    }

    // Records anything that has released and isn't already known. Returns true
    // if the file changed, so callers can avoid a pointless write.
    bool sweep(const QList<TileData>& tiles)
    {
        ensureLoaded();
        const QDate today = QDate::currentDate();
        bool added = false;

        for (const TileData& td : tiles) {
            if (!td.hasDate()) continue;
            // A window ("March 2027") is not a release — targetDate is only the
            // last day of the window, and treating it as one would announce a
            // release on a day nothing was said to happen. Same rule the
            // notifier uses; see TileData::isWindowDate().
            if (td.isWindowDate()) continue;
            if (td.noDateOverride) continue;
            // Looped tiles ARE recorded. The rule is "if it would fire a
            // notification, it belongs here", and the notifier announces a
            // looped occurrence like any other (it advances it too — see
            // loopschedule.h). A birthday that came round is a thing that
            // happened, and leaving it out made the page disagree with the
            // notifications the user had already been shown.
            //
            // V5.4.26 — and asking recapCandidateDate() rather than
            // effectiveDate() is what finally makes that true. A looped tile
            // is advanced within a second of its occurrence arriving, so its
            // own date is essentially never in the past for a sweep to find:
            // the intent above was right and the code could not act on it.
            const QDate when = td.recapCandidateDate();
            if (!when.isValid() || when > today) continue;   // hasn't happened yet

            Entry e;
            e.date         = when;
            e.time         = td.effectiveTime();
            e.title        = td.displayTitle();
            e.statusLabel  = td.statusLabel.startsWith('S') ? td.statusLabel : QString();
            e.episodeTotal = td.seasonEpisodeCount;
            e.mediaType    = td.mediaType;
            e.tileId       = td.id;

            if (m_keys.contains(e.key())) {
                // Already recorded — but entries written before V5.4.26 have
                // no time on them. While the tile that produced one is still
                // showing that release, the time can be filled in, so an old
                // row doesn't sit in the list missing what every row around it
                // has. Nothing else about a recorded entry is ever rewritten.
                if (!e.time.isValid()) continue;
                for (Entry& stored : m_entries) {
                    if (stored.key() != e.key() || stored.time.isValid()) continue;
                    stored.time = e.time;
                    added = true;
                    break;
                }
                continue;
            }
            if (m_removed.contains(e.key())) continue;   // deleted on purpose
            m_keys.insert(e.key());
            m_entries.append(e);
            added = true;
        }

        if (added) {
            std::sort(m_entries.begin(), m_entries.end(),
                      [](const Entry& a, const Entry& b) { return a.date > b.date; });
            save();
        }
        return added;
    }

    // Removes one entry, permanently.
    //
    // The key is remembered as removed, not merely dropped. sweep() records
    // whatever a tile is currently showing with a past date, so deleting the
    // episode a tile is STILL showing would last until the next save and then
    // silently come back — which a test caught doing exactly that. A deletion
    // the user has to make twice is worse than no delete button.
    bool remove(const QString& entryKey)
    {
        ensureLoaded();
        m_removed.insert(entryKey);
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].key() != entryKey) continue;
            m_keys.remove(entryKey);
            m_entries.removeAt(i);
            save();
            return true;
        }
        save();   // remember the removal even if the entry was already gone
        return false;
    }

    QString filePath() const
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
               + "/release_history.json";
    }

private:
    ReleaseHistory() = default;

    void ensureLoaded() const
    {
        if (!m_loaded) const_cast<ReleaseHistory*>(this)->load();
    }

    void load()
    {
        m_loaded = true;
        QFile f(filePath());
        if (!f.open(QIODevice::ReadOnly)) return;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        // The first version of this file was a bare array. Both shapes are
        // read, so an existing history is not thrown away by the upgrade.
        const QJsonArray arr = doc.isArray() ? doc.array()
                                             : doc.object()["entries"].toArray();
        for (const QJsonValue& rv : doc.object()["removed"].toArray())
            m_removed.insert(rv.toString());
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            Entry e;
            e.date         = QDate::fromString(o["date"].toString(), Qt::ISODate);
            e.time         = QTime::fromString(o["time"].toString(), "HH:mm");   // V5.4.26; absent on older entries
            e.title        = o["title"].toString();
            e.statusLabel  = o["statusLabel"].toString();
            e.episodeTotal = o["episodeTotal"].toInt(0);
            e.mediaType    = o["mediaType"].toString();
            e.tileId       = o["tileId"].toString();
            if (!e.date.isValid() || e.title.isEmpty()) continue;
            if (m_keys.contains(e.key())) continue;
            m_keys.insert(e.key());
            m_entries.append(e);
        }
        std::sort(m_entries.begin(), m_entries.end(),
                  [](const Entry& a, const Entry& b) { return a.date > b.date; });
    }

    void save() const
    {
        QJsonArray arr;
        for (const Entry& e : m_entries) {
            QJsonObject o;
            o["date"]         = e.date.toString(Qt::ISODate);
            o["time"]         = e.time.isValid() ? e.time.toString("HH:mm") : QString();   // V5.4.26
            o["title"]        = e.title;
            o["statusLabel"]  = e.statusLabel;
            o["episodeTotal"] = e.episodeTotal;
            o["mediaType"]    = e.mediaType;
            o["tileId"]       = e.tileId;
            arr.append(o);
        }
        QJsonArray removed;
        for (const QString& k : m_removed) removed.append(k);
        QJsonObject root;
        root["entries"] = arr;
        root["removed"] = removed;

        // QSaveFile for the same reason tiles.json uses one: this is a record
        // that cannot be rebuilt from anywhere else, so a half-written file
        // would be a permanent loss rather than an inconvenience.
        QDir().mkpath(QFileInfo(filePath()).absolutePath());
        QSaveFile f(filePath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (f.write(payload) != payload.size()) { f.cancelWriting(); return; }
        f.commit();
    }

    // mutable so the const readers above can fault the file in on first use.
    mutable bool   m_loaded = false;
    QList<Entry>   m_entries;
    QSet<QString>  m_keys;
    QSet<QString>  m_removed;   // keys deleted by hand; sweep() must not re-add them
};
