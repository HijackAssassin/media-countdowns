#pragma once
#include <QList>
#include <QString>
#include <QSet>
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

    // `rolledIds` names the looped tiles this program advanced to their next
    // occurrence during this heartbeat. Their schedule fields are written as
    // well as the notification state, because advancing them IS this
    // program's decision — for every other tile only notified/notifStatus is
    // touched and the stored object is left exactly as the main app wrote it.
    bool            saveTiles(const QList<TileData>& tiles,
                              const QSet<QString>& rolledIds = {}) const;
    QString         dataFilePath() const;

private:
    JsonManager() = default;
    JsonManager(const JsonManager&) = delete;
    JsonManager& operator=(const JsonManager&) = delete;
};
