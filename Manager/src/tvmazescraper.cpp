#include "tvmazescraper.h"
#include "applogger.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QStandardPaths>
#include <QTextDocument>
#include <QDateTime>
#include <QTimeZone>
#include <algorithm>
#include "timezoneutil.h"

// Wikimedia-style UA policy applies to TVmaze too — identify the app and a
// contact so a misbehaving build can be reached rather than blanket-blocked.
static const char* kUserAgent =
    "MediaCountdowns/5.0 (+https://github.com/HijackAssassin/MediaCountdowns)";

// Backdrops are capped at 720p. TVmaze serves episode stills at up to 4K,
// which is far more than a tile ever displays and would bloat the image
// cache for no visible gain.
static constexpr int kBackdropWidth = 1280;

// One API request every 550ms — comfortably inside TVmaze's ~20-per-10s
// allowance even with the app's own image downloads running alongside.
static constexpr int kRequestIntervalMs = 550;
static constexpr int kMaxAttempts       = 2;

TvmazeScraper::TvmazeScraper(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_pump(new QTimer(this))
{
    m_pump->setInterval(kRequestIntervalMs);
    connect(m_pump, &QTimer::timeout, this, &TvmazeScraper::pumpQueue);
}

void TvmazeScraper::enqueue(const QString& url,
                             std::function<void(const QByteArray&, bool)> cb)
{
    m_queue.enqueue(PendingRequest{url, std::move(cb), 0});
    if (!m_pump->isActive() && !m_inFlight) {
        pumpQueue();            // send the first one immediately
        m_pump->start();
    }
}

void TvmazeScraper::pumpQueue()
{
    if (m_inFlight) return;
    if (m_queue.isEmpty()) { m_pump->stop(); return; }

    PendingRequest req = m_queue.dequeue();
    ++req.attempts;
    m_inFlight = true;

    QNetworkRequest nreq{QUrl(req.url)};
    nreq.setRawHeader("User-Agent", kUserAgent);
    nreq.setRawHeader("Accept", "application/json");

    QNetworkReply* r = m_nam->get(nreq);
    connect(r, &QNetworkReply::finished, this, [r, req, this]() mutable {
        r->deleteLater();
        m_inFlight = false;

        int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool ok  = (r->error() == QNetworkReply::NoError) && (http == 200);

        // A dropped or throttled request gets one more go at the back of the
        // queue, by which point the burst that caused it has drained.
        if (!ok && req.attempts < kMaxAttempts) {
            m_queue.enqueue(req);
            if (!m_pump->isActive()) m_pump->start();
            return;
        }
        if (req.cb) req.cb(ok ? r->readAll() : QByteArray(), ok);
    });
}

// Strip the HTML TVmaze wraps summaries in ("<p><b>Show</b> is…</p>").
static QString plainText(const QString& html)
{
    if (html.isEmpty()) return {};
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText().simplified();
}

static int yearOf(const QString& isoDate)
{
    return QDate::fromString(isoDate, Qt::ISODate).year();
}

static SearchResult toSearchResult(const QJsonObject& show)
{
    SearchResult sr;
    sr.id        = show["id"].toInt();
    sr.mediaType = "tv";
    sr.source    = "tvmaze";
    sr.title     = show["name"].toString();
    sr.year      = yearOf(show["premiered"].toString());
    sr.posterPath = show["image"].toObject()["medium"].toString();

    // The network/web channel is the most useful one-line disambiguator when
    // several shows share a name (e.g. a remake on a different service).
    QJsonObject net = show["network"].toObject();
    if (net.isEmpty()) net = show["webChannel"].toObject();
    sr.castLine = net["name"].toString();
    QJsonObject country = net["country"].toObject();
    sr.isUS = (country["code"].toString() == "US");

    // TVmaze exposes a 0..100-ish weight; reuse the popularity slot so the
    // existing picker ordering keeps working unchanged.
    sr.popularity = show["weight"].toDouble();
    return sr;
}

// =============================================================================
//  searchMedia — GET /search/shows?q=
// =============================================================================
void TvmazeScraper::searchMedia(const QString& query)
{
    QString url = QString("%1/search/shows?q=%2")
        .arg(BASE, QString::fromUtf8(QUrl::toPercentEncoding(query)));

    enqueue(url, [this](const QByteArray& body, bool ok) {
        if (!ok) {
            emit scraperError("TVmaze search failed");
            return;
        }
        QList<SearchResult> out;
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            QJsonObject hit = v.toObject();
            SearchResult sr = toSearchResult(hit["show"].toObject());
            // TVmaze's own relevance score is a better sort key than weight
            // for a typed query; fold it in so exact-ish titles come first.
            sr.popularity = hit["score"].toDouble() * 1000.0 + sr.popularity;
            if (sr.id > 0) out.append(sr);
        }
        std::sort(out.begin(), out.end(), [](const SearchResult& a, const SearchResult& b) {
            return a.popularity > b.popularity;
        });
        emit searchResultsReady(out);
    });
}

// =============================================================================
//  applyEpisodes — the heart of this class.
//
//  Picks the next upcoming episode out of the season schedule TVmaze already
//  gave us, so there is nothing to estimate: if the season is dated, the real
//  date is used. Falls back to the most recently aired episode when the show
//  has nothing upcoming, which is what leaves a tile correctly in Released.
// =============================================================================
void TvmazeScraper::applyEpisodes(TileData& td, const QJsonArray& episodes,
                                   QString* episodeImageOut, bool* hasUpcomingOut)
{
    const QDate today = QDate::currentDate();

    // The user's configured zone (Settings → Time Zone). airstamp is an
    // absolute instant, so converting it here gives a genuinely correct local
    // date AND time — replacing the "assume midnight Pacific" approximation
    // that TimeZoneUtil has to fall back on for TMDB tiles.
    QTimeZone zone(TimeZoneUtil::currentZoneId().toUtf8());

    // stampUtc is the absolute instant, kept so a later Time Zone change can be
    // applied exactly and locally rather than needing a refetch. See
    // TileData::airStampUtc.
    struct Ep { int season; int number; QDate date; QTime time; QString image; QDateTime stampUtc; };
    QList<Ep> parsed;
    for (const QJsonValue& v : episodes) {
        QJsonObject e = v.toObject();
        Ep ep;
        ep.season = e["season"].toInt();
        ep.number = e["number"].toInt();
        ep.image  = e["image"].toObject()["original"].toString();

        // Only a POPULATED airtime means TVmaze actually knows when this
        // airs. Verified against the live API: linear broadcasters carry a
        // real slot (Adult Swim "00:00", HBO "21:00") and their airstamp
        // matches it, while every streaming service (Disney+, Prime Video,
        // Netflix) leaves airtime empty and gets a filler airstamp of
        // exactly 12:00 UTC — which would land as a bogus 08:00 ET and is
        // nothing like the real ~3 AM ET drop. So an empty airtime is
        // treated as "no time known", leaving airTime invalid so the app's
        // existing TimeZoneUtil default applies exactly as it does today.
        //
        // The distinction matters for the DATE too, not just the time:
        // a late-night broadcast is filed under the previous evening's
        // airdate (the "TV night"), so Adult Swim's 2026-08-16T04:00Z
        // carries airdate 2026-08-15. Trusting airdate there would put the
        // countdown a full day early. Streamers don't have that problem —
        // the noon-UTC filler stays on the same calendar day as airdate.
        bool hasRealAirTime = !e["airtime"].toString().trimmed().isEmpty();
        QDateTime stamp = QDateTime::fromString(e["airstamp"].toString(), Qt::ISODate);

        if (hasRealAirTime && stamp.isValid()) {
            QDateTime local = zone.isValid() ? stamp.toTimeZone(zone) : stamp.toLocalTime();
            ep.date = local.date();
            ep.time = local.time();          // genuine broadcast slot
            ep.stampUtc = stamp.toUTC();     // the instant behind that slot
        } else {
            ep.date = QDate::fromString(e["airdate"].toString(), Qt::ISODate);
            ep.time = QTime();               // invalid → TimeZoneUtil default
        }
        // number is null for specials — they aren't part of a season's
        // numbering and would corrupt both the label and the episode count.
        if (ep.season > 0 && ep.number > 0) parsed.append(ep);
    }
    if (hasUpcomingOut) *hasUpcomingOut = false;
    if (parsed.isEmpty()) {
        td.dateDisplay = "No Release Date Yet";
        return;
    }

    // Next upcoming episode, else the last one that aired.
    //
    // Compared by air INSTANT, not just date: an episode that dropped
    // earlier today has already aired, and "date >= today" would keep
    // selecting it for the rest of the day. That left the tile expiring
    // immediately and falling back to a local +7 guess — throwing away the
    // real next date TVmaze had already handed us. Mirrors TileData's own
    // isExpired() rule so the two agree: a real broadcast slot when TVmaze
    // published one, otherwise the app's Time Zone default.
    const QDateTime now = QDateTime::currentDateTime();
    auto airMoment = [&](const Ep& e) {
        QTime t = e.time.isValid() ? e.time : TimeZoneUtil::defaultDigitalTime();
        return QDateTime(e.date, t);
    };

    const Ep* chosen = nullptr;
    for (const Ep& e : parsed)
        if (e.date.isValid() && airMoment(e) > now
            && (!chosen || e.date < chosen->date))
            chosen = &e;

    bool upcoming = (chosen != nullptr);
    if (hasUpcomingOut) *hasUpcomingOut = upcoming;
    if (!chosen) {
        for (const Ep& e : parsed)
            if (e.date.isValid() && (!chosen || e.date > chosen->date))
                chosen = &e;
    }
    if (!chosen) {
        td.dateDisplay = "No Release Date Yet";
        return;
    }

    // Multi-episode drop: every episode sharing the chosen date, which is how
    // premieres and batch releases get labelled "S04E01+E02+E03".
    QList<int> sameDay;
    for (const Ep& e : parsed)
        if (e.season == chosen->season && e.date == chosen->date)
            sameDay.append(e.number);
    std::sort(sameDay.begin(), sameDay.end());

    QString label = QString("S%1E%2")
        .arg(chosen->season, 2, 10, QChar('0'))
        .arg(sameDay.isEmpty() ? chosen->number : sameDay.first(), 2, 10, QChar('0'));
    for (int i = 1; i < sameDay.size(); ++i)
        label += QString("+E%1").arg(sameDay[i], 2, 10, QChar('0'));

    int seasonTotal = 0;
    for (const Ep& e : parsed)
        if (e.season == chosen->season) ++seasonTotal;

    td.targetDate         = chosen->date;
    // V5.4 — these are the source's own values, which replace anything a
    // relay correction had baked in. They become the new base a correction
    // is measured against, so the old bookkeeping must not survive: undoing
    // against it later would restore a stale time and un-shift a date that
    // was never shifted.
    td.clearOverrideBase();
    // V5 — when TVmaze publishes a real air time, use it. Full stop.
    //
    // This was briefly a Settings preference (streaming vs broadcast) on
    // the theory that a cable slot differs from when the same episode hits
    // the streaming app. In practice that distinction doesn't pay for
    // itself: HBO Max simulcasts with the linear broadcast, so Lanterns
    // genuinely is 9 PM Eastern on both — and every streaming-only service
    // (Netflix, Disney+, Prime Video) publishes NO airtime at all, so those
    // fall through to the default regardless. A published time is therefore
    // strictly better information than the default, and the setting only
    // ever hid it.
    //
    // chosen->time is invalid whenever airtime was empty, which is exactly
    // the streaming case — effectiveTime() then uses the app's Time Zone
    // default (midnight Pacific / 3 AM Eastern) as it always has.
    td.airTime            = chosen->time;
    td.airStampUtc        = chosen->stampUtc;   // invalid when TVmaze published no time
    td.dateDisplay        = chosen->date.toString("MMMM d, yyyy");
    td.statusLabel        = label;
    td.seasonEpisodeCount = seasonTotal;
    td.rawDateText        = QString("%1|%2|%3")
        .arg(chosen->date.toString(Qt::ISODate)).arg(chosen->season).arg(chosen->number);

    // A real, published date — never an estimate, so all the guessing state
    // clears out. This is the entire point of the source swap.
    td.isEstimatedDate  = false;
    td.inMidSeasonBreak = false;
    td.unverifiedSince  = QDate();
    if (upcoming) {
        td.lastVerifiedDate        = chosen->date;
        td.lastVerifiedStatusLabel = label;
    }

    if (episodeImageOut) *episodeImageOut = chosen->image;
}

// =============================================================================
//  downloadEpisodeImage — fetch, downscale to 720p, save as JPEG.
// =============================================================================
void TvmazeScraper::downloadEpisodeImage(const QString& tileId, const QString& imageUrl,
                                          bool makeActive)
{
    if (imageUrl.isEmpty()) return;
    QNetworkRequest req{QUrl(imageUrl)};
    req.setRawHeader("User-Agent", kUserAgent);
    QNetworkReply* r = m_nam->get(req);

    connect(r, &QNetworkReply::finished, this, [r, tileId, makeActive, this]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) return;

        QImage img;
        if (!img.loadFromData(r->readAll())) return;
        // TVmaze stills are 4K; a tile never shows anything near that.
        if (img.width() > kBackdropWidth)
            img = img.scaledToWidth(kBackdropWidth, Qt::SmoothTransformation);

        QString dir = QStandardPaths::writableLocation(
                          QStandardPaths::AppDataLocation) + "/fetched_images";
        QDir().mkpath(dir);
        QString localPath = dir + "/" + tileId + ".jpg";
        if (img.save(localPath, "JPEG", 88))
            emit posterReady(tileId, localPath, makeActive);
    });
}

// =============================================================================
//  handleShowPayload — shared by fetchDetails and refreshTile.
// =============================================================================
void TvmazeScraper::handleShowPayload(const QJsonObject& show, TileData td,
                                       bool isRefresh, bool forceBackdropRefetch)
{
    td.tvmazeId    = show["id"].toInt();
    td.mediaType   = "tv";
    td.title       = show["name"].toString();
    td.releaseYear = yearOf(show["premiered"].toString());
    if (td.id.isEmpty()) td.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QString episodeImage;
    bool hasUpcoming = false;
    applyEpisodes(td, show["_embedded"].toObject()["episodes"].toArray(),
                  &episodeImage, &hasUpcoming);
    td.tvmazeUrl = show["url"].toString();   // its own field — never tmdbUrl

    // V5 — TVmaze no longer supplies the tile's picture. Its episode stills
    // are genuinely 16:9 and were a reasonable stand-in while the app had no
    // API keys, but TMDB's curated backdrops are better artwork, and with
    // the app free (non-commercial) TMDB is available again. So this class
    // is now purely a DATES source: MainWindow asks TmdbScraper for the
    // backdrop separately (see fetchBackdropOnly), which also keeps a
    // TVmaze refresh from ever overwriting TMDB-sourced imagery.
    Q_UNUSED(forceBackdropRefetch);
    Q_UNUSED(episodeImage);

    // V5 — nothing upcoming means the show is between seasons, which is
    // exactly when an announced month is worth chasing. See the header for
    // why this needs the HTML page rather than the API.
    if (!hasUpcoming && !td.tvmazeUrl.isEmpty()) {
        fetchReturningWindow(td, isRefresh);
        return;                      // that path emits instead
    }

    if (isRefresh) emit tileRefreshed(td);
    else           emit dataReady(td);
}

// =============================================================================
//  fetchReturningWindow — see the header.
// =============================================================================
void TvmazeScraper::fetchReturningWindow(TileData td, bool isRefresh)
{
    QNetworkRequest req{QUrl(td.tvmazeUrl)};
    req.setRawHeader("User-Agent", kUserAgent);
    QNetworkReply* r = m_nam->get(req);

    connect(r, &QNetworkReply::finished, this, [r, td, isRefresh, this]() mutable {
        r->deleteLater();
        auto giveUp = [&]{
            if (isRefresh) emit tileRefreshed(td); else emit dataReady(td);
        };
        if (r->error() != QNetworkReply::NoError) { giveUp(); return; }

        const QString html = QString::fromUtf8(r->readAll());
        // The page's Status line is markup, and measured it looks exactly
        // like this (Dexter: Resurrection, verified against the live page):
        //
        //   <strong>Status:</strong> Running; returning
        //   <a href="/seasons/190013/dexter-resurrection-season-2">2026</a>
        //
        // Matching the LINK and reading its text is deliberately tighter
        // than scanning for a loose "returning <something> <year>": the word
        // "returning" can appear in a show's summary too, and a prose
        // sentence must never be mistaken for an announcement. The second
        // pattern keeps the unlinked wording working as it always did.
        static const QRegularExpression reLinked(
            "returning\\s*<a\\b[^>]*>\\s*([^<]{1,32}?)\\s*</a>");
        static const QRegularExpression reBare(
            "returning\\s+((?:[A-Z][a-z]+\\s+)?\\d{4})\\b");

        // Every candidate is tried, not just the first: a page with an
        // unrelated "returning …" link earlier in the summary would
        // otherwise consume the match and hide the real Status line.
        QStringList candidates;
        for (auto re : { &reLinked, &reBare }) {
            QRegularExpressionMatchIterator it = re->globalMatch(html);
            while (it.hasNext()) candidates << it.next().captured(1).trimmed();
        }

        // A usable candidate is either "March 2027" or a bare "2026".
        static const QRegularExpression reWindow(
            "^(?:([A-Za-z]+)\\s+)?(\\d{4})$");
        QRegularExpressionMatch w;
        for (const QString& c : candidates) {
            w = reWindow.match(c);
            if (w.hasMatch()) break;
        }
        if (!w.hasMatch()) { giveUp(); return; }

        static const QStringList kMonths = {
            "January","February","March","April","May","June",
            "July","August","September","October","November","December" };
        // -1 (not a month name) covers two cases that mean the same thing:
        // no leading word at all ("2026"), and a word that isn't a month
        // ("Fall 2026"). Neither narrows the window below the year, so both
        // take the year branch rather than being discarded — a year is still
        // real information, and discarding it is what left Dexter:
        // Resurrection showing "No Release Date Yet".
        int month = kMonths.indexOf(w.captured(1));
        int year  = w.captured(2).toInt();
        if (year < 1900) { giveUp(); return; }

        if (month >= 0) {
            QDate firstOfMonth(year, month + 1, 1);
            if (!firstOfMonth.isValid()) { giveUp(); return; }
            // LAST day of the month, so the countdown is the maximum possible
            // wait and can't hit zero before the show could air.
            td.targetDate  = QDate(year, month + 1, firstOfMonth.daysInMonth());
            td.dateDisplay = firstOfMonth.toString("MMMM yyyy");   // no day shown
            td.isMonthOnlyDate = true;
            td.isYearOnlyDate  = false;
        } else {
            // Year only — same maximum-countdown rule one step coarser:
            // December 31 is the last day the show could still make the year
            // it was announced for.
            QDate lastOfYear(year, 12, 31);
            if (!lastOfYear.isValid()) { giveUp(); return; }
            td.targetDate  = lastOfYear;
            td.dateDisplay = QString::number(year);   // the tile shows just "2026"
            td.isMonthOnlyDate = false;
            td.isYearOnlyDate  = true;
        }

        // A window carries no time of day. Any air time left over from the
        // previous season would otherwise be printed against Dec 31, which
        // states something the source never did.
        td.airTime = QTime();
        td.clearOverrideBase();   // see applyEpisodes — this is a fresh base
        td.isEstimatedDate = false;    // not a guess — an announced window
        td.inMidSeasonBreak = false;
        td.unverifiedSince = QDate();

        // V5.4.4 — a custom date belonging to the season that has already
        // finished must not survive the announcement of the next one.
        //
        // The label below moves to the new season, but effectiveDate() prefers
        // customDate over targetDate, so a leftover custom date kept the tile
        // showing the OLD season's day beside the NEW season's label. That is
        // how Invincible ended up reading "S05E01 · April 22, 2026" — April 22
        // was season 4's finale, hand-set back when that was the next thing
        // due, and season 5 was then announced for 2027 around it.
        //
        // Deliberately narrow: only a custom date already in the PAST is
        // cleared, and only when the window being announced is in the future.
        // A past date cannot be what a countdown is counting to, so nothing
        // anyone is still using gets thrown away; a custom date for something
        // upcoming is left completely alone.
        if (td.customDate.isValid()
            && td.customDate < QDate::currentDate()
            && td.targetDate > QDate::currentDate()) {
            APPLOG(QString("fetchReturningWindow: '%1' — clearing spent custom date %2, "
                           "superseded by the announced %3")
                       .arg(td.displayTitle(), td.customDate.toString(Qt::ISODate),
                            td.dateDisplay));
            td.customDate    = QDate();
            td.customDateStr = QString();
            // Its air time went with it — it described that day, not this one.
            td.customAirTime = QTime();
        }

        // Point the label at the next season's first episode, which is what
        // "returning" refers to.
        EpisodeLabel el = parseEpisodeLabel(td.statusLabel);
        if (el.valid)
            td.statusLabel = QString("S%1E01").arg(el.season + 1, 2, 10, QChar('0'));
        td.seasonEpisodeCount = 0;     // unknown until the season is listed

        if (isRefresh) emit tileRefreshed(td); else emit dataReady(td);
    });
}

// =============================================================================
//  fetchDetails / refreshTile — one request each: show + every episode.
// =============================================================================
void TvmazeScraper::fetchDetails(int tvmazeId, const QString&, const QString&)
{
    QString url = QString("%1/shows/%2?embed=episodes").arg(BASE).arg(tvmazeId);
    enqueue(url, [this](const QByteArray& body, bool ok) {
        if (!ok) {
            emit scraperError("TVmaze detail fetch failed");
            return;
        }
        handleShowPayload(QJsonDocument::fromJson(body).object(),
                          TileData{}, /*isRefresh=*/false, /*force=*/false);
    });
}

void TvmazeScraper::refreshTile(const TileData& existing, bool forceBackdropRefetch)
{
    if (existing.tvmazeId <= 0) { resolveTvmazeId(existing); return; }

    QString url = QString("%1/shows/%2?embed=episodes").arg(BASE).arg(existing.tvmazeId);
    enqueue(url, [existing, forceBackdropRefetch, this](const QByteArray& body, bool ok) {
        if (!ok) {
            // V5.4.21 — the tile is left exactly as it was, but the caller
            // still has to hear about it. TmdbScraper and IgdbScraper have done
            // this since v3.3.37; TVmaze arrived in V5 and never got the same
            // treatment, so a failed TV refresh silently never decremented
            // MainWindow's pending count. A batch containing one was then only
            // ever finished by the watchdog — which is why an ordinary refresh
            // with a failing tile took thirty seconds to settle.
            //
            // Re-emitting the unchanged tile is a no-op for tile state:
            // onTileRefreshed treats identical data as nothing changed.
            emit tileRefreshed(existing);
            return;
        }
        handleShowPayload(QJsonDocument::fromJson(body).object(),
                          existing, /*isRefresh=*/true, forceBackdropRefetch);
    });
}

// =============================================================================
//  resolveTvmazeId — migrate a TMDB-era tile.
//
//  TVmaze's /lookup endpoint does not accept TMDB ids, so there is no id
//  bridge from the existing data — matching is by title, confirmed by
//  premiere year. Anything less than an obvious match is handed back to the
//  user instead of being guessed at, because silently rebinding a tile to the
//  wrong show would be far worse than asking.
// =============================================================================
void TvmazeScraper::resolveTvmazeId(const TileData& existing)
{
    QString title = existing.title.isEmpty() ? existing.customTitle : existing.title;
    // V5.4.21 — reached from refreshTile(), so every exit here owes the caller
    // a tileRefreshed as well; see the note there.
    if (title.isEmpty()) { emit tileRefreshed(existing); return; }

    QString url = QString("%1/search/shows?q=%2")
        .arg(BASE, QString::fromUtf8(QUrl::toPercentEncoding(title)));

    enqueue(url, [existing, title, this](const QByteArray& body, bool ok) {
        if (!ok) { emit tileRefreshed(existing); return; }

        QList<SearchResult> candidates;
        double topScore = 0.0;
        for (const QJsonValue& v : QJsonDocument::fromJson(body).array()) {
            QJsonObject hit = v.toObject();
            double score = hit["score"].toDouble();
            SearchResult sr = toSearchResult(hit["show"].toObject());
            if (sr.id <= 0) continue;
            if (candidates.isEmpty()) topScore = score;
            candidates.append(sr);
        }
        if (candidates.isEmpty()) { emit tileRefreshed(existing); return; }

        const SearchResult& best = candidates.first();
        bool titlesMatch = (best.title.compare(title, Qt::CaseInsensitive) == 0);
        bool yearAgrees  = (existing.releaseYear <= 0 || best.year <= 0
                            || qAbs(best.year - existing.releaseYear) <= 1);

        if (titlesMatch && yearAgrees && topScore > 0.7) {
            TileData td = existing;
            td.tvmazeId = best.id;
            refreshTile(td);          // confident — go straight on to the real fetch
        } else {
            emit tileNeedsConfirmation(existing.id, candidates);
        }
    });
}
