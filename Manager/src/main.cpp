#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QProcess>
#include <QLocalSocket>
#include <QLocalServer>
#include <QCoreApplication>
#include "mainwindow.h"

static const char* kSingleInstanceServer = "MediaCountdownsMainApp";

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);

    // Identity — used by QStandardPaths to resolve %APPDATA%\MediaCountdowns\
    app.setOrganizationName("MediaCountdowns");
    app.setApplicationName("MediaCountdowns");
    app.setApplicationVersion("1.0.0");
    // Icon is embedded via resources/resources.qrc — Qt's rcc compiler,
    // not windres, so no path-with-spaces build issues.
    app.setWindowIcon(QIcon(":/appicon.ico"));

    // ── Single-instance guard ─────────────────────────────────────────────────
    // If another instance is already running, send it a "show" command and exit.
    // Root cause of "window doesn't appear on launch": closing the window left
    // the process alive (setQuitOnLastWindowClosed=false), so the next
    // double-click spawned a silent second instance that never showed a window.
    {
        QLocalSocket probe;
        probe.connectToServer(kSingleInstanceServer);
        if (probe.waitForConnected(300)) {
            probe.write("show\n");
            probe.waitForBytesWritten(300);
            return 0;
        }
    }

    // First instance — start listening so later launches find us.
    QLocalServer instanceServer;
    QLocalServer::removeServer(kSingleInstanceServer);
    instanceServer.listen(kSingleInstanceServer);

    // Ensure data directory exists
    QDir().mkpath(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));

    // ── Migrate old folder names from CineCountdown era ───────────────────────
    {
        QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (QDir(base + "/backdrops").exists() && !QDir(base + "/fetched_images").exists())
            QDir().rename(base + "/backdrops", base + "/fetched_images");
        if (QDir(base + "/images").exists() && !QDir(base + "/custom_images").exists())
            QDir().rename(base + "/images", base + "/custom_images");
    }

    // ── Global dark stylesheet ────────────────────────────────────────────────
    app.setStyleSheet(R"(
        QWidget       { font-family: "Segoe UI", sans-serif; }
        QDialog       { background-color: #1a1a1a; color: #e0e0e0; }
        QMessageBox   { background-color: #1a1a1a; }
        QPushButton {
            background-color: #2a2a2a;
            color: #cccccc;
            border: 1px solid #444;
            border-radius: 4px;
            padding: 5px 14px;
            font-size: 13px;
        }
        QPushButton:hover   { background-color: #383838; border-color: #666; }
        QPushButton:pressed { background-color: #111; }
        QLineEdit {
            background-color: #222;
            color: #ffffff;
            border: 1px solid #444;
            border-radius: 4px;
            padding: 4px 8px;
        }
        QLineEdit:focus { border-color: #0078d4; }
        QLabel          { color: #e0e0e0; }
        QScrollArea     { border: none; }
        QMenu {
            background-color: #252525;
            color: #e0e0e0;
            border: 1px solid #444;
        }
        QMenu::item:selected { background-color: #0078d4; }
    )");

    // ── Auto-launch tray app if not already running ───────────────────────────
    {
        QString appDir = QCoreApplication::applicationDirPath();
        // Check two locations:
        //   1. Deployed layout:   <appDir>/Notifier/MediaCountdownsNotifier.exe
        //   2. Build layout:      <appDir>/../Notifier/MediaCountdownsNotifier.exe
        //      (Qt Creator puts MainApp.exe in build/MainApp/ but Notifier in build/Notifier/)
        QString trayExe;
        for (const QString& candidate : {
            appDir + "/Notifier/MediaCountdownsNotifier.exe",
            appDir + "/../Notifier/MediaCountdownsNotifier.exe"
        }) {
            if (QFileInfo::exists(candidate)) { trayExe = candidate; break; }
        }
        if (!trayExe.isEmpty()) {
            QLocalSocket trayProbe;
            trayProbe.connectToServer("MediaCountdownsTray");
            if (!trayProbe.waitForConnected(200))
                QProcess::startDetached(trayExe, {});
        }
    }

    MainWindow w;

    // When a second instance connects, only bring the window to front if it
    // actually sends "show". A bare connect-then-disconnect (a probe) must
    // NOT raise the window — which is also what makes the notifier's
    // "Open Manager" action safe: it just launches this exe, the guard above
    // hands off to the running copy, and no second main app appears.
    QObject::connect(&instanceServer, &QLocalServer::newConnection, [&]() {
        QLocalSocket* sock = instanceServer.nextPendingConnection();
        if (!sock) return;
        QObject::connect(sock, &QLocalSocket::readyRead, sock, [sock, &w]() {
            QString msg = QString::fromUtf8(sock->readAll()).trimmed();
            if (msg == "show") {
                w.showMaximized();
                w.raise();
                w.activateWindow();
            }
            sock->deleteLater();
        });
        QObject::connect(sock, &QLocalSocket::disconnected, sock, &QLocalSocket::deleteLater);
    });

    w.showMaximized();
    w.raise();
    w.activateWindow();
    return app.exec();
}
