#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QList>
#include <QSettings>
#include <functional>
#include "tiledata.h"
#include "mediasource.h"   // V5 — SearchResult moved here, shared by every source

class TmdbScraper : public QObject
{
    Q_OBJECT
public:
    explicit TmdbScraper(QObject* parent = nullptr);

    void searchMedia(const QString& query);
    void fetchDetails(int tmdbId, const QString& mediaType, const QString& posterPath = {});
    void fetchCreditsForResults(const QList<SearchResult>& results);

    // Called on startup to refresh each saved tile's date/backdrop.
    // forceBackdropRefetch bypasses the "only if missing" check — used by
    // the Edit dialog's "Refetch Image" button to force a fresh download
    // even when a backdrop is already present.
    void refreshTile(const TileData& existing, bool forceBackdropRefetch = false);

    // V5 — fetch ONLY the backdrop, emitting posterReady and nothing else.
    //
    // TV tiles get their dates from TVmaze (which publishes a season's full
    // schedule, where TMDB leaves unaired episodes blank) but their artwork
    // from TMDB. Calling the normal refreshTile() for that would emit
    // tileRefreshed carrying TMDB's own — worse — date data, which would
    // then overwrite what TVmaze just supplied. This path touches images
    // only, so the two sources can't fight over the same fields.
    void fetchBackdropOnly(const QString& tileId, int tmdbId,
                           const QString& mediaType, bool makeActive);

signals:
    void searchResultsReady(const QList<SearchResult>& results);
    void creditsReady(int tmdbId, const QString& director, const QString& castLine);
    void dataReady(const TileData& data);
    void tileRefreshed(const TileData& updated);   // startup refresh result
    // v3.1.0 — makeActive: whether this newly-fetched backdrop should become
    // the tile's currently-displayed image (vs just updating the "fetched"
    // slot silently while a custom image stays active).
    void posterReady(const QString& tileId, const QString& localPath, bool makeActive);
    void scraperError(const QString& message);

private:
    QNetworkReply* getJson(const QString& url);
    void downloadBackdrop(const QString& tileId, const QString& backdropPath, bool makeActive);
    void fetchSeasonForMultiEp(int showId, int season, const QString& airDate,
                                const TileData& td, bool isRefresh);
    void fetchSeasonForFutureEp(int showId, int season, int afterEpisodeNum,
                                TileData td, bool isRefresh);
    TileData parseDetailsJson(const QJsonObject& obj, const QString& mediaType,
                              const QString& existingId = {}, const QString& existingImage = {});
    // V4.1.1 — curated collection precision (e.g. matching "avengers"
    // against a specific, hand-picked TMDB collection ID, not a guess).
    // Called directly from searchMedia() when the query matches one of
    // the curated triggers there; owns emitting the rest of that search
    // (searchResultsReady/scraperError) once done.
    void fetchCuratedCollections(const QList<int>& collectionIds);

    QNetworkAccessManager* m_nam;

public:
    // V4 — cleared. With the relay routing TMDB calls by default, no
    // baked-in key is needed at all; this only matters if relay mode is
    // off in Settings and no personal key has been entered there either.
    static constexpr const char* DEFAULT_API_KEY = "";
    static constexpr const char* BASE            = "https://api.themoviedb.org/3";
    static QString apiKey() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        QString k = s.value("tmdbApiKey").toString();
        return k.isEmpty() ? QString(DEFAULT_API_KEY) : k;
    }
};
