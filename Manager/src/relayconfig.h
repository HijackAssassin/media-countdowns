#pragma once
#include <QString>
#include <QSettings>
#include <QUuid>
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

// =============================================================================
//  RelayConfig — V4. Support for routing TMDB/IGDB requests through
//  Patrick's relay server instead of calling TMDB/IGDB directly. When
//  enabled, the app never needs its own TMDB/IGDB credentials at all —
//  the relay holds the real ones and injects them server-side. See
//  tmdbscraper.cpp's getJson() and igdbscraper.cpp's postToIgdb() for
//  where this actually gets used.
//
//  In the public source BOTH DEFAULTS BELOW ARE DELIBERATELY BLANK.
//
//  RELAY_SHARED_SECRET_DEFAULT is a password — it is what lets an app
//  authenticate to a relay — and a password committed to a public repo
//  is a password everyone has. So there isn't one here.
//
//  With both blank, RelayConfig::isConfigured() answers false and the app
//  never calls a relay at all. It still works: TV comes from TVmaze, which
//  needs no credentials of any kind. To get movies and games as well you
//  have two choices, and README.md walks through both —
//
//    • put your own TMDB key / IGDB credentials in Settings, and the app
//      calls those services directly, no relay involved; or
//    • run the relay server yourself (its source is in ../Relay) and enter
//      its address and secret in Settings → Network → Relay Key.
//
//  The values come from relaysecret.local.h when that file exists (it is
//  git-ignored: Patrick's own builds have his relay baked in through it),
//  else from relaysecret.example.h, which is blank. If you are building a
//  private build for yourself, copy relaysecret.example.h to
//  relaysecret.local.h and put your own values in it — never commit it,
//  for the reason above. Both are user-overridable in Settings either way.
// =============================================================================

#if __has_include("relaysecret.local.h")
#  include "relaysecret.local.h"
#else
#  include "relaysecret.example.h"
#endif
static constexpr const char* RELAY_BASE_URL_DEFAULT      = RELAY_BASE_URL_VALUE;
static constexpr const char* RELAY_SHARED_SECRET_DEFAULT = RELAY_SHARED_SECRET_VALUE;

class RelayConfig
{
public:
    // V4.3 — per-service relay routing, replacing a single global
    // useRelay() flag. The actual rule (see mainwindow.cpp's Settings
    // dialog for the full reasoning and the UI side of this):
    //
    //  - If FEWER than both a custom TMDB key and custom IGDB
    //    credentials are configured (0 or 1 of the two): each service
    //    independently uses ITS OWN credential if it has one,
    //    regardless of the "Use Media Countdowns Server" checkbox — a service
    //    with no credential falls back to the checkbox instead (relay
    //    if checked; if unchecked, it simply has no data source and
    //    will fail, by design — the checkbox is a real, meaningful
    //    choice, not just a suggestion).
    //  - If BOTH are configured: the checkbox becomes the sole,
    //    authoritative switch for BOTH services at once — checked
    //    forces relay for both (ignoring the saved credentials
    //    entirely), unchecked forces direct for both.
    //
    // Re-implemented here (rather than calling TmdbScraper::apiKey()/
    // IgdbScraper::isConfigured() directly) to avoid a circular
    // include, since both of those headers already include this one.
    static bool hasCustomTmdbKey() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return !s.value("tmdbApiKey").toString().trimmed().isEmpty();
    }
    static bool hasCustomIgdbCredentials() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return !s.value("igdbClientId").toString().trimmed().isEmpty()
            && !s.value("igdbClientSecret").toString().trimmed().isEmpty();
    }
    static bool relayCheckboxValue() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("useRelay", true).toBool();
    }
    // V5.4.26 — is there a relay to talk to at all?
    //
    // A build from the public source ships both slots BLANK, on purpose: the
    // secret must not be in a public repo, and a URL without one is useless.
    // Every relay call concatenates "<baseUrl>/whatever", so with no baseUrl
    // the request is a relative URL with no scheme, which QNetworkAccessManager
    // refuses — a stream of failing requests, an unexplained error on the
    // status line, and a "Checking…" that never resolves. Asking first turns
    // that into simply not calling: the app runs keyless on TVmaze, which needs
    // no credentials of any kind, until Settings → Network → Relay Key is
    // filled in. Anyone with their own TMDB key or IGDB credentials is
    // unaffected — those paths never touch the relay.
    static bool isConfigured() {
        return !baseUrl().isEmpty() && !sharedSecret().isEmpty();
    }

    static bool shouldUseRelayForTmdb() {
        if (!isConfigured()) return false;
        bool hasTmdb = hasCustomTmdbKey();
        bool hasIgdb = hasCustomIgdbCredentials();
        if (hasTmdb && hasIgdb) return relayCheckboxValue();   // both configured: checkbox wins, for both
        return !hasTmdb && relayCheckboxValue();               // otherwise: own credential always wins; else defer to checkbox
    }
    static bool shouldUseRelayForIgdb() {
        if (!isConfigured()) return false;
        bool hasTmdb = hasCustomTmdbKey();
        bool hasIgdb = hasCustomIgdbCredentials();
        if (hasTmdb && hasIgdb) return relayCheckboxValue();
        return !hasIgdb && relayCheckboxValue();
    }

    static QString baseUrl() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        QString v = s.value("relayBaseUrl").toString().trimmed();
        // Strip any trailing slash so "<baseUrl>/tmdb/..." concatenation
        // never ends up with an accidental "//".
        while (v.endsWith('/')) v.chop(1);
        if (!v.isEmpty()) return v;
        return QString(RELAY_BASE_URL_DEFAULT);
    }
    static QString sharedSecret() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        QString v = s.value("relaySharedSecret").toString().trimmed();
        return v.isEmpty() ? QString(RELAY_SHARED_SECRET_DEFAULT) : v;
    }

    // ── The installation id ─────────────────────────────────────────────
    //
    // An anonymous id that lets the relay count distinct installations and
    // rate-limit them fairly. No personal information of any kind is in it.
    //
    // V5.4.8 — the relay ISSUES it now, and signs it, rather than the app
    // making up a UUID. Two reasons:
    //
    //  1. Only a signed id counts as an installation, which is what stops the
    //     count being inflated by anything that happens to send an
    //     X-Client-ID header. The dashboard read 22 installations when one
    //     person had ever run the app, because every throwaway value used
    //     while debugging registered as one.
    //  2. An id edited by hand doesn't verify, so it is refused and replaced
    //     with a fresh one — nobody can graft themselves onto someone else's
    //     id, deliberately or by copying a config around.
    //
    // Stored in TWO places and read from whichever still has it:
    //
    //  • a hidden file under the user's profile, outside the app's own data
    //    directory, so it survives an uninstall/reinstall and isn't sitting
    //    next to tiles.json where it could be deleted during a clear-out
    //  • the registry, as it always was
    //
    // Either alone would be enough; both together mean losing one doesn't
    // silently turn a returning user into a new one.
    static QString idFilePath() {
        return QDir::homePath() + "/.mediacountdowns/install.id";
    }

    // An id the relay issued looks like <32 hex>.<16 hex signature>. Anything
    // else cannot verify, so it is never counted as an installation and never
    // gets a User number in the console log.
    //
    // V5.4.9 — this exists because of what upgrading actually looked like.
    // Before V5.4.8 the app made up its own id, a plain QUuid, and every
    // machine that had ever run it still had one sitting in the registry.
    // ensureRegisteredWithRelay() only asked for an id when nothing at all was
    // stored, so on those machines it asked for nothing: the old UUID was sent
    // on every request, verified on none of them, and the dashboard sat at 0
    // installations while the console logged "unregistered client" — which is
    // exactly what happened here. An id that isn't in the issued format means
    // "not registered yet", so registration runs once and replaces it.
    static bool looksIssued(const QString& id) {
        static const QRegularExpression re("^[0-9a-f]{32}\\.[0-9a-f]{16}$");
        return re.match(id.trimmed()).hasMatch();
    }

    // True when this installation still has to collect an id from the relay.
    static bool needsRegistration() { return !looksIssued(installationId()); }

    static QString installationId() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        QString fromSettings = s.value("relayInstallationId").toString().trimmed();

        QString fromFile;
        {
            QFile f(idFilePath());
            if (f.open(QIODevice::ReadOnly))
                fromFile = QString::fromUtf8(f.readAll()).trimmed();
        }

        // Prefer an id the relay can verify, from whichever store has one:
        // straight after the upgrade one copy can still be holding the old
        // self-made UUID while the other already has the issued id, and
        // picking the wrong one would undo the registration that just
        // happened. A legacy id is still returned when that's all there is —
        // it is harmless to send, keeps rate limiting keyed per machine, and
        // gets replaced the moment registration succeeds.
        QString id;
        if      (looksIssued(fromFile))     id = fromFile;
        else if (looksIssued(fromSettings)) id = fromSettings;
        else                                id = !fromFile.isEmpty() ? fromFile : fromSettings;

        // Re-mirror, so the next uninstall or registry clean can't take the
        // last copy with it — and so the store that lost the race above is
        // brought up to date rather than left holding a stale id.
        if (!id.isEmpty()) {
            if (fromFile     != id) writeIdFile(id);
            if (fromSettings != id) s.setValue("relayInstallationId", id);
        }
        return id;
    }

    // Called once the relay has handed out an id (see MainWindow's startup
    // registration). Writes both copies.
    static void setInstallationId(const QString& id) {
        if (id.trimmed().isEmpty()) return;
        QSettings s("HijackAssassin", "MediaCountdowns");
        s.setValue("relayInstallationId", id.trimmed());
        writeIdFile(id.trimmed());
    }

private:
    static void writeIdFile(const QString& id) {
        QString path = idFilePath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        f.write(id.toUtf8());
        f.close();
#ifdef Q_OS_WIN
        // Hidden so it doesn't invite deletion from a folder people do open.
        SetFileAttributesW(reinterpret_cast<const wchar_t*>(
                               QDir::toNativeSeparators(QFileInfo(path).absolutePath()).utf16()),
                           FILE_ATTRIBUTE_HIDDEN);
#endif
    }
};
