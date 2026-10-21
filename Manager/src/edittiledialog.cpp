#include "edittiledialog.h"
#include "timezoneutil.h"
#include "showoverrides.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QPixmap>
#include <QFrame>
#include <QMessageBox>
#include <QMouseEvent>
#include <QDateTime>
#include <QSet>
#include <QTimer>
#include <QRegularExpression>
#include <QDesktopServices>
#include <QEnterEvent>
#include <QResizeEvent>

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

// Reset button styles: greyed = unchanged, active = something was modified
static const char* kResetGreyed =
    "QPushButton { background:#1a1a1a; color:#444; border:1px solid #252525; "
    "border-radius:4px; padding:8px 14px; font-size:12px; }";
static const char* kResetActive =
    "QPushButton { background:#2a2a2a; color:#aaa; border:1px solid #3a3a3a; "
    "border-radius:4px; padding:8px 14px; font-size:12px; }"
    "QPushButton:hover { background:#383838; color:#fff; }";

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

static const QSet<QString> kHolidayPresets = {
    "Christmas","Easter","Halloween","Thanksgiving","New Year","April Fools",
    "Good Friday","Veterans Day","Independence Day","Birthday"
};

// v3.0.0 — Type is derived from the tile's real mediaType (movie/tv/game came
// either from TMDB or from a tile manually tagged that way in this dialog),
// falling back to Special/Custom based on the stored presetType.
static QString typeForData(const TileData& d)
{
    if (d.mediaType == "movie") return "Movie";
    if (d.mediaType == "tv")    return "Show";
    if (d.mediaType == "game")  return "Game";
    return kHolidayPresets.contains(d.presetType) ? "Special" : "Custom";
}

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
        auto thanksgiving = [](int y) -> QDate {
            QDate d(y, 11, 1);
            int dow = d.dayOfWeek();
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

// =============================================================================
//  ImagePreviewWidget — v3.1.0. See header for the overview. The widget
//  itself has no idea what a TileData is; the dialog owns all the state and
//  just tells it what to show and which controls make sense right now.
// =============================================================================
ImagePreviewWidget::ImagePreviewWidget(QWidget* parent) : QWidget(parent)
{
    setFixedSize(500, 281);
    setMouseTracking(true);

    m_imageLabel = new QLabel(this);
    m_imageLabel->setGeometry(0, 0, 500, 281);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setStyleSheet(
        "QLabel { background:#111; border:1px solid #3a3a3a; border-radius:4px; }");

    auto makeOverlayBtn = [this](const QString& text, int w, int h) {
        auto* b = new QPushButton(text, this);
        b->setFixedSize(w, h);
        b->setCursor(Qt::PointingHandCursor);
        b->setVisible(false);
        return b;
    };
    // v3.1.2 fix #4 — delete now shares the same square, bordered look as
    // the arrows (was transparent-until-hover); bigger box (40x40, was
    // 34x34) plus explicit zero padding keeps the ❌ glyph from clipping.
    m_leftBtn   = makeOverlayBtn("<", 46, 72);
    m_rightBtn  = makeOverlayBtn(">", 46, 72);
    m_deleteBtn = makeOverlayBtn(QString::fromUtf8("\xe2\x9d\x8c"), 40, 40);

    const QString navStyle =
        "QPushButton { background:rgba(20,20,20,175); color:#fff; "
        "border:1px solid rgba(255,255,255,70); border-radius:6px; "
        "font-size:30px; font-weight:bold; padding:0px; }"
        "QPushButton:hover { background:rgba(0,120,212,205); border-color:rgba(255,255,255,130); }";
    m_leftBtn->setStyleSheet(navStyle);
    m_rightBtn->setStyleSheet(navStyle);
    m_deleteBtn->setStyleSheet(
        "QPushButton { background:rgba(20,20,20,175); color:#fff; "
        "border:1px solid rgba(255,255,255,70); border-radius:6px; "
        "font-size:20px; padding:0px; }"
        "QPushButton:hover { background:rgba(190,40,40,210); border-color:rgba(255,255,255,130); }");
    m_deleteBtn->setToolTip("Delete this image");

    connect(m_leftBtn,   &QPushButton::clicked, this, &ImagePreviewWidget::previousRequested);
    connect(m_rightBtn,  &QPushButton::clicked, this, &ImagePreviewWidget::nextRequested);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ImagePreviewWidget::deleteRequested);

    layoutOverlays();
}

void ImagePreviewWidget::layoutOverlays()
{
    int w = width(), h = height();
    m_leftBtn->move(8, (h - m_leftBtn->height()) / 2);
    m_rightBtn->move(w - 8 - m_rightBtn->width(), (h - m_rightBtn->height()) / 2);
    m_deleteBtn->move(w - 8 - m_deleteBtn->width(), 8);
}

void ImagePreviewWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_imageLabel->setGeometry(0, 0, width(), height());
    layoutOverlays();
}

void ImagePreviewWidget::setImage(const QString& path)
{
    QPixmap px = path.isEmpty() ? QPixmap() : QPixmap(path);
    if (px.isNull()) {
        m_imageLabel->clear();
        return;
    }
    m_imageLabel->setPixmap(px.scaled(width(), height(),
                             Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
}

void ImagePreviewWidget::setNavAllowed(bool showLeft, bool showRight)
{
    m_navLeftAllowed  = showLeft;
    m_navRightAllowed = showRight;
    // v3.1.1 fix #2 — refresh immediately (was only re-applied on the next
    // hover-enter, so e.g. clicking "next" to reach the last image left the
    // now-invalid right arrow showing until you moved off and back onto
    // the preview).
    updateOverlayVisibility(m_hovering);
}

void ImagePreviewWidget::setDeleteAllowed(bool allowed)
{
    m_deleteAllowed = allowed;
    updateOverlayVisibility(m_hovering);
}

void ImagePreviewWidget::updateOverlayVisibility(bool hovering)
{
    m_leftBtn->setVisible(hovering && m_navLeftAllowed);
    m_rightBtn->setVisible(hovering && m_navRightAllowed);
    m_deleteBtn->setVisible(hovering && m_deleteAllowed);
}

// =============================================================================
//  V5.4.21 — clicking the preview is the same as pressing "Select Image…".
//
//  The arrows and the delete button are real child widgets sitting on top of
//  this one, so a click that lands on any of them is consumed by that button
//  and never arrives here. That is exactly the wanted rule — "anywhere that
//  isn't a control" — without having to work out where the controls are.
// =============================================================================
void ImagePreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) emit selectRequested();
    QWidget::mousePressEvent(event);
}

void ImagePreviewWidget::enterEvent(QEnterEvent* event)
{
    QWidget::enterEvent(event);
    m_hovering = true;
    updateOverlayVisibility(true);
}

void ImagePreviewWidget::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    m_hovering = false;
    updateOverlayVisibility(false);
}

EditTileDialog::EditTileDialog(const TileData& data, QWidget* parent)
    : QDialog(parent), m_data(data)
{
    setWindowTitle("Edit Tile");
    setMinimumWidth(540);
    setMaximumWidth(540);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setModal(true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // Store initial values for dirty-checking reset buttons.
    // m_initDate is always the TMDB/original targetDate — so the reset button
    // stays active whenever the user has overridden it with a custom date.
    m_initTitle     = data.customTitle.isEmpty() ? data.title : data.customTitle;
    m_initDate      = data.targetDate;

    auto* vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(20, 16, 20, 16);
    vlay->setSpacing(4);

    auto addLabel = [&](const QString& t) -> QLabel* {
        auto* l = new QLabel(t, this);
        l->setStyleSheet("color:#888; font-size:11px; margin-top:6px;");
        vlay->addWidget(l); return l;
    };

    // ── Type ──────────────────────────────────────────────────────────────────
    addLabel("Type");
    m_typeCombo = new QComboBox(this);
    for (int i = 0; i < kTypeCount; ++i) m_typeCombo->addItem(kTypes[i]);
    {
        int idx = m_typeCombo->findText(typeForData(data));
        m_typeCombo->setCurrentIndex(idx < 0 ? 0 : idx);
        m_lastAppliedType = (idx < 0) ? "Custom" : m_typeCombo->itemText(idx);
    }
    // Tiles linked to an external database (tmdbId > 0 — TMDB for movies/
    // shows, IGDB for games) keep the type they were added as — changing it
    // would break the refresh/link, so it's locked here rather than left
    // free to edit.
    bool isApiLinkedTile = (data.tmdbId > 0);
    m_typeCombo->setEnabled(!isApiLinkedTile);
    m_typeCombo->setStyleSheet(isApiLinkedTile ? kDisabledField : kField);
    if (isApiLinkedTile)
        m_typeCombo->setToolTip(data.mediaType == "game"
            ? "Locked — this tile is linked to IGDB."
            : "Locked — this tile is linked to TMDB.");
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EditTileDialog::onTypeChanged);
    vlay->addWidget(m_typeCombo);

    // ── Special Day  (only shown when Type == Special) ───────────────────────
    m_specialDayLabel = addLabel("Special Day");
    m_specialDayCombo = new QComboBox(this);
    m_specialDayCombo->setStyleSheet(kField);
    for (int i = 0; i < kSpecialDayCount; ++i) m_specialDayCombo->addItem(kSpecialDays[i]);
    {
        int idx = m_specialDayCombo->findText(data.presetType);
        m_specialDayCombo->setCurrentIndex(idx < 0 ? 0 : idx);
    }
    connect(m_specialDayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EditTileDialog::onSpecialDayChanged);
    vlay->addWidget(m_specialDayCombo);
    m_specialDayLabel->setVisible(m_typeCombo->currentText() == "Special");
    m_specialDayCombo->setVisible(m_typeCombo->currentText() == "Special");

    // ── Display Name ──────────────────────────────────────────────────────────
    addLabel("Display Name");
    auto* titleRow = new QHBoxLayout; titleRow->setSpacing(6);
    m_titleEdit = new QLineEdit(this);
    m_titleEdit->setStyleSheet(kField);
    m_titleEdit->setText(m_initTitle);
    m_titleEdit->setPlaceholderText(data.title);
    titleRow->addWidget(m_titleEdit, 1);
    m_rTitleBtn = new QPushButton("Reset", this);
    m_rTitleBtn->setFixedWidth(64);
    connect(m_rTitleBtn, &QPushButton::clicked, this, &EditTileDialog::onResetTitle);
    titleRow->addWidget(m_rTitleBtn);
    vlay->addLayout(titleRow);

    // ── Season / Episode  (V5, TV only) ───────────────────────────────────────
    //
    // Lets a show be pointed at an episode the data source doesn't know about
    // yet — the common case being a season that has just ended, where you
    // already know the next season starts at episode 1 on some announced
    // date, but no source lists it. Set season and episode here, set the date
    // below, and the tile counts down to it.
    //
    // These are an OVERRIDE, not a permanent edit: the moment a real fetched
    // date arrives for this show, it replaces both the numbers and the date,
    // exactly like every other scraped field. That's deliberate — the source
    // catching up should always win over a manual guess.
    m_seasonEpisodeLabel = addLabel("Episode Numbering");

    // A caption centred over each box — three bare number fields side by
    // side gave no indication of which was which.
    auto* seCaptions = new QHBoxLayout; seCaptions->setSpacing(6);
    auto addCaption = [&](const QString& text) {
        auto* l = new QLabel(text, this);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
        seCaptions->addWidget(l, 1);
        m_seCaptions << l;
    };
    addCaption("Season");
    addCaption("Episode");
    addCaption("Total");
    // Empty stand-in matching the Reset button's width, so the three
    // captions stay centred over their own boxes rather than drifting.
    auto* capSpacer = new QLabel(QString(), this);
    capSpacer->setFixedWidth(64);
    seCaptions->addWidget(capSpacer);
    m_seCaptions << capSpacer;
    vlay->addLayout(seCaptions);

    auto* seRow = new QHBoxLayout; seRow->setSpacing(6);

    auto makeNumBox = [this](const QString& tip) {
        auto* box = new QSpinBox(this);
        box->setRange(0, 999);
        box->setStyleSheet(kField);
        box->setToolTip(tip);
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);   // digits only, no arrows
        box->setAlignment(Qt::AlignCenter);
        return box;
    };

    m_seasonSpin  = makeNumBox("Season number. 0 in season or episode clears the label entirely.");
    m_episodeSpin = makeNumBox("Episode number within that season.");
    m_totalSpin   = makeNumBox("Total episodes this season. Shown as the \"/08\" part of "
                               "S04E04/08, and used to work out how many episodes are left.");
    seRow->addWidget(m_seasonSpin,  1);
    seRow->addWidget(m_episodeSpin, 1);
    seRow->addWidget(m_totalSpin,   1);

    // Its own Reset, separate from the title/date/time ones: this restores
    // ONLY these three boxes to what the data source last reported, without
    // touching anything else in the dialog.
    m_rSeasonEpisodeBtn = new QPushButton("Reset", this);
    m_rSeasonEpisodeBtn->setFixedWidth(64);
    connect(m_rSeasonEpisodeBtn, &QPushButton::clicked,
            this, &EditTileDialog::onResetSeasonEpisode);
    seRow->addWidget(m_rSeasonEpisodeBtn);
    vlay->addLayout(seRow);

    // Greys out when there's nothing to undo, matching the other Reset
    // buttons, and re-enables the moment any of the three boxes differ from
    // what the data source last reported.
    for (QSpinBox* box : { m_seasonSpin, m_episodeSpin, m_totalSpin })
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &EditTileDialog::refreshSeasonEpisodeResetState);

    {
        // Prefill from whatever the tile currently shows. A multi-episode
        // label ("S04E01+E02+E03") prefills with its LAST episode, since
        // that's where the season actually stands.
        EpisodeLabel el = parseEpisodeLabel(data.statusLabel);
        m_seasonSpin->setValue(el.valid ? el.season : 0);
        m_episodeSpin->setValue(el.valid ? el.lastEpisode : 0);
        m_totalSpin->setValue(data.seasonEpisodeCount);
        m_initSeason  = m_seasonSpin->value();
        m_initEpisode = m_episodeSpin->value();
        m_initTotal   = m_totalSpin->value();

        // V5 — Reset targets what the SOURCE last reported, not what the
        // tile currently shows. For an overridden tile those differ, and
        // using the current values made Reset a no-op (it restored the
        // override to itself). officialStatusLabel is the stored copy.
        EpisodeLabel off = parseEpisodeLabel(data.officialStatusLabel);
        if (off.valid) {
            m_sourceSeason  = off.season;
            m_sourceEpisode = off.lastEpisode;
            m_sourceTotal   = data.officialSeasonEpisodeCount;
        } else {
            // No source values recorded yet (a tile added before V5.0.4, or
            // one that has never refreshed) — the current values are the
            // best available baseline.
            m_sourceSeason  = m_initSeason;
            m_sourceEpisode = m_initEpisode;
            m_sourceTotal   = m_initTotal;
        }
        m_startedOverridden = data.episodeOverride;
    }

    bool isShow = (data.mediaType == "tv");
    m_seasonEpisodeLabel->setVisible(isShow);
    m_seasonSpin->setVisible(isShow);
    m_episodeSpin->setVisible(isShow);
    m_totalSpin->setVisible(isShow);
    m_rSeasonEpisodeBtn->setVisible(isShow);
    for (QLabel* l : std::as_const(m_seCaptions)) l->setVisible(isShow);
    refreshSeasonEpisodeResetState();   // starts greyed until something changes

    // ── Date ──────────────────────────────────────────────────────────────────
    m_dateLabel = addLabel("Date");
    bool hasDate = data.hasDate();  // respects noDateOverride
    m_dateCheckState = hasDate;
    m_dateCheck = new QCheckBox("Has a date", this);
    m_dateCheck->setStyleSheet("color:#cccccc; font-size:13px;");
    m_dateCheck->setChecked(hasDate);
    connect(m_dateCheck, &QCheckBox::toggled, this, &EditTileDialog::onDateToggled);
    vlay->addWidget(m_dateCheck);

    QDate showDate = data.customDate.isValid() ? data.customDate
                   : data.targetDate.isValid() ? data.targetDate
                   : QDate::currentDate();

    m_dateRowWidget = new QWidget(this);
    m_dateRowWidget->setAttribute(Qt::WA_TranslucentBackground);
    auto* dateRow = new QHBoxLayout(m_dateRowWidget);
    dateRow->setContentsMargins(0,0,0,0); dateRow->setSpacing(6);

    m_monthCombo = new QComboBox(this);
    m_monthCombo->setStyleSheet(kField);
    for (int i = 0; i < 12; ++i) m_monthCombo->addItem(kMonths[i]);
    m_monthCombo->setCurrentIndex(showDate.month() - 1);
    m_monthCombo->setEnabled(hasDate);
    connect(m_monthCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EditTileDialog::onMonthChanged);
    dateRow->addWidget(m_monthCombo, 2);

    m_dayCombo = new QComboBox(this);
    m_dayCombo->setStyleSheet(kField);
    m_dayCombo->setFixedWidth(80);
    m_dayCombo->setEnabled(hasDate);
    refreshDayCombo();
    m_dayCombo->setCurrentIndex(showDate.day() - 1);
    dateRow->addWidget(m_dayCombo, 1);

    m_yearSpin = new QSpinBox(this);
    m_yearSpin->setStyleSheet(kField);
    m_yearSpin->setRange(2000, 2099);
    m_yearSpin->setValue(showDate.year());
    m_yearSpin->setFixedWidth(90);
    m_yearSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_yearSpin->setEnabled(hasDate);
    connect(m_yearSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int){ refreshDayCombo(); updateResetButtons(); });
    dateRow->addWidget(m_yearSpin, 1);

    m_rDateBtn = new QPushButton("Reset", this);
    m_rDateBtn->setFixedWidth(64);
    connect(m_rDateBtn, &QPushButton::clicked, this, &EditTileDialog::onResetDate);
    dateRow->addWidget(m_rDateBtn);
    vlay->addWidget(m_dateRowWidget);

    // ── Time ─────────────────────────────────────────────────────────────────
    m_timeLabel = addLabel("Time  (optional — defaults to midnight)");
    m_timeRowWidget = new QWidget(this);
    m_timeRowWidget->setAttribute(Qt::WA_TranslucentBackground);
    auto* timeRow = new QHBoxLayout(m_timeRowWidget);
    timeRow->setContentsMargins(0,0,0,0); timeRow->setSpacing(6);

    m_hourCombo = new QComboBox(this); m_hourCombo->setStyleSheet(kField); m_hourCombo->setFixedWidth(70);
    for (int h = 1; h <= 12; ++h) m_hourCombo->addItem(QString::number(h));
    m_minuteCombo = new QComboBox(this); m_minuteCombo->setStyleSheet(kField); m_minuteCombo->setFixedWidth(68);
    for (int m = 0; m < 60; ++m) m_minuteCombo->addItem(QString("%1").arg(m, 2, 10, QChar('0')));
    m_ampmCombo = new QComboBox(this); m_ampmCombo->setStyleSheet(kField); m_ampmCombo->setFixedWidth(68);
    m_ampmCombo->addItem("AM"); m_ampmCombo->addItem("PM");

    // v3.0.2 — if there's no explicit saved time, show the REAL computed
    // default (Time Zone-aware for Movie/Show, noon for theatrical) instead
    // of always defaulting the picker display to midnight.
    m_timeExplicit = data.customAirTime.isValid();
    QTime showTime = m_timeExplicit ? data.customAirTime : computeDefaultTime();
    m_initTime = showTime;

    int h12 = showTime.hour() % 12; if (h12 == 0) h12 = 12;
    m_hourCombo->setCurrentIndex(h12 - 1);
    m_minuteCombo->setCurrentIndex(showTime.minute());
    m_ampmCombo->setCurrentIndex(showTime.hour() >= 12 ? 1 : 0);

    timeRow->addWidget(new QLabel("Hour:", this)); timeRow->addWidget(m_hourCombo);
    timeRow->addWidget(new QLabel(":", this));      timeRow->addWidget(m_minuteCombo);
    timeRow->addWidget(m_ampmCombo);

    // V5 — flag a wrong air time, sitting with the time controls it's about
    // rather than off with the external links.
    //
    // TV only: no source publishes a per-title air time for a film or a
    // game, so the button would be reporting the absence of something that
    // was never going to exist.
    //
    // One click, no typing — it sends which show and nothing else. The relay
    // counts one report per installation per show, so pressing it repeatedly
    // achieves nothing; it switches to "Report Sent" immediately so that's
    // visible rather than looking ignored.
    if (data.mediaType == "tv" && (data.tvmazeId > 0 || data.tmdbId > 0)) {
        auto* reportBtn = new QPushButton("Wrong Air Time?", this);
        reportBtn->setCursor(Qt::PointingHandCursor);
        reportBtn->setStyleSheet(
            "QPushButton { background:#2a2a2a; color:#aaa; border:1px solid #3a3a3a; "
            "border-radius:4px; padding:8px 14px; font-size:12px; }"
            "QPushButton:hover { background:#3a2f16; color:#e0b050; }");
        reportBtn->setToolTip(
            "Tell the developer this show's air time looks wrong.\n"
            "Sends only the show name — nothing else, and only once.");
        connect(reportBtn, &QPushButton::clicked, this, [reportBtn, data]{
            ShowOverrides::instance().report(data);
            reportBtn->setEnabled(false);
            reportBtn->setText("Report Sent");
            reportBtn->setStyleSheet(
                "QPushButton { background:#1a1a1a; color:#557a55; "
                "border:1px solid #252525; border-radius:4px; "
                "padding:8px 14px; font-size:12px; }");
        });
        timeRow->addSpacing(8);
        timeRow->addWidget(reportBtn);
    }

    timeRow->addStretch();
    m_rTimeBtn = new QPushButton("Reset", this);
    m_rTimeBtn->setFixedWidth(64);
    m_rTimeBtn->setToolTip("Reset to default");
    connect(m_rTimeBtn, &QPushButton::clicked, this, &EditTileDialog::onResetTime);
    timeRow->addWidget(m_rTimeBtn);
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
    m_weekdayCombo->setCurrentIndex(qMax(0, data.loopWeekday - 1));
    wdRow->addWidget(m_weekdayCombo); wdRow->addStretch();
    vlay->addWidget(m_weekdayRowWidget);

    // ── Day of month row (Monthly only) ──────────────────────────────────────
    m_domLabel = addLabel("Day of Month");
    m_domRowWidget = new QWidget(this);
    m_domRowWidget->setAttribute(Qt::WA_TranslucentBackground);
    auto* domRow = new QHBoxLayout(m_domRowWidget);
    domRow->setContentsMargins(0,0,0,0); domRow->setSpacing(6);
    m_domSpin = new QSpinBox(this); m_domSpin->setStyleSheet(kField);
    m_domSpin->setRange(1, 31); m_domSpin->setValue(qMax(1, data.loopDayOfMonth));
    m_domSpin->setFixedWidth(90); m_domSpin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    auto* domSufLbl = new QLabel("of each month", this);
    domSufLbl->setStyleSheet("color:#aaa; font-size:13px;");
    domRow->addWidget(m_domSpin); domRow->addWidget(domSufLbl); domRow->addStretch();
    vlay->addWidget(m_domRowWidget);

    // ── Loop controls ─────────────────────────────────────────────────────────
    auto* sepLoop = new QFrame(this);
    sepLoop->setFrameShape(QFrame::HLine); sepLoop->setStyleSheet("color:#2a2a2a;");
    vlay->addWidget(sepLoop);

    auto* loopRow = new QHBoxLayout; loopRow->setSpacing(10);
    m_loopCheck = new QCheckBox("Loop", this);
    m_loopCheck->setStyleSheet("color:#cccccc; font-size:13px; font-weight:bold;");
    m_loopCheck->setChecked(data.isLooped);
    connect(m_loopCheck, &QCheckBox::toggled, this, &EditTileDialog::onLoopToggled);
    loopRow->addWidget(m_loopCheck);

    m_loopIntervalCombo = new QComboBox(this);
    m_loopIntervalCombo->addItem("Yearly");
    m_loopIntervalCombo->addItem("Monthly");
    m_loopIntervalCombo->addItem("Weekly");
    m_loopIntervalCombo->addItem("Daily");
    {
        int idx = m_loopIntervalCombo->findText(data.loopInterval);
        m_loopIntervalCombo->setCurrentIndex(idx < 0 ? 0 : idx);
    }
    bool loopEnabled = data.isLooped;
    m_loopIntervalCombo->setEnabled(loopEnabled);
    m_loopIntervalCombo->setStyleSheet(loopEnabled ? kField : kDisabledField);
    // Hide interval picker for holiday presets (always yearly)
    m_loopIntervalCombo->setVisible(!kHolidayPresets.contains(data.presetType));
    connect(m_loopIntervalCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EditTileDialog::onLoopIntervalChanged);
    loopRow->addWidget(m_loopIntervalCombo, 1);
    loopRow->addStretch();
    vlay->addLayout(loopRow);

    // ── View online  (V5) ─────────────────────────────────────────────────────
    //
    // Both links were already stored on every tile and nothing in the app
    // ever opened them. One button per source rather than a single "view
    // online": they're different sites with different information, and a
    // tile can legitimately have both.
    {
        auto* linkRow = new QHBoxLayout; linkRow->setSpacing(6);
        auto makeLinkBtn = [this, &linkRow](const QString& label, const QString& url) {
            if (url.isEmpty()) return;
            auto* b = new QPushButton(label, this);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(
                "QPushButton { background:#2a2a2a; color:#aaa; border:1px solid #3a3a3a; "
                "border-radius:4px; padding:7px 14px; font-size:12px; }"
                "QPushButton:hover { background:#383838; color:#fff; }");
            b->setToolTip(url);
            connect(b, &QPushButton::clicked, this, [url]{ QDesktopServices::openUrl(QUrl(url)); });
            linkRow->addWidget(b);
        };
        makeLinkBtn("View on TMDB",   data.tmdbUrl);
        makeLinkBtn("View on TVmaze", data.tvmazeUrl);

        if (linkRow->count() > 0) {
            addLabel("View Online");
            linkRow->addStretch();
            vlay->addLayout(linkRow);
        } else {
            delete linkRow;
        }
    }

    // ── Divider ───────────────────────────────────────────────────────────────
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine); sep->setStyleSheet("color:#2a2a2a;");
    vlay->addWidget(sep);

    // ── Backdrop Image ────────────────────────────────────────────────────────
    m_backdropHeadingLabel = addLabel("Backdrop Image");
    m_previewWidget = new ImagePreviewWidget(this);
    connect(m_previewWidget, &ImagePreviewWidget::previousRequested, this, &EditTileDialog::onPreviousImage);
    connect(m_previewWidget, &ImagePreviewWidget::nextRequested,     this, &EditTileDialog::onNextImage);
    connect(m_previewWidget, &ImagePreviewWidget::deleteRequested,   this, &EditTileDialog::onDeleteImage);
    connect(m_previewWidget, &ImagePreviewWidget::selectRequested,   this, &EditTileDialog::onSelectImage);
    m_previewWidget->setCursor(Qt::PointingHandCursor);
    m_previewWidget->setToolTip("Click to choose a different image");
    vlay->addWidget(m_previewWidget);

    m_pathLabel = new QLabel(this);
    m_pathLabel->setStyleSheet("color:#555; font-size:10px;");
    vlay->addWidget(m_pathLabel);

    // v3.1.0 — working copies of the tile's image history for this session.
    m_fetchedPath = data.fetchedImagePath;
    m_customPaths = data.customImagePaths;
    {
        QStringList imgs = allImages();
        m_previewIndex = imgs.indexOf(data.imagePath);
        if (m_previewIndex < 0) m_previewIndex = imgs.isEmpty() ? -1 : imgs.size() - 1;
    }
    refreshImagePreview();

    auto* imgRow = new QHBoxLayout; imgRow->setSpacing(8);
    auto* selBtn = new QPushButton("Select Image…", this);
    selBtn->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }");
    connect(selBtn, &QPushButton::clicked, this, &EditTileDialog::onSelectImage);
    imgRow->addWidget(selBtn);
    m_refetchImageBtn = new QPushButton("Refetch Image", this);
    connect(m_refetchImageBtn, &QPushButton::clicked, this, &EditTileDialog::onRefetchImage);
    imgRow->addWidget(m_refetchImageBtn);
    imgRow->addStretch();
    vlay->addLayout(imgRow);
    updateRefetchButtonState();   // v3.1.3 fix #1 — sets enabled state + tooltip

    // ── Bottom buttons ────────────────────────────────────────────────────────
    auto* sep2 = new QFrame(this);
    sep2->setFrameShape(QFrame::HLine); sep2->setStyleSheet("color:#2a2a2a;");
    vlay->addWidget(sep2);
    auto* btnRow = new QHBoxLayout; btnRow->setSpacing(8);
    auto* removeBtn = new QPushButton("Remove Tile", this);
    removeBtn->setStyleSheet(
        "QPushButton { background:#4a1515; color:#ff8888; border:1px solid #662222; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#661818; }");
    connect(removeBtn, &QPushButton::clicked, this, &EditTileDialog::onRemove);
    btnRow->addWidget(removeBtn);

    // V5.4.19 — Customize Colors, where Test Notification used to be. Test
    // Notification is still on the tile's right-click menu, which is where a
    // diagnostic belongs; this row is for things people actually reach for.
    //
    // It closes this dialog and opens the colour menu, with no way back. A
    // return path would mean holding this dialog open behind the other one and
    // deciding what happens if a colour is saved while the edits behind it are
    // not — two dialogs deep for a tile is more clutter than it removes.
    auto* colorsBtn = new QPushButton("🎨  Customize Colors", this);
    colorsBtn->setStyleSheet(
        "QPushButton { background:#1a2333; color:#8fb8ff; border:1px solid #2d4a7a; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#223050; }");
    connect(colorsBtn, &QPushButton::clicked, this, [this](){
        // V5.4.20 — leaving for the colour menu closes this dialog, so anything
        // typed here would be lost without asking. Only asked when something
        // actually changed: a guard that appears every time is one people learn
        // to dismiss without reading.
        if (hasUnsavedChanges()) {
            QMessageBox box(this);
            box.setWindowTitle("Save changes?");
            box.setText("Save your changes to this tile before opening the color menu?");
            box.setIcon(QMessageBox::Question);
            QPushButton* saveBtn    = box.addButton("Save", QMessageBox::AcceptRole);
            QPushButton* discardBtn = box.addButton("Don't Save", QMessageBox::DestructiveRole);
            box.addButton("Cancel", QMessageBox::RejectRole);
            box.setDefaultButton(saveBtn);
            box.exec();

            if (box.clickedButton() == saveBtn) {
                m_wantsCustomizeColors = true;
                onSave();               // same path the Save button takes
                return;
            }
            if (box.clickedButton() != discardBtn) return;   // Cancel — stay here
        }
        // Nothing to lose (or the user chose to lose it): reject(), so the
        // caller applies nothing and only opens the colour menu.
        m_wantsCustomizeColors = true;
        reject();
    });
    btnRow->addWidget(colorsBtn);

    btnRow->addStretch();
    auto* cancelBtn = new QPushButton("Cancel", this);
    cancelBtn->setStyleSheet(
        "QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
        "border-radius:4px; padding:8px 20px; font-size:13px; }"
        "QPushButton:hover { background:#333; }");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);
    auto* saveBtn = new QPushButton("Save", this);
    saveBtn->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; "
        "border-radius:4px; padding:8px 28px; font-size:13px; font-weight:bold; }"
        "QPushButton:hover { background:#1a8de4; }");
    connect(saveBtn, &QPushButton::clicked, this, &EditTileDialog::onSave);
    btnRow->addWidget(saveBtn);
    vlay->addLayout(btnRow);

    // Connect change signals for reset button dirty-checking
    connect(m_titleEdit, &QLineEdit::textChanged,
            this, &EditTileDialog::updateResetButtons);
    connect(m_monthCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EditTileDialog::updateResetButtons);
    connect(m_dayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EditTileDialog::updateResetButtons);
    // v3.1.1 fix #1 — a single connection per time combo, in order: mark it
    // explicit FIRST, then recompute the reset button. Previously these were
    // two separate connections on the same signal (one to updateResetButtons,
    // one setting m_timeExplicit); Qt fires same-signal connections in the
    // order they were made, so updateResetButtons always ran first and saw
    // the OLD (still-false) value — the button only looked right after a
    // SECOND combo change caused the first one's flag update to finally be
    // visible in time.
    auto onTimeComboChanged = [this] { m_timeExplicit = true; updateResetButtons(); };
    connect(m_hourCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged), this, onTimeComboChanged);
    connect(m_minuteCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onTimeComboChanged);
    connect(m_ampmCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged), this, onTimeComboChanged);

    applyLoopFieldVisibility();
    updateResetButtons();

    // V5.4.20 — the baseline for hasUnsavedChanges(). Taken last, once every
    // widget holds the value it will show, so simply opening the dialog never
    // counts as a change.
    m_openingSignature = stateSignature();
}

// =============================================================================
//  stateSignature — everything the user can change in this dialog, as one
//  string, so "has anything been touched" is a comparison rather than a list.
//
//  V5.4.20. Deliberately built by walking this dialog's OWN widgets instead of
//  comparing each getter against the tile: a field-by-field comparison is the
//  same hand-maintained list that has already caused five bugs in this
//  codebase, and it would go quietly wrong the first time somebody adds a row
//  and doesn't think of this function. A new QLineEdit or QComboBox is picked
//  up here automatically.
//
//  The few things that aren't widget values — the reset buttons and the image
//  choice — are added explicitly, because they change what Save will do
//  without changing anything findChildren can see.
// =============================================================================
QString EditTileDialog::stateSignature() const
{
    QStringList bits;
    for (const QLineEdit* w  : findChildren<const QLineEdit*>())  bits << w->text();
    for (const QCheckBox* w  : findChildren<const QCheckBox*>())  bits << QString::number(w->isChecked());
    for (const QComboBox* w  : findChildren<const QComboBox*>())  bits << QString::number(w->currentIndex());
    for (const QSpinBox* w   : findChildren<const QSpinBox*>())   bits << QString::number(w->value());

    bits << selectedImagePath()
         << m_customPaths.join(QLatin1Char('|'))
         << QString::number(m_wantsForcedRefetch)
         << QString::number(m_titleReset)
         << QString::number(m_dateReset)
         << QString::number(m_timeReset)
         << QString::number(m_seasonEpisodeReset);

    // Unit separator: it cannot appear in a title, a path or a number, so no
    // combination of values can accidentally produce another combination's
    // signature.
    return bits.join(QLatin1Char(''));
}

// =============================================================================
QString EditTileDialog::presetType() const
{
    return (m_typeCombo->currentText() == "Special") ? m_specialDayCombo->currentText() : "Custom";
}

QString EditTileDialog::mediaType() const
{
    QString type = m_typeCombo->currentText();
    if (type == "Movie") return "movie";
    if (type == "Show")  return "tv";
    if (type == "Game")  return "game";
    return "custom";   // Custom or Special
}

// =============================================================================
void EditTileDialog::updateResetButtons()
{
    // Title dirty: current text differs from TMDB original title
    bool titleDirty = (m_titleEdit->text().trimmed() != m_data.title);
    m_rTitleBtn->setStyleSheet(titleDirty ? kResetActive : kResetGreyed);
    m_rTitleBtn->setEnabled(titleDirty);

    // Date dirty: stored customDate differs from original fetched date
    bool dateDirty = (m_data.customDate.isValid() && m_data.customDate != m_data.targetDate)
                  || (m_dateRowWidget->isVisible() && selectedDate() != m_initDate);
    m_rDateBtn->setStyleSheet(dateDirty ? kResetActive : kResetGreyed);
    m_rDateBtn->setEnabled(dateDirty);

    // Time dirty: v3.0.4 fix — reflects whether an explicit time override is
    // currently in play (existing, or set this session), not whether the
    // picker has changed since the dialog opened. Previously this compared
    // against a baseline that was itself set to the existing override at
    // open time, so a tile that ALREADY had a custom time always looked
    // "unchanged" and the button stayed greyed out.
    m_rTimeBtn->setStyleSheet(m_timeExplicit ? kResetActive : kResetGreyed);
    m_rTimeBtn->setEnabled(m_timeExplicit);

    // v3.1.0 — "Refetch Image" isn't a "did something change" reset anymore;
    // it's always available whenever the tile has a TMDB link to fetch from
    // (set once at construction, not re-evaluated here).
}

// =============================================================================
void EditTileDialog::applyLoopFieldVisibility()
{
    bool looped  = m_loopCheck->isChecked();
    QString intv = looped ? m_loopIntervalCombo->currentText() : "Yearly";
    bool isHoliday = (m_typeCombo->currentText() == "Special");

    // If "has a date" checkbox is unchecked, hide everything below it
    bool dateEnabled = isHoliday || m_dateCheckState;

    bool showDate    = dateEnabled && (!looped || intv == "Yearly");
    bool showWeekday = dateEnabled && looped && intv == "Weekly";
    bool showDOM     = dateEnabled && looped && intv == "Monthly";

    // "Has a date" checkbox hidden for Special Day tiles (always have a date)
    m_dateCheck->setVisible(!isHoliday && (!looped || intv == "Yearly"));
    m_dateLabel->setVisible(showDate);
    m_dateRowWidget->setVisible(showDate);

    if (looped && intv != "Yearly") m_timeLabel->setText("Time of day");
    else                            updateTimeLabel();
    m_timeLabel->setVisible(dateEnabled);
    m_timeRowWidget->setVisible(dateEnabled);

    m_weekdayLabel->setVisible(showWeekday);
    m_weekdayRowWidget->setVisible(showWeekday);

    m_domLabel->setVisible(showDOM);
    m_domRowWidget->setVisible(showDOM);

    m_loopCheck->setVisible(dateEnabled);
    m_loopIntervalCombo->setVisible(dateEnabled && !isHoliday);

    // Shrink/grow dialog to fit visible content — eliminates empty gaps
    QTimer::singleShot(0, this, [this]{ adjustSize(); });
}

// v3.0.2 — reflects the real default from the Time Zone setting. Theatrical
// movies (isTheatrical, TMDB-only) always default to noon regardless of zone.
QTime EditTileDialog::computeDefaultTime() const
{
    // V5 — a published air time IS this tile's default now, so the picker
    // has to show it. Without this the dialog displayed the Time Zone
    // default (3 AM Eastern) for a show like Lanterns while the tile was
    // correctly counting down to its real 9 PM broadcast — the same
    // precedence TileData::effectiveTime() uses, just reflected in the UI.
    if (m_data.airTime.isValid()) return m_data.airTime;

    QString type = m_typeCombo->currentText();
    if (type == "Movie" && m_data.isTheatrical) return TimeZoneUtil::defaultTheatricalTime();
    if (type == "Movie" || type == "Show")      return TimeZoneUtil::defaultDigitalTime();
    return QTime(0, 0);
}

// Sets the picker widgets without marking them dirty (programmatic update).
void EditTileDialog::setPickerTime(const QTime& t)
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

void EditTileDialog::updateTimeLabel()
{
    QTime def = computeDefaultTime();
    QString type = m_typeCombo->currentText();
    if (type == "Movie" && m_data.isTheatrical) {
        m_timeLabel->setText("Time  (optional — defaults to 12:00 PM, theatrical)");
    } else if (type == "Movie" || type == "Show") {
        m_timeLabel->setText(
            QString("Time  (optional — defaults to %1, per your Time Zone setting)")
                .arg(def.toString("h:mm AP")));
    } else {
        m_timeLabel->setText("Time  (optional — defaults to midnight)");
    }
    // Reflect the live default in the picker itself, and keep the dirty-check
    // baseline in sync, as long as there's no explicit override in play.
    if (!m_timeExplicit) {
        setPickerTime(def);
        m_initTime = def;
    }
}

// =============================================================================
// Helpers
// =============================================================================
int EditTileDialog::daysInSelectedMonth() const
{
    int month = m_monthCombo->currentIndex() + 1;
    int year  = m_yearSpin ? m_yearSpin->value() : QDate::currentDate().year();
    return QDate(year, month, 1).daysInMonth();
}

void EditTileDialog::refreshDayCombo()
{
    int prevDay = m_dayCombo->currentIndex() + 1;
    int days    = daysInSelectedMonth();
    m_dayCombo->blockSignals(true);
    m_dayCombo->clear();
    for (int d = 1; d <= days; ++d) m_dayCombo->addItem(QString::number(d));
    m_dayCombo->setCurrentIndex(qMin(prevDay, days) - 1);
    m_dayCombo->blockSignals(false);
}

void EditTileDialog::onMonthChanged(int) { refreshDayCombo(); updateResetButtons(); }

QDate EditTileDialog::selectedDate() const
{
    return QDate(m_yearSpin->value(),
                 m_monthCombo->currentIndex() + 1,
                 m_dayCombo->currentIndex() + 1);
}

void EditTileDialog::setSelectedDate(const QDate& d)
{
    if (!d.isValid()) return;
    m_monthCombo->setCurrentIndex(d.month() - 1);
    refreshDayCombo();
    m_dayCombo->setCurrentIndex(d.day() - 1);
    m_yearSpin->setValue(d.year());
}

// V5 — Season/Episode override accessors. See the constructor for the
// reasoning; these only report a change when the user actually moved the
// numbers, so an untouched dialog never rewrites a scraped label.
bool EditTileDialog::seasonEpisodeChanged() const
{
    if (!m_seasonSpin || !m_episodeSpin || !m_totalSpin) return false;
    if (mediaType() != "tv") return false;
    return m_seasonSpin->value()  != m_initSeason
        || m_episodeSpin->value() != m_initEpisode
        || m_totalSpin->value()   != m_initTotal;
}

QString EditTileDialog::overriddenStatusLabel() const
{
    if (!m_seasonSpin || !m_episodeSpin) return {};
    int s = m_seasonSpin->value();
    int e = m_episodeSpin->value();
    if (s <= 0 || e <= 0) return {};   // 0 in either box means "no label"
    return QString("S%1E%2").arg(s, 2, 10, QChar('0')).arg(e, 2, 10, QChar('0'));
}

int EditTileDialog::overriddenEpisodeTotal() const
{
    return m_totalSpin ? m_totalSpin->value() : 0;
}

// V5 — restores just these three boxes to the last values a data source
// reported, leaving the title, date, time and everything else untouched.
// Deliberately its own button rather than folding into the existing Reset
// controls, which each own one field.
// Hands the numbers back to the data source.
//
// This can't simply restore "the previous values": once a tile is
// overridden, the numbers stored on it ARE the override, and the originals
// aren't kept anywhere. So a reset drops the override flag and asks for a
// refetch — the source then repopulates the real season, episode and total.
// The boxes are blanked meanwhile so it's visibly no longer a manual value.
void EditTileDialog::onResetSeasonEpisode()
{
    if (!m_seasonSpin || !m_episodeSpin || !m_totalSpin) return;
    m_seasonSpin->blockSignals(true);
    m_episodeSpin->blockSignals(true);
    m_totalSpin->blockSignals(true);
    m_seasonSpin->setValue(m_sourceSeason);
    m_episodeSpin->setValue(m_sourceEpisode);
    m_totalSpin->setValue(m_sourceTotal);
    m_seasonSpin->blockSignals(false);
    m_episodeSpin->blockSignals(false);
    m_totalSpin->blockSignals(false);
    refreshSeasonEpisodeResetState();
}

// Enabled only while the boxes differ from what the data source reported.
// Typing the source values back in by hand therefore greys it again, exactly
// as if Reset had been pressed — the button tracks the actual state rather
// than remembering that an edit once happened.
void EditTileDialog::refreshSeasonEpisodeResetState()
{
    if (!m_rSeasonEpisodeBtn || !m_seasonSpin || !m_episodeSpin || !m_totalSpin) return;
    bool differs = m_seasonSpin->value()  != m_sourceSeason
                || m_episodeSpin->value() != m_sourceEpisode
                || m_totalSpin->value()   != m_sourceTotal;
    m_seasonEpisodeReset = !differs;   // back on the source values = no override
    m_rSeasonEpisodeBtn->setEnabled(differs);
    m_rSeasonEpisodeBtn->setStyleSheet(differs ? kResetActive : kResetGreyed);
}

QDate EditTileDialog::customDate() const
{
    bool looped  = m_loopCheck->isChecked();
    QString intv = m_loopIntervalCombo->currentText();

    if (looped && intv == "Weekly") {
        int wd = m_weekdayCombo->currentIndex() + 1;
        QDate today = QDate::currentDate();
        int daysAhead = (wd - today.dayOfWeek() + 7) % 7;
        if (daysAhead == 0) daysAhead = 7;
        return today.addDays(daysAhead);
    }
    if (looped && intv == "Monthly") {
        int dom = m_domSpin->value();
        QDate today = QDate::currentDate();
        QDate d = QDate(today.year(), today.month(), qMin(dom, today.daysInMonth()));
        if (d <= today) d = d.addMonths(1);
        return QDate(d.year(), d.month(), qMin(dom, d.daysInMonth()));
    }
    if (looped && intv == "Daily") {
        int h12 = m_hourCombo->currentIndex() + 1;
        int mins = m_minuteCombo->currentIndex();
        bool pm  = (m_ampmCombo->currentIndex() == 1);
        int h24  = (h12 % 12) + (pm ? 12 : 0);
        QTime t(h24, mins);
        QDateTime now  = QDateTime::currentDateTime();
        QDateTime next(now.date(), t);
        if (next <= now) next = next.addDays(1);
        return next.date();
    }
    if (!m_dateCheck->isChecked()) return QDate();
    QDate chosen = selectedDate();
    return (chosen == m_data.targetDate && !m_data.customDate.isValid()) ? QDate() : chosen;
}

QString EditTileDialog::customDateStr() const
{
    QDate d = customDate();
    return d.isValid() ? d.toString("MMMM d, yyyy") : QString();
}

QTime EditTileDialog::customAirTime() const
{
    // No explicit override — let the dynamic Time Zone/theatrical default
    // apply, whatever it computes to (and however it may change later if the
    // Time Zone setting changes).
    if (!m_timeExplicit) return QTime();

    int h12  = m_hourCombo->currentIndex() + 1;
    int mins = m_minuteCombo->currentIndex();
    bool pm  = (m_ampmCombo->currentIndex() == 1);
    int h24  = (h12 % 12) + (pm ? 12 : 0);
    return QTime(h24, mins);
}

// =============================================================================
// Slots
// =============================================================================
void EditTileDialog::onTypeChanged(int)
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
        // Leaving Special — undo the holiday auto-fill, restore what was
        // there before (the tile's original title/date).
        m_titleEdit->setText(m_initTitle);
        QDate restoreDate = m_initDate.isValid() ? m_initDate
                          : QDate(QDate::currentDate().year(), 1, 1);
        setSelectedDate(restoreDate);
        m_dateCheckState = m_initDate.isValid();
        m_dateCheck->setChecked(m_dateCheckState);
    }
    // Switching between Custom/Movie/Show/Game — don't touch title/date/loop,
    // only the tile's Type (and mediaType) changes.
    m_loopIntervalCombo->setVisible(true);
    applyLoopFieldVisibility();
    updateResetButtons();
    m_lastAppliedType = type;
}

void EditTileDialog::onSpecialDayChanged(int index)
{
    QString day = m_specialDayCombo->itemText(index);

    m_titleEdit->setText(day);
    // Special Days: enable loop, set yearly, hide interval picker (always yearly)
    m_loopCheck->setChecked(true);
    m_loopIntervalCombo->setEnabled(true);
    m_loopIntervalCombo->setStyleSheet(kField);
    m_loopIntervalCombo->setCurrentIndex(0); // Yearly
    m_loopIntervalCombo->setVisible(false);

    // Birthday: default Jan 1 current year
    if (day == "Birthday") {
        setSelectedDate(QDate(QDate::currentDate().year(), 1, 1));
    } else {
        QDate d = nextOccurrence(day, QDate::currentDate());
        if (d.isValid()) setSelectedDate(d);
    }
    applyLoopFieldVisibility();
    updateResetButtons();
}

void EditTileDialog::onLoopToggled(bool checked)
{
    m_loopIntervalCombo->setEnabled(checked);
    m_loopIntervalCombo->setStyleSheet(checked ? kField : kDisabledField);
    applyLoopFieldVisibility();
}

void EditTileDialog::onLoopIntervalChanged(int) { applyLoopFieldVisibility(); }


void EditTileDialog::onDateToggled(bool enabled)
{
    m_dateCheckState = enabled;
    m_monthCombo->setEnabled(enabled);
    m_dayCombo->setEnabled(enabled);
    m_yearSpin->setEnabled(enabled);
    QString s = enabled ? kField : kDisabledField;
    m_monthCombo->setStyleSheet(s);
    m_dayCombo->setStyleSheet(s);
    m_yearSpin->setStyleSheet(s);
    applyLoopFieldVisibility();
}

void EditTileDialog::onResetTitle()
{
    m_titleReset = true;
    m_titleEdit->setText(m_data.title);
    // updateResetButtons called via textChanged signal
}

void EditTileDialog::onResetDate()
{
    m_dateReset = true;
    m_data.customDate    = QDate();   // clear so dirty check sees no override
    m_data.customDateStr = "";
    if (m_data.targetDate.isValid()) {
        m_dateCheckState = true;
        m_dateCheck->setChecked(true);
        setSelectedDate(m_data.targetDate);
    } else {
        m_dateCheckState = false;
        m_dateCheck->setChecked(false);
    }
    applyLoopFieldVisibility();
    updateResetButtons();
}

void EditTileDialog::onResetTime()
{
    m_timeReset = true;
    m_timeExplicit = false;
    m_data.customAirTime = QTime();
    QTime def = computeDefaultTime();
    m_initTime = def;
    setPickerTime(def);
    updateResetButtons();
}

// =============================================================================
//  allImages / refreshImagePreview — v3.1.0. The working image list for this
//  session: fetched slot first (if present), then custom images oldest to
//  newest. refreshImagePreview() syncs the preview widget + path label +
//  which nav/delete controls make sense to whatever m_previewIndex points at.
// =============================================================================
QStringList EditTileDialog::allImages() const
{
    QStringList list;
    if (!m_fetchedPath.isEmpty()) list << m_fetchedPath;
    list += m_customPaths;
    return list;
}

void EditTileDialog::refreshImagePreview()
{
    QStringList imgs = allImages();
    QString current = (m_previewIndex >= 0 && m_previewIndex < imgs.size())
                       ? imgs[m_previewIndex] : QString();

    m_previewWidget->setImage(current);
    m_previewWidget->setNavAllowed(m_previewIndex > 0, m_previewIndex < imgs.size() - 1);
    m_previewWidget->setDeleteAllowed(!current.isEmpty());

    // v3.1.1 fix #3 — "(1 of 2)" belongs on the section heading, not
    // crammed in front of the path on the line below it.
    m_backdropHeadingLabel->setText(
        imgs.size() > 1 ? QString("Backdrop Image (%1 of %2)").arg(m_previewIndex + 1).arg(imgs.size())
                        : "Backdrop Image");

    m_pathLabel->setText(current.isEmpty() ? "No image" : current);
}

QString EditTileDialog::selectedImagePath() const
{
    QStringList imgs = allImages();
    return (m_previewIndex >= 0 && m_previewIndex < imgs.size()) ? imgs[m_previewIndex] : QString();
}

void EditTileDialog::onPreviousImage()
{
    if (m_previewIndex > 0) { --m_previewIndex; refreshImagePreview(); }
}

void EditTileDialog::onNextImage()
{
    if (m_previewIndex < allImages().size() - 1) { ++m_previewIndex; refreshImagePreview(); }
}

// v3.1.0 Feature 2 — removes whichever image is currently shown from this
// session's working copy. Doesn't touch disk here; Cancel discards it like
// any other change, and Save diffs the before/after image sets to know what
// to actually delete.
void EditTileDialog::onDeleteImage()
{
    QStringList imgs = allImages();
    if (m_previewIndex < 0 || m_previewIndex >= imgs.size()) return;

    // V5.4.22 — ask first. The delete button sits on the picture itself, a
    // couple of centimetres from the arrows used to flick through images, and
    // it is the one control here with a consequence that reaches the disk: on
    // Save, an image dropped from this list is deleted from fetched_images or
    // custom_images. A custom image the user chose themselves may not exist
    // anywhere else on the machine.
    {
        const bool deletingFetched = (!m_fetchedPath.isEmpty() && m_previewIndex == 0);
        QMessageBox box(this);
        box.setWindowTitle("Delete backdrop?");
        box.setIcon(QMessageBox::Warning);
        box.setText(deletingFetched
                        ? "Remove this fetched backdrop from the tile?"
                        : "Remove this image from the tile?");
        box.setInformativeText(deletingFetched
                        ? "It can be fetched again later with Refetch Image."
                        : "The file is deleted from the app's images folder when you save.");
        QPushButton* deleteBtn = box.addButton("Delete", QMessageBox::DestructiveRole);
        QPushButton* keepBtn   = box.addButton("Cancel", QMessageBox::RejectRole);
        box.setDefaultButton(keepBtn);   // Enter keeps the image, never deletes it
        box.exec();
        if (box.clickedButton() != deleteBtn) return;
    }

    bool deletingFetchedSlot = (!m_fetchedPath.isEmpty() && m_previewIndex == 0);
    if (deletingFetchedSlot) {
        m_fetchedPath.clear();
    } else {
        int customIdx = m_previewIndex - (m_fetchedPath.isEmpty() ? 0 : 1);
        if (customIdx >= 0 && customIdx < m_customPaths.size())
            m_customPaths.removeAt(customIdx);
    }

    QStringList remaining = allImages();
    if (remaining.isEmpty())            m_previewIndex = -1;
    else if (m_previewIndex >= remaining.size()) m_previewIndex = remaining.size() - 1;
    // else: whatever slid into this index is shown next — index stays put

    refreshImagePreview();
    updateRefetchButtonState();   // v3.1.3 fix #1 — deleting the fetched image re-enables it
}

// v3.1.3 fix #1 — only clickable when there's currently no fetched image, to
// avoid the button inviting repeated TMDB requests for something already in
// hand. Also disabled immediately after being clicked, since the request is
// already queued (see onRefetchImage) — clicking again wouldn't do anything
// new until the next time the fetched slot is actually empty.
void EditTileDialog::updateRefetchButtonState()
{
    bool canRefetch = (m_data.tmdbId > 0) && m_fetchedPath.isEmpty() && !m_wantsForcedRefetch;
    m_refetchImageBtn->setEnabled(canRefetch);
    m_refetchImageBtn->setStyleSheet(canRefetch ? kResetActive : kResetGreyed);
    m_refetchImageBtn->setToolTip(
        m_data.tmdbId <= 0 ? "Not linked to TMDB"
      : m_wantsForcedRefetch ? "Already queued — will fetch on save"
      : m_fetchedPath.isEmpty() ? "Fetch a backdrop from TMDB and make it the active image"
      : "Already have a fetched image — delete it first to fetch a new one");
}

// v3.1.0 — queues a forced backdrop re-download (bypassing the "only if
// missing" check) that becomes the active image once it completes. The
// actual network fetch happens after Save, same timing as everything else
// in this dialog — TileWidget bubbles this up to MainWindow's scraper.
void EditTileDialog::onRefetchImage()
{
    m_wantsForcedRefetch = true;
    m_pathLabel->setText("Will fetch a fresh backdrop from TMDB on save");
    updateRefetchButtonState();
}

void EditTileDialog::onSelectImage()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Select Backdrop Image", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.webp)");
    if (path.isEmpty()) return;
    QString ext  = QFileInfo(path).suffix().toLower();
    QString dir  = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                   + "/custom_images";
    QDir().mkpath(dir);
    QString dest = dir + "/" + m_data.id + "_"
                 + QString::number(QDateTime::currentMSecsSinceEpoch())
                 + "." + ext;
    QString finalPath = QFile::copy(path, dest) ? dest : path;

    // v3.1.0 — appended as the newest custom image and becomes the active
    // (currently previewed) one; the fetched image and any earlier custom
    // ones stay right where they are, still reachable via the arrows.
    m_customPaths << finalPath;
    m_previewIndex = allImages().size() - 1;
    refreshImagePreview();
}

void EditTileDialog::onSave()  { accept(); }

void EditTileDialog::onRemove()
{
    auto btn = QMessageBox::question(this, "Remove Tile",
        QString("Remove \"%1\"?").arg(m_data.displayTitle()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (btn == QMessageBox::Yes) { m_wantsRemove = true; accept(); }
}

