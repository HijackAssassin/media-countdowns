#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QLocalServer>
#include <QLocalSocket>
#include "trayapp.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    app.setOrganizationName("MediaCountdowns");
    app.setApplicationName("MediaCountdowns");
    app.setApplicationDisplayName("Media Countdowns Notification Manager");

    // Only ever one notifier.
    //
    // Two copies of this program is the one arrangement that genuinely
    // produces duplicate notifications, and nothing prevented it: TrayApp's
    // constructor calls QLocalServer::removeServer() unconditionally, so a
    // second launch simply took the IPC socket from the first and both kept
    // running their own once-a-minute heartbeat. Each loads tiles.json,
    // each sees the same tile still Active, and each fires — the second one
    // having read the file before the first wrote Inactive back to it.
    //
    // It is easy to end up with two: the Startup shortcut launches one at
    // login, and running the exe again (or the main app's "Open" action
    // reaching a copy in a different folder) launches another with no
    // complaint. The main app has guarded against its own double-launch
    // since v3 — this is the same probe, pointed at the tray socket.
    {
        QLocalSocket probe;
        probe.connectToServer("MediaCountdownsTray");
        if (probe.waitForConnected(300))
            return 0;   // a live notifier already owns the socket
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "Media Countdowns Notification Manager",
            "No system tray available. Notifications will not work.");
        return 1;
    }

    TrayApp tray;
    return app.exec();
}
