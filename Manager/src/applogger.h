#pragma once
#include <QObject>
#include <QDateTime>

// =============================================================================
//  AppLogger  —  Simple singleton logger.
//
//  Use APPLOG("message") anywhere in the main app.
//  Connect to AppLogger::instance().newEntry(QString) to display in the
//  debug window.
// =============================================================================
class AppLogger : public QObject
{
    Q_OBJECT
public:
    static AppLogger& instance()
    {
        static AppLogger inst;
        return inst;
    }

    void log(const QString& msg)
    {
        QString entry = QDateTime::currentDateTime().toString("[hh:mm:ss.zzz]  ") + msg;
        emit newEntry(entry);
    }

signals:
    void newEntry(const QString& entry);

private:
    AppLogger() = default;
    Q_DISABLE_COPY(AppLogger)
};

// Convenience macro — use APPLOG("msg") anywhere
#define APPLOG(msg) AppLogger::instance().log(msg)
