#include "trayapp.h"
#include "jsonmanager.h"
#include "loopschedule.h"
#include "notifierversion.h"
#include <QLocalSocket>
#include <QProcess>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>
#include <QFileInfo>
#include <QStyle>
#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QPixmap>
#include <QStandardPaths>
#include <QDir>

#ifdef Q_OS_WIN
#  include <qt_windows.h>
#  include <shobjidl.h>      // IShellLinkW
#  include <shlobj.h>        // SHGetFolderPathW / CSIDL_STARTUP
#  include <objbase.h>       // CoInitialize / CoCreateInstance
#endif

// =============================================================================
TrayApp::TrayApp(QObject* parent) : QObject(parent)
{
#ifdef Q_OS_WIN
    // Required for Windows 10 Action Center notifications to appear correctly
    SetCurrentProcessExplicitAppUserModelID(L"MediaCountdowns.Notifier");
#endif

    // ── Tray icon ─────────────────────────────────────────────────────────────
    // V5.4.21 — the app's own icon rather than Qt's generic play symbol. This
    // sits in the notification area next to everything else running on the
    // machine, and a stock icon there looks like an unrelated program.
    QIcon icon(":/appicon.ico");
    if (icon.isNull()) icon = QApplication::style()->standardIcon(QStyle::SP_MediaPlay);

    m_menu = new QMenu;
    // V5.4.26 — the version, in the two places this program can show anything.
    // It had one from V5.4.25 but nothing ever displayed it, so a user asked
    // "which notifier are you running?" had no way to answer. The tray icon and
    // its menu are the whole UI; hovering the icon is where someone would look.
    auto* heading    = m_menu->addAction(
        QString("Media Countdowns  V%1").arg(MC_NOTIFIER_VERSION));
    heading->setEnabled(false);
    m_menu->addSeparator();
    auto* openAct    = m_menu->addAction("Open Manager");
    auto* refreshAct = m_menu->addAction("Refresh");
    m_menu->addSeparator();
    auto* quitAct    = m_menu->addAction("Quit");

    m_tray = new QSystemTrayIcon(icon, this);
    m_tray->setContextMenu(m_menu);
    m_tray->setToolTip(
        QString("Media Countdowns Notifier  V%1").arg(MC_NOTIFIER_VERSION));
    m_tray->show();

    connect(openAct,    &QAction::triggered, this, &TrayApp::onOpenMainApp);
    connect(refreshAct, &QAction::triggered, this, &TrayApp::reloadTiles);
    connect(quitAct,    &QAction::triggered, this, &TrayApp::onQuit);
    connect(m_tray, &QSystemTrayIcon::activated, this, &TrayApp::onTrayActivated);

    // ── IPC server ────────────────────────────────────────────────────────────
    m_ipcServer = new QLocalServer(this);
    QLocalServer::removeServer("MediaCountdownsTray");
    if (m_ipcServer->listen("MediaCountdownsTray"))
        connect(m_ipcServer, &QLocalServer::newConnection,
                this, &TrayApp::onNewConnection);

    // ── 1-second tick timer ───────────────────────────────────────────────────
    // onTick() gates the real work to once per minute, by watching for the
    // minute to change. Resilient to sleep/hibernate: it reads the clock on
    // every wakeup rather than relying on a 60-second interval not drifting.
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    m_tickTimer->setTimerType(Qt::PreciseTimer);
    connect(m_tickTimer, &QTimer::timeout, this, &TrayApp::onTick);
    m_tickTimer->start();

    // Add ourselves to Windows Startup folder so we auto-launch on login.
    createStartupShortcut();

    // Startup check: catch anything that expired while the notifier wasn't
    // running. onHeartbeat() reloads first, so no separate load is needed.
    onHeartbeat();
}

TrayApp::~TrayApp() = default;

// =============================================================================
//  onTick — fires every second, and does the real work once per minute.
//
//  The gate is "the minute changed", not "the clock reads second 01". The
//  old form needed the timer to land on one specific second, so anything
//  that stole that tick — the machine waking from sleep, a busy moment —
//  skipped the whole minute. Tracking the minute itself cannot miss: the
//  first tick of a new minute does the work, whichever second that is.
//  The id doubles as the once-per-minute guard it already was.
// =============================================================================
void TrayApp::onTick()
{
    QTime now = QTime::currentTime();
    int minuteId = now.hour() * 60 + now.minute();
    if (minuteId == m_lastCheckedMinute) return;
    m_lastCheckedMinute = minuteId;

    qDebug() << "[TrayApp] Tick at" << now.toString("HH:mm:ss");
    onHeartbeat();
}

// =============================================================================
//  onHeartbeat — the real work.
//
//  Conditions to fire a notification for a tile:
//    1. notifStatus is not Inactive — i.e. Active OR Ready, both of which
//       mean "expired, still owed a notification"
//    2. it has a date the user hasn't removed, and that date is a real
//       release rather than a window bound or a guess
//    3. that date+time is in the past
//
//  A looped tile is then advanced to its next occurrence instead of being
//  marked done, since this program is the only one running to do it.
//
//  This used to claim a third condition — "main app is NOT running, it
//  handles notifications itself when open" — inherited from EventCountdown.
//  It was never true here and must not be reintroduced: the main app's only
//  notification path is fireDirectNotification(), which is wired solely to
//  the right-click "Test Notification" action. It never fires one on its
//  own, so if this program stayed quiet while the app was open, a release
//  that happened during that time would be announced by nobody. The main
//  app's IPC message is "REFRESH", i.e. re-read the file — not "stand down".
//
//  effectiveTime() here is deliberately simpler than the main app's: the
//  time stored in tiles.json is ALREADY the resolved, Time-Zone-adjusted
//  value, because MainWindow's saveTiles() writes td.effectiveTime() into
//  the airTime field (its v3.3.45 fix, made for exactly this reader). So a
//  theatrical release arrives here as 12:00 and a digital one as the
//  zone-shifted default; re-deriving any of that would be wrong.
//
//  Tiles always reloaded fresh from disk so we pick up any edits the user
//  made via the main app before closing it.
// =============================================================================
void TrayApp::onHeartbeat()
{
    reloadTiles();

    QDateTime now = QDateTime::currentDateTime();
    bool anyChanged = false;

    QSet<QString> rolled;   // looped tiles advanced to their next occurrence

    for (TileData& td : m_tiles) {
        // A looped occurrence the main app already advanced past. Its date is
        // now in the FUTURE, so none of the checks below would ever reach it —
        // the flag is the whole record that it happened. Handled first and
        // separately for that reason.
        if (td.pendingLoopNotice) {
            sendNotification(td);
            td.pendingLoopNotice = false;
            anyChanged = true;
            qDebug() << "[TrayApp] Announced a looped occurrence handed over by the main app";
            continue;
        }

        // Anything but Inactive is still awaiting its notification.
        //
        // This used to require Active exactly, which silently dropped every
        // Ready tile — and Ready is what the main app sets the instant a tile
        // expires while it is open. Nothing ever moved those back to Active,
        // so they were never announced at all: Invincible sat in Ready from
        // 2026-04-22 for nearly four months. The enum has always documented
        // Ready as "tile just expired; notifier will fire then flip to
        // Inactive" — this is that, finally true.
        if (td.notifStatus == NotifStatus::Inactive) continue;
        // hasDate() now honours the user having removed the date; without
        // that, a tile they deliberately cleared still carried its old
        // targetDate and was announced as released on the spot.
        if (!td.hasDate()) continue;
        // A passed date that never meant "it came out" — a month/year window
        // bound or a guessed episode date. See notificationSuppressed().
        if (td.notificationSuppressed()) continue;
        QTime t = td.effectiveTime().isValid() ? td.effectiveTime() : QTime(0, 0, 0);
        QDateTime target(td.effectiveDate(), t);
        if (now < target) continue;

        sendNotification(td);

        if (td.isLooped) {
            // A birthday or holiday doesn't finish when it arrives, it comes
            // round again. Marking it Inactive like a release left it stuck
            // on a past date forever, because the main app only advances a
            // looped tile that is still Active — so once this program had
            // announced one, nothing could ever move it again.
            QDate next = LoopSchedule::nextOccurrence(
                td.loopInterval, td.presetType,
                td.loopWeekday, td.loopDayOfMonth,
                td.targetDate, td.customDate);
            if (next.isValid()) {
                // V5.4.26 — the occurrence being advanced away from, kept so
                // the main app's startup recap can report it. This program is
                // what rolls a looped tile while the app is closed, so it is
                // the only thing in a position to write it down.
                td.loopLastOccurrence = td.effectiveDate();
                td.targetDate    = next;
                td.customDate    = QDate();
                td.dateDisplay   = next.toString("MMMM d, yyyy");
                td.customDateStr = QString();
                td.notifStatus   = NotifStatus::Active;   // counting down again
                td.notified      = false;
                rolled.insert(td.id);
                qDebug() << "[TrayApp] Looped tile advanced to" << next.toString(Qt::ISODate);
            } else {
                // No next occurrence can be derived — stop rather than
                // announce the same day again on the next heartbeat.
                td.notifStatus = NotifStatus::Inactive;
            }
        } else {
            td.notifStatus = NotifStatus::Inactive;
        }
        anyChanged = true;
    }

    if (anyChanged) {
        qDebug() << "[TrayApp] Saving updated statuses to JSON";
        JsonManager::instance().saveTiles(m_tiles, rolled);
    }
}

// =============================================================================
//  reloadTiles
// =============================================================================
void TrayApp::reloadTiles()
{
    m_tiles = JsonManager::instance().loadTiles();
}

// =============================================================================
//  sendNotification
// =============================================================================
void TrayApp::sendNotification(const TileData& td)
{
    QString title = td.displayTitle();
    int bullet = title.indexOf(QChar(0x2022));  // strip "  •  N Seasons" suffix
    if (bullet >= 0) title = title.left(bullet).trimmed();

    QString body;
    if (td.statusLabel.isEmpty()
        || td.statusLabel == "Releases"
        || td.statusLabel == "Released"
        || td.statusLabel == "No Release Date Yet")
    {
        body = "Now available!";
    } else {
        body = QString("%1 is out!").arg(td.statusLabel);
    }

    qDebug() << "[TrayApp] Notification:" << title << "|" << body;

    QIcon notifIcon;
    if (!td.imagePath.isEmpty() && QFile::exists(td.imagePath)) {
        QPixmap px(td.imagePath);
        if (!px.isNull())
            notifIcon = QIcon(px.scaled(256, 144,
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    if (notifIcon.isNull())
        notifIcon = QApplication::style()->standardIcon(QStyle::SP_MediaPlay);

    m_tray->setIcon(notifIcon);
    m_tray->showMessage(title, body, notifIcon, 10000);

    QTimer::singleShot(12000, this, [this]() {
        QIcon fallback(":/appicon.ico");
        m_tray->setIcon(fallback.isNull()
                            ? QApplication::style()->standardIcon(QStyle::SP_MediaPlay)
                            : fallback);
    });
}

// =============================================================================
//  createStartupShortcut
//
//  Creates a .lnk shortcut to this exe in the Windows Startup folder so the
//  notifier launches automatically on login.
//  Does nothing if the shortcut already exists or on non-Windows platforms.
// =============================================================================
void TrayApp::createStartupShortcut()
{
#ifdef Q_OS_WIN
    // Resolve %APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
    WCHAR startupW[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, SHGFP_TYPE_CURRENT, startupW)))
        return;

    QString shortcutPath = QString::fromWCharArray(startupW)
                           + "\\MediaCountdownsNotifier.lnk";

    if (QFile::exists(shortcutPath)) return;   // already installed

    QString exePath = QCoreApplication::applicationFilePath();
    exePath.replace('/', '\\');

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**>(&psl))))
    {
        psl->SetPath(exePath.toStdWString().c_str());
        psl->SetDescription(L"Media Countdowns Notifier — runs in the system tray");
        psl->SetWorkingDirectory(
            QCoreApplication::applicationDirPath().replace('/', '\\')
            .toStdWString().c_str());

        IPersistFile* ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile,
                                          reinterpret_cast<void**>(&ppf))))
        {
            ppf->Save(shortcutPath.toStdWString().c_str(), TRUE);
            ppf->Release();
            qDebug() << "[TrayApp] Startup shortcut created:" << shortcutPath;
        }
        psl->Release();
    }

    CoUninitialize();
#endif
}

// =============================================================================
void TrayApp::onNewConnection()
{
    QLocalSocket* sock = m_ipcServer->nextPendingConnection();
    if (!sock) return;
    connect(sock, &QLocalSocket::readyRead, this, [sock, this]() {
        while (sock->canReadLine()) {
            QString cmd = QString::fromUtf8(sock->readLine()).trimmed();
            qDebug() << "[TrayApp] IPC:" << cmd;
            if (cmd == "REFRESH") {
                // Just reload cache; main app is alive so heartbeat won't fire
                reloadTiles();
            } else if (cmd.startsWith("TEST:")) {
                QString tileId = cmd.mid(5);
                reloadTiles();
                for (const TileData& td : std::as_const(m_tiles)) {
                    if (td.id == tileId) { sendNotification(td); break; }
                }
            }
        }
        sock->deleteLater();
    });
    connect(sock, &QLocalSocket::disconnected, sock, &QLocalSocket::deleteLater);
}

void TrayApp::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::DoubleClick) onOpenMainApp();
}

void TrayApp::onOpenMainApp()
{
    QString path = QCoreApplication::applicationDirPath() + "/MediaCountdowns.exe";
    if (QFileInfo::exists(path))
        QProcess::startDetached(path, {});
    else
        m_tray->showMessage("Media Countdowns",
            "Could not find MediaCountdowns.exe next to this app.",
            QSystemTrayIcon::Warning, 4000);
}

void TrayApp::onQuit()
{
    m_tray->hide();
    QCoreApplication::quit();
}
