#include "imagequeue.h"
#include "tmdbscraper.h"
#include "languageutil.h"
#include "relayconfig.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QRegularExpression>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QTimeZone>
#include <QDebug>
#include <QSet>
#include <algorithm>

// Lowercase, alphanumeric-only compacting used only for matching a query
// against the curated collection trigger list in searchMedia() — strips
// spaces and hyphens too, so "spider-man", "Spider-Man", and "spiderman"
// all compact to the same "spiderman" and compare equal.
static QString simpleCompact(const QString& s)
{
    QString normalized = s.normalized(QString::NormalizationForm_D);
    QString stripped;
    for (const QChar& c : normalized)
        if (c.category() != QChar::Mark_NonSpacing) stripped += c;
    QString out;
    for (const QChar& c : stripped.toLower())
        if (c.isLetterOrNumber()) out += c;
    return out;
}

TmdbScraper::TmdbScraper(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{}

// =============================================================================
//  pickBackdropPath — v3.1.2. Picks the best backdrop out of TMDB's list for
//  the user's Language setting: prefers a textless one (iso_639_1 null/empty)
//  or one matching the selected language over whatever happens to be first
//  in the list, which is how a backdrop in an unexpected language (e.g.
//  Hindi) could end up chosen. Falls back to the first entry if nothing
//  textless or matching turns up.
// =============================================================================
static QString pickBackdropPath(const QJsonArray& backdrops)
{
    QString langCode = LanguageUtil::currentCode();
    QString fallback;
    for (const QJsonValue& v : backdrops) {
        QJsonObject b = v.toObject();
        QString path = b["file_path"].toString();
        if (path.isEmpty()) continue;
        if (fallback.isEmpty()) fallback = path;
        QString lang = b["iso_639_1"].toString();
        if (lang.isEmpty() || lang == langCode) return path;
    }
    return fallback;
}

// =============================================================================
//  Phase 1 — search with improved filtering
// =============================================================================
void TmdbScraper::searchMedia(const QString& query)
{
    QString q = query.trimmed();
    // Normalize common hyphenated titles
    q.replace(QRegularExpression("\\bspiderman\\b", QRegularExpression::CaseInsensitiveOption), "spider-man");
    q.replace(QRegularExpression("\\bxmen\\b",      QRegularExpression::CaseInsensitiveOption), "x-men");

    // Marvel alias map — used in filter below, NOT to rewrite the API query
    // so that searching "punisher" still returns The Punisher (1989) etc.
    static const QVector<QPair<QString,QString>> kMarvelAliases = {
        {"daredevil",               "Marvel's Daredevil"},
        {"the punisher",            "Marvel's The Punisher"},
        {"punisher",                "Marvel's The Punisher"},
        {"jessica jones",           "Marvel's Jessica Jones"},
        {"luke cage",               "Marvel's Luke Cage"},
        {"iron fist",               "Marvel's Iron Fist"},
        {"the defenders",           "Marvel's The Defenders"},
        {"runaways",                "Marvel's Runaways"},
        {"cloak and dagger",        "Marvel's Cloak & Dagger"},
        {"cloak & dagger",          "Marvel's Cloak & Dagger"},
        {"inhumans",                "Marvel's Inhumans"},
        {"agent carter",            "Marvel's Agent Carter"},
        {"agents of shield",        "Marvel's Agents of S.H.I.E.L.D."},
        {"agents of s.h.i.e.l.d.", "Marvel's Agents of S.H.I.E.L.D."},
        {"ultimate spider-man",     "Marvel's Ultimate Spider-Man"},
        {"ultimate spiderman",      "Marvel's Ultimate Spider-Man"},
        {"marvels avengers",        "Marvel's Avengers"},
    };
    // Find the Marvel full title that matches this query (if any)
    QString marvelFullTitle;
    for (const auto& alias : kMarvelAliases) {
        if (q.compare(alias.first, Qt::CaseInsensitive) == 0) {
            marvelFullTitle = alias.second;
            break;
        }
    }

    // Detect optional trailing year e.g. "Supergirl 2026"
    int yearFilter = 0;
    QRegularExpression yearRe(R"(\b(19\d{2}|20\d{2})$)");
    auto ym = yearRe.match(q);
    if (ym.hasMatch()) {
        yearFilter = ym.captured(1).toInt();
        q = q.left(ym.capturedStart()).trimmed();
    }

    // ── V4.1.2 — curated collection precision ───────────────────────────────
    // Replaces the earlier fuzzy auto-detection entirely (it once matched
    // "avengers" against the wrong, unrelated "Avengers Grimm Collection").
    // Only these exact, hand-picked queries are recognized — nothing is
    // guessed. A match takes over completely: results come ONLY from the
    // mapped collection(s), not blended with the regular search at all.
    //
    // A year does NOT apply to a collection (a franchise isn't a single
    // year) — if one was specified (e.g. "spiderman 2002"), skip the
    // collection path entirely and fall through to the regular search
    // below, which already filters by year correctly on its own and
    // returns just the one matching title (e.g. "Spider-Man (2002)").
    QString compact = simpleCompact(q);
    static const QMap<QString, QList<int>> kCuratedCollections = {
        {"avengers",             {86311}},                          // The Avengers Collection
        {"ironman",              {131292}},                         // Iron Man Collection
        {"spiderverse",          {573436}},                         // Spider-Man: Spider-Verse Collection
        {"theamazingspiderman",  {125574}},                         // The Amazing Spider-Man Collection
        {"spiderman",            {556, 125574, 573436, 531241}},    // Spider-Man + Amazing + Spider-Verse + MCU
        {"captainamerica",       {131295}},                         // Captain America Collection
        {"thor",                 {131296}},                         // Thor Collection
    };
    if (yearFilter == 0 && kCuratedCollections.contains(compact)) {
        fetchCuratedCollections(kCuratedCollections.value(compact));
        return;
    }

    // Fetch 2 pages to get more candidates before filtering (up to ~40 results)
    QString url1 = QString("%1/search/multi?api_key=%2&query=%3&include_adult=false&page=1")
        .arg(BASE, apiKey(), QString::fromUtf8(QUrl::toPercentEncoding(q)));
    QString url2 = QString("%1/search/multi?api_key=%2&query=%3&include_adult=false&page=2")
        .arg(BASE, apiKey(), QString::fromUtf8(QUrl::toPercentEncoding(q)));

    // Use a shared state for both pages
    struct PageState {
        QJsonArray combined;
        int        pagesReceived = 0;
    };
    auto state = QSharedPointer<PageState>::create();


    auto processPage = [this, state, q, yearFilter, marvelFullTitle](const QJsonArray& arr) {
        for (const QJsonValue& v : arr)
            state->combined.append(v);
        state->pagesReceived++;
        if (state->pagesReceived < 2) return;

        // ── Normalisation helpers ─────────────────────────────────────────────
        auto strip = [](const QString& s) -> QString {
            QString d = s.normalized(QString::NormalizationForm_D);
            QString r;
            for (const QChar& c : d)
                if (c.category() != QChar::Mark_NonSpacing) r += c;
            return r;
        };

        auto compact = [&strip](const QString& s) -> QString {
            QString r;
            for (const QChar& c : strip(s.toLower()))
                if (c.isLetterOrNumber()) r += c;
            return r;
        };

        auto words = [&strip](const QString& s) -> QStringList {
            QStringList out;
            QString replaced = s;
            replaced.replace('-', ' ').replace('_', ' ');
            for (const QString& w : strip(replaced.toLower()).split(' ', Qt::SkipEmptyParts)) {
                QString clean;
                for (const QChar& c : w)
                    if (c.isLetterOrNumber()) clean += c;
                if (!clean.isEmpty()) out << clean;
            }
            return out;
        };

        QStringList queryWords   = words(q);
        QString     queryCompact = compact(q);

        QSettings prefs("HijackAssassin", "MediaCountdowns");
        bool showMovies   = prefs.value("pref_movies",  true).toBool();
        bool showShows    = prefs.value("pref_shows",   true).toBool();
        bool showReality  = prefs.value("pref_reality", false).toBool();
        bool showDocs     = prefs.value("pref_docs",    false).toBool();
        bool showTalk     = prefs.value("pref_talk",    false).toBool();
        // V4.1.1 — default flipped to false: foreign-language results
        // (e.g. an unrelated Indian film matching "daredevil" by title
        // alone) no longer show unless explicitly opted into in Settings.
        bool showForeign  = prefs.value("pref_foreign", false).toBool();

        QSet<int> blockedGenres;
        if (!showReality) blockedGenres << 10764;
        if (!showDocs)    blockedGenres << 99;
        if (!showTalk)    blockedGenres << 10767;

        QList<SearchResult> results;

        for (const QJsonValue& v : state->combined) {
            QJsonObject o = v.toObject();
            QString type = o["media_type"].toString();
            if (type != "movie" && type != "tv") continue;
            if (type == "movie" && !showMovies) continue;
            if (type == "tv"    && !showShows)  continue;

            QString rawTitle = (type == "movie") ? o["title"].toString()
                                                 : o["name"].toString();
            if (rawTitle.isEmpty()) continue;

            // ── Block podcasts and low-quality content ────────────────────────
            if (rawTitle.contains("podcast",     Qt::CaseInsensitive)) continue;
            if (rawTitle.contains("fan film",    Qt::CaseInsensitive)) continue;
            if (rawTitle.contains("fan-made",    Qt::CaseInsensitive)) continue;

            // ── Genre filter — block documentaries, talk shows, reality ───────
            bool hasBlockedGenre = false;
            for (const QJsonValue& g : o["genre_ids"].toArray()) {
                if (blockedGenres.contains(g.toInt())) { hasBlockedGenre = true; break; }
            }
            if (hasBlockedGenre) continue;

            // ── Original language must be English (unless foreign allowed) ─────
            if (!showForeign && o["original_language"].toString() != "en") continue;

            // ── Title matching — query words must all appear in title ──────────
            QStringList titleWords = words(rawTitle);
            bool wordMatch = true;
            for (const QString& qw : queryWords)
                if (!titleWords.contains(qw)) { wordMatch = false; break; }

            bool compactMatch = compact(rawTitle).contains(queryCompact);
            if (!wordMatch && !compactMatch) continue;

            // ── First-word anchor + prefix-sequence check ─────────────────────
            // Rules:
            // 1. The first meaningful word of the title must match the first word
            //    of the query (skipping leading articles in the title only when
            //    the query itself doesn't start with an article).
            // 2. All query words must appear as a contiguous prefix of the title
            //    word list (so "the boys" cannot match "The Napa Boys").
            //    Exception: when only compactMatch fired (e.g. "spiderman" →
            //    "Spider-Man") we skip the sequence check because the hyphen
            //    split makes the words unequal.
            if (!queryWords.isEmpty()) {
                static const QSet<QString> articles = {"the","a","an"};
                bool queryStartsWithArticle = articles.contains(queryWords[0]);

                // Build title word list, optionally skipping a leading article
                QStringList anchoredTitle = titleWords;
                if (!queryStartsWithArticle && !anchoredTitle.isEmpty()
                    && articles.contains(anchoredTitle[0]))
                    anchoredTitle.removeFirst();

                // First-word anchor — also check subtitle after " - ", " – ", or ": "
                // Also allow pass-through if this title matches a Marvel alias
                bool marvelMatch = !marvelFullTitle.isEmpty() &&
                                   rawTitle.compare(marvelFullTitle, Qt::CaseInsensitive) == 0;
                bool firstWordOk = marvelMatch ||
                                   (!anchoredTitle.isEmpty() && anchoredTitle[0] == queryWords[0]);
                // V5 \u2014 the subtitle fallback is deliberately gone.
                //
                // It used to let the part after a ":" or " - " satisfy the
                // anchor, so "born again" could match "Daredevil: Born Again".
                // In practice it was dead code: the loop discarded its working
                // set on the first separator that didn't appear, and since
                // " - " was checked first it almost never got past step one.
                //
                // Repairing it made matching noticeably looser \u2014 "daredevil"
                // started returning "Super Dave: Daredevil for Hire", because
                // any title with the word after a colon now qualified. Removed
                // entirely instead, which is both stricter and honest about
                // what the code does: a title must begin with what you typed.
                //
                // Searching a subtitle still works when the franchise name
                // leads \u2014 "spiderman brand new day" matches "Spider-Man: Brand
                // New Day" through the ordinary prefix rule below.
                if (!firstWordOk) continue;

                // Prefix-sequence check (only when wordMatch — not compactMatch-only, not Marvel alias)
                // Ensures "the boys" doesn't match "The Napa Boys"
                // (subtitleMatch is gone along with the fallback above — with
                // the anchor now always satisfied by the title's own first
                // word, there is no looser case left to exempt.)
                if (wordMatch && !marvelMatch && queryWords.size() > 1) {
                    bool seqOk = true;
                    for (int qi = 0; qi < queryWords.size(); ++qi) {
                        if (qi >= anchoredTitle.size() || anchoredTitle[qi] != queryWords[qi]) {
                            seqOk = false; break;
                        }
                    }
                    if (!seqOk) continue;
                }
            }

            // ── Year filter ───────────────────────────────────────────────────
            QString dateStr = (type == "movie") ? o["release_date"].toString()
                                                : o["first_air_date"].toString();
            int year = dateStr.left(4).toInt();
            if (yearFilter > 0 && year != yearFilter) continue;

            // ── Popularity floor — block very obscure results (fan films etc.) ─
            double popularity = o["popularity"].toDouble();
            if (popularity < 2.0) continue;

            SearchResult sr;
            sr.id         = o["id"].toInt();
            sr.mediaType  = type;
            sr.year       = year;
            sr.title      = (year > 0)
                ? QString("%1 (%2)").arg(rawTitle).arg(year)
                : rawTitle;
            sr.posterPath = o["poster_path"].toString();
            sr.isUS       = true;   // all results are US at this point
            sr.popularity = popularity;

            results.append(sr);
        }

        // ── Deduplicate by TMDB id ────────────────────────────────────────────
        QList<int> seen;
        results.erase(std::remove_if(results.begin(), results.end(),
            [&seen](const SearchResult& sr) {
                if (seen.contains(sr.id)) return true;
                seen.append(sr.id); return false;
            }), results.end());

        // ── Sort: newest year first, then by popularity within same year ──────
        std::sort(results.begin(), results.end(),
            [](const SearchResult& a, const SearchResult& b) {
                if (a.year != b.year) {
                    // No-year to end
                    if (a.year == 0) return false;
                    if (b.year == 0) return true;
                    return a.year > b.year;   // newest first
                }
                return a.popularity > b.popularity;
            });

        if (results.size() > 15) results = results.mid(0, 15);

        if (results.isEmpty()) {
            emit scraperError(
                yearFilter > 0
                ? QString("No results for that title in %1. Try without the year.").arg(yearFilter)
                : "No results found. Try a different spelling.");
            return;
        }
        emit searchResultsReady(results);
    };

    // Fire both page requests in parallel
    for (const QString& url : {url1, url2}) {
        QNetworkReply* r = getJson(url);
        connect(r, &QNetworkReply::finished, this, [r, processPage, this]() {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) {
                emit scraperError("Search failed: " + r->errorString());
                return;
            }
            QJsonArray arr = QJsonDocument::fromJson(r->readAll())
                                 .object()["results"].toArray();
            processPage(arr);
        });
    }
}

// =============================================================================
//  fetchCuratedCollections  —  V4.1.3. Fetches an exact, known set of TMDB
//  collection IDs (see the kCuratedCollections map in searchMedia()) and
//  emits their combined, deduplicated movie list as the search results —
//  no blending with a regular search, no guessing which collection the
//  query "probably" meant. searchMedia() calls this directly and returns
//  immediately; this owns the rest of that search from here.
//
//  NOTE: a collection's own "parts" (its movie list) uses "title"/
//  "original_title" — the SAME field names as every other TMDB movie
//  endpoint, and has no "media_type" field at all (a collection is
//  implicitly movies-only). An earlier version of this comment claimed
//  the opposite ("name"/"original_name", with a media_type filter) —
//  that was wrong, based on a misreading of documentation rather than a
//  real response, and it silently discarded every single movie as a
//  result. Corrected after checking an actual, live TMDB API response.
// =============================================================================
void TmdbScraper::fetchCuratedCollections(const QList<int>& collectionIds)
{
    struct CollectionState {
        QList<SearchResult> results;
        int                 remaining;
    };
    auto state = QSharedPointer<CollectionState>::create();
    state->remaining = collectionIds.size();

    auto finish = [this, state]() {
        if (state->remaining > 0) return;

        // Dedupe by TMDB id — the curated lists can genuinely overlap
        // (e.g. "spiderman" intentionally maps to four collections that
        // share some of the same films).
        QList<int> seen;
        QList<SearchResult> results;
        for (const SearchResult& sr : state->results) {
            if (seen.contains(sr.id)) continue;
            seen << sr.id;
            results << sr;
        }

        std::sort(results.begin(), results.end(),
            [](const SearchResult& a, const SearchResult& b) {
                if (a.year != b.year) {
                    if (a.year == 0) return false;
                    if (b.year == 0) return true;
                    return a.year > b.year;
                }
                return a.popularity > b.popularity;
            });

        if (results.isEmpty()) {
            emit scraperError("No results found.");
            return;
        }
        emit searchResultsReady(results);
    };

    for (int collectionId : collectionIds) {
        QString url = QString("%1/collection/%2?api_key=%3")
            .arg(BASE).arg(collectionId).arg(apiKey());
        QNetworkReply* r = getJson(url);
        connect(r, &QNetworkReply::finished, this, [r, this, state, finish]() {
            r->deleteLater();
            state->remaining--;
            if (r->error() != QNetworkReply::NoError) { finish(); return; }

            QJsonArray parts = QJsonDocument::fromJson(r->readAll())
                                    .object()["parts"].toArray();
            for (const QJsonValue& v : parts) {
                QJsonObject o = v.toObject();
                // A collection's "parts" has no "media_type" field at all —
                // a collection is implicitly movies-only, so there's
                // nothing to filter on here. Confirmed against a real,
                // live TMDB response (not just documentation) after the
                // earlier assumption here turned out to be wrong.

                // Collection parts use "title"/"original_title" — same as
                // every normal movie endpoint. The earlier "name" here was
                // an incorrect assumption; verified against a real TMDB
                // response this time, not just documentation.
                QString rawTitle = o["title"].toString();
                if (rawTitle.isEmpty()) continue;

                QString dateStr = o["release_date"].toString();
                int year = dateStr.left(4).toInt();

                SearchResult sr;
                sr.id         = o["id"].toInt();
                sr.mediaType  = "movie";
                sr.year       = year;
                sr.title      = (year > 0) ? QString("%1 (%2)").arg(rawTitle).arg(year) : rawTitle;
                sr.posterPath = o["poster_path"].toString();
                sr.isUS       = true;
                sr.popularity = o["popularity"].toDouble();
                state->results.append(sr);
            }
            finish();
        });
    }
}

// =============================================================================
//  parseDetailsJson  —  shared logic between fetchDetails and refreshTile
// =============================================================================
TileData TmdbScraper::parseDetailsJson(const QJsonObject& obj,
                                       const QString& mediaType,
                                       const QString& existingId,
                                       const QString& existingImage)
{
    TileData td;
    td.id        = existingId.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces) : existingId;
    td.tmdbId    = obj["id"].toInt();
    td.mediaType = mediaType;
    td.tmdbUrl   = QString("https://www.themoviedb.org/%1/%2")
                       .arg(mediaType).arg(td.tmdbId);
    td.imagePath = existingImage;  // keep existing; backdrop download updates it

    // Title + year + season count
    QString rawTitle    = (mediaType == "tv") ? obj["name"].toString()
                                              : obj["title"].toString();
    QString dateForYear = (mediaType == "tv") ? obj["first_air_date"].toString()
                                              : obj["release_date"].toString();
    int year = dateForYear.left(4).toInt();

    // ── All-caps title fix (e.g. INVINCIBLE → Invincible) ────────────────────
    {
        bool allCaps = !rawTitle.isEmpty()
                    && rawTitle == rawTitle.toUpper()
                    && rawTitle.contains(QRegularExpression("[A-Z]{2}"));
        if (allCaps) {
            static const QStringList minor = {
                "a","an","the","and","but","or","nor","for","yet","so",
                "at","by","in","of","on","to","up","as","is"
            };
            QStringList ws = rawTitle.toLower().split(' ', Qt::SkipEmptyParts);
            for (int i = 0; i < ws.size(); ++i) {
                bool cap = (i == 0) || !minor.contains(ws[i]);
                if (cap && !ws[i].isEmpty()) ws[i][0] = ws[i][0].toUpper();
            }
            rawTitle = ws.join(' ');
        }
    }

    if (mediaType == "tv") {
        // v3.1.7 fix #3 — season count used to be appended here ("Show  •  N
        // Seasons") but it never actually showed up on the tile (both
        // extractShowName() and fireDirectNotification() already stripped
        // it back out before display/notifications), so it was dead weight.
        td.title = rawTitle;
    } else {
        // Movie — just the name, no year
        td.title = rawTitle;
    }
    td.releaseYear = year;   // v3.0.2 — used by the "Year" tile display option

    // ── Movie ─────────────────────────────────────────────────────────────────
    if (mediaType == "movie") {
        QString releaseDate;

        QJsonArray releaseResults = obj["release_dates"].toObject()["results"].toArray();
        bool isTheatrical = false;
        for (const QJsonValue& country : releaseResults) {
            if (country.toObject()["iso_3166_1"].toString() != "US") continue;
            QJsonArray dates = country.toObject()["release_dates"].toArray();

            // Priority: type 3 (Theatrical) > type 2 (Limited) > type 4 (Digital) > any
            int foundType = 0;
            for (int wantType : {3, 2, 4, 1, 5, 6}) {
                for (const QJsonValue& d : dates) {
                    if (d.toObject()["type"].toInt() == wantType) {
                        releaseDate = d.toObject()["release_date"].toString().left(10);
                        foundType   = wantType;
                        break;
                    }
                }
                if (!releaseDate.isEmpty()) break;
            }
            isTheatrical = (foundType == 3 || foundType == 2);
            break;
        }
        td.isTheatrical = isTheatrical;

        // Only fall back to global release_date if we truly found nothing for US
        if (releaseDate.isEmpty())
            releaseDate = obj["release_date"].toString();

        if (releaseDate.isEmpty()) {
            td.statusLabel = "No Release Date Yet";
            td.dateDisplay = "No Release Date Yet";
            return td;
        }

        td.targetDate = QDate::fromString(releaseDate, Qt::ISODate);

        // ── US early-screening adjustment ─────────────────────────────────────
        // US theaters typically run Thursday night previews the day before
        // official Friday release. Detect US timezone and subtract 1 day.
        QString tzId = QTimeZone::systemTimeZone().id();
        bool isUS = tzId.startsWith("America/") || tzId.startsWith("US/")
                 || tzId == "EST5EDT" || tzId == "CST6CDT"
                 || tzId == "MST7MDT" || tzId == "PST8PDT";
        if (isUS && isTheatrical && td.targetDate.isValid())
            td.targetDate = td.targetDate.addDays(-1);

        td.dateDisplay = td.targetDate.toString("MMMM d, yyyy");
        td.statusLabel = (td.targetDate > QDate::currentDate()) ? "Releases" : "Released";
    }

    // ── TV show ───────────────────────────────────────────────────────────────
    else {
        // TMDB show-level status: "Ended"/"Canceled" means truly done; anything
        // else (Returning Series, In Production, Planned, Pilot) means more
        // episodes are still expected even if the exact next date isn't
        // confirmed by TMDB yet.
        QString showStatus = obj["status"].toString();
        static const QSet<QString> kFinishedStatuses = { "Ended", "Canceled" };
        bool showFinished = kFinishedStatuses.contains(showStatus);

        QJsonObject nextEp = obj["next_episode_to_air"].toObject();
        QDate nextDate = nextEp.isEmpty() ? QDate()
                       : QDate::fromString(nextEp["air_date"].toString(), Qt::ISODate);

        // v3.0.2 — total episode count for a given season, used by the
        // "Total Episodes" tile display option (e.g. S01E01/E08).
        auto episodeCountForSeason = [&](int seasonNum) -> int {
            for (const QJsonValue& sv : obj["seasons"].toArray()) {
                QJsonObject s = sv.toObject();
                if (s["season_number"].toInt() == seasonNum)
                    return s["episode_count"].toInt();
            }
            return 0;
        };

        // v3.0.2 fix — TMDB sometimes returns a next_episode_to_air entry
        // with the season/episode number known but no confirmed air_date yet
        // (episode-level details often aren't filled in until a few days out).
        // Treat that the same as "no next episode" and fall through to the
        // season-scan fallback below, instead of silently producing a blank
        // target date.
        // v3.3.1 fix #3 — also treat a next_episode_to_air date that's
        // already in the past the same way. TMDB's "next episode" pointer
        // can lag right after an episode airs — for a little while it may
        // still point at the episode that JUST released (with a now-past
        // date) rather than the actual upcoming one. Without this check,
        // that stale past date was being shown as-is, and the estimate
        // logic below (which is exactly designed for this situation) never
        // got a chance to run.
        if (!nextEp.isEmpty() && nextDate.isValid() && nextDate >= QDate::currentDate()) {
            QString airDate = nextEp["air_date"].toString();
            int season  = nextEp["season_number"].toInt();
            int episode = nextEp["episode_number"].toInt();
            td.targetDate  = nextDate;
            td.dateDisplay = td.targetDate.toString("MMMM d, yyyy");

            // Multi-episode detection: look in the season's episode list for
            // other episodes sharing the same air_date (premiere dumps / double bills)
            QString sLabel = QString("S%1E%2")
                .arg(season,  2, 10, QChar('0'))
                .arg(episode, 2, 10, QChar('0'));

            // Check if the full season data was included (it isn't by default,
            // but we scan next_episode_to_air's siblings if present).
            // We do a quick scan of any "seasons" array entries with the same date.
            // Real multi-ep detection happens via fetchSeasonForMultiEp called below.
            td.statusLabel = sLabel;
            // Store season/episode for post-process lookup
            td.rawDateText = QString("%1|%2|%3").arg(airDate).arg(season).arg(episode);
            td.seasonEpisodeCount = episodeCountForSeason(season);

            // A verified date arrived — clear any earlier estimate/break state.
            td.isEstimatedDate  = false;
            td.inMidSeasonBreak = false;

            // air_time from TMDB is unreliable (often wrong timezone/value)
            // Always leave td.airTime invalid — countdown targets the Time
            // Zone-aware default by default. Users can set a custom time via
            // the edit dialog.
        } else {
            QJsonObject lastEp = obj["last_episode_to_air"].toObject();
            td.statusLabel = "Last Episode";
            if (!lastEp.isEmpty()) {
                QDate lastDate = QDate::fromString(lastEp["air_date"].toString(), Qt::ISODate);
                int lastSeason = lastEp["season_number"].toInt();
                int lastEpNum  = lastEp["episode_number"].toInt();
                QString lastLabel = "Last Episode";
                if (lastSeason > 0 && lastEpNum > 0)
                    lastLabel = QString("S%1E%2")
                        .arg(lastSeason, 2, 10, QChar('0'))
                        .arg(lastEpNum,  2, 10, QChar('0'));

                td.lastVerifiedDate        = lastDate;
                td.lastVerifiedStatusLabel = lastLabel;

                int seasonTotal = (lastSeason > 0) ? episodeCountForSeason(lastSeason) : 0;
                if (lastSeason > 0) td.seasonEpisodeCount = seasonTotal;

                // v3.1.0 Feature 3 — TMDB hasn't confirmed a next-episode date
                // yet (this is common: episode-level details, including the
                // date, often aren't filled in by moderators until a few days
                // out) but the episode COUNT for a season is reliably accurate
                // ahead of time. If the season isn't finished, estimate the
                // next episode's date on a weekly cadence from the last
                // verified one, rather than showing the show as if it had
                // wrapped. Advances by full weeks until landing on a date
                // that's still in the future, so it keeps making sense across
                // any number of missed refreshes.
                bool moreEpisodesThisSeason =
                    (seasonTotal > 0 && lastEpNum > 0 && lastEpNum < seasonTotal);

                // v3.3.13 fix — TMDB's season episode_count can lag behind an
                // actively, weekly-releasing show: it may say the season
                // "looks" done (lastEpNum >= seasonTotal) even though new
                // episodes are clearly still coming out on a regular
                // cadence. If the last episode aired recently and the show
                // overall isn't confirmed Ended/Canceled, estimate the same
                // way rather than trusting a metadata field that may simply
                // not have caught up yet. Time-bounded to ~3 weeks so a show
                // genuinely between seasons (a months-long gap) doesn't get
                // a misleading "1 week away" guess.
                bool recentlyActive = lastDate.isValid()
                    && lastDate.daysTo(QDate::currentDate()) <= 21;
                bool possiblyStillOngoing = !showFinished && recentlyActive
                    && !moreEpisodesThisSeason;

                if ((moreEpisodesThisSeason || possiblyStillOngoing) && lastDate.isValid()) {
                    QDate estimate = lastDate.addDays(7);
                    QDate today = QDate::currentDate();
                    while (estimate < today) estimate = estimate.addDays(7);

                    td.targetDate  = estimate;
                    td.dateDisplay = estimate.toString("MMMM d, yyyy");
                    td.isEstimatedDate  = true;
                    td.inMidSeasonBreak = false;
                    td.statusLabel = (lastSeason > 0)
                        ? QString("S%1E%2")
                              .arg(lastSeason,     2, 10, QChar('0'))
                              .arg(lastEpNum + 1,  2, 10, QChar('0'))
                        : "Next Episode";
                } else {
                    // Season's fully aired (or we don't know the total) — show
                    // the last episode like before.
                    td.targetDate      = lastDate;
                    td.dateDisplay     = lastDate.isValid() ? lastDate.toString("MMMM d, yyyy") : "";
                    td.statusLabel     = lastLabel;
                    td.isEstimatedDate = false;
                }

                // Only worth scanning ahead if the show isn't confirmed done —
                // avoids wasted requests for genuinely-Ended/Canceled shows.
                // v3.3.15 — also encodes lastEpNum, so the season-scan below
                // can exclude the already-aired episode by number, not just
                // by date. Without this, if the last-aired episode's date
                // happened to BE today (not strictly in the past — which can
                // easily happen right on release day, and even for a day or
                // two after depending on when a refresh runs), the scan's
                // own "is this date still upcoming" check wouldn't skip it,
                // and it would keep re-selecting that same, already-aired
                // episode as if it were the next one — overwriting a
                // correct estimate with the very episode the tile was
                // already showing, which is exactly why a tile could get
                // stuck never advancing even across repeated refreshes.
                if (lastSeason > 0 && !showFinished)
                    td.rawDateText = QString("SCAN|%1|%2").arg(lastSeason).arg(lastEpNum);
            } else {
                td.dateDisplay = "No Release Date Yet";
            }
        }
    }

    return td;
}

// =============================================================================
//  fetchSeasonForMultiEp  —  given a show id, season, airDate and a prepared
//  TileData, checks for multiple episodes on the same date and updates
//  statusLabel to "S04E01+E02" if needed, then emits the correct signal.
// =============================================================================
void TmdbScraper::fetchSeasonForMultiEp(int showId, int season,
                                         const QString& airDate,
                                         const TileData& td,
                                         bool isRefresh)
{
    QString url = QString("%1/tv/%2/season/%3?api_key=%4")
        .arg(BASE).arg(showId).arg(season).arg(apiKey());

    QNetworkReply* r = getJson(url);
    connect(r, &QNetworkReply::finished, this, [r, airDate, td, isRefresh, this]() mutable {
        r->deleteLater();

        if (r->error() == QNetworkReply::NoError) {
            QJsonArray eps = QJsonDocument::fromJson(r->readAll())
                                 .object()["episodes"].toArray();

            QList<int> sameDay;
            for (const QJsonValue& v : eps) {
                QJsonObject ep = v.toObject();
                if (ep["air_date"].toString() == airDate)
                    sameDay.append(ep["episode_number"].toInt());
            }

            if (sameDay.size() > 1) {
                std::sort(sameDay.begin(), sameDay.end());
                // V5 — was mid(1,2), which silently assumed a 2-digit season
                // and broke on anything else (e.g. a 3-digit season number).
                int s = parseEpisodeLabel(td.statusLabel).season;
                QString label = QString("S%1E%2")
                    .arg(s, 2, 10, QChar('0'))
                    .arg(sameDay[0], 2, 10, QChar('0'));
                for (int i = 1; i < sameDay.size(); ++i)
                    label += QString("+E%1").arg(sameDay[i], 2, 10, QChar('0'));
                const_cast<TileData&>(td).statusLabel = label;
            }
        }

        if (isRefresh) emit tileRefreshed(td);
        else           emit dataReady(td);
    });
}

// =============================================================================
//  fetchSeasonForFutureEp — called when next_episode_to_air is empty.
//  Scans the season episode list for any episode with a future air_date.
//  If found, updates the tile's targetDate and statusLabel to that episode.
// =============================================================================
void TmdbScraper::fetchSeasonForFutureEp(int showId, int season, int afterEpisodeNum,
                                          TileData td, bool isRefresh)
{
    QString url = QString("%1/tv/%2/season/%3?api_key=%4")
        .arg(BASE).arg(showId).arg(season).arg(apiKey());

    QNetworkReply* r = getJson(url);
    connect(r, &QNetworkReply::finished, this, [r, showId, season, afterEpisodeNum, td, isRefresh, this]() mutable {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            if (isRefresh) emit tileRefreshed(td); else emit dataReady(td);
            return;
        }
        QJsonArray eps = QJsonDocument::fromJson(r->readAll())
                             .object()["episodes"].toArray();
        QDate today = QDate::currentDate();
        QDate bestDate;
        int bestEp = -1;

        for (const QJsonValue& v : eps) {
            QJsonObject ep = v.toObject();
            int epNum = ep["episode_number"].toInt();
            // v3.3.15 fix — must be a LATER episode than the one already
            // known to have aired, not just dated today-or-later. A same-
            // day date check alone let the already-aired episode get
            // re-selected as if it were the next one (see the SCAN marker
            // comment above for the full explanation).
            if (epNum <= afterEpisodeNum) continue;
            QDate d = QDate::fromString(ep["air_date"].toString(), Qt::ISODate);
            // v3.0.2 fix — an episode airing today still counts as upcoming;
            // it was being skipped here (d <= today), which could leave a
            // same-day episode undetected.
            if (!d.isValid() || d < today) continue;
            if (!bestDate.isValid() || d < bestDate) {
                bestDate = d;
                bestEp   = epNum;
            }
        }

        if (bestDate.isValid() && bestEp > 0) {
            td.targetDate  = bestDate;
            td.dateDisplay = bestDate.toString("MMMM d, yyyy");
            td.statusLabel = QString("S%1E%2")
                .arg(season, 2, 10, QChar('0'))
                .arg(bestEp, 2, 10, QChar('0'));
            td.rawDateText = QString("%1|%2|%3")
                .arg(bestDate.toString(Qt::ISODate)).arg(season).arg(bestEp);
            td.seasonEpisodeCount = eps.size();
            td.isEstimatedDate  = false;  // v3.1.0 — a real date beats an estimate
            td.inMidSeasonBreak = false;
            if (isRefresh) emit tileRefreshed(td); else emit dataReady(td);
            return;
        }

        // v3.0.2 fix — nothing dated in this season. TMDB's show-level
        // next_episode_to_air can lag behind the actual season/episode
        // endpoints, especially right after a season finale — the next
        // season's episodes (and dates) may already exist there even though
        // the show-level field hasn't caught up. Try season+1 once before
        // giving up, so an ongoing show doesn't get stuck showing as Released.
        if (!eps.isEmpty()) {
            QString url2 = QString("%1/tv/%2/season/%3?api_key=%4")
                .arg(BASE).arg(showId).arg(season + 1).arg(apiKey());
            QNetworkReply* r2 = getJson(url2);
            connect(r2, &QNetworkReply::finished, this,
                    [r2, td, isRefresh, season, this]() mutable {
                r2->deleteLater();
                if (r2->error() == QNetworkReply::NoError) {
                    QJsonArray eps2 = QJsonDocument::fromJson(r2->readAll())
                                         .object()["episodes"].toArray();
                    QDate today2 = QDate::currentDate();
                    QDate bestDate2;
                    int bestEp2 = -1;
                    for (const QJsonValue& v : eps2) {
                        QJsonObject ep = v.toObject();
                        int epNum = ep["episode_number"].toInt();
                        if (epNum <= 0) continue;
                        QDate d = QDate::fromString(ep["air_date"].toString(), Qt::ISODate);
                        if (!d.isValid() || d < today2) continue;
                        if (!bestDate2.isValid() || d < bestDate2) {
                            bestDate2 = d;
                            bestEp2   = epNum;
                        }
                    }
                    if (bestDate2.isValid() && bestEp2 > 0) {
                        td.targetDate  = bestDate2;
                        td.dateDisplay = bestDate2.toString("MMMM d, yyyy");
                        td.statusLabel = QString("S%1E%2")
                            .arg(season + 1, 2, 10, QChar('0'))
                            .arg(bestEp2,    2, 10, QChar('0'));
                        td.rawDateText = QString("%1|%2|%3")
                            .arg(bestDate2.toString(Qt::ISODate)).arg(season + 1).arg(bestEp2);
                        td.seasonEpisodeCount = eps2.size();
                        td.isEstimatedDate  = false;
                        td.inMidSeasonBreak = false;
                    }
                }
                if (isRefresh) emit tileRefreshed(td); else emit dataReady(td);
            });
            return;
        }

        // Season fetch came back empty — nothing more to try.
        if (isRefresh) emit tileRefreshed(td); else emit dataReady(td);
    });
}

// =============================================================================
//  Phase 2 — fetch full details for a new tile
// =============================================================================
void TmdbScraper::fetchDetails(int tmdbId, const QString& mediaType, const QString&)
{
    QString appendTo = (mediaType == "tv") ? "next_episode_to_air,images" : "release_dates,images";
    // v3.1.2 — bias the images sub-response toward the user's Language
    // setting (plus textless backdrops) rather than TMDB's default order,
    // which can surface a backdrop in an unexpected language.
    QString url = QString("%1/%2/%3?api_key=%4&append_to_response=%5&images.include_image_language=%6,null")
        .arg(BASE, mediaType, QString::number(tmdbId), apiKey(), appendTo, LanguageUtil::currentCode());

    QNetworkReply* r = getJson(url);
    connect(r, &QNetworkReply::finished, this, [r, mediaType, tmdbId, this]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            emit scraperError("Detail fetch failed: " + r->errorString());
            return;
        }
        QJsonObject obj = QJsonDocument::fromJson(r->readAll()).object();
        TileData td = parseDetailsJson(obj, mediaType);

        // Backdrop
        QJsonArray backdrops = obj["images"].toObject()["backdrops"].toArray();
        if (!backdrops.isEmpty())
            downloadBackdrop(td.id, pickBackdropPath(backdrops), /*makeActive=*/true);

        // For TV with an upcoming episode, check for multi-episode premiere
        if (mediaType == "tv" && !td.rawDateText.isEmpty() &&
            td.rawDateText.contains('|'))
        {
            QStringList parts = td.rawDateText.split('|');
            if (parts[0] == "SCAN") {
                // next_episode_to_air was empty — scan the season for future episodes
                int season = parts[1].toInt();
                int afterEpisodeNum = parts.size() > 2 ? parts[2].toInt() : 0;
                fetchSeasonForFutureEp(tmdbId, season, afterEpisodeNum, td, false);
            } else {
                QString airDate = parts[0];
                int season      = parts[1].toInt();
                fetchSeasonForMultiEp(tmdbId, season, airDate, td, false);
            }
        } else {
            emit dataReady(td);
        }
    });
}

// =============================================================================
//  refreshTile — called on startup for each saved tile
//               Preserves existing id and user-set imagePath unless backdrop missing
// =============================================================================
void TmdbScraper::refreshTile(const TileData& existing, bool forceBackdropRefetch)
{
    if (existing.tmdbId <= 0) return;

    QString appendTo = (existing.mediaType == "tv") ? "next_episode_to_air,images" : "release_dates,images";
    QString url = QString("%1/%2/%3?api_key=%4&append_to_response=%5&images.include_image_language=%6,null")
        .arg(BASE, existing.mediaType, QString::number(existing.tmdbId), apiKey(), appendTo, LanguageUtil::currentCode());

    QNetworkReply* r = getJson(url);
    connect(r, &QNetworkReply::finished, this, [r, existing, forceBackdropRefetch, this]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            qDebug() << "[TmdbScraper] Refresh failed for" << existing.title;
            // v3.3.37 fix — previously returned here with no signal at all,
            // which meant the caller's pending-refresh counter never got
            // decremented for this tile. Emitting the unchanged data is a
            // safe no-op (onTileRefreshed already treats identical data as
            // "nothing changed"), and ensures a batch of refreshes can
            // always tell when it's truly finished, even if one fails.
            emit tileRefreshed(existing);
            return;
        }
        QJsonObject obj = QJsonDocument::fromJson(r->readAll()).object();
        TileData updated = parseDetailsJson(obj, existing.mediaType,
                                            existing.id, existing.imagePath);

        // Always preserve user customisations — TMDB refresh must never overwrite these
        updated.customTitle   = existing.customTitle;
        updated.customDate    = existing.customDate;
        updated.customDateStr = existing.customDateStr;
        updated.customAirTime = existing.customAirTime;  // never overwrite user's time

        // v3.1.0 — parseDetailsJson builds a fresh TileData, so the image
        // history has to be carried over explicitly or every refresh would
        // silently wipe out the fetched slot and any custom images.
        updated.fetchedImagePath = existing.fetchedImagePath;
        updated.customImagePaths = existing.customImagePaths;
        updated.imagePath = existing.imagePath;

        // Re-download if the fetched backdrop specifically is missing/gone,
        // or if the caller explicitly wants a fresh one regardless (the
        // Edit dialog's "Refetch Image" button).
        bool backdropMissing = existing.fetchedImagePath.isEmpty() ||
                               !QFile::exists(existing.fetchedImagePath);

        // V5 — a cached backdrop isn't kept forever any more. It's replaced
        // when the show has moved to a new season (artwork changes with the
        // season, and last season's image looks plainly wrong), and in any
        // case before it reaches the 6-month limit TMDB's terms place on
        // caching their content. updated.statusLabel is the freshly fetched
        // one, so this compares the season the tile is on NOW against the
        // season the stored image was fetched for.
        int currentSeason = parseEpisodeLabel(updated.statusLabel).season;
        bool artworkStale = existing.artworkNeedsRefresh(currentSeason);

        if (backdropMissing || forceBackdropRefetch || artworkStale) {
            QJsonArray backdrops = obj["images"].toObject()["backdrops"].toArray();
            if (!backdrops.isEmpty()) {
                // A forced refetch always takes over as the active image; a
                // routine "it's missing" re-download only takes over if
                // nothing else is currently active (doesn't yank away a
                // custom image the user is already looking at).
                // A stale-artwork replacement behaves like the routine
                // "it's missing" case, not like a forced refetch: it fills
                // the fetched slot and only becomes visible if the user
                // isn't looking at a custom image of their own.
                bool makeActive = forceBackdropRefetch || existing.imagePath.isEmpty();
                downloadBackdrop(existing.id, pickBackdropPath(backdrops), makeActive);
            }
            updated.artworkSeason    = currentSeason;
            updated.artworkFetchedOn = QDate::currentDate();
        } else {
            // Nothing re-downloaded — carry the existing provenance across
            // so the age clock isn't reset by an unrelated refresh.
            updated.artworkSeason    = existing.artworkSeason;
            updated.artworkFetchedOn = existing.artworkFetchedOn;
        }

        // Preserve notified flag unless date changed
        updated.notified = (existing.targetDate == updated.targetDate) && existing.notified;

        // For TV with upcoming episode, check for multi-episode premiere
        if (existing.mediaType == "tv" && !updated.rawDateText.isEmpty() &&
            updated.rawDateText.contains('|'))
        {
            QStringList parts = updated.rawDateText.split('|');
            QString airDate = parts[0];
            int season      = parts[1].toInt();
            if (parts[0] == "SCAN") {
                int afterEpisodeNum = parts.size() > 2 ? parts[2].toInt() : 0;
                fetchSeasonForFutureEp(existing.tmdbId, season, afterEpisodeNum, updated, true);
            } else {
                fetchSeasonForMultiEp(existing.tmdbId, season, airDate, updated, true);
            }
        } else {
            emit tileRefreshed(updated);
        }
    });
}

// =============================================================================
//  fetchCreditsForResults — parallel credits for the drop-up
// =============================================================================
void TmdbScraper::fetchCreditsForResults(const QList<SearchResult>& results)
{
    for (const SearchResult& sr : results) {
        QString url = QString("%1/%2/%3/credits?api_key=%4")
            .arg(BASE, sr.mediaType, QString::number(sr.id), apiKey());
        int tmdbId    = sr.id;
        QString mtype = sr.mediaType;
        QNetworkReply* r = getJson(url);

        connect(r, &QNetworkReply::finished, this, [r, tmdbId, mtype, this]() {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) return;
            QJsonObject obj  = QJsonDocument::fromJson(r->readAll()).object();
            QJsonArray  crew = obj["crew"].toArray();
            QJsonArray  cast = obj["cast"].toArray();

            QString director;
            QStringList dirJobs = (mtype == "movie")
                ? QStringList{"Director"}
                : QStringList{"Creator", "Series Creator", "Executive Producer"};
            for (const QJsonValue& v : crew) {
                QJsonObject c = v.toObject();
                if (dirJobs.contains(c["job"].toString())) {
                    director = c["name"].toString();
                    break;
                }
            }
            QStringList castNames;
            for (int i = 0; i < qMin(3, (int)cast.size()); ++i)
                castNames << cast[i].toObject()["name"].toString();

            emit creditsReady(tmdbId, director, castNames.join(", "));
        });
    }
}

// =============================================================================
void TmdbScraper::downloadBackdrop(const QString& tileId, const QString& backdropPath, bool makeActive)
{
    // v3.1.3 — "w1280" caps the image at 1280px wide. TMDB's backdrop size
    // presets are w300/w780/w1280/"original" — there's nothing between
    // w1280 and "original" (which is uncontrolled and can be 4K+), so this
    // is already the largest capped option and safely under 1920x1080 for
    // the landscape aspect ratio backdrops always use (1280x720 at 16:9).
    QString url = QString("https://image.tmdb.org/t/p/w1280%1").arg(backdropPath);
    // V5.4.25 — through the shared queue rather than straight at the CDN. This
    // is one of only two places the app talks to an image host directly, with
    // nothing else pacing it; see imagequeue.h for the published limits this
    // is kept inside.
    ImageFetchQueue::instance().fetch(QUrl(url),
        [this, tileId, backdropPath, makeActive](const QByteArray& body, bool ok) {
            if (!ok) return;
            QString dir = QStandardPaths::writableLocation(
                              QStandardPaths::AppDataLocation) + "/fetched_images";
            QDir().mkpath(dir);
            QString ext       = backdropPath.section('.', -1);
            QString localPath = dir + "/" + tileId + "." + ext;
            QFile f(localPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(body);
                f.close();
                emit posterReady(tileId, localPath, makeActive);
            }
        });
}

// =============================================================================
//  fetchBackdropOnly — V5. See the header for why this exists separately
//  from refreshTile(): it must not emit tileRefreshed, or TMDB's weaker
//  episode dates would overwrite TVmaze's.
// =============================================================================
void TmdbScraper::fetchBackdropOnly(const QString& tileId, int tmdbId,
                                     const QString& mediaType, bool makeActive)
{
    if (tmdbId <= 0) return;
    QString url = QString("%1/%2/%3/images?api_key=%4&include_image_language=%5,null")
        .arg(BASE, mediaType, QString::number(tmdbId), apiKey(), LanguageUtil::currentCode());

    QNetworkReply* r = getJson(url);
    connect(r, &QNetworkReply::finished, this, [r, tileId, makeActive, this]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) return;
        QJsonArray backdrops = QJsonDocument::fromJson(r->readAll())
                                   .object()["backdrops"].toArray();
        if (backdrops.isEmpty()) return;
        downloadBackdrop(tileId, pickBackdropPath(backdrops), makeActive);
    });
}

QNetworkReply* TmdbScraper::getJson(const QString& url)
{
    QNetworkRequest req;
    if (RelayConfig::shouldUseRelayForTmdb()) {
        // Every call site already built a full "BASE/path?api_key=...&..."
        // URL string — rewriting just the BASE prefix to the relay's
        // /tmdb path means none of those 8 call sites need to change at
        // all. The leftover api_key=... query param is harmless: the
        // relay always overwrites it with the real key server-side
        // before forwarding, regardless of whatever value shows up here.
        QString rewritten = url;
        rewritten.replace(QString(BASE), RelayConfig::baseUrl() + "/tmdb");
        req.setUrl(QUrl(rewritten));
        req.setRawHeader("Authorization", ("Bearer " + RelayConfig::sharedSecret()).toUtf8());
        req.setRawHeader("X-Client-ID", RelayConfig::installationId().toUtf8());
    } else {
        req.setUrl(QUrl(url));
    }
    req.setRawHeader("Accept", "application/json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_nam->get(req);
}
