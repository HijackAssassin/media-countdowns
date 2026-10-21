#pragma once
#include <QList>
#include <QString>
#include <QJsonObject>
#include "tiledata.h"

// =============================================================================
//  JsonManager – singleton that reads/writes tiles.json
//                Location:  %APPDATA%\CineCountdown\tiles.json  (Windows)
// =============================================================================
class JsonManager
{
public:
    static JsonManager& instance();

    QList<TileData> loadTiles() const;
    bool            saveTiles(const QList<TileData>& tiles) const;
    QString         dataFilePath() const;

    // V5 — the single place a tile is deserialized. This used to be copied
    // out by hand in MainWindow's import path, and the copy had drifted:
    // it silently dropped isLooped, loopInterval, loopWeekday,
    // loopDayOfMonth, presetType, tagColor, isFavorite,
    // notified and noDateOverride, so an imported
    // tile lost its loop schedule, tag colour and favourite star. Both
    // callers now share this, so a newly persisted field can't be wired
    // into one reader and forgotten in the other.
    static TileData tileFromJson(const QJsonObject& o);

private:
    JsonManager() = default;
    JsonManager(const JsonManager&) = delete;
    JsonManager& operator=(const JsonManager&) = delete;
};
