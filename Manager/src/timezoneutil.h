#pragma once
#include <QString>
#include <QStringList>
#include <QTime>
#include <QSettings>
#include <QTimeZone>

// =============================================================================
//  TimeZoneUtil  —  v3.0.0
//
//  Powers the Settings → "Time Zone" preference. TMDB digital release dates
//  are conventionally midnight Pacific Time (the US "Hollywood" release
//  clock), so a digital movie/show isn't really out for someone in a later
//  zone until that same instant reaches them — e.g. midnight PT is 3 AM ET.
//  Theatrical movies instead open at the same local wall-clock time on both
//  coasts (~12 PM, after the -1 day Thursday-preview adjustment already
//  applied to the release date), so they're excluded from this shift.
//  Games always release at local midnight and are also excluded.
// =============================================================================
namespace TimeZoneUtil
{
    // IANA ids, in the order they should appear in the Settings dropdown.
    inline const QStringList& ids()
    {
        static const QStringList v = {
            "America/Los_Angeles", "America/Denver",
            "America/Chicago",     "America/New_York"
        };
        return v;
    }

    // Matching display labels for the dropdown.
    inline const QStringList& labels()
    {
        static const QStringList v = {
            "Pacific Time (Hollywood default)",
            "Mountain Time",
            "Central Time",
            "Eastern Time"
        };
        return v;
    }

    // Hours a zone sits ahead of Pacific Time. All four US continental zones
    // shift for DST together, so this stays constant year-round.
    inline int hoursAheadOfPacific(const QString& id)
    {
        if (id == "America/Denver")   return 1;
        if (id == "America/Chicago")  return 2;
        if (id == "America/New_York") return 3;
        return 0; // Los Angeles, or anything unrecognized
    }

    // The saved zone id. On first run (nothing saved yet) this tries to match
    // the system time zone to one of the 4 above; if it doesn't match any of
    // them, it falls back to Pacific per the app default.
    inline QString currentZoneId()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        if (s.contains("digitalTimeZone"))
            return s.value("digitalTimeZone").toString();

        QString sys = QTimeZone::systemTimeZone().id();
        QString chosen = "America/Los_Angeles";
        for (const QString& id : ids()) {
            if (sys == id) { chosen = id; break; }
        }
        s.setValue("digitalTimeZone", chosen);
        return chosen;
    }

    inline void setZoneId(const QString& id)
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        s.setValue("digitalTimeZone", id);
    }

    // Default wall-clock time for a *digital* release (movie or show) with no
    // custom time set — midnight Pacific, converted forward to the saved zone.
    inline QTime defaultDigitalTime()
    {
        return QTime(0, 0).addSecs(hoursAheadOfPacific(currentZoneId()) * 3600);
    }

    // Default wall-clock time for a *theatrical* movie — fixed at noon,
    // independent of the Time Zone setting (see note above).
    inline QTime defaultTheatricalTime()
    {
        return QTime(12, 0);
    }

    // V5 note — there is deliberately no "streaming vs broadcast" setting.
    // A published air time is always used when one exists (see
    // TvmazeScraper::applyEpisodes), because it's strictly better
    // information: services that stream without a broadcast slot publish no
    // time at all, so they fall through to defaultDigitalTime() above
    // regardless, while a network that does publish one (HBO, Adult Swim)
    // is simulcast at that time anyway.
}
