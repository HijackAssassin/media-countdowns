#include "imagequeue.h"
#include "igdbscraper.h"
#include "applogger.h"
#include "relayconfig.h"
#include <QNetworkRequest>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QUuid>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QStandardPaths>
#include <algorithm>
#include <QSet>
#include <QMap>
#include <QSharedPointer>
#include <QRegularExpression>

IgdbScraper::IgdbScraper(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

// =============================================================================
void IgdbScraper::withAccessToken(std::function<void(const QString&)> onReady)
{
    if (RelayConfig::shouldUseRelayForIgdb()) {
        // The relay handles the real Twitch OAuth token entirely
        // server-side — no fetch needed here at all. This placeholder
        // is never actually used as a real bearer token in this mode
        // (postToIgdb below ignores it and uses the relay's shared
        // secret instead); it only exists to satisfy the existing
        // onReady(token) callback signature every call site expects.
        onReady(QStringLiteral("relay-managed"));
        return;
    }
    if (!isConfigured()) {
        emit scraperError("IGDB Client ID/Secret not set — add them in Settings → IGDB API Credentials.");
        return;
    }
    qint64 now = QDateTime::currentSecsSinceEpoch();
    // 60s safety buffer before the token's real expiry
    if (!m_accessToken.isEmpty() && now < m_tokenExpiryEpoch - 60) {
        onReady(m_accessToken);
        return;
    }
    fetchAccessToken(onReady);
}

void IgdbScraper::fetchAccessToken(std::function<void(const QString&)> onReady)
{
    QString url = QString("%1?client_id=%2&client_secret=%3&grant_type=client_credentials")
        .arg(TOKEN_URL, clientId(), clientSecret());
    QNetworkRequest req{QUrl(url)};
    QNetworkReply* r = m_nam->post(req, QByteArray());
    connect(r, &QNetworkReply::finished, this, [this, r, onReady]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            emit scraperError(
                "Couldn't authenticate with IGDB — double check your Client ID and "
                "Client Secret in Settings → IGDB API Credentials.");
            return;
        }
        QJsonObject obj = QJsonDocument::fromJson(r->readAll()).object();
        QString token = obj.value("access_token").toString();
        int expiresIn = obj.value("expires_in").toInt();
        if (token.isEmpty()) {
            emit scraperError("IGDB authentication failed — no access token returned.");
            return;
        }
        m_accessToken = token;
        m_tokenExpiryEpoch = QDateTime::currentSecsSinceEpoch() + expiresIn;
        onReady(m_accessToken);
    });
}

// =============================================================================
//  V4 — postToIgdb() centralizes what all 5 IGDB request-sending call
//  sites used to duplicate individually: building the request and
//  setting the Client-ID/Authorization/Content-Type headers. This is
//  the one place that needs to know about relay vs. direct mode,
//  mirroring the role TmdbScraper::getJson() plays for TMDB.
QNetworkReply* IgdbScraper::postToIgdb(const QString& path, const QByteArray& body, const QString& token)
{
    QNetworkRequest req;
    if (RelayConfig::shouldUseRelayForIgdb()) {
        req.setUrl(QUrl(RelayConfig::baseUrl() + "/igdb/" + path));
        req.setRawHeader("Authorization", ("Bearer " + RelayConfig::sharedSecret()).toUtf8());
        req.setRawHeader("X-Client-ID", RelayConfig::installationId().toUtf8());
        // No Client-ID header here — the relay injects the real one
        // server-side, same as it injects the real Twitch bearer token.
    } else {
        req.setUrl(QUrl(QString("%1/%2").arg(BASE, path)));
        req.setRawHeader("Client-ID", clientId().toUtf8());
        req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    }
    req.setRawHeader("Content-Type", "text/plain");
    return m_nam->post(req, body);
}

// =============================================================================

// =============================================================================
//  v3.3.15 fix #2 — a result's title must actually start with (or, for a
//  short, explicit list of known exceptions, contain as a recognizable
//  subtitle/abbreviation) the search query. Without this, IGDB's own fuzzy
//  `search` can return anything containing the query phrase ANYWHERE in
//  the name — e.g. searching "Modern Warfare" returning "Drone: Modern
//  Warfare," or "halo" returning "Angel Halo." Exceptions are deliberately
//  a short, named list rather than a general "subtitle counts" rule, since
//  a general rule would let "Drone: Modern Warfare" back in too.
static QString normalizeForMatching(const QString& s)
{
    QString out = s;
    out.remove(':');
    return out.simplified();   // collapses whitespace left behind, trims
}

// v3.3.24 fix #1 — known search abbreviations that should behave
// identically to the full name they stand for, used both here and in
// series detection below.
static QString expandKnownAlias(const QString& q)
{
    QString lower = q.trimmed().toLower();
    static const QVector<QPair<QString, QString>> kAliases = {
        {"cod", "call of duty"},
        {"gta", "grand theft auto"},
    };

    // V5 — expand the alias when it's the leading WORD, not only when it's
    // the entire query. This used to match "gta" alone and nothing else, so
    // "gta v" and "gta 5" were left unexpanded — IGDB's own fuzzy search
    // still returned Grand Theft Auto V, but titleMatchesQuery() then threw
    // it away because "grand theft auto v" doesn't start with "gta 5".
    // Searching the full name worked while the abbreviation didn't, which
    // is exactly backwards from what an abbreviation is for.
    //
    // The numeral form is handled separately: titleMatchesQuery() already
    // retries with generateNumeralAlternate(), so "grand theft auto 5"
    // also matches "Grand Theft Auto V" once the prefix itself lines up.
    for (const auto& a : kAliases) {
        if (lower == a.first) return a.second;
        if (lower.startsWith(a.first + " "))
            return a.second + lower.mid(a.first.size());
    }
    return q;
}

// v3.3.43 — converts a number (1-50, a reasonable range for game
// sequels) to its Roman numeral form.
static QString numberToRoman(int n)
{
    if (n <= 0 || n > 50) return QString();
    static const QList<QPair<int, QString>> table = {
        {50,"L"}, {40,"XL"}, {10,"X"}, {9,"IX"}, {5,"V"}, {4,"IV"}, {1,"I"}
    };
    QString result;
    for (const auto& pair : table)
        while (n >= pair.first) { result += pair.second; n -= pair.first; }
    return result;
}

// v3.3.43 — converts a Roman numeral (I-L range) back to a number.
// Returns -1 if the string isn't a valid Roman numeral at all, so
// ordinary words aren't mistaken for one.
static int romanToNumber(const QString& s)
{
    static const QMap<QChar, int> values = {{'I',1},{'V',5},{'X',10},{'L',50}};
    QString upper = s.toUpper();
    if (upper.isEmpty()) return -1;
    for (QChar c : upper) if (!values.contains(c)) return -1;
    int total = 0;
    for (int i = 0; i < upper.length(); ++i) {
        int cur = values[upper[i]];
        int next = (i + 1 < upper.length()) ? values[upper[i + 1]] : 0;
        total += (cur < next) ? -cur : cur;
    }
    return total;
}

// v3.3.43 — finds a standalone number or Roman numeral word in the
// query and returns the same query with that one word converted to the
// other form (e.g. "Black Ops 2" -> "Black Ops II", or the reverse).
// Returns an empty string if no such word is found, meaning there's no
// alternate form to also search for. This is what lets a search for
// "Black Ops 2" also find "Black Ops II" — IGDB's actual title — since
// without it, the two are simply different search text sent to the API,
// the same class of bug fixed for abbreviations like "cod" in v3.3.30.
// V5 — "black ops 1" should find "Call of Duty: Black Ops".
//
// The first entry in a series is almost never numbered: it's "Black Ops",
// "Grand Theft Auto", "Half-Life" — the "1" only exists in how people talk
// about them once a sequel arrives. Searching that way returned nothing at
// all, because neither IGDB nor the title filter has any reason to connect
// "black ops 1" to a title with no 1 in it.
//
// Returns the query with a trailing "1" or "I" removed, or empty when the
// query doesn't end that way. Deliberately only the LAST word: "Half-Life 2
// Episode 1" shouldn't lose its meaningful trailing 1... which it would, so
// this is applied as an ADDITIONAL candidate rather than a replacement,
// leaving the original to match first when it can.
static QString generateFirstEntryAlternate(const QString& query)
{
    QStringList words = query.trimmed().split(' ', Qt::SkipEmptyParts);
    if (words.size() < 2) return {};
    QString last = words.last().toUpper();
    if (last != "1" && last != "I") return {};
    words.removeLast();
    return words.join(' ');
}

static QString generateNumeralAlternate(const QString& query)
{
    QStringList words = query.split(' ', Qt::SkipEmptyParts);
    for (int i = 0; i < words.size(); ++i) {
        bool isNum = false;
        int num = words[i].toInt(&isNum);
        if (isNum && num > 0 && num <= 50) {
            QString roman = numberToRoman(num);
            if (roman.isEmpty()) continue;
            QStringList alt = words;
            alt[i] = roman;
            return alt.join(' ');
        }
        int romanVal = romanToNumber(words[i]);
        if (romanVal > 0) {
            QStringList alt = words;
            alt[i] = QString::number(romanVal);
            return alt.join(' ');
        }
    }
    return QString();
}

static bool titleMatchesQuery(const QString& title, const QString& query, bool allowNumeralAlternate = true)
{
    QString t = title.trimmed().toLower();
    QString q = expandKnownAlias(query).trimmed().toLower();
    if (q.isEmpty()) return true;

    if (t.startsWith(q)) return true;

    // v3.3.19 fix — punctuation (colons specifically) shouldn't be
    // required to match: "Halo Campaign Evolved" should find "Halo:
    // Campaign Evolved" without the colon being typed. A second,
    // colon-normalized comparison catches this without loosening the
    // original strict check (which still runs first, above).
    QString tNorm = normalizeForMatching(t);
    QString qNorm = normalizeForMatching(q);
    if (tNorm.startsWith(qNorm)) return true;

    // Known franchises where searching just the subtitle is a natural,
    // common way to search (e.g. "Modern Warfare," "Black Ops," "Warzone"
    // for Call of Duty) without typing the whole franchise name first.
    static const QStringList kFranchisePrefixes = { "call of duty" };
    static const QRegularExpression kLeadingSeparator("^\\s*\\d*\\s*[:\\-]?\\s*");
    for (const QString& prefix : kFranchisePrefixes) {
        if (!t.startsWith(prefix)) continue;
        QString remainder = t.mid(prefix.length());
        remainder.remove(kLeadingSeparator);
        if (remainder.startsWith(q)) return true;
    }

    // "gta" already expanded to "grand theft auto" above, so q itself
    // covers this case via the startsWith checks; kept as an explicit
    // fallback in case of normalization edge cases.
    if (t.startsWith("grand theft auto") && q == "grand theft auto") return true;

    // v3.3.43 — if none of the above matched, also try the numeral
    // alternate form (e.g. a search for "Black Ops 2" should still match
    // a title of "Black Ops II"). allowNumeralAlternate=false on the
    // recursive call prevents this from looping — the alternate of an
    // alternate is just the original form again.
    if (allowNumeralAlternate) {
        QString altQuery = generateNumeralAlternate(query);
        if (!altQuery.isEmpty() && titleMatchesQuery(title, altQuery, false)) return true;

        // V5 — and try it as an unnumbered first entry, so "black ops 1"
        // matches "Call of Duty: Black Ops" and "gta 1" matches "Grand
        // Theft Auto". Same non-recursive guard.
        QString firstEntry = generateFirstEntryAlternate(query);
        if (!firstEntry.isEmpty() && titleMatchesQuery(title, firstEntry, false)) return true;
    }

    return false;
}

void IgdbScraper::searchMedia(const QString& query)
{
    QString q = query.trimmed();
    if (q.isEmpty()) return;

    // v3.3.15 fix — detect an optional trailing year (e.g. "gta 2013"),
    // same approach TmdbScraper::searchMedia() already uses for movies/
    // shows: strip it from the actual search text, and use it afterward
    // to restrict results to that release year specifically.
    int yearFilter = 0;
    static const QRegularExpression yearRe(R"(\b(19\d{2}|20\d{2})$)");
    auto ym = yearRe.match(q);
    if (ym.hasMatch()) {
        yearFilter = ym.captured(1).toInt();
        q = q.left(ym.capturedStart()).trimmed();
    }

    // v3.3.30 fix — critical: expand a known abbreviation (cod, gta)
    // BEFORE building the actual queries sent to IGDB. Previously this
    // expansion only happened inside the client-side filtering logic
    // (filterAndSortResults), never here — meaning searching "cod" was
    // literally asking IGDB's API to search for the text "cod", not
    // "Call of Duty", which returns a completely different (and far
    // noisier) set of raw candidates than searching "Call of Duty"
    // directly. This is the confirmed root cause of "cod" and "Call of
    // Duty" producing different results — they were, in fact, different
    // queries sent over the network, not just different client-side
    // matching.
    q = expandKnownAlias(q);
    APPLOG(QString("IgdbScraper::searchMedia — original query, after alias expansion: \"%1\"").arg(q));

    // v3.3.43 — if the query has a number or Roman numeral in it, also
    // search for the other form (see generateNumeralAlternate above).
    // Empty if no such word is found, meaning there's nothing extra to
    // search for this round.
    QString altQ = generateNumeralAlternate(q);
    // V5 — a trailing "1" has no numeral alternate worth searching ("I"
    // isn't how anything is titled), but dropping it entirely is: the first
    // entry in a series is unnumbered. Prefer that as the alternate query
    // so "black ops 1" actually reaches "Call of Duty: Black Ops" upstream,
    // not just in the client-side filter.
    QString firstEntryQ = generateFirstEntryAlternate(q);
    if (!firstEntryQ.isEmpty()) altQ = firstEntryQ;
    if (!altQ.isEmpty())
        APPLOG(QString("IgdbScraper::searchMedia — also searching alternate: \"%1\"").arg(altQ));

    withAccessToken([this, q, altQ, yearFilter](const QString& token) {
        QString escaped = QString(q).replace('\\', "\\\\").replace('"', "\\\"");
        QString escapedAlt = altQ.isEmpty() ? QString()
                              : QString(altQ).replace('\\', "\\\\").replace('"', "\\\"");

        // v3.3.22 fix — three queries now run in parallel and get merged
        // before filtering. Query 1 is IGDB's own fuzzy `search`. Query 2
        // is a strict "name contains" filter sorted by rating, a safety
        // net for well-known titles a fuzzy search might rank low. Query
        // 3 (new) queries by IGDB's "collection" field directly — this is
        // what IGDB's own site presents as a game's "Series" — since a
        // specific title (like a remake or remaster) can rank poorly or
        // not show up at all in a name-based search, but reliably belongs
        // to the same series as its well-known relatives. Results get
        // de-duplicated by id when merged.
        auto pending  = QSharedPointer<int>::create(altQ.isEmpty() ? 3 : 4);
        auto combined = QSharedPointer<QJsonArray>::create();
        auto onBothDone = [this, pending, combined, q, yearFilter]() {
            if (--(*pending) > 0) return;
            APPLOG(QString("IgdbScraper::searchMedia — query \"%1\": %2 raw candidate(s) from IGDB (all 3 queries merged, before filtering)")
                   .arg(q).arg(combined->size()));
            // v3.3.31 — direct confirmation of which fields the live API
            // is actually populating: "category"/"collection" are marked
            // deprecated in IGDB's own docs (replaced by "game_type" and
            // "collections"), and this logs which ones a sample of the
            // raw results actually have data in, for the first 5 items.
            {
                int checked = 0;
                for (const QJsonValue& v : *combined) {
                    if (checked >= 5) break;
                    QJsonObject obj = v.toObject();
                    APPLOG(QString("  sample \"%1\": category=%2 game_type=%3 collection=%4 collections=%5")
                        .arg(obj.value("name").toString())
                        .arg(obj.contains("category") ? QString::number(obj.value("category").toInt(-999)) : "absent")
                        .arg(obj.contains("game_type") ? (obj.value("game_type").isNull() ? "null" : QString::number(obj.value("game_type").toInt(-999))) : "absent")
                        .arg(obj.value("collection").toObject().value("name").toString().isEmpty() ? "empty" : obj.value("collection").toObject().value("name").toString())
                        .arg(obj.value("collections").toArray().isEmpty() ? "empty" : QString::number(obj.value("collections").toArray().size()) + " item(s)"));
                    ++checked;
                }
            }
            QList<SearchResult> results = filterAndSortResults(*combined, q, yearFilter);
            QStringList resultTitles;
            for (const auto& r : results) resultTitles << r.title;
            APPLOG(QString("IgdbScraper::searchMedia — query \"%1\": %2 result(s) after filtering: %3")
                   .arg(q).arg(results.size()).arg(resultTitles.join(", ")));
            if (results.isEmpty()) {
                emit scraperError(combined->isEmpty()
                    ? "No games found for that search."
                    : "Found results, but all were filtered out (editions/DLC/"
                      "bundles/undated/platform/title-match) — try a more "
                      "specific search.");
                return;
            }
            emit searchResultsReady(results);
        };

        QString fields =
            // V5 — width/height are what let preferredGameImage() pick a
            // genuinely landscape image instead of whichever happens to be
            // first (often square key art).
            // V5.4 — artworks.image_type.id is what separates real key art
            // from a logo strip or an icon that IGDB files in the same array.
            "fields name,first_release_date,cover.image_id,cover.width,cover.height,"
            "artworks.image_id,artworks.width,artworks.height,artworks.image_type.id,"
            "screenshots.image_id,screenshots.width,screenshots.height,total_rating,category,"
            "game_type,version_parent,parent_game,dlcs,expansions,bundles,standalone_expansions,"
            "platforms.name,collection.name,collections.name;\n";

        // Query 1 — fuzzy search (existing behavior).
        {
            QString body = QString("search \"%1\";\n%2limit 100;\n").arg(escaped, fields);
            APPLOG(QString("IgdbScraper query 1 (fuzzy search) body:\n%1").arg(body));
            QNetworkReply* r = postToIgdb("games", body.toUtf8(), token);
            connect(r, &QNetworkReply::finished, this, [r, combined, onBothDone]() {
                r->deleteLater();
                if (r->error() == QNetworkReply::NoError) {
                    QJsonArray arr = QJsonDocument::fromJson(r->readAll()).array();
                    APPLOG(QString("IgdbScraper query 1 (fuzzy search) — %1 result(s)").arg(arr.size()));
                    for (const auto& v : arr)
                        combined->append(v);
                } else {
                    APPLOG(QString("IgdbScraper query 1 (fuzzy search) FAILED: %1").arg(r->errorString()));
                }
                onBothDone();
            });
        }

        // Query 2 — strict "contains" backfill, sorted by rating so the
        // most well-known matches are prioritized within its own limit.
        // v3.3.43 — when a numeral alternate exists (e.g. searching
        // "Black Ops 2" also means "Black Ops II"), OR-combine both forms
        // into this same query rather than firing a second one.
        {
            QString whereClause = escapedAlt.isEmpty()
                ? QString("where name ~ *\"%1\"*;\n").arg(escaped)
                : QString("where name ~ *\"%1\"* | name ~ *\"%2\"*;\n").arg(escaped, escapedAlt);
            QString body = QString(
                "%1"
                "%2"
                "sort total_rating desc;\n"
                "limit 60;\n"
            ).arg(fields, whereClause);
            APPLOG(QString("IgdbScraper query 2 (contains backfill) body:\n%1").arg(body));
            QNetworkReply* r = postToIgdb("games", body.toUtf8(), token);
            connect(r, &QNetworkReply::finished, this, [r, combined, onBothDone]() {
                r->deleteLater();
                if (r->error() == QNetworkReply::NoError) {
                    QJsonArray arr = QJsonDocument::fromJson(r->readAll()).array();
                    APPLOG(QString("IgdbScraper query 2 (contains backfill) — %1 result(s)").arg(arr.size()));
                    for (const auto& v : arr)
                        combined->append(v);
                } else {
                    APPLOG(QString("IgdbScraper query 2 (contains backfill) FAILED: %1").arg(r->errorString()));
                }
                onBothDone();
            });
        }

        // Query 3 — search by Series (IGDB's "collection" field)
        // directly. This is what makes "show everything in the Halo
        // series" or "everything in the Call of Duty series" reliable —
        // a specific title can rank poorly (or not appear at all) in a
        // name-based search, but it's still reliably linked to the same
        // collection as its well-known relatives. My existing category/
        // parent_game/dlcs/name-keyword filters still apply to whatever
        // comes back here, so DLC, mods, bundles, and fan-made content
        // within a series are still excluded — this just widens which
        // titles are considered in the first place.
        {
            QString whereClause = escapedAlt.isEmpty()
                ? QString("where collection.name ~ *\"%1\"*;\n").arg(escaped)
                : QString("where collection.name ~ *\"%1\"* | collection.name ~ *\"%2\"*;\n").arg(escaped, escapedAlt);
            QString body = QString(
                "%1"
                "%2"
                "limit 100;\n"
            ).arg(fields, whereClause);
            APPLOG(QString("IgdbScraper query 3 (series/collection) body:\n%1").arg(body));
            QNetworkReply* r = postToIgdb("games", body.toUtf8(), token);
            connect(r, &QNetworkReply::finished, this, [r, combined, onBothDone]() {
                r->deleteLater();
                if (r->error() == QNetworkReply::NoError) {
                    QJsonArray arr = QJsonDocument::fromJson(r->readAll()).array();
                    APPLOG(QString("IgdbScraper query 3 (series/collection) — %1 result(s)").arg(arr.size()));
                    for (const auto& v : arr)
                        combined->append(v);
                } else {
                    APPLOG(QString("IgdbScraper query 3 (series/collection) FAILED: %1").arg(r->errorString()));
                }
                onBothDone();
            });
        }

        // Query 4 — v3.3.43. A fuzzy search on the numeral-alternate
        // form only, when one exists (see generateNumeralAlternate
        // above). Query 2/3 above already OR-combine both forms in a
        // single request, but IGDB's fuzzy `search` operator (query 1)
        // can't OR-combine two search texts in one request — this gives
        // the alternate form the same fuzzy-ranking treatment the
        // original text gets from query 1, rather than relying solely
        // on the stricter "contains" match from query 2.
        if (!escapedAlt.isEmpty()) {
            QString body = QString("search \"%1\";\n%2limit 100;\n").arg(escapedAlt, fields);
            APPLOG(QString("IgdbScraper query 4 (fuzzy search, numeral alternate) body:\n%1").arg(body));
            QNetworkReply* r = postToIgdb("games", body.toUtf8(), token);
            connect(r, &QNetworkReply::finished, this, [r, combined, onBothDone]() {
                r->deleteLater();
                if (r->error() == QNetworkReply::NoError) {
                    QJsonArray arr = QJsonDocument::fromJson(r->readAll()).array();
                    APPLOG(QString("IgdbScraper query 4 (fuzzy search, numeral alternate) — %1 result(s)").arg(arr.size()));
                    for (const auto& v : arr)
                        combined->append(v);
                } else {
                    APPLOG(QString("IgdbScraper query 4 (fuzzy search, numeral alternate) FAILED: %1").arg(r->errorString()));
                }
                onBothDone();
            });
        }
    });
}

// =============================================================================
// =============================================================================
// v3.3.31 fix — IGDB's own docs mark "category" as deprecated in favor
// of "game_type", and "collection" (singular) as deprecated in favor of
// "collections" (plural, since a game can now belong to more than one).
// Since it's unverified which of these a given API response actually
// populates, both are read and the non-deprecated one is preferred when
// present, falling back to the old field otherwise. This is likely the
// real root cause of category- and collection-based checks not behaving
// as expected: if "category"/"collection" have gone unpopulated for many
// entries following IGDB's migration to the new fields, every check
// built against them would silently do nothing for those entries.
static int effectiveCategory(const QJsonObject& obj)
{
    if (obj.contains("game_type") && !obj.value("game_type").isNull())
        return obj.value("game_type").toInt(-1);
    return obj.value("category").toInt(-1);
}

static QStringList effectiveCollectionNames(const QJsonObject& obj)
{
    QStringList names;
    QString singular = obj.value("collection").toObject().value("name").toString();
    if (!singular.isEmpty())
        names << singular;
    for (const QJsonValue& c : obj.value("collections").toArray()) {
        QString name = c.toObject().value("name").toString();
        if (!name.isEmpty())
            names << name;
    }
    return names;
}

// preferredGameImage — which IGDB image becomes a game tile's picture.
//
// History, because each step fixed a visible bug and none of them should be
// undone:
//   v3.3.43  only the cover (portrait box art) was ever used, and the tile's
//            crop-to-fill scaling cut most of its width away — the original
//            "zoomed in, looks bad" report. Artworks were preferred instead.
//   V5       taking artworks.first() unconditionally was still wrong:
//            Grand Theft Auto VI's first artwork is 2160x2160 (square) while
//            a later one is 3840x2160, and a 16:9 crop of a square source is
//            a narrow band from the middle. Position stopped mattering.
//   V5.3.4   ranking by WIDTH picked Halo: Campaign Evolved's 1280x256 logo
//            banner over its own 3840x2160 key art, and cropping a 5:1 strip
//            to 16:9 keeps only empty background — a pure white tile. Shape
//            (closeness to 16:9) became the ranking.
//
// isLandscape reports whether anything genuinely wide was found, so the
// caller can pick a resize preset that doesn't hard-crop a portrait image
// when a cover is all that exists.
//
// V5.4 — every candidate from all three arrays is now scored TOGETHER,
// against two things the old code couldn't see:
//
//  1. WHAT the image is. IGDB tags each artwork with an image_type, and the
//     "artworks" array is not one kind of thing: measured on Halo: Campaign
//     Evolved it holds "Artwork" (3840x2160), "Key art with logo"
//     (2880x2160), "Icon" (256x256) and "Game logo (color)" (1280x256) side
//     by side. Shape alone can't tell a promotional still from a logo strip,
//     which is how the blank-tile bug got in; the type can. Older entries
//     carry no image_type at all (Grand Theft Auto VI has none), so an
//     untyped artwork is treated as plain "Artwork" — its array position is
//     the only claim being made about it.
//  2. WHETHER the source can actually fill a 720p tile. A 1024x1024 artwork
//     and a 3840x2160 one are not equally good even at the same aspect
//     ratio: only one of them still has real detail at 1280x720.
//
// Order of authority, highest first — type tier, then whether it can fill
// 720p, then closeness to 16:9. Type outranks shape deliberately: real key
// art that needs a small crop beats a perfectly-16:9 screenshot, because a
// screenshot is a moment of gameplay while key art was drawn to be looked at.
//
// With ONE exception, which measurement forced. Type cannot outrank shape
// without limit: Elden Ring's only non-logo artwork is a 1920x620 banner
// (ratio 3.1), and letting "artwork beats screenshot" stand unconditionally
// picks it over four flawless 1920x1080 screenshots — then throws away 43%
// of its width to reach 16:9. That is the same failure V5.3.4 was about,
// just less extreme. So a candidate that cannot reach 16:9 without losing
// more than 30% of itself drops one rank, which is enough for the
// screenshots to win there and not enough to unseat Halo: Campaign
// Evolved's 4:3 key art (which loses 25%) from its own 16:9 artwork.
namespace {

// Higher = more wanted. Values are spaced so no lower tier can ever climb
// past a higher one on ratio or resolution alone.
enum GameImageTier {
    TIER_UNUSABLE   =   0,   // icons, infographics — never a tile
    TIER_LOGO       = 100,   // last resort: mostly transparent/flat, wrong shape
    TIER_COVER      = 200,   // last resort: box art, portrait by definition
    TIER_SCREENSHOT = 300,   // reliably 16:9, but it's gameplay, not art
    TIER_ARTWORK    = 400,   // promotional stills
    TIER_KEYART     = 500,   // what the game was marketed with
};

// IGDB image_type ids, measured live against the API (see the image_types
// endpoint). Only the ones that can appear on artworks/covers matter here.
int tierForImageType(int imageTypeId)
{
    switch (imageTypeId) {
        case 2:  return TIER_KEYART + 5;   // Key art without logo — the tile
                                            // draws its own title, so art that
                                            // doesn't repeat it wins the tie
        case 3:  return TIER_KEYART;       // Key art with logo
        case 1:  return TIER_ARTWORK;      // Artwork
        case 16: return TIER_ARTWORK - 5;  // Historical artwork (older campaign)
        case 4:  return TIER_ARTWORK - 10; // Concept art — not final art
        case 8:  return TIER_COVER;        // Main cover
        case 10: return TIER_COVER - 5;    // Alternative cover
        case 9:  return TIER_COVER - 10;   // Historical cover
        case 11: return TIER_COVER - 15;   // Square cover
        case 5:  case 6:  case 7: return TIER_LOGO;   // Game logo white/black/colour
        case 12: return TIER_UNUSABLE;     // Infographic — a wall of text
        case 13: return TIER_UNUSABLE;     // Icon — 256x256, nothing to show
        default: return TIER_ARTWORK;      // untyped/unknown: trust the array
    }
}

struct Candidate {
    QString imageId;
    int     tier  = TIER_UNUSABLE;
    int     w     = 0;
    int     h     = 0;
};

}   // namespace

GameImagePick preferredGameImage(const QJsonObject& obj)
{
    // The tile draws 16:9, so the ideal source is AS CLOSE TO 16:9 AS
    // POSSIBLE — not as wide as possible.
    //
    // Ranking by raw width was wrong in a way that produced blank tiles:
    // Halo: Campaign Evolved carries a 1280x256 logo banner (ratio 5.0),
    // which beat its own 3840x2160 key art, and cropping a 5:1 strip to
    // 16:9 keeps only the empty middle — a pure white image. That image is
    // now excluded twice over: by its type (Game logo) and by its height.
    static constexpr int    kMinUsableWidth  = 640;
    static constexpr int    kMinUsableHeight = 360;   // 1280x256 banners fail here
    static constexpr int    k720pWidth       = 1280;
    static constexpr int    k720pHeight      = 720;
    static constexpr double kTargetRatio     = 16.0 / 9.0;
    // How much of an image must survive a crop to 16:9 for that crop to be
    // the better option than fitting it with bars. 0.70 sits between the two
    // real cases it has to separate: 4:3 key art keeps 0.75 and still looks
    // like itself cropped, a 3.1:1 banner keeps 0.57 and does not.
    static constexpr double kMinKeptFraction = 0.70;

    QList<Candidate> pool;

    for (const QJsonValue& v : obj.value("artworks").toArray()) {
        QJsonObject o = v.toObject();
        Candidate c;
        c.imageId = o.value("image_id").toString();
        c.w       = o.value("width").toInt();
        c.h       = o.value("height").toInt();
        c.tier    = o.contains("image_type")
                      ? tierForImageType(o.value("image_type").toObject().value("id").toInt())
                      : TIER_ARTWORK;
        if (!c.imageId.isEmpty()) pool.append(c);
    }
    for (const QJsonValue& v : obj.value("screenshots").toArray()) {
        QJsonObject o = v.toObject();
        Candidate c;
        c.imageId = o.value("image_id").toString();
        c.w       = o.value("width").toInt();
        c.h       = o.value("height").toInt();
        c.tier    = TIER_SCREENSHOT;   // the screenshots endpoint has no image_type
        if (!c.imageId.isEmpty()) pool.append(c);
    }
    {
        QJsonObject o = obj.value("cover").toObject();
        Candidate c;
        c.imageId = o.value("image_id").toString();
        c.w       = o.value("width").toInt();
        c.h       = o.value("height").toInt();
        c.tier    = o.contains("image_type")
                      ? tierForImageType(o.value("image_type").toObject().value("id").toInt())
                      : TIER_COVER;
        if (!c.imageId.isEmpty()) pool.append(c);
    }

    GameImagePick best;
    double bestScore = -1e9;
    bool   anyUsableTier = false;
    for (const Candidate& c : pool)
        if (c.tier > TIER_UNUSABLE) { anyUsableTier = true; break; }

    for (const Candidate& c : pool) {
        // An icon or infographic is never worth showing — unless it is
        // genuinely all this game has, in which case something beats the
        // blank placeholder.
        if (c.tier <= TIER_UNUSABLE && anyUsableTier) continue;

        bool   measured  = (c.w > 0 && c.h > 0);
        double ratio     = measured ? double(c.w) / c.h : 0.0;
        bool   bigEnough = (c.w >= kMinUsableWidth && c.h >= kMinUsableHeight);
        // Can it fill a 720p tile without being upscaled? This is the
        // "prefer 720p" rule: a source at or above 1280x720 renders a real
        // 720p tile, anything smaller is stretched to get there.
        bool   fills720  = (c.w >= k720pWidth && c.h >= k720pHeight);

        // How much of the picture survives a crop to 16:9. Whichever axis is
        // in surplus is the one that gets trimmed:
        //   3840x2160 (1.78) -> 1.00    2880x2160 (1.33) -> 0.75
        //   1920x620  (3.10) -> 0.57    1581x2108 (0.75) -> 0.42
        double kept = 1.0;
        if (measured)
            kept = (ratio > kTargetRatio) ? kTargetRatio / ratio : ratio / kTargetRatio;

        // Close enough to 16:9 to crop-to-fill without losing the picture.
        // Everything else is fitted and padded instead — that's the whole
        // point of the rule, and it's what stops a cover or a near-square
        // artwork from being shown as a zoomed-in band through its middle.
        bool cropsCleanly = measured && bigEnough && kept >= kMinKeptFraction;

        // Shape can cost a rank, but only one, and only when the crop would
        // be severe. See the note above the tier table for why.
        double effectiveTier = double(c.tier) - (kept < kMinKeptFraction ? 100.0 : 0.0);

        double score = effectiveTier * 1000.0             // type is the top authority
                     + (bigEnough    ? 300.0 : 0.0)
                     + (fills720     ? 150.0 : 0.0)       // prefer a true 720p source
                     + (cropsCleanly ?  50.0 : 0.0)
                     + (measured ? kept * 10.0 : -10.0);  // then shape, finest grain

        if (score > bestScore) {
            bestScore        = score;
            best.imageId     = c.imageId;
            // isLandscape drives the download preset: only an image that
            // crops cleanly may be hard-cropped to 16:9, everything else is
            // letterboxed — see downloadCover().
            best.isLandscape = cropsCleanly;
            best.fills720p   = fills720;
        }
    }
    return best;
}

// Back-compat helper for the call sites that only need the id.
static QString preferredGameImageId(const QJsonObject& obj)
{
    return preferredGameImage(obj).imageId;
}

static bool hasAllowedPlatform(const QJsonObject& obj)
{
    // v3.3.25 fix — precise allowlist per explicit clarification. This
    // also helps filter out fan-made/homebrew entries, which often list
    // no platform at all or an unofficial one (e.g. "Web browser") rather
    // than an actual console/PC release — though this is a partial
    // measure, not a guarantee, since IGDB doesn't have a dedicated
    // "official vs fan-made" flag to check directly.
    QJsonArray platforms = obj.value("platforms").toArray();
    if (platforms.isEmpty()) return false;
    for (const QJsonValue& p : platforms) {
        QString name = p.toObject().value("name").toString().toLower();

        if (name.contains("pc")) return true;
        if (name.contains("xbox")) return true;   // no PlayStation-style spin-off Xbox devices exist

        if (name.contains("playstation")) {
            // Mainline PlayStation only — exclude spin-off devices
            // (VR/VR2, Portable, Vita) that don't typically get mainline
            // releases.
            if (name.contains("vr") || name.contains("portable") || name.contains("vita"))
                continue;   // this platform entry doesn't count; check the next one
            return true;
        }

        // Nintendo — explicit allowlist of the current/modern consoles
        // only (Switch, Switch 2, Wii, Wii U), excluding older/handheld
        // devices (3DS, DS, Game Boy family, GameCube, N64, etc.).
        if (name.contains("nintendo switch")) return true;
        if (name.contains("wii")) return true;   // covers both "Wii" and "Wii U"
    }
    return false;
}

QList<SearchResult> IgdbScraper::filterAndSortResults(const QJsonArray& arr, const QString& query, int yearFilter)
{
    // v3.3.27 — full rewrite per explicit numbered spec. Series-search
    // detection (unchanged mechanism): if the query, after expanding a
    // known abbreviation like "cod" or "gta", matches a collection name
    // that actually appears among the results, this is a series search.
    // This stays general (not hardcoded to just the 5 named examples)
    // since the underlying mechanism already covers all of them and any
    // other franchise the same way.
    //
    // v3.3.32 fix — tightened from "contains anywhere" to "starts with or
    // ends with": still matches a broader franchise search against a
    // sub-series collection ("Call of Duty" is a PREFIX of "Call of
    // Duty: Modern Warfare") and a sub-series search against that same
    // collection ("Modern Warfare" is a SUFFIX of it), but no longer
    // matches a search term that just happens to appear somewhere in the
    // middle of an unrelated, longer collection name — which is likely
    // what was letting unrelated series content through.
    QSet<QString> matchingCollections;
    QString lowerQuery = expandKnownAlias(query).trimmed().toLower();
    if (!lowerQuery.isEmpty()) {
        for (const QJsonValue& v : arr) {
            for (const QString& collectionName : effectiveCollectionNames(v.toObject())) {
                QString lowerCollection = collectionName.toLower();
                if (lowerCollection.startsWith(lowerQuery) || lowerCollection.endsWith(lowerQuery))
                    matchingCollections.insert(lowerCollection);
            }
        }
    }
    bool isSeriesSearch = !matchingCollections.isEmpty();

    // Derivative content (remasters, ports, expansions, etc.) can rely on
    // parent_game to establish its place in a series rather than
    // carrying its own collection tag(s) — this maps id -> collection
    // names so a parent's collection(s) can be looked up as a fallback.
    QHash<int, QStringList> idToCollections;
    for (const QJsonValue& v : arr) {
        QJsonObject obj = v.toObject();
        QStringList names = effectiveCollectionNames(obj);
        if (!names.isEmpty())
            idToCollections[obj.value("id").toInt()] = names;
    }

    // v3.3.28 — global exclusions, applied unconditionally to every
    // result regardless of series-search context (explicitly including
    // within a matching series/sub-series, per request). Category is the
    // primary signal; name-based checks are a backup for entries IGDB's
    // own category tagging might miss. "mod" specifically uses a word-
    // boundary regex rather than a plain substring check, since a plain
    // "contains" would false-positive on words like "Modern" (as in
    // "Modern Warfare").
    static const QRegularExpression kSeasonNumber(
        "\\bseason\\s*\\d+\\b", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kModWord(
        "\\bmods?\\b", QRegularExpression::CaseInsensitiveOption);

    auto buildList = [&]() {
        QHash<int, QJsonObject> cache;
        QList<SearchResult> out;
        QSet<int> seenIds;   // de-dupe, results can arrive from multiple merged queries
        QSet<QString> seenNameYear;   // v3.3.32 — catches the same game duplicated under two different ids (a known IGDB data-quality issue), which id-based dedup alone can't catch
        for (const QJsonValue& v : arr) {
            QJsonObject obj = v.toObject();
            QString name = obj.value("name").toString();
            qint64 ts = (qint64)obj.value("first_release_date").toDouble();
            int id = obj.value("id").toInt();
            int category = effectiveCategory(obj);
            QStringList collectionNames = effectiveCollectionNames(obj);
            int year = ts > 0 ? QDateTime::fromSecsSinceEpoch(ts, Qt::UTC).date().year() : 0;
            QString nameYearKey = name.trimmed().toLower() + "|" + QString::number(year);

            if (seenIds.contains(id))
                continue;   // already included from another merged query
            if (seenNameYear.contains(nameYearKey))
                continue;   // v3.3.32 — same title+year already included under a different id
            if (ts <= 0)
                continue;   // no release date

            // v3.3.33 — explicit safety net for two specific spin-offs
            // named directly, not part of the real series despite
            // sharing the franchise name — in case either carries its
            // own direct collection tag rather than only inheriting one
            // through a parent link (which the fix above addresses).
            // v3.3.34 — "Minecraft 4k" added for the same reason, but a
            // different underlying cause: this is very likely tagged with
            // its own DIRECT "Minecraft" collection in IGDB's community
            // data (as a fan tribute someone associated with Minecraft),
            // not inherited through a parent link — so last round's fix
            // (which only affects inheritance) doesn't apply to it. The
            // structural fix itself is fully franchise-agnostic; this
            // specific title just isn't the kind of problem it addresses.
            {
                QString lowerName2 = name.toLower();
                if (lowerName2 == "call of duty: heroes" || lowerName2 == "call of duty: online" ||
                    lowerName2 == "minecraft 4k")
                    continue;
            }

            // v3.3.46 fix #3 — IGDB's base "Minecraft" entry (no edition
            // suffix) is renamed to "Minecraft: Bedrock Edition" for
            // clarity, since people search a bare "Minecraft" without
            // realizing IGDB's is the actively-developed Bedrock
            // codebase specifically, not a generic catch-all. Applied
            // this early so the renamed form is what flows through to
            // the rest of this filter (e.g. the Edition-exclusion check
            // right below, which already exempts "bedrock edition"), the
            // displayed search result, and the tile saved if it's picked.
            if (name.trimmed().toLower() == "minecraft")
                name = "Minecraft: Bedrock Edition";

            // v3.3.46 fix #2 — an explicit whitelist of legitimate
            // Minecraft console ports that would otherwise be caught by
            // the general-purpose Edition-name exclusion below (since
            // their titles end in "Edition" but aren't a premium upsell
            // variant) or the platform filter further down (Vita
            // specifically is excluded there as a rule, but this is a
            // real, wanted port, not a scam/unofficial one).
            static const QSet<QString> kMinecraftPortWhitelist = {
                "minecraft: xbox 360 edition", "minecraft: xbox one edition",
                "minecraft: playstation 3 edition", "minecraft: playstation 4 edition",
                "minecraft: playstation vita edition", "minecraft: wii u edition",
                "minecraft: nintendo switch edition"
            };
            bool isWhitelistedPort = kMinecraftPortWhitelist.contains(name.trimmed().toLower());

            // v3.3.28 — global, unconditional exclusions (applies even
            // within a matching series/sub-series):
            // Seasons.
            if (category == 7) continue;
            if (kSeasonNumber.match(name).hasMatch()) continue;
            // DLC.
            if (category == 1) continue;
            if (name.toLower().contains("dlc")) continue;
            // Episodes.
            if (category == 6) continue;
            if (name.toLower().contains("episode")) continue;
            // Bundles.
            if (category == 3) continue;
            if (name.toLower().contains("bundle")) continue;
            // Mods.
            if (category == 5) continue;
            if (kModWord.match(name).hasMatch()) continue;
            // Packs.
            if (category == 13) continue;
            if (name.toLower().contains("pack")) continue;
            // Updates.
            if (category == 14) continue;
            if (name.toLower().contains("update")) continue;
            // Expansions.
            if (category == 2) continue;
            if (name.toLower().contains("expansion")) continue;
            // v3.3.33 — Standalone Expansion (category 4), a distinct
            // IGDB category from regular Expansion (2) — excluded by
            // category only, since a standalone expansion's title won't
            // necessarily contain the word "expansion" itself (e.g.
            // "Halo 3: ODST" is IGDB's standalone_expansion of Halo 3).
            if (category == 4) continue;
            // v3.3.29 — Editions (e.g. "Ultimate Edition," "Deluxe
            // Edition") checked by name only, regardless of category —
            // IGDB doesn't have a distinct category for this, and per the
            // reported example, an edition variant is often tagged as
            // main_game (0) just like the base game it's a variant of.
            // "Java Edition" / "Bedrock Edition" are exempted since
            // those are Minecraft's actual official release names, not a
            // premium upsell variant of a base game. v3.3.46 — the
            // explicit console-port whitelist above is exempted too.
            if (!isWhitelistedPort) {
                QString lowerName = name.toLower();
                bool isKnownLegitimateEdition =
                    lowerName.contains("java edition") || lowerName.contains("bedrock edition");
                if (!isKnownLegitimateEdition && lowerName.contains("edition"))
                    continue;
            }

            // Rules 2-6 — for a series search, ONLY results genuinely in
            // a matching series show at all; no title-match fallback.
            // For a non-series (regular) search, title-matching is the
            // only relevance check.
            if (isSeriesSearch) {
                QStringList effectiveCollections = collectionNames;
                // v3.3.33 fix — only fall back to a parent's collection
                // for categories where "this is genuinely the same game,
                // just in a different form" is a safe assumption: remake,
                // remaster, expanded_game, port. Previously this fallback
                // applied to ANY item with a parent_game link regardless
                // of category, which is what let spin-offs slip in — a
                // spin-off (typically tagged as a plain main_game) can
                // reference a flagship title via parent_game for reasons
                // that have nothing to do with sharing its series (e.g.
                // "loosely based on" or "themed after"), and inheriting
                // that title's collection through the link incorrectly
                // made it look like part of the series.
                static const QSet<int> kTrustedForParentCollectionFallback = {8, 9, 10, 11};
                if (effectiveCollections.isEmpty() &&
                    kTrustedForParentCollectionFallback.contains(category) &&
                    obj.contains("parent_game") && !obj.value("parent_game").isNull()) {
                    int parentId = obj.value("parent_game").toInt();
                    effectiveCollections = idToCollections.value(parentId);
                }
                bool collectionMatches = false;
                for (const QString& c : effectiveCollections) {
                    if (matchingCollections.contains(c.toLower())) { collectionMatches = true; break; }
                }
                if (!collectionMatches)
                    continue;
            } else {
                if (!titleMatchesQuery(name, query))
                    continue;
            }

            if (yearFilter > 0 && year != yearFilter)
                continue;

            // Rule 1 — platform filtering, applied globally. v3.3.46 —
            // except for the explicit Minecraft port whitelist above,
            // which is what actually lets "PlayStation Vita Edition"
            // through despite Vita normally being excluded as a rule.
            if (!isWhitelistedPort && !hasAllowedPlatform(obj))
                continue;

            cache[id] = obj;
            seenIds.insert(id);
            seenNameYear.insert(nameYearKey);

            SearchResult sr;
            sr.id = id;
            sr.mediaType = "game";
            sr.title = name;
            sr.year = year;
            sr.posterPath  = preferredGameImageId(obj);
            sr.popularity  = obj.value("total_rating").toDouble();
            out.append(sr);
        }
        // Rule 7 — ordered by date. Newest first (existing behavior,
        // unchanged this round).
        std::stable_sort(out.begin(), out.end(),
                          [](const SearchResult& a, const SearchResult& b) {
                              return a.year > b.year;
                          });
        m_cachedResults = cache;
        return out;
    };

    // Rule 1 — platform filtering is global, with no fallback/exception:
    // if a game doesn't have an allowed platform, it's excluded, full
    // stop, even if that leaves a search with no results at all.
    return buildList();
}


// =============================================================================
void IgdbScraper::fetchDetails(int gameId, const QString& mediaType, const QString& posterPath)
{
    Q_UNUSED(mediaType);
    Q_UNUSED(posterPath);
    if (!m_cachedResults.contains(gameId)) {
        emit scraperError("Game details not found — try searching again.");
        return;
    }
    QJsonObject obj = m_cachedResults.value(gameId);
    TileData td = buildTileData(obj);
    emit dataReady(td);

    GameImagePick pick = preferredGameImage(obj);
    if (!pick.imageId.isEmpty())
        downloadCover(td.id, pick.imageId, /*makeActive=*/true,
                      pick.isLandscape, pick.fills720p);
}

// =============================================================================
TileData IgdbScraper::buildTileData(const QJsonObject& obj, const QString& existingId,
                                     const QString& existingImage)
{
    TileData td;
    td.id = existingId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : existingId;
    td.tmdbId    = obj.value("id").toInt();   // reusing this generic "external id" field
    td.mediaType = "game";
    td.imagePath = existingImage;

    QString rawTitle = obj.value("name").toString();
    // v3.3.46 — same rename as in the search-result filter loop, applied
    // here too since this function (not the search result list) is what
    // actually determines the tile's saved title — both when a result is
    // first selected and on every later refresh.
    if (rawTitle.trimmed().toLower() == "minecraft")
        rawTitle = "Minecraft: Bedrock Edition";
    td.title = rawTitle;

    qint64 ts = (qint64)obj.value("first_release_date").toDouble();
    if (ts > 0) {
        td.targetDate  = QDateTime::fromSecsSinceEpoch(ts, Qt::UTC).date();
        td.releaseYear = td.targetDate.year();
        td.dateDisplay = td.targetDate.toString("MMMM d, yyyy");
        td.statusLabel = (td.targetDate > QDate::currentDate()) ? "Releases" : "Released";
    } else {
        td.statusLabel = "No Release Date Yet";
        td.dateDisplay = "No Release Date Yet";
    }
    return td;
}

// =============================================================================
//  normaliseTo16x9 — V5.4. Return the image as exactly 1280x720, fitted
//  rather than cropped, with any leftover space filled black.
//
//  This is the "make it fit the tile" half of the artwork rework. A cover or
//  a logo is never going to be 16:9, and the two ways to force it there are
//  to cut pieces off or to leave space; leaving space is the one that still
//  shows the whole image. Whichever dimension runs out first sets the scale,
//  so the bars land on the top and bottom for a wide source and on the left
//  and right for a tall one.
//
//  A source already at 16:9 comes back untouched apart from the scale, since
//  the fitted size then fills the canvas exactly and no bars are drawn.
// =============================================================================
static QImage normaliseTo16x9(const QImage& src)
{
    static constexpr int kW = 1280;
    static constexpr int kH = 720;

    if (src.isNull()) return src;
    if (src.width() == kW && src.height() == kH) return src;

    QImage fitted = src.scaled(kW, kH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (fitted.width() == kW && fitted.height() == kH)
        return fitted;   // already 16:9 — nothing to pad

    QImage canvas(kW, kH, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter p(&canvas);
    p.drawImage((kW - fitted.width())  / 2,
                (kH - fitted.height()) / 2, fitted);
    p.end();
    return canvas;
}

// =============================================================================
void IgdbScraper::downloadCover(const QString& tileId, const QString& imageId,
                                 bool makeActive, bool isLandscape, bool fills720p)
{
    // V5 — "screenshot_huge" (always exactly 1280x720) rather than
    // "1080p". IGDB's t_720p/t_1080p presets PRESERVE the source aspect
    // ratio rather than producing those dimensions, so what came back
    // varied wildly per game — measured 1080x1080 for Grand Theft Auto VI
    // and 1920x620 for Elden Ring, neither of which is the 16:9 the tile
    // draws. screenshot_huge is the only preset guaranteed to match the
    // tile's shape, it's 720p to match the TMDB backdrops exactly, and it
    // averages smaller on disk than 1080p did.
    //
    // V5.4 — three presets now, and every path ends at exactly 1280x720.
    // Measured against the live CDN rather than assumed:
    //
    //   t_screenshot_huge  crops-to-FILL an exact 1280x720
    //   t_720p             FITS inside 1280x720, aspect preserved
    //                      (3840x2160 -> 1280x720, 1581x2108 -> 540x720)
    //   t_1080p            FITS inside 1920x1080, and DOES upscale a small
    //                      source (1280x256 came back as 1920x384)
    //
    //  • Crops cleanly (isLandscape) -> t_screenshot_huge. A near-16:9 image
    //    loses almost nothing to that crop, and for a true 16:9 source the
    //    two presets return byte-identical files. This case is checked FIRST
    //    because it already lands on an exact 1280x720 whatever the source
    //    resolution was — routing it anywhere else would letterbox an image
    //    that should have filled the tile.
    //  • Anything else (a cover, a logo, square key art) -> t_720p, then
    //    letterboxed onto a 1280x720 canvas below. It must NOT go through
    //    screenshot_huge: cropping box art to 16:9 keeps a thin band from
    //    the middle, which is what made some game tiles look zoomed in.
    //  • …and if such a source is below 720p -> t_1080p, resampled down
    //    locally. IGDB's own upscale is a straight enlargement; rendering
    //    larger and downsampling with a smooth filter keeps more of what
    //    detail there is.
    const char* preset = isLandscape ? "t_screenshot_huge"
                       : fills720p   ? "t_720p"
                                     : "t_1080p";
    QString url = QString("%1/%2/%3.jpg").arg(IMAGE_BASE, preset, imageId);
    // V5.4.25 — the same shared queue the TMDB backdrops use, so the ceiling
    // is on the app's image traffic as a whole rather than per provider.
    ImageFetchQueue::instance().fetch(QUrl(url),
        [this, tileId, makeActive](const QByteArray& bytes, bool ok) {
        if (!ok) return;

        // Look at the image before trusting it. These bytes used to go
        // straight to disk, so anything the server returned became the tile —
        // including a JPEG that decodes perfectly but is a single flat colour.
        // That is what a blank tile looks like: cropping an ultra-wide banner
        // to 16:9 can keep nothing but empty background. The scoring above now
        // avoids picking such a source, but a bad image should never be able to
        // replace a good one on disk regardless of how it was chosen.
        QImage img;
        if (!img.loadFromData(bytes)) return;

        // Downscale first so this stays a few hundred pixel reads rather than a
        // million: a genuinely flat image is still flat at thumbnail size.
        QImage probe = img.scaled(8, 8, Qt::IgnoreAspectRatio,
                                  Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_RGB32);
        bool flat = true;
        QRgb first = probe.pixel(0, 0);
        for (int y = 0; y < probe.height() && flat; ++y)
            for (int x = 0; x < probe.width(); ++x)
                if (probe.pixel(x, y) != first) { flat = false; break; }
        if (flat) return;

        // V5.4 — normalise to exactly 1280x720 before saving.
        //
        // The tile paints its image with KeepAspectRatioByExpanding, i.e.
        // crop-to-FILL, so anything that isn't already 16:9 gets its sides
        // (or its top and bottom) cut off at display time — a portrait cover
        // arrived as 540x720 and the tile showed a narrow strip of its
        // middle. Fitting the image inside a 16:9 canvas here means the tile
        // is handed something that already matches its own shape and crops
        // to nothing.
        //
        // The empty space is black because the tile's own background is
        // black (TileWidget sets background:#000 on both the image label and
        // its container), so the bars read as part of the tile rather than
        // as a border around the picture.
        QImage out = normaliseTo16x9(img);

        QString dir = QStandardPaths::writableLocation(
                          QStandardPaths::AppDataLocation) + "/fetched_images";
        QDir().mkpath(dir);
        QString localPath = dir + "/" + tileId + ".jpg";
        if (out.save(localPath, "JPEG", 88))
            emit posterReady(tileId, localPath, makeActive);
    });
}

// =============================================================================
void IgdbScraper::refreshTile(const TileData& existing, bool forceCoverRefetch)
{
    if (existing.tmdbId <= 0) return;

    withAccessToken([this, existing, forceCoverRefetch](const QString& token) {
        QString body = QString(
            "fields name,first_release_date,cover.image_id,cover.width,cover.height,"
            "artworks.image_id,artworks.width,artworks.height,artworks.image_type.id,"
            "screenshots.image_id,screenshots.width,screenshots.height;\n"
            "where id = %1;\n"
        ).arg(existing.tmdbId);

        QNetworkReply* r = postToIgdb("games", body.toUtf8(), token);
        connect(r, &QNetworkReply::finished, this, [this, r, existing, forceCoverRefetch]() {
            r->deleteLater();
            // v3.3.37 fix — both of these previously returned with no
            // signal at all, meaning the caller's pending-refresh counter
            // never got decremented for this tile. Emitting the
            // unchanged data is a safe no-op for tile state, and ensures
            // a batch of refreshes can always tell when it's truly
            // finished, even if one fails or the game is no longer found.
            if (r->error() != QNetworkReply::NoError) {
                emit tileRefreshed(existing);
                return;
            }
            QJsonArray arr = QJsonDocument::fromJson(r->readAll()).array();
            if (arr.isEmpty()) {
                emit tileRefreshed(existing);
                return;
            }
            QJsonObject obj = arr[0].toObject();

            TileData updated = buildTileData(obj, existing.id, existing.imagePath);
            // Preserve everything that isn't re-derived from the API.
            updated.customTitle       = existing.customTitle;
            updated.customDateStr     = existing.customDateStr;
            updated.customDate        = existing.customDate;
            updated.airTime           = existing.airTime;
            updated.customAirTime     = existing.customAirTime;
            // V5.4 — airTime is carried across a refresh here (IGDB publishes
            // no time of day), so whatever a relay correction baked into it
            // is carried too. The bookkeeping that explains that value has to
            // come along with it, or the correction becomes irreversible and
            // deleting it on the dashboard would never revert a game tile.
            updated.overrideKey         = existing.overrideKey;
            updated.overrideBaseAirTime = existing.overrideBaseAirTime;
            updated.overrideDayShift    = existing.overrideDayShift;
            updated.fetchedImagePath  = existing.fetchedImagePath;
            updated.customImagePaths  = existing.customImagePaths;
            updated.notified          = existing.notified;
            updated.notifStatus       = existing.notifStatus;
            updated.isLooped          = existing.isLooped;
            updated.noDateOverride    = existing.noDateOverride;
            updated.presetType        = existing.presetType;
            updated.loopInterval      = existing.loopInterval;
            updated.loopWeekday       = existing.loopWeekday;
            updated.loopDayOfMonth    = existing.loopDayOfMonth;
            emit tileRefreshed(updated);

            GameImagePick pick = preferredGameImage(obj);
            bool needsCover = forceCoverRefetch || existing.fetchedImagePath.isEmpty();
            if (!pick.imageId.isEmpty() && needsCover)
                downloadCover(existing.id, pick.imageId,
                              /*makeActive=*/existing.fetchedImagePath.isEmpty(),
                              pick.isLandscape, pick.fills720p);
        });
    });
}
