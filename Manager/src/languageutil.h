#pragma once
#include <QString>
#include <QStringList>
#include <QSettings>
#include <QLocale>

// =============================================================================
//  LanguageUtil  —  v3.1.2
//
//  Powers the Settings → "Language" preference, which determines which
//  language's backdrop images get pulled from TMDB (some backdrops have
//  text/logos baked in for a specific language — without a preference,
//  TMDB's returned order can surface one in an unexpected language).
// =============================================================================
namespace LanguageUtil
{
    // ISO 639-1 codes, in the order they appear in the Settings dropdown.
    inline const QStringList& codes()
    {
        static const QStringList v = {
            "en", "es", "fr", "de", "it", "pt", "ja", "ko", "zh", "hi",
            "ru", "ar", "nl", "sv", "tr", "pl", "he", "th", "vi", "id"
        };
        return v;
    }
    inline const QStringList& labels()
    {
        static const QStringList v = {
            "English", "Spanish", "French", "German", "Italian", "Portuguese",
            "Japanese", "Korean", "Chinese", "Hindi", "Russian", "Arabic",
            "Dutch", "Swedish", "Turkish", "Polish", "Hebrew", "Thai",
            "Vietnamese", "Indonesian"
        };
        return v;
    }

    // Best-effort system language detection — falls back to English if the
    // system locale isn't one of the codes above (or can't be read at all).
    inline QString detectSystemLanguageCode()
    {
        QString sysCode = QLocale::system().name().section('_', 0, 0).toLower();
        return codes().contains(sysCode) ? sysCode : "en";
    }

    // The saved language code. On first run (nothing saved yet) this tries
    // the system language, falling back to English.
    inline QString currentCode()
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        if (s.contains("backdropLanguage"))
            return s.value("backdropLanguage").toString();
        QString detected = detectSystemLanguageCode();
        s.setValue("backdropLanguage", detected);
        return detected;
    }

    inline void setCode(const QString& code)
    {
        QSettings s("HijackAssassin", "MediaCountdowns");
        s.setValue("backdropLanguage", code);
    }
}
