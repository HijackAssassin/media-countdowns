#pragma once
#include <QString>

// =============================================================================
//  mediasource.h — V5.
//
//  SearchResult used to live in tmdbscraper.h, which made it awkward for a
//  second data source to produce one without dragging the whole TMDB header
//  (and its relay/credential includes) along. It lives here now so every
//  source — TMDB, IGDB, TVmaze, Wikipedia — speaks the same result type and
//  MainWindow's search/pick flow doesn't care which one answered.
//
//  Deliberately NOT an abstract base class with virtuals: the existing
//  scrapers are QObjects that communicate purely through signals with
//  identical shapes (searchResultsReady / dataReady / tileRefreshed /
//  posterReady / scraperError). Matching those signatures is the whole
//  contract, and inheritance would buy nothing while forcing a Qt
//  moc/multiple-inheritance tangle.
// =============================================================================

struct SearchResult {
    int     id         = 0;      // source-specific id (TMDB id, TVmaze id, IGDB id…)
    QString mediaType;           // "tv" | "movie" | "game"
    QString title;
    int     year       = 0;
    QString posterPath;
    QString director;
    QString castLine;
    bool    isUS       = false;
    double  popularity = 0.0;    // higher = ranked sooner in the picker

    // V5 — which backend produced this result, so MainWindow routes the
    // follow-up detail fetch to the same source that answered the search
    // instead of assuming TMDB.
    QString source;              // "tmdb" | "tvmaze" | "igdb"
};
