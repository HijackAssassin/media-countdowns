#pragma once
#include <QString>
#include <QDate>
#include <QTime>

// Notification lifecycle:
//   Active   — tile counting down, notification not yet sent
//   Ready    — tile just expired; notifier will fire then flip to Inactive
//   Inactive — notification sent; never re-fires
enum class NotifStatus { Active, Ready, Inactive };

struct TileData
{
    QString id;
    QString title;
    QString customTitle;
    int     tmdbId    = 0;
    QString mediaType;
    QString tmdbUrl;
    QString statusLabel;
    QString rawDateText;
    QString dateDisplay;
    QString customDateStr;
    QDate   targetDate;
    QDate   customDate;
    QTime   airTime;
    QTime   customAirTime;
    QString imagePath;
    bool    notified    = false;
    NotifStatus notifStatus = NotifStatus::Active;

    // ── State this program does not own, but must not ignore ────────────────
    //
    // Everything below is written by the main app and decides whether a date
    // that has passed actually means something CAME OUT. Without them this
    // program sees only "a date, and it is behind us", which is not the same
    // question, and answers it out loud.
    //
    // Deliberately a minimal set: read only what changes the decision, so
    // this stays a notifier rather than a second copy of the main app.

    // The user removed the date on purpose. targetDate is usually still
    // sitting there from before, so without this the tile reads as a release
    // that happened the moment they cleared it.
    bool    noDateOverride = false;

    // Birthdays and holidays, which advance to their next occurrence instead
    // of releasing. This program announces them AND advances them itself —
    // it is the only thing running when the main app is closed, and simply
    // marking one Inactive left it stranded on a past date forever. The
    // schedule fields below are what LoopSchedule::nextOccurrence() needs.
    bool    isLooped = false;
    QString loopInterval;        // "Yearly" | "Monthly" | "Weekly" | "Daily"
    int     loopWeekday    = 1;  // 1=Mon..7=Sun, used when Weekly
    int     loopDayOfMonth = 1;  // 1-31, used when Monthly
    QString presetType;          // "Easter"/"Good Friday" move every year

    // Set by the main app when IT advanced a looped tile — meaning the
    // occurrence that just arrived still owes a notification, even though the
    // date has already moved on to next time.
    //
    // The main app advances a looped tile within a second of it expiring, so
    // this program never saw it in the expired state and a birthday arriving
    // while the app was open was announced by nobody. The main app could fire
    // its own notification instead, but then two programs would be racing to
    // announce the same instant, which is how duplicates happen. This keeps
    // exactly one announcer: the main app hands the job over, and this
    // program clears the flag once it has done it.
    bool    pendingLoopNotice = false;

    // V5.4.26 — the looped occurrence most recently advanced past, written by
    // whichever program did the advancing.
    //
    // This program never reads it. It writes it because it is usually the one
    // that rolls a looped tile — it is what runs when the main app is closed,
    // which is exactly when the main app's startup recap needs to know that a
    // birthday happened. Without it the occurrence is gone the moment the roll
    // lands, and the recap has nothing to report. The main app owns the field;
    // see its tiledata.h and recapCandidateDate().
    QDate   loopLastOccurrence;

    // A guessed episode date, not a confirmed one. The main app rolls these
    // forward on expiry rather than releasing them, precisely so nothing is
    // announced off a guess.
    bool    isEstimatedDate = false;

    // The only thing announced was a month or a year ("March 2027", "2026").
    // targetDate holds the LAST day of that window purely as a countdown
    // bound — nobody ever said the show comes out on December 31.
    bool    isMonthOnlyDate = false;
    bool    isYearOnlyDate  = false;

    QString displayTitle()  const { return customTitle.isEmpty() ? title : customTitle; }
    QString displayDate()   const { return customDateStr.isEmpty() ? dateDisplay : customDateStr; }
    QDate   effectiveDate() const { return customDate.isValid() ? customDate : targetDate; }

    // Deliberately simpler than the main app's version of this, and correct
    // because of it: MainWindow::saveTiles() writes td.effectiveTime() into
    // the airTime field (its v3.3.45 fix, made for this reader), so the value
    // on disk is ALREADY resolved — a theatrical release arrives as 12:00 and
    // a digital one as the Time-Zone-shifted default. Re-deriving any of that
    // here would double-apply it.
    QTime   effectiveTime() const { return customAirTime.isValid() ? customAirTime : airTime; }

    // A window bound is not a real day. Mirrors the main app exactly,
    // including the part that matters for Invincible and Spider-Man in the
    // current data: a hand-typed customDate IS a real day and outranks the
    // window, so those still notify normally.
    bool isWindowDate() const {
        return (isMonthOnlyDate || isYearOnlyDate) && !customDate.isValid();
    }

    // Everything that means "a passed date is not a release here". Kept as
    // one predicate so the heartbeat reads as a single question and a future
    // reason can be added in one place.
    //
    // isLooped is deliberately NOT here: a birthday or holiday arriving IS an
    // event worth announcing. It is handled separately because it also has to
    // be advanced afterwards rather than marked done.
    bool notificationSuppressed() const {
        return isEstimatedDate || isWindowDate();
    }

    bool isExpired() const {
        QDate d = effectiveDate();
        if (!d.isValid()) return false;
        if (d < QDate::currentDate()) return true;
        if (d > QDate::currentDate()) return false;
        // d == today: use midnight as default when no specific air time is set.
        // This matches the countdown widget which also defaults to QTime(0,0,0).
        // Without this fix, tiles with "today" + no time show countdown=0 but
        // never move to the Released tab because isExpired() returned false.
        QTime t = effectiveTime().isValid() ? effectiveTime() : QTime(0, 0, 0);
        return QTime::currentTime() >= t;
    }
    bool hasDate()       const { return !noDateOverride && effectiveDate().isValid(); }
    int  daysRemaining() const { return QDate::currentDate().daysTo(effectiveDate()); }
    bool isValid()       const { return !id.isEmpty() && !title.isEmpty(); }
};
