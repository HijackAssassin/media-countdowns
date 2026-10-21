#pragma once
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <QUrl>
#include <functional>

// =============================================================================
//  ImageFetchQueue — V5.4.25. One queue for every artwork download the app
//  makes, with a hard ceiling on how many are in the air at once.
//
//  Why this exists at all. Everything else the app fetches is paced by
//  something: TMDB and IGDB metadata go through the relay, which holds them
//  well under the published limits, and TVmaze has its own 550ms serial queue
//  because its limit is per end-user IP. Images were the exception — they go
//  DIRECT from each user's machine to the two image CDNs, so nothing the relay
//  does can slow them, and a refresh of a large library fired as many as Qt
//  happened to allow.
//
//  What the providers actually publish, checked rather than assumed:
//
//    TMDB   The rate-limiting page gives "somewhere in the 40 requests per
//           second range", notes the old 40-per-10-seconds limit was disabled
//           in December 2019, and asks callers to "be respectful of the
//           service" and honour a 429. Nothing is published for
//           image.tmdb.org specifically — not a rate, not a concurrency
//           figure. Widely-repeated numbers for it (20 connections per IP)
//           are not in TMDB's documentation and are NOT relied on here.
//
//    IGDB   4 requests per second, maximum 8 open at any moment. That is the
//           strictest number either provider publishes for anything, and
//           images.igdb.com publishes nothing separate.
//
//  So the ceiling is set from the strictest published figure that exists —
//  IGDB's 8 open requests — and then halved. FOUR at a time is comfortably
//  inside every number above, including the unpublished ones people quote for
//  TMDB, and is still four times faster than fetching one at a time. Being
//  wrong in this direction costs a few seconds on a first-time library fill;
//  being wrong in the other direction risks the credential every user shares.
//
//  A 429 or 503 parks the whole queue for the interval the server asked for,
//  rather than letting each remaining download rediscover the same wall.
// =============================================================================
class ImageFetchQueue : public QObject
{
    Q_OBJECT
public:
    // Four at once, from the reasoning above. Deliberately a named constant:
    // if a provider ever publishes a real image limit, this is the one number
    // that needs to change.
    static constexpr int kMaxConcurrent = 4;

    // How long to park everything when a CDN says "slow down" and doesn't say
    // for how long.
    static constexpr int kDefaultBackoffMs = 5000;

    static ImageFetchQueue& instance()
    {
        static ImageFetchQueue inst;
        return inst;
    }

    // ok is false for any failure, including one this queue gave up retrying.
    using Callback = std::function<void(const QByteArray& body, bool ok)>;

    void fetch(const QUrl& url, Callback cb)
    {
        m_pending.enqueue({url, std::move(cb), 0});
        pump();
    }

    int inFlight() const { return m_inFlight; }
    int queued()   const { return m_pending.size(); }

private:
    struct Job {
        QUrl     url;
        Callback cb;
        int      attempts;
    };

    ImageFetchQueue()
        : m_nam(new QNetworkAccessManager(this))
    {
        m_resume.setSingleShot(true);
        connect(&m_resume, &QTimer::timeout, this, [this] { m_paused = false; pump(); });
    }

    void pump()
    {
        if (m_paused) return;
        while (m_inFlight < kMaxConcurrent && !m_pending.isEmpty())
            start(m_pending.dequeue());
    }

    void start(Job job)
    {
        ++m_inFlight;
        QNetworkRequest req{job.url};
        // Keep-alive is Qt's default and is left on deliberately: reusing a
        // connection is the difference between four connections and four
        // hundred over a library refresh.
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(20000);

        QNetworkReply* r = m_nam->get(req);
        connect(r, &QNetworkReply::finished, this, [this, r, job]() mutable {
            r->deleteLater();
            --m_inFlight;

            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 429 || status == 503) {
                // Park everything, not just this one. The server is telling the
                // whole client to wait, and letting the other three run into the
                // same answer is how a slow-down becomes a block.
                int waitMs = kDefaultBackoffMs;
                const QByteArray retryAfter = r->rawHeader("Retry-After");
                if (!retryAfter.isEmpty()) {
                    bool okNum = false;
                    const int secs = retryAfter.toInt(&okNum);
                    if (okNum && secs > 0) waitMs = qMin(secs, 120) * 1000;
                }
                m_paused = true;
                m_resume.start(waitMs);

                if (job.attempts < 1) {          // one retry, then give up quietly
                    ++job.attempts;
                    m_pending.prepend(job);      // keeps its place at the front
                    return;
                }
                if (job.cb) job.cb({}, false);
                return;
            }

            const bool ok = (r->error() == QNetworkReply::NoError);
            if (job.cb) job.cb(ok ? r->readAll() : QByteArray(), ok);
            pump();
        });
    }

    QNetworkAccessManager* m_nam;
    QQueue<Job>            m_pending;
    int                    m_inFlight = 0;
    bool                   m_paused   = false;
    QTimer                 m_resume;
};
