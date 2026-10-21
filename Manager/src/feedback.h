#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <functional>
#include "relayconfig.h"

// =============================================================================
//  Feedback — V5.4.3. Bug reports and feature ideas, written in the app.
//
//  Separate from ShowOverrides::report(), which is a one-click "this air time
//  looks wrong" about a specific show and carries no text. This is for the
//  things the app has no button for, so free text is the whole point.
//
//  Anonymous. The only thing identifying the sender is the same random
//  per-install id every relay request already carries — no name, no address,
//  nothing asked for. There is deliberately no reply channel: adding one would
//  mean a free app started holding contact details, which it does not today.
//
//  Send-and-forget from the caller's perspective, but the result is reported
//  back so the dialog can say whether it actually arrived. A failure here must
//  never be silent — someone who took the time to write a paragraph deserves
//  to know it didn't send, rather than assuming it did.
// =============================================================================
class Feedback : public QObject
{
    Q_OBJECT
public:
    enum class Kind { Bug, Request };

    static Feedback& instance()
    {
        static Feedback inst;
        return inst;
    }

    // Longer than anyone is likely to type, but bounded — the relay rejects
    // anything past this, so the dialog stops it before the round trip.
    static constexpr int kMaxLength = 4000;

    // onDone(ok, message): message is empty on success, otherwise something
    // showable to the user.
    void send(Kind kind, const QString& text, const QString& appVersion,
              std::function<void(bool ok, const QString& message)> onDone)
    {
        // V5.4.26 — feedback goes to a relay, and a build from the public
        // source has none until one is set. Saying so beats a network error
        // the writer can do nothing about, and it keeps what they typed.
        if (!RelayConfig::isConfigured()) {
            onDone(false, "There's no Media Countdowns Server set up for this "
                          "install, so there's nowhere to send this. You can add "
                          "one under Settings → Network → Relay Key.");
            return;
        }
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty()) { onDone(false, "Please write something first."); return; }
        if (trimmed.size() > kMaxLength) {
            onDone(false, QString("That's a bit long — please keep it under %1 characters.")
                              .arg(kMaxLength));
            return;
        }

        QNetworkRequest req{QUrl(RelayConfig::baseUrl() + "/feedback")};
        req.setRawHeader("Authorization", ("Bearer " + RelayConfig::sharedSecret()).toUtf8());
        req.setRawHeader("X-Client-ID", RelayConfig::installationId().toUtf8());
        req.setRawHeader("Content-Type", "application/json");

        QJsonObject body;
        body["kind"]        = (kind == Kind::Bug) ? "bug" : "request";
        body["body"]        = trimmed;
        body["app_version"] = appVersion;

        QNetworkReply* r = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [r, onDone]() {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) {
                onDone(false, "Couldn't reach the server — check your connection "
                              "and try again.");
                return;
            }
            QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
            // There is deliberately no send limit — sending three bugs in a
            // row is a normal way to work through a list, and being told to
            // wait after writing something out is a good way to make someone
            // not bother again. accepted=false is therefore not expected;
            // it stays handled so a future server-side guard can't fail
            // silently and leave someone thinking their message arrived.
            if (!o.value("accepted").toBool(false)) {
                onDone(false, "The server didn't accept that — please try again.");
                return;
            }
            onDone(true, QString());
        });
    }

private:
    Feedback() : m_nam(new QNetworkAccessManager(this)) {}
    Q_DISABLE_COPY(Feedback)

    QNetworkAccessManager* m_nam;
};
