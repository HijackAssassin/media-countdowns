#pragma once
#include <QDate>
#include <QString>

// =============================================================================
//  LoopSchedule — V5.4.2. Where a looped tile goes when its date passes.
//
//  Looped tiles (birthdays, holidays, "every Friday") don't release and stay
//  released — they advance to their next occurrence and keep counting down.
//  That advance used to live inline in MainWindow::onGlobalTick(), which was
//  fine while the main app was the only thing that did it. The notifier now
//  has to do it too: it is the only one running when the app is closed, and
//  a looped tile it notified without advancing was left marked Inactive and
//  never rolled again — the countdown simply stopped.
//
//  So the rule lives here, in a header that depends on nothing but QDate and
//  QString, and BOTH programs call it. An identical copy ships in the
//  notifier's src/. If you change one, change the other — this is the same
//  arrangement tiledata.h already has, and the reason it is a single small
//  file with one entry point is so that comparing them is trivial.
//
//  Nothing here reads the clock beyond QDate::currentDate(), and nothing
//  mutates a tile: the caller applies the result. That keeps it testable and
//  keeps the two programs' differing TileData shapes out of it entirely.
// =============================================================================
namespace LoopSchedule
{
    // Anonymous Gregorian computus. Kept as its own function because both
    // Easter and Good Friday need it, and the original had two copies of the
    // identical lambda a few lines apart.
    inline QDate easterFor(int yr)
    {
        int a = yr % 19, b = yr / 100, c = yr % 100, d = b / 4, e = b % 4, f = (b + 8) / 25;
        int g = (b - f + 1) / 3, h = (19 * a + b - d - g + 15) % 30, i = c / 4, k = c % 4;
        int l = (32 + 2 * e + 2 * i - h - k) % 7, m = (a + 11 * h + 22 * l) / 451;
        return QDate(yr, (h + l - 7 * m + 114) / 31, ((h + l - 7 * m + 114) % 31) + 1);
    }

    // The next occurrence strictly after today, or an invalid QDate when the
    // inputs can't produce one (which the caller must treat as "leave the
    // tile alone" rather than clearing its date).
    //
    // `interval` is "Daily" | "Weekly" | "Monthly" | "Yearly"; empty means
    // Yearly, which is what a tile created before the field existed carries.
    // loopWeekday is Qt's 1=Mon..7=Sun. loopDayOfMonth is clamped to 1..28 so
    // a "31st" loop can't skip February.
    inline QDate nextOccurrence(const QString& interval,
                                const QString& presetType,
                                int loopWeekday,
                                int loopDayOfMonth,
                                const QDate& targetDate,
                                const QDate& customDate)
    {
        QDate next;
        // The search floor is TOMORROW, not today: a tile that just expired
        // must move to its next occurrence, not land back on today and
        // immediately expire again.
        const QDate today = QDate::currentDate().addDays(1);
        const QString iv = interval.isEmpty() ? QStringLiteral("Yearly") : interval;

        if (iv == "Daily") {
            next = QDate::currentDate().addDays(1);
        } else if (iv == "Weekly") {
            int daysAhead = (loopWeekday - today.dayOfWeek() + 7) % 7;
            if (daysAhead == 0) daysAhead = 7;
            next = today.addDays(daysAhead);
        } else if (iv == "Monthly") {
            int dom = qMax(1, qMin(loopDayOfMonth, 28));
            next = QDate(today.year(), today.month(), dom);
            if (next <= QDate::currentDate()) next = next.addMonths(1);
            // Clamp to valid days in that month
            next = QDate(next.year(), next.month(), qMin(dom, next.daysInMonth()));
        } else {   // Yearly
            if (presetType == "Easter") {
                int y = today.year();
                next = easterFor(y);
                if (next < today) next = easterFor(y + 1);
            } else if (presetType == "Good Friday") {
                int y = today.year();
                next = easterFor(y).addDays(-2);
                if (next < today) next = easterFor(y + 1).addDays(-2);
            } else {
                QDate base = targetDate.isValid() ? targetDate : customDate;
                if (!base.isValid()) return QDate();
                next = base.addYears(1);
                if (!next.isValid())
                    next = QDate(base.year() + 1, base.month(),
                                 qMin(base.day(),
                                      QDate(base.year() + 1, base.month(), 1).daysInMonth()));
            }
        }
        return next;
    }
}
