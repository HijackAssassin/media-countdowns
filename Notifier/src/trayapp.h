#pragma once
#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QTimer>
#include <QLocalServer>
#include <QSet>
#include <QList>
#include "tiledata.h"

class TrayApp : public QObject
{
    Q_OBJECT
public:
    explicit TrayApp(QObject* parent = nullptr);
    ~TrayApp() override;

private slots:
    void onTick();          // fires every second — gates real work to HH:MM:01
    void onHeartbeat();     // actual check: reload tiles, fire notifications if main app closed
    void onNewConnection();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onOpenMainApp();
    void onQuit();

private:
    void reloadTiles();
    void sendNotification(const TileData& td);
    void createStartupShortcut();

    QSystemTrayIcon* m_tray       = nullptr;
    QMenu*           m_menu       = nullptr;
    QLocalServer*    m_ipcServer  = nullptr;
    QTimer*          m_tickTimer  = nullptr;   // 1-second tick

    QList<TileData>  m_tiles;

    // Prevents double-firing if the 1s timer drifts and fires twice at :01
    int  m_lastCheckedMinute = -1;
};
