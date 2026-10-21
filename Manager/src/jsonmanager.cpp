#include "jsonmanager.h"
#include <QStandardPaths>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

JsonManager& JsonManager::instance()
{
    static JsonManager inst;
    return inst;
}

QString JsonManager::dataFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tiles.json";
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
        TileData td = tileFromJson(v.toObject());
        if (!td.id.isEmpty()) result.append(td);
    }
    return result;
}

// V5 — see the header for why this exists. Shared by loadTiles() above and
// MainWindow's import path, which previously hand-rolled its own copy.
TileData JsonManager::tileFromJson(const QJsonObject& o)
{
    {
        TileData td;
        td.id            = o["id"].toString();
        td.title         = o["title"].toString();
        td.customTitle   = o["customTitle"].toString();
        td.tmdbId        = o["tmdbId"].toInt();
        td.tvmazeId      = o["tvmazeId"].toInt(0);   // V5
        td.artworkSeason    = o["artworkSeason"].toInt(0);   // V5
        td.artworkFetchedOn = QDate::fromString(o["artworkFetchedOn"].toString(), Qt::ISODate);
        td.mediaType     = o["mediaType"].toString();
        td.tmdbUrl       = o["tmdbUrl"].toString();
        td.tvmazeUrl     = o["tvmazeUrl"].toString();   // V5
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
        // V5.4.28 — the absolute instant behind airTime, so a Time Zone change
        // can be applied without refetching. Absent on pre-V5.4.28 tiles.
        td.airStampUtc   = QDateTime::fromString(o["airStampUtc"].toString(), Qt::ISODate);
        if (td.airStampUtc.isValid()) td.airStampUtc.setTimeSpec(Qt::UTC);
        td.imagePath     = o["imagePath"].toString();
        td.mediaType     = o["mediaType"].toString();
        td.notified      = o["notified"].toBool(false);
        td.isLooped      = o["isLooped"].toBool(false);
        td.pendingLoopNotice = o["pendingLoopNotice"].toBool(false);   // V5.4.3
        td.loopLastOccurrence = QDate::fromString(o["loopLastOccurrence"].toString(), Qt::ISODate);   // V5.4.26
        td.noDateOverride= o["noDateOverride"].toBool(false);
        td.presetType    = o["presetType"].toString();
        {
            QString tagHex = o["tagColor"].toString();
            td.tagColor = tagHex.isEmpty() ? QColor() : QColor(tagHex);
            QString textHex = o["textColor"].toString();
            td.textColor = textHex.isEmpty() ? QColor() : QColor(textHex);
        }
        td.isFavorite = o["isFavorite"].toBool();
        td.loopInterval  = o["loopInterval"].toString("Yearly");
        td.loopWeekday   = o["loopWeekday"].toInt(1);
        td.loopDayOfMonth= o["loopDayOfMonth"].toInt(1);
        td.isTheatrical  = o["isTheatrical"].toBool(false);
        td.releaseYear        = o["releaseYear"].toInt(0);
        td.seasonEpisodeCount = o["seasonEpisodeCount"].toInt(0);

        // v3.1.0 Feature 1 — multi-image support
        if (o.contains("fetchedImagePath") || o.contains("customImagePaths")) {
            td.fetchedImagePath = o["fetchedImagePath"].toString();
            for (const QJsonValue& iv : o["customImagePaths"].toArray())
                td.customImagePaths << iv.toString();
        } else if (!td.imagePath.isEmpty()) {
            // Migrating a pre-3.1.0 tile — it only ever had one image path.
            // Guess whether it was a user-picked custom image or the
            // auto-fetched backdrop from where the file lives on disk.
            if (td.imagePath.contains("custom_images"))
                td.customImagePaths << td.imagePath;
            else
                td.fetchedImagePath = td.imagePath;
        }

        // v3.1.0 Feature 3 — estimated (unverified) episode dates
        td.isEstimatedDate       = o["isEstimatedDate"].toBool(false);
        td.inMidSeasonBreak      = o["inMidSeasonBreak"].toBool(false);
        td.lastVerifiedDate      = QDate::fromString(o["lastVerifiedDate"].toString(), Qt::ISODate);
        td.lastVerifiedStatusLabel = o["lastVerifiedStatusLabel"].toString();
        td.unverifiedSince       = QDate::fromString(o["unverifiedSince"].toString(), Qt::ISODate);   // V5
        td.recappedDate          = QDate::fromString(o["recappedDate"].toString(), Qt::ISODate);   // V5
        td.recappedLabel         = o["recappedLabel"].toString();   // V5
        td.episodeOverride       = o["episodeOverride"].toBool(false);   // V5
        td.officialStatusLabel   = o["officialStatusLabel"].toString();   // V5
        td.officialSeasonEpisodeCount = o["officialSeasonEpisodeCount"].toInt(0);   // V5
        td.isMonthOnlyDate       = o["isMonthOnlyDate"].toBool(false);   // V5
        td.isYearOnlyDate        = o["isYearOnlyDate"].toBool(false);    // V5.4
        // V5.4 — what a relay correction replaced, so deleting it can revert.
        td.overrideKey           = o["overrideKey"].toString();
        td.overrideBaseAirTime   = o["overrideBaseAirTime"].toString().isEmpty()
                                     ? QTime()
                                     : QTime::fromString(o["overrideBaseAirTime"].toString(), "HH:mm");
        td.overrideDayShift      = o["overrideDayShift"].toInt(0);
        td.overrideBaseTargetDate  = QDate::fromString(o["overrideBaseTargetDate"].toString(), Qt::ISODate);
        td.overrideBaseDateDisplay = o["overrideBaseDateDisplay"].toString();
        td.overrideBaseWindowKind  = o["overrideBaseWindowKind"].toInt(0);

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

        return td;
    }
}

// v3.3.44/45 fix — investigated a report of episode notifications firing
// at midnight despite a tile's displayed countdown correctly showing a
// Time-Zone-adjusted time (e.g. 3 AM). Root cause: the separate TrayApp
// process (not part of this codebase) reads this same tiles.json file to
// decide when to fire a notification, and the "airTime" field below was
// only ever populated when an explicit time was set (never the case in
// the current codebase — TMDB's own air_time data is deliberately
// discarded elsewhere as unreliable, and there's no other code path that
// ever assigns it), so it saved as blank for virtually every tile. With
// no Time Zone awareness of its own, TrayApp's own fallback for a blank
// time field is midnight. v3.3.44 first tried adding a separate
// "effectiveAirTime" field to fix this — this is a simpler correction to
// that: since "airTime" is confirmed dead for its original purpose, the
// current, Time Zone-adjusted default now gets written directly into it
// instead. TrayApp needs no changes at all, since it already reads this
// exact field; "customAirTime" — a real user override — is untouched.
bool JsonManager::saveTiles(const QList<TileData>& tiles) const
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QJsonArray arr;
    for (const TileData& td : tiles) {
        QJsonObject o;
        o["id"]            = td.id;
        o["title"]         = td.title;
        o["customTitle"]   = td.customTitle;
        o["tmdbId"]        = td.tmdbId;
        o["mediaType"]     = td.mediaType;
        o["tmdbUrl"]       = td.tmdbUrl;
        o["tvmazeUrl"]     = td.tvmazeUrl;   // V5
        o["statusLabel"]   = td.statusLabel;
        o["rawDateText"]   = td.rawDateText;
        o["dateDisplay"]   = td.dateDisplay;
        o["customDateStr"] = td.customDateStr;
        o["targetDate"]    = td.targetDate.toString(Qt::ISODate);
        o["customDate"]    = td.customDate.isValid() ? td.customDate.toString(Qt::ISODate) : "";
        // v3.3.45 — always the current, correctly Time Zone-adjusted
        // value (customAirTime already wins over this when it's set, so
        // writing the computed default here doesn't change precedence
        // for anything that has a real user override).
        o["airTime"]       = td.effectiveTime().toString("HH:mm");
        // Store as minutes-since-midnight integer; -1 = not set
        o["customAirMins"] = td.customAirTime.isValid()
            ? (td.customAirTime.hour() * 60 + td.customAirTime.minute())
            : -1;
        o["airStampUtc"]   = td.airStampUtc.isValid()
            ? td.airStampUtc.toUTC().toString(Qt::ISODate) : "";   // V5.4.28
        o["imagePath"]     = td.imagePath;
        o["fetchedImagePath"] = td.fetchedImagePath;
        {
            QJsonArray customArr;
            for (const QString& p : td.customImagePaths) customArr.append(p);
            o["customImagePaths"] = customArr;
        }
        o["notified"]      = td.notified;
        o["isLooped"]      = td.isLooped;
        o["pendingLoopNotice"] = td.pendingLoopNotice;   // V5.4.3
        o["loopLastOccurrence"] = td.loopLastOccurrence.isValid()
            ? td.loopLastOccurrence.toString(Qt::ISODate) : "";   // V5.4.26
        o["noDateOverride"]= td.noDateOverride;
        o["presetType"]    = td.presetType;
        o["tagColor"]      = td.tagColor.isValid() ? td.tagColor.name() : QString();
        o["textColor"]     = td.textColor.isValid() ? td.textColor.name() : QString();
        o["isFavorite"] = td.isFavorite;
        o["loopInterval"]  = td.loopInterval;
        o["loopWeekday"]   = td.loopWeekday;
        o["loopDayOfMonth"]= td.loopDayOfMonth;
        o["isTheatrical"]  = td.isTheatrical;
        o["releaseYear"]        = td.releaseYear;
        o["seasonEpisodeCount"] = td.seasonEpisodeCount;
        o["tvmazeId"]           = td.tvmazeId;    // V5
        o["artworkSeason"]      = td.artworkSeason;   // V5
        o["artworkFetchedOn"]   = td.artworkFetchedOn.isValid()
                                      ? td.artworkFetchedOn.toString(Qt::ISODate) : "";
        o["isEstimatedDate"]         = td.isEstimatedDate;
        o["inMidSeasonBreak"]        = td.inMidSeasonBreak;
        o["lastVerifiedDate"]        = td.lastVerifiedDate.isValid() ? td.lastVerifiedDate.toString(Qt::ISODate) : "";
        o["unverifiedSince"]         = td.unverifiedSince.isValid() ? td.unverifiedSince.toString(Qt::ISODate) : "";   // V5
        o["recappedDate"]            = td.recappedDate.isValid() ? td.recappedDate.toString(Qt::ISODate) : "";   // V5
        o["recappedLabel"]           = td.recappedLabel;   // V5
        o["episodeOverride"]         = td.episodeOverride;   // V5
        o["officialStatusLabel"]     = td.officialStatusLabel;   // V5
        o["officialSeasonEpisodeCount"] = td.officialSeasonEpisodeCount;   // V5
        o["isMonthOnlyDate"]         = td.isMonthOnlyDate;   // V5
        o["isYearOnlyDate"]          = td.isYearOnlyDate;    // V5.4
        o["overrideKey"]             = td.overrideKey;       // V5.4
        o["overrideBaseAirTime"]     = td.overrideBaseAirTime.isValid()
                                         ? td.overrideBaseAirTime.toString("HH:mm") : QString();
        o["overrideDayShift"]        = td.overrideDayShift;  // V5.4
        o["overrideBaseTargetDate"]  = td.overrideBaseTargetDate.isValid()
                                         ? td.overrideBaseTargetDate.toString(Qt::ISODate) : QString();
        o["overrideBaseDateDisplay"] = td.overrideBaseDateDisplay;
        o["overrideBaseWindowKind"]  = td.overrideBaseWindowKind;
        o["lastVerifiedStatusLabel"] = td.lastVerifiedStatusLabel;
        // notifStatus
        QString ns;
        switch (td.notifStatus) {
            case NotifStatus::Inactive: ns = "Inactive"; break;
            case NotifStatus::Ready:    ns = "Ready";    break;
            default:                    ns = "Active";   break;
        }
        o["notifStatus"]   = ns;
        arr.append(o);
    }
    // V5.4.12 — QSaveFile, not QFile. This used to open tiles.json itself with
    // Truncate and write straight into it, which means the moment the write
    // begins the only copy of every tile the user has ever added is a
    // half-empty file. A crash, a power cut, a full disk or the process being
    // killed mid-write took the lot; the write's return value wasn't checked
    // either, so a failure looked exactly like a success.
    //
    // QSaveFile writes to a temporary file beside it and renames over the
    // original on commit(), which on Windows is atomic. The old file stays
    // untouched until a complete new one exists, so the failure modes above
    // now leave the previous save in place instead of wreckage.
    QSaveFile f(dataFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray payload = QJsonDocument(arr).toJson(QJsonDocument::Indented);
    if (f.write(payload) != payload.size()) {
        f.cancelWriting();   // leaves the existing tiles.json exactly as it was
        return false;
    }
    return f.commit();
}
