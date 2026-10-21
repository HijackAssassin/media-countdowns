#pragma once
#include <QSettings>

// =============================================================================
//  TileDisplayPrefs  —  v3.0.2
//
//  Powers the Settings → "Tile Display" preferences: which pieces of info
//  show up on each tile (Title, Year, Season, Episode, Total Episodes,
//  Weekday, Date). Mirrors the TimeZoneUtil pattern — small, cheap-to-call
//  getters backed by QSettings, read fresh each time (not a hot path; only
//  read when a tile's overlay text is (re)built, not on every 1-second tick).
// =============================================================================
namespace TileDisplayPrefs
{
    inline bool showTitle()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("tileShowTitle", true).toBool();
    }
    inline bool showYear()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("tileShowYear", false).toBool();
    }
    inline bool showSeason()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("tileShowSeason", true).toBool();
    }
    inline bool showEpisode()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("tileShowEpisode", true).toBool();
    }
    // Only ever applies alongside Episode — even if the checkbox itself is
    // still checked (remembered) while Episode is off, it shouldn't render.
    inline bool showTotalEpisodes()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("tileShowTotalEpisodes", false).toBool() && showEpisode();
    }
    inline bool showWeekday()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("tileShowWeekday", false).toBool();
    }
    inline bool showDate()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("tileShowDate", true).toBool();
    }
    // V5.4 — the release TIME, printed after the date. Off by default: most
    // tiles fall through to the Time Zone default rather than a time any
    // source actually published, so showing it for everyone would present a
    // derived value as if it were announced. Opt-in, like Weekday and Year.
    //
    // Independent of showDate() on purpose — unlike Total Episodes, a time
    // still reads fine on its own ("Lanterns S01E03 • 9:00 PM").
    inline bool showTime()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        return s.value("tileShowTime", false).toBool();
    }
}
