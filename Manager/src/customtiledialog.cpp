#include "customtiledialog.h"
#include "timezoneutil.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QStandardPaths>
#include <QPixmap>
#include <QFrame>
#include <QMessageBox>
#include <QDateTime>

static const char* kField =
    "QLineEdit, QComboBox, QSpinBox { background:#1e1e1e; color:#ffffff; "
    "border:1px solid #3a3a3a; border-radius:4px; padding:8px 10px; font-size:14px; }"
    "QLineEdit:focus, QSpinBox:focus { border-color:#0078d4; }"
    "QComboBox::drop-down { border:none; }"
    "QComboBox QAbstractItemView { background:#1e1e1e; color:#fff; "
    "selection-background-color:#0078d4; }"
    "QSpinBox::up-button, QSpinBox::down-button { width:0; border:none; }";

static const char* kDisabledField =
    "QComboBox, QSpinBox { background:#141414; color:#444; "
    "border:1px solid #222; border-radius:4px; padding:8px 10px; font-size:14px; }";

static const char* kSection = "color:#888; font-size:11px; margin-top:4px;";

static const char* kMonths[] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December"
};

// v3.0.0 — "Custom" moved out of this list and into the new Type dropdown.
static const char* kSpecialDays[] = {
    "Birthday","Christmas","Easter","Halloween","Thanksgiving",
    "New Year","April Fools","Good Friday","Veterans Day","Independence Day"
};
static const int kSpecialDayCount = 10;

// v3.0.0 — Type dropdown, in the order requested.
static const char* kTypes[] = { "Custom", "Movie", "Show", "Game", "Special" };
static const int kTypeCount = 5;

static QDate easterDate(int y) {
    int a=y%19,b=y/100,c=y%100,d=b/4,e=b%4,f=(b+8)/25;
    int g=(b-f+1)/3,h=(19*a+b-d-g+15)%30,i=c/4,k=c%4;
    int l=(32+2*e+2*i-h-k)%7,m=(a+11*h+22*l)/451;
    return QDate(y,(h+l-7*m+114)/31,((h+l-7*m+114)%31)+1);
}
static QDate goodFridayDate(int y) { return easterDate(y).addDays(-2); }

static QDate nextOccurrence(const QString& preset, const QDate& ref) {
    int y = ref.year();
    auto tryBoth = [&](int m, int day) -> QDate {
        QDate a(y,m,day); return (a>=ref)?a:QDate(y+1,m,day);
    };
    if      (preset=="Christmas")        return tryBoth(12,25);
    else if (preset=="Halloween")        return tryBoth(10,31);
    else if (preset=="Thanksgiving") {
        // 4th Thursday of November
        auto thanksgiving = [](int y) -> QDate {
            QDate d(y, 11, 1);
            int dow = d.dayOfWeek(); // 1=Mon..7=Sun
            int firstThurs = 1 + ((4 - dow + 7) % 7);
            return QDate(y, 11, firstThurs + 21);
        };
        QDate d = thanksgiving(y); return (d >= ref) ? d : thanksgiving(y+1);
    }
    else if (preset=="New Year")         return tryBoth(1,1);
    else if (preset=="April Fools")      return tryBoth(4,1);
    else if (preset=="Veterans Day")     return tryBoth(11,11);
    else if (preset=="Independence Day") return tryBoth(7,4);
    else if (preset=="Easter")  { QDate d=easterDate(y);     return (d>=ref)?d:easterDate(y+1); }
    else if (preset=="Good Friday") { QDate d=goodFridayDate(y); return (d>=ref)?d:goodFridayDate(y+1); }
    return QDate();
}

CustomTileDialog::CustomTileDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Create Custom Tile");
    setFixedWidth(540);
    setModal(true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(20, 20, 20, 20);
    vlay->setSpacing(8);

    auto addLabel = [&](const QString& t) -> QLabel* {
        auto* l = new QLabel(t, this); l->setStyleSheet(kSection); vlay->addWidget(l); return l;
    };

    // ── Type ──────────────────────────────────────────────────────────────────
    addLabel("Type");
    m_typeCombo = new QComboBox(this);
    m_typeCombo->setStyleSheet(kField);
    for (int i = 0; i < kTypeCount; ++i) m_typeCombo->addItem(kTypes[i]);
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CustomTileDialog::onTypeChanged);
    vlay->addWidget(m_typeCombo);

    // ── Special Day  (only shown when Type == Special) ───────────────────────
    m_specialDayLabel = addLabel("Special Day");
    m_specialDayCombo = new QComboBox(this);
    m_specialDayCombo->setStyleSheet(kField);
    for (int i = 0; i < kSpecialDayCount; ++i) m_specialDayCombo->addItem(kSpecialDays[i]);
    connect(m_specialDayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CustomTileDialog::onSpecialDayChanged);
    vlay->addWidget(m_specialDayCombo);

    // ── Title ─────────────────────────────────────────────────────────────────
    addLabel("Title  (required)");
    m_titleEdit = new QLineEdit(this);
    m_titleEdit->setStyleSheet(kField);
    m_titleEdit->setPlaceholderText("e.g.  My Birthday,  Movie Night…");
    vlay->addWidget(m_titleEdit);

    // ── Date ──────────────────────────────────────────────────────────────────
    m_dateLabel = addLabel("Date");
    m_dateCheck = new QCheckBox("Has a date", this);
    m_dateCheck->setStyleSheet("color:#cccccc; font-size:13px;");
    m_dateCheck->setChecked(true);
    connect(m_dateCheck, &QCheckBox::toggled, this, &CustomTileDialog::onDateToggled);
    vlay->addWidget(m_dateCheck);

    m_dateRowWidget = new QWidget(this);
    m_dateRowWidget->setAttribute(Qt::WA_TranslucentBackground);
    auto* dateRow = new QHBoxLayout(m_dateRowWidget);
    dateRow->setContentsMargins(0,0,0,0); dateRow->setSpacing(6);

    m_monthCombo = new QComboBox(this);
    m_monthCombo->setStyleSheet(kField);
    for (int i = 0; i < 12; ++i) m_monthCombo->addItem(kMonths[i]);
    m_monthCombo->setCurrentIndex(QDate::currentDate().month() - 1);
    connect(m_monthCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CustomTileDialog::onMonthChanged);
    dateRow->addWidget(m_monthCombo, 2);

    m_dayCombo = new QComboBox(this);
    m_dayCombo->setStyleSheet(kField); m_dayCombo->setFixedWidth(80);
    refreshDayCombo();
    m_dayCombo->setCurrentIndex(QDate::currentDate().day() - 1);
    dateRow->addWidget(m_dayCombo, 1);

    m_yearSpin = new QSpinBox(this);
    m_yearSpin->setStyleSheet(kField);
    m_yearSpin->setRange(2000, 2099);
    m_yearSpin->setValue(QDate::currentDate().year());
    m_yearSpin->setFixedWidth(90);
    m_yearSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    connect(m_yearSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int){ refreshDayCombo(); });
    dateRow->addWidget(m_yearSpin, 1);
    vlay->addWidget(m_dateRowWidget);

    // ── Time ──────────────────────────────────────────────────────────────────
    m_timeLabel = addLabel("Time  (optional — defaults to midnight)");
    m_timeRowWidget = new QWidget(this);
    m_timeRowWidget->setAttribute(Qt::WA_TranslucentBackground);
    auto* timeRow = new QHBoxLayout(m_timeRowWidget);
    timeRow->setContentsMargins(0,0,0,0); timeRow->setSpacing(6);

    m_hourCombo = new QComboBox(this); m_hourCombo->setStyleSheet(kField); m_hourCombo->setFixedWidth(70);
    for (int h = 1; h <= 12; ++h) m_hourCombo->addItem(QString::number(h));
    m_hourCombo->setCurrentIndex(11);
    m_minuteCombo = new QComboBox(this); m_minuteCombo->setStyleSheet(kField); m_minuteCombo->setFixedWidth(68);
    for (int m = 0; m < 60; ++m) m_minuteCombo->addItem(QString("%1").arg(m,2,10,QChar('0')));
    m_ampmCombo = new QComboBox(this); m_ampmCombo->setStyleSheet(kField); m_ampmCombo->setFixedWidth(68);
    m_ampmCombo->addItem("AM"); m_ampmCombo->addItem("PM");
    // v3.0.2 — any direct user interaction with the picker marks it dirty, so
    // it's saved as an explicit override instead of following the dynamic
    // Time Zone/theatrical default from then on.
    connect(m_hourCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]{ m_timeDirty = true; });
    connect(m_minuteCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]{ m_timeDirty = true; });
    connect(m_ampmCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]{ m_timeDirty = true; });
    timeRow->addWidget(new QLabel("Hour:", this)); timeRow->addWidget(m_hourCombo);
    timeRow->addWidget(new QLabel(":", this));      timeRow->addWidget(m_minuteCombo);
    timeRow->addWidget(m_ampmCombo); timeRow->addStretch();
    vlay->addWidget(m_timeRowWidget);

    // ── Weekday row (Weekly only) ─────────────────────────────────────────────
    m_weekdayLabel = addLabel("Weekday");
    m_weekdayRowWidget = new QWidget(this);
    m_weekdayRowWidget->setAttribute(Qt::WA_TranslucentBackground);
    auto* wdRow = new QHBoxLayout(m_weekdayRowWidget);
    wdRow->setContentsMargins(0,0,0,0); wdRow->setSpacing(6);
    m_weekdayCombo = new QComboBox(this); m_weekdayCombo->setStyleSheet(kField);
    const char* days[] = {"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
    for (const char* d : days) m_weekdayCombo->addItem(d);
    wdRow->addWidget(m_weekdayCombo); wdRow->addStretch();
    vlay->addWidget(m_weekdayRowWidget);

    // ── Day of month row (Monthly only) ──────────────────────────────────────
    m_domLabel = addLabel("Day of Month");
    m_domRowWidget = new QWidget(this);
    m_domRowWidget->setAttribute(Qt::WA_TranslucentBackground);
    auto* domRow = new QHBoxLayout(m_domRowWidget);
    domRow->setContentsMargins(0,0,0,0); domRow->setSpacing(6);
    m_domSpin = new QSpinBox(this); m_domSpin->setStyleSheet(kField);
    m_domSpin->setRange(1, 31); m_domSpin->setValue(1);
    m_domSpin->setFixedWidth(90); m_domSpin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    auto* domSufLbl = new QLabel("of each month", this); domSufLbl->setStyleSheet("color:#aaa; font-size:13px;");
    domRow->addWidget(m_domSpin); domRow->addWidget(domSufLbl); domRow->addStretch();
    vlay->addWidget(m_domRowWidget);

    // ── Loop controls ─────────────────────────────────────────────────────────
    auto* sepLoop = new QFrame(this); sepLoop->setFrameShape(QFrame::HLine); sepLoop->setStyleSheet("color:#2a2a2a;");
    vlay->addWidget(sepLoop);

    auto* loopRow = new QHBoxLayout; loopRow->setSpacing(10);
    m_loopCheck = new QCheckBox("Loop", this);
    m_loopCheck->setStyleSheet("color:#cccccc; font-size:13px; font-weight:bold;");
    m_loopCheck->setChecked(false);
    connect(m_loopCheck, &QCheckBox::toggled, this, &CustomTileDialog::onLoopToggled);
    loopRow->addWidget(m_loopCheck);

    m_loopIntervalCombo = new QComboBox(this);
    m_loopIntervalCombo->addItem("Yearly");
    m_loopIntervalCombo->addItem("Monthly");
    m_loopIntervalCombo->addItem("Weekly");
    m_loopIntervalCombo->addItem("Daily");
    m_loopIntervalCombo->setEnabled(false);
    m_loopIntervalCombo->setStyleSheet(kDisabledField);
    connect(m_loopIntervalCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CustomTileDialog::onLoopIntervalChanged);
    loopRow->addWidget(m_loopIntervalCombo, 1);
    loopRow->addStretch();
    vlay->addLayout(loopRow);

    // ── Divider ───────────────────────────────────────────────────────────────
    auto* sep = new QFrame(this); sep->setFrameShape(QFrame::HLine); sep->setStyleSheet("color:#2a2a2a;");
    vlay->addWidget(sep);

    // ── Image ─────────────────────────────────────────────────────────────────
    addLabel("Backdrop Image  (optional)");
    m_previewLabel = new QLabel(this);
    m_previewLabel->setFixedSize(500, 281);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setStyleSheet("QLabel { background:#000; border:1px solid #3a3a3a; border-radius:4px; }");
    vlay->addWidget(m_previewLabel);
    m_pathLabel = new QLabel("No image selected", this);
    m_pathLabel->setStyleSheet("color:#555; font-size:10px;");
    vlay->addWidget(m_pathLabel);

    auto* imgRow = new QHBoxLayout; imgRow->setSpacing(8);
    auto* selBtn = new QPushButton("Select Image…", this);
    selBtn->setStyleSheet("QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; } QPushButton:hover { background:#383838; }");
    connect(selBtn, &QPushButton::clicked, this, &CustomTileDialog::onSelectImage);
    imgRow->addWidget(selBtn);
    auto* clearBtn = new QPushButton("Clear", this);
    clearBtn->setStyleSheet("QPushButton { background:#2a2a2a; color:#aaa; border:1px solid #3a3a3a; "
        "border-radius:4px; padding:8px 14px; font-size:12px; } QPushButton:hover { background:#383838; color:#fff; }");
    connect(clearBtn, &QPushButton::clicked, this, &CustomTileDialog::onClearImage);
    imgRow->addWidget(clearBtn);
    imgRow->addStretch();
    vlay->addLayout(imgRow);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* sep2 = new QFrame(this); sep2->setFrameShape(QFrame::HLine); sep2->setStyleSheet("color:#2a2a2a;");
    vlay->addWidget(sep2);
    auto* btnRow = new QHBoxLayout; btnRow->setSpacing(8);
    btnRow->addStretch();
    auto* cancelBtn = new QPushButton("Cancel", this);
    cancelBtn->setStyleSheet("QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
        "border-radius:4px; padding:8px 20px; font-size:13px; } QPushButton:hover { background:#333; }");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);
    auto* saveBtn = new QPushButton("Create Tile", this);
    saveBtn->setStyleSheet("QPushButton { background:#0078d4; color:#fff; border:none; "
        "border-radius:4px; padding:8px 28px; font-size:13px; font-weight:bold; } QPushButton:hover { background:#1a8de4; }");
    connect(saveBtn, &QPushButton::clicked, this, &CustomTileDialog::onSave);
    btnRow->addWidget(saveBtn);
    vlay->addLayout(btnRow);

    // Apply initial field visibility (Type starts on "Custom" — Special Day stays hidden)
    onTypeChanged(0);
}

// =============================================================================
void CustomTileDialog::applyLoopFieldVisibility()
{
    bool looped  = m_loopCheck->isChecked();
    QString intv = looped ? m_loopIntervalCombo->currentText() : "Yearly";
    bool isSpecial = (m_typeCombo->currentText() == "Special");

    bool showDate    = !looped || intv == "Yearly";
    bool showWeekday = looped && intv == "Weekly";
    bool showDOM     = looped && intv == "Monthly";

    m_dateLabel->setVisible(showDate);
    // Hide "Has a date" checkbox for Special Day tiles — they always have a date
    m_dateCheck->setVisible(showDate && !isSpecial);
    m_dateRowWidget->setVisible(showDate);

    if (looped && intv != "Yearly") m_timeLabel->setText("Time of day");
    else                            updateTimeLabel();
    m_timeLabel->setVisible(true);
    m_timeRowWidget->setVisible(true);

    m_weekdayLabel->setVisible(showWeekday);
    m_weekdayRowWidget->setVisible(showWeekday);

    m_domLabel->setVisible(showDOM);
    m_domRowWidget->setVisible(showDOM);
}

// v3.0.2 — the actual default time for the currently-selected Type, matching
// TileData::effectiveTime()'s logic (Movie: theatrical noon vs digital zone
// shift; Show: digital zone shift; Custom/Game/Special: midnight).
QTime CustomTileDialog::computeDefaultTime() const
{
    QString type = m_typeCombo->currentText();
    if (type == "Movie" || type == "Show") return TimeZoneUtil::defaultDigitalTime();
    return QTime(0, 0);
}

// Sets the picker widgets without marking them dirty (programmatic update).
void CustomTileDialog::setPickerTime(const QTime& t)
{
    m_hourCombo->blockSignals(true);
    m_minuteCombo->blockSignals(true);
    m_ampmCombo->blockSignals(true);
    int h12 = t.hour() % 12; if (h12 == 0) h12 = 12;
    m_hourCombo->setCurrentIndex(h12 - 1);
    m_minuteCombo->setCurrentIndex(t.minute());
    m_ampmCombo->setCurrentIndex(t.hour() >= 12 ? 1 : 0);
    m_hourCombo->blockSignals(false);
    m_minuteCombo->blockSignals(false);
    m_ampmCombo->blockSignals(false);
}

// v3.0.2 — reflects the real default in both the label AND the picker itself
// (previously only the label was updated, so the picker kept showing 12:00 AM
// even when the effective default was e.g. 3:00 AM for a digital Show).
void CustomTileDialog::updateTimeLabel()
{
    QString type = m_typeCombo->currentText();
    QTime def = computeDefaultTime();
    if (type == "Movie" || type == "Show") {
        m_timeLabel->setText(
            QString("Time  (optional — defaults to %1, per your Time Zone setting)")
                .arg(def.toString("h:mm AP")));
    } else {
        m_timeLabel->setText("Time  (optional — defaults to midnight)");
    }
    if (!m_timeDirty) setPickerTime(def);
}

// =============================================================================
// Helpers
// =============================================================================
int CustomTileDialog::daysInSelectedMonth() const
{
    int month = m_monthCombo->currentIndex() + 1;
    int year  = m_yearSpin ? m_yearSpin->value() : QDate::currentDate().year();
    return QDate(year, month, 1).daysInMonth();
}

void CustomTileDialog::refreshDayCombo()
{
    int prevDay = m_dayCombo ? m_dayCombo->currentIndex() + 1 : 1;
    int days    = daysInSelectedMonth();
    m_dayCombo->blockSignals(true);
    m_dayCombo->clear();
    for (int d = 1; d <= days; ++d) m_dayCombo->addItem(QString::number(d));
    m_dayCombo->setCurrentIndex(qMin(prevDay, days) - 1);
    m_dayCombo->blockSignals(false);
}

void CustomTileDialog::onMonthChanged(int) { refreshDayCombo(); }

QDate CustomTileDialog::selectedDate() const
{
    return QDate(m_yearSpin->value(),
                 m_monthCombo->currentIndex() + 1,
                 m_dayCombo->currentIndex() + 1);
}

void CustomTileDialog::setSelectedDate(const QDate& d)
{
    if (!d.isValid()) return;
    m_monthCombo->setCurrentIndex(d.month() - 1);
    refreshDayCombo();
    m_dayCombo->setCurrentIndex(d.day() - 1);
    m_yearSpin->setValue(d.year());
}

// =============================================================================
// Slots
// =============================================================================
void CustomTileDialog::onLoopToggled(bool checked)
{
    m_loopIntervalCombo->setEnabled(checked);
    m_loopIntervalCombo->setStyleSheet(checked ? kField : kDisabledField);
    applyLoopFieldVisibility();
}

void CustomTileDialog::onLoopIntervalChanged(int) { applyLoopFieldVisibility(); }

void CustomTileDialog::onDateToggled(bool enabled)
{
    m_monthCombo->setEnabled(enabled);
    m_dayCombo->setEnabled(enabled);
    m_yearSpin->setEnabled(enabled);
    QString s = enabled ? kField : kDisabledField;
    m_monthCombo->setStyleSheet(s);
    m_dayCombo->setStyleSheet(s);
    m_yearSpin->setStyleSheet(s);
}

void CustomTileDialog::onTypeChanged(int)
{
    QString type = m_typeCombo->currentText();
    bool wasSpecial = (m_lastAppliedType == "Special");
    bool isSpecial  = (type == "Special");

    m_specialDayLabel->setVisible(isSpecial);
    m_specialDayCombo->setVisible(isSpecial);

    if (isSpecial) {
        onSpecialDayChanged(m_specialDayCombo->currentIndex());
        m_lastAppliedType = type;
        return;
    }

    if (wasSpecial) {
        // Leaving Special — undo the holiday auto-fill
        m_titleEdit->clear();
        m_loopCheck->setChecked(false);
        m_loopIntervalCombo->setEnabled(false);
        m_loopIntervalCombo->setStyleSheet(kDisabledField);
    }
    // Switching between Custom/Movie/Show/Game — leave title/date/loop as-is,
    // only the Type (and therefore mediaType) actually changes.
    m_loopIntervalCombo->setVisible(true);
    applyLoopFieldVisibility();
    m_lastAppliedType = type;
}

void CustomTileDialog::onSpecialDayChanged(int index)
{
    QString day = m_specialDayCombo->itemText(index);

    m_titleEdit->setText(day);
    m_loopCheck->setChecked(true);
    m_loopIntervalCombo->setEnabled(true);
    m_loopIntervalCombo->setStyleSheet(kField);
    m_loopIntervalCombo->setCurrentIndex(0); // Yearly
    m_loopIntervalCombo->setVisible(false);  // Special Days are always Yearly

    if (day == "Birthday") {
        m_dateCheck->setChecked(true);
        setSelectedDate(QDate(QDate::currentDate().year(), 1, 1));
        applyLoopFieldVisibility();
        return;
    }

    QDate d = nextOccurrence(day, QDate::currentDate());
    if (d.isValid()) {
        m_dateCheck->setChecked(true);
        setSelectedDate(d);
    }
    applyLoopFieldVisibility();
}

void CustomTileDialog::onSave()
{
    if (m_titleEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "Title Required", "Please enter a title for this tile.");
        return;
    }

    QString id       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool    looped   = m_loopCheck->isChecked();
    QString interval = looped ? m_loopIntervalCombo->currentText() : "Yearly";

    QDate date;
    QTime t;
    int   weekday    = 1;
    int   dayOfMonth = 1;

    auto readTime = [&]() {
        int h12 = m_hourCombo->currentIndex() + 1;
        int mins = m_minuteCombo->currentIndex();
        bool pm  = (m_ampmCombo->currentIndex() == 1);
        int h24  = (h12 % 12) + (pm ? 12 : 0);
        // For non-looped tiles, if the user never touched the time picker,
        // leave it unset so it dynamically follows the Time Zone/theatrical
        // default; for looped reminders, always save the literal time
        // (midnight is a valid explicit choice there, not a "no data" sentinel).
        t = (!looped && !m_timeDirty) ? QTime() : QTime(h24, mins);
    };

    if (!looped || interval == "Yearly") {
        date = m_dateCheck->isChecked() ? selectedDate() : QDate();
        readTime();
    } else if (interval == "Monthly") {
        dayOfMonth = m_domSpin->value();
        readTime();
        QDate today = QDate::currentDate();
        date = QDate(today.year(), today.month(), qMin(dayOfMonth, today.daysInMonth()));
        if (date <= today) date = date.addMonths(1);
        date = QDate(date.year(), date.month(), qMin(dayOfMonth, date.daysInMonth()));
    } else if (interval == "Weekly") {
        weekday = m_weekdayCombo->currentIndex() + 1;
        readTime();
        QDate today = QDate::currentDate();
        int daysAhead = (weekday - today.dayOfWeek() + 7) % 7;
        if (daysAhead == 0) daysAhead = 7;
        date = today.addDays(daysAhead);
    } else { // Daily
        readTime();
        QDateTime now = QDateTime::currentDateTime();
        QDateTime next(now.date(), t.isValid() ? t : QTime(0,0));
        if (next <= now) next = next.addDays(1);
        date = next.date();
    }

    QString type = m_typeCombo->currentText();
    QString mediaType;
    if      (type == "Movie") mediaType = "movie";
    else if (type == "Show")  mediaType = "tv";
    else if (type == "Game")  mediaType = "game";
    else                      mediaType = "custom";   // Custom or Special

    m_result.id             = id;
    m_result.tmdbId         = 0;
    m_result.mediaType      = mediaType;
    m_result.title          = m_titleEdit->text().trimmed();
    m_result.customTitle    = "";
    m_result.targetDate     = date;
    m_result.dateDisplay    = date.isValid() ? date.toString("MMMM d, yyyy") : "";
    m_result.customAirTime  = t;
    m_result.imagePath      = m_imagePath;
    m_result.statusLabel    = "";
    m_result.notified       = false;
    m_result.isLooped       = looped;
    m_result.presetType     = (type == "Special") ? m_specialDayCombo->currentText() : "Custom";
    m_result.isTheatrical   = false;   // manually created — no TMDB release-type data
    m_result.loopInterval   = interval;
    m_result.loopWeekday    = weekday;
    m_result.loopDayOfMonth = dayOfMonth;
    m_result.releaseYear    = date.isValid() ? date.year() : 0;

    if (!m_imagePath.isEmpty() &&
        (m_imagePath.contains("/custom_images/") || m_imagePath.contains("\\custom_images\\"))) {
        QFileInfo fi(m_imagePath);
        QString newPath = fi.absolutePath() + "/" + id + "_custom." + fi.suffix().toLower();
        if (QFile::rename(m_imagePath, newPath)) m_result.imagePath = newPath;
    }
    accept();
}

void CustomTileDialog::onSelectImage()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Select Backdrop Image", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.webp)");
    if (path.isEmpty()) return;
    QString copied = copyImageToAppFolder(path);
    m_imagePath = copied.isEmpty() ? path : copied;
    updatePreview(m_imagePath);
    m_pathLabel->setText(m_imagePath);
}

void CustomTileDialog::onClearImage()
{
    if (!m_imagePath.isEmpty() &&
        (m_imagePath.contains("/custom_images/") || m_imagePath.contains("\\custom_images\\")))
        QFile::remove(m_imagePath);
    m_imagePath.clear();
    m_previewLabel->clear();
    m_previewLabel->setStyleSheet("QLabel { background:#000; border:1px solid #3a3a3a; border-radius:4px; }");
    m_pathLabel->setText("No image selected");
}

QString CustomTileDialog::copyImageToAppFolder(const QString& srcPath)
{
    QString ext  = QFileInfo(srcPath).suffix().toLower();
    QString dir  = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/custom_images";
    QDir().mkpath(dir);
    QString dest = dir + "/tmp_" + QUuid::createUuid().toString(QUuid::WithoutBraces) + "." + ext;
    return QFile::copy(srcPath, dest) ? dest : QString();
}

void CustomTileDialog::updatePreview(const QString& path)
{
    if (path.isEmpty()) { m_previewLabel->clear(); return; }
    QPixmap px(path);
    if (px.isNull()) return;
    m_previewLabel->setPixmap(
        px.scaled(500, 281, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
}
