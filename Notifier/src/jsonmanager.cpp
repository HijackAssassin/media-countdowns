#include "jsonmanager.h"
#include <QStandardPaths>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QSet>

JsonManager& JsonManager::instance()
{
    static JsonManager inst;
    return inst;
}

QString JsonManager::dataFilePath() const
{
    return qEnvironmentVariable("APPDATA") + "/MediaCountdowns/tiles.json";
}

QList<TileData> JsonManager::loadTiles() const
{
    QList<TileData> result;
    QFile f(dataFilePath());
    if (!f.open(QIODevice::ReadOnly)) return result;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isArray()) return result;

    for (const QJsonValue& v : doc.array()) {
        QJsonObject o = v.toObject();
        TileData td;
        td.id            = o["id"].toString();
        td.title         = o["title"].toString();
        td.customTitle   = o["customTitle"].toString();
        td.tmdbId        = o["tmdbId"].toInt();
        td.mediaType     = o["mediaType"].toString();
        td.tmdbUrl       = o["tmdbUrl"].toString();
        td.statusLabel   = o["statusLabel"].toString();
        td.rawDateText   = o["rawDateText"].toString();
        td.dateDisplay   = o["dateDisplay"].toString();
        td.customDateStr = o["customDateStr"].toString();
        td.targetDate    = QDate::fromString(o["targetDate"].toString(), Qt::ISODate);
        td.customDate    = QDate::fromString(o["customDate"].toString(), Qt::ISODate);
        td.airTime       = QTime::fromString(o["airTime"].toString(), "HH:mm");
        // customAirTime stored as minutes-since-midnight integer (e.g. 21*60=1260 for 9pm)
        // -1 means not set (no override)
        int airMins = o["customAirMins"].toInt(-1);
        td.customAirTime = (airMins >= 0 && airMins < 1440)
            ? QTime(airMins / 60, airMins % 60)
            : QTime();   // invalid = no override
        td.imagePath     = o["imagePath"].toString();
        td.mediaType     = o["mediaType"].toString();
        td.notified      = o["notified"].toBool(false);

        // State the main app owns that decides whether a passed date is
        // actually a release. All default to false, so a tiles.json written
        // by an older main app still loads and behaves exactly as before.
        td.noDateOverride  = o["noDateOverride"].toBool(false);
        td.isLooped        = o["isLooped"].toBool(false);
        td.loopInterval    = o["loopInterval"].toString();
        td.loopWeekday     = o["loopWeekday"].toInt(1);
        td.loopDayOfMonth  = o["loopDayOfMonth"].toInt(1);
        td.presetType      = o["presetType"].toString();
        td.pendingLoopNotice = o["pendingLoopNotice"].toBool(false);
        td.loopLastOccurrence = QDate::fromString(o["loopLastOccurrence"].toString(), Qt::ISODate);   // V5.4.26
        td.isEstimatedDate = o["isEstimatedDate"].toBool(false);
        td.isMonthOnlyDate = o["isMonthOnlyDate"].toBool(false);
        td.isYearOnlyDate  = o["isYearOnlyDate"].toBool(false);

        // notifStatus: "Active" / "Ready" / "Inactive"
        QString ns = o["notifStatus"].toString("Active");
        if (ns == "Inactive")     td.notifStatus = NotifStatus::Inactive;
        else if (ns == "Ready")   td.notifStatus = NotifStatus::Ready;
        else                      td.notifStatus = NotifStatus::Active;
        // Legacy migration: if old notified==true, treat as Inactive
        if (td.notified && td.notifStatus == NotifStatus::Active)
            td.notifStatus = NotifStatus::Inactive;

        // Sanitise legacy status labels
        if (td.statusLabel == "Returning Series" || td.statusLabel == "Ended")
            td.statusLabel = "Last Episode";

        if (!td.id.isEmpty()) result.append(td);
    }
    return result;
}

// =============================================================================
//  saveTiles — write back ONLY what this program owns.
//
//  This used to rebuild every tile's JSON object from the hand-written field
//  list below and then truncate the file, which meant a save here DELETED
//  every field the notifier doesn't know about. That is most of them: the
//  main app writes tvmazeId/tvmazeUrl, fetchedImagePath, customImagePaths,
//  isFavorite, tagColor, the loop settings, presetType, episodeOverride,
//  officialStatusLabel, isMonthOnlyDate/isYearOnlyDate, recappedDate/Label,
//  artworkSeason/artworkFetchedOn and more. saveTiles() runs from
//  TrayApp::onHeartbeat() whenever a notification fires, so an ordinary
//  release quietly wiped a user's favourites, tag colours, custom images and
//  loop schedules.
//
//  The notifier's actual job is two fields — `notified` and `notifStatus`.
//  So the file on disk is re-read here and used as the base: each tile's
//  stored object is kept exactly as it is and only those two keys are
//  updated, leaving every unknown key untouched. Nothing else needs to know
//  what fields exist, which is the point — this is the same
//  hand-maintained-field-list bug that hit the main app three times, and a
//  list that no longer needs maintaining cannot fall behind again.
//
//  The disk array also drives the ORDER and the membership: it is the
//  freshest statement of which tiles exist, so a tile the main app deleted a
//  moment ago doesn't come back from this program's in-memory copy. A tile
//  held in memory but absent from disk is still appended (see below), so
//  nothing is lost if the file is missing, empty or unreadable.
// =============================================================================
bool JsonManager::saveTiles(const QList<TileData>& tiles,
                            const QSet<QString>& rolledIds) const
{
    QDir().mkpath(qEnvironmentVariable("APPDATA") + "/MediaCountdowns");

    // Re-read rather than trusting the copy loaded at the start of the tick:
    // the main app may have written since, and its version of every
    // non-notifier field is newer than ours by definition.
    QJsonArray onDisk;
    {
        QFile f(dataFilePath());
        if (f.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isArray()) onDisk = doc.array();
        }
    }

    // The notifier-owned state, keyed by tile id.
    //
    // pendingLoopNotice is included because this program is what DISCHARGES
    // it: the main app raises the flag when it advances a looped tile, and
    // announcing that occurrence is what clears it. Writing false here can
    // only ever follow having actually notified.
    struct OwnedState { bool notified; QString notifStatus; bool pendingLoopNotice; };
    auto ownedStateOf = [](const TileData& td) {
        QString ns;
        switch (td.notifStatus) {
            case NotifStatus::Inactive: ns = "Inactive"; break;
            case NotifStatus::Ready:    ns = "Ready";    break;
            default:                    ns = "Active";   break;
        }
        return OwnedState{ td.notified, ns, td.pendingLoopNotice };
    };

    QHash<QString, OwnedState> updates;
    QHash<QString, TileData>   rolled;   // looped tiles this heartbeat advanced
    for (const TileData& td : tiles) {
        if (td.id.isEmpty()) continue;
        updates.insert(td.id, ownedStateOf(td));
        if (rolledIds.contains(td.id)) rolled.insert(td.id, td);
    }

    QJsonArray arr;
    QSet<QString> written;

    // Pass 1 — every tile already on disk, preserved whole.
    for (const QJsonValue& v : onDisk) {
        QJsonObject o  = v.toObject();
        QString     id = o["id"].toString();
        auto it = updates.constFind(id);
        if (it != updates.constEnd()) {
            o["notified"]          = it->notified;
            o["notifStatus"]       = it->notifStatus;
            o["pendingLoopNotice"] = it->pendingLoopNotice;

            // A looped tile we advanced: its new occurrence is ours to
            // record. Only these five move — everything else about the tile
            // still belongs to the main app and stays exactly as written.
            auto r = rolled.constFind(id);
            if (r != rolled.constEnd()) {
                o["targetDate"]    = r->targetDate.toString(Qt::ISODate);
                o["customDate"]    = QString();   // cleared by the advance
                o["dateDisplay"]   = r->dateDisplay;
                o["customDateStr"] = QString();
                // V5.4.26 — and the occurrence it advanced away from. Written
                // here rather than left to the main app because the main app
                // wasn't running: this roll is the whole reason the date it
                // describes is no longer anywhere on the tile.
                o["loopLastOccurrence"] = r->loopLastOccurrence.isValid()
                    ? r->loopLastOccurrence.toString(Qt::ISODate) : QString();
            }
            written.insert(id);
        }
        // A tile we have no state for (or one with no id at all) is copied
        // through untouched rather than dropped — not recognising an entry
        // is not a reason to delete somebody's data.
        arr.append(o);
    }

    // Pass 2 — anything held in memory that wasn't on disk. Only here does
    // the field list below apply, and only because there is no stored object
    // to preserve; it cannot destroy anything by definition.
    for (const TileData& td : tiles) {
        if (td.id.isEmpty() || written.contains(td.id)) continue;

        QJsonObject o;
        o["id"]            = td.id;
        o["title"]         = td.title;
        o["customTitle"]   = td.customTitle;
        o["tmdbId"]        = td.tmdbId;
        o["mediaType"]     = td.mediaType;
        o["tmdbUrl"]       = td.tmdbUrl;
        o["statusLabel"]   = td.statusLabel;
        o["rawDateText"]   = td.rawDateText;
        o["dateDisplay"]   = td.dateDisplay;
        o["customDateStr"] = td.customDateStr;
        o["targetDate"]    = td.targetDate.toString(Qt::ISODate);
        o["customDate"]    = td.customDate.isValid() ? td.customDate.toString(Qt::ISODate) : "";
        o["airTime"]       = td.airTime.isValid() ? td.airTime.toString("HH:mm") : "";
        // Store as minutes-since-midnight integer; -1 = not set
        o["customAirMins"] = td.customAirTime.isValid()
            ? (td.customAirTime.hour() * 60 + td.customAirTime.minute())
            : -1;
        o["imagePath"]     = td.imagePath;

        OwnedState st = ownedStateOf(td);
        o["notified"]      = st.notified;
        o["notifStatus"]   = st.notifStatus;
        arr.append(o);
    }

    // V5.4.12 — the same QSaveFile change made in the main app's copy, and for
    // the same reason: this writes the user's ONLY tiles.json, and doing it
    // in place meant any interruption mid-write left a truncated file behind.
    // The notifier matters more here, not less — it runs unattended in the
    // background, so a machine shutting down while it saves is an ordinary
    // event rather than a rare one.
    QSaveFile f(dataFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray payload = QJsonDocument(arr).toJson(QJsonDocument::Indented);
    if (f.write(payload) != payload.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}
