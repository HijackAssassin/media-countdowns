#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QList>
#include <QQueue>
#include <QTimer>
#include <functional>
#include "tiledata.h"
#include "mediasource.h"

// =============================================================================
//  TvmazeScraper — V5. TV shows, with no API key of any kind.
//
//  Why this exists, given TmdbScraper already works:
//
//   1. TMDB leaves air_date NULL for episodes its moderators haven't
//      confirmed yet, which is exactly the data the app needs most. That's
//      the root of the "sitting in Released with a stale date" behaviour —
//      no amount of local guessing fixes a source that doesn't have the
//      dates. TVmaze publishes a season's full schedule up front.
//   2. No key means no credential to rate-limit, revoke, or route through a
//      relay server. TVmaze's limit (~20 requests / 10s) applies per END
//      USER's IP, so it scales with the userbase instead of concentrating
//      on one shared token.
//   3. Licensed CC BY-SA — the same license as Wikipedia — so it can be
//      used in a paid app as long as TVmaze is credited. Attribution lives
//      in the About dialog.
//
//  Signal shapes deliberately mirror TmdbScraper exactly so MainWindow can
//  route to either without special-casing. One request does everything:
//  /shows/{id}?embed=episodes returns the show plus every episode.
//
//  Backdrops: TVmaze show images are portrait posters, but per-EPISODE
//  images are 16:9 (measured 3840x2160). The tile backdrop is therefore the
//  NEXT episode's still, downscaled to 720p on save.
// =============================================================================
class TvmazeScraper : public QObject
{
    Q_OBJECT
public:
    explicit TvmazeScraper(QObject* parent = nullptr);

    void searchMedia(const QString& query);
    void fetchDetails(int tvmazeId, const QString& mediaType = "tv",
                      const QString& posterPath = {});
    void refreshTile(const TileData& existing, bool forceBackdropRefetch = false);

    // V5 — existing tiles carry a tmdbId but no tvmazeId, and TVmaze's
    // /lookup endpoint does not accept TMDB ids, so there is no id bridge.
    // Resolves by title, confirming with the premiere year so a common
    // title can't silently bind a tile to the wrong show. Emits
    // tileNeedsConfirmation when it isn't confident enough to decide alone.
    void resolveTvmazeId(const TileData& existing);

    static constexpr const char* BASE = "https://api.tvmaze.com";

signals:
    void searchResultsReady(const QList<SearchResult>& results);
    void dataReady(const TileData& data);
    void tileRefreshed(const TileData& updated);
    void posterReady(const QString& tileId, const QString& localPath, bool makeActive);
    void scraperError(const QString& message);

    // V5 — ambiguous title match during migration; MainWindow asks the user
    // rather than guessing. Never fired for an unambiguous match.
    void tileNeedsConfirmation(const QString& tileId, const QList<SearchResult>& candidates);

private:
    // V5 — TVmaze allows ~20 requests per 10s per IP and simply drops the
    // overflow (measured: 6 of 26 parallel requests failed outright). A
    // startup refresh of 11 show tiles fires 22 requests at once — search
    // plus fetch for each — so without pacing, several tiles silently never
    // resolve. Every API call goes through this queue, which dispatches one
    // request at a time on a timer and retries a dropped one once.
    struct PendingRequest {
        QString url;
        std::function<void(const QByteArray&, bool ok)> cb;
        int attempts = 0;
    };
    QQueue<PendingRequest> m_queue;
    QTimer*                m_pump      = nullptr;
    bool                   m_inFlight  = false;

    void enqueue(const QString& url, std::function<void(const QByteArray&, bool)> cb);
    void pumpQueue();
    // episodeImageOut receives the chosen episode's 16:9 still URL, if any.
    // hasUpcomingOut: false when every listed episode has already aired,
    // which is what marks a show as being between seasons.
    void applyEpisodes(TileData& td, const QJsonArray& episodes,
                       QString* episodeImageOut, bool* hasUpcomingOut = nullptr);
    void downloadEpisodeImage(const QString& tileId, const QString& imageUrl, bool makeActive);

    // V5 — a show between seasons often has an announced MONTH but no date.
    // TVmaze shows it ("Status: Running; returning March 2027") but their
    // API does not expose it: /seasons/{id} reports premiereDate null even
    // when the site displays the month. So this reads the show's public
    // page, which is the only place the information exists.
    //
    // V5.4 — the page just as often gives only a YEAR ("returning 2026",
    // which is what Dexter: Resurrection currently shows). That used to fail
    // the parse outright and leave the tile on "No Release Date Yet", losing
    // real information the source had published. Both widths are handled
    // here and set the matching flag on TileData; the only difference is how
    // wide the window is and how it's labelled.
    //
    // Only called when the API reports nothing upcoming — that's the sole
    // case where it can help, and it keeps the extra request off every
    // other refresh. A parse failure is treated as "no date known", so if
    // TVmaze ever restyles the page, tiles simply behave as they did
    // before rather than showing something invented.
    void fetchReturningWindow(TileData td, bool isRefresh);
    void handleShowPayload(const QJsonObject& show, TileData td, bool isRefresh,
                           bool forceBackdropRefetch);

    QNetworkAccessManager* m_nam;
};
