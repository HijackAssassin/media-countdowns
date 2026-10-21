#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <functional>
#include "tiledata.h"
#include "tmdbscraper.h"   // reuse SearchResult — same shape works for games
#include "igdbcredentials.h"

// =============================================================================
//  IgdbScraper — v3.3.0. Video game search/details via IGDB (api.igdb.com),
//  which authenticates through Twitch's OAuth2 "client credentials" flow.
//  Deliberately mirrors TmdbScraper's public interface and signal
//  signatures (searchMedia/fetchDetails/fetchCreditsForResults, and the
//  searchResultsReady/dataReady/posterReady/scraperError signals) so it can
//  be wired into the exact same UI code paths already used for movies/
//  shows, just routed to a different scraper instance when the search
//  mode is set to "Game".
//
//  IMPORTANT: the Client ID is not secret (same treatment as TMDB's key),
//  but the Client Secret is more sensitive — more like a password than a
//  read-only API key, since it's what proves this app's identity to
//  Twitch's OAuth server. Both are stored via QSettings, entered by the
//  user in Settings, and never hardcoded here or anywhere else in source.
// =============================================================================
// V5 — which IGDB image to use for a game tile, and whether it's actually
// wide. isLandscape decides the resize preset: a wide source crops to 16:9
// almost losslessly, while a portrait cover has to keep its aspect ratio or
// it becomes an unrecognisable zoomed-in band through the middle.
struct GameImagePick {
    QString imageId;
    bool    isLandscape = false;
    // V5.4 — is the SOURCE at least 1280x720? Tiles are rendered at 720p, so
    // a smaller source can only get there by being upscaled. When that's the
    // case the download asks IGDB for t_1080p and resamples down locally
    // instead, which reads better than IGDB's own upscale.
    bool    fills720p   = false;
};

class IgdbScraper : public QObject
{
    Q_OBJECT
public:
    explicit IgdbScraper(QObject* parent = nullptr);

    void searchMedia(const QString& query);
    void fetchDetails(int gameId, const QString& mediaType, const QString& posterPath = {});
    // No-op — IGDB search results don't need a separate credits fetch the
    // way TMDB's do, but this keeps the interface identical so shared
    // calling code (onSearchResultsReady) doesn't need to branch on which
    // scraper is active.
    void fetchCreditsForResults(const QList<SearchResult>& results) { Q_UNUSED(results); }

    // Startup refresh — mirrors TmdbScraper::refreshTile(), re-checks a
    // saved game tile's release date in case IGDB's data has since changed
    // (e.g. a delay or a firmer date announced). forceCoverRefetch mirrors
    // TmdbScraper's forceBackdropRefetch — used by "Refetch Image".
    void refreshTile(const TileData& existing, bool forceCoverRefetch = false);

signals:
    void searchResultsReady(const QList<SearchResult>& results);
    void dataReady(const TileData& data);
    void tileRefreshed(const TileData& updated);
    void posterReady(const QString& tileId, const QString& localPath, bool makeActive);
    void scraperError(const QString& message);

private:
    void withAccessToken(std::function<void(const QString& token)> onReady);
    void fetchAccessToken(std::function<void(const QString& token)> onReady);
    // V4 — centralizes what all 5 IGDB request-sending call sites used to
    // duplicate individually (building the request, setting Client-ID/
    // Authorization/Content-Type headers). This is the one place that
    // needs to know about relay vs. direct mode, same role getJson()
    // plays for TmdbScraper.
    QNetworkReply* postToIgdb(const QString& path, const QByteArray& body, const QString& token);
    void downloadCover(const QString& tileId, const QString& imageId,
                       bool makeActive, bool isLandscape = true,
                       bool fills720p = true);
    TileData buildTileData(const QJsonObject& obj, const QString& existingId = {},
                           const QString& existingImage = {});
    // v3.3.1 — extracted so the filter/sort logic is directly testable
    // against a mock JSON array, without needing a live network call.
    QList<SearchResult> filterAndSortResults(const QJsonArray& arr, const QString& query, int yearFilter);

    QNetworkAccessManager* m_nam;
    QString m_accessToken;
    qint64  m_tokenExpiryEpoch = 0;           // epoch seconds; 0 = no token fetched yet
    QHash<int, QJsonObject> m_cachedResults;  // game id -> raw object from the last search,
                                               // so fetchDetails() doesn't need a second API call

public:
    static constexpr const char* BASE       = "https://api.igdb.com/v4";
    static constexpr const char* TOKEN_URL  = "https://id.twitch.tv/oauth2/token";
    static constexpr const char* IMAGE_BASE = "https://images.igdb.com/igdb/image/upload";

    static QString clientId() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        QString v = s.value("igdbClientId").toString().trimmed();
        return v.isEmpty() ? QString(IGDB_CLIENT_ID_DEFAULT) : v;
    }
    static QString clientSecret() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        QString v = s.value("igdbClientSecret").toString().trimmed();
        return v.isEmpty() ? QString(IGDB_CLIENT_SECRET_DEFAULT) : v;
    }
    static bool isConfigured() { return !clientId().isEmpty() && !clientSecret().isEmpty(); }
};
