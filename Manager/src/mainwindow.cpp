#include "mainwindow.h"
#include "releasehistory.h"
#include "tiledisplayprefs.h"
#include "tilewidget.h"
#include "jsonmanager.h"
#include "customtiledialog.h"
#include "aboutdialog.h"
#include "applogger.h"
#include "timezoneutil.h"
#include "languageutil.h"
#include "relayconfig.h"
#include "colorwheel.h"
#include "loopschedule.h"
#include "appversion.h"
#include "feedback.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUuid>
#include <QWidgetAction>
#include <QComboBox>
#include <QMenu>
#include <QScreen>
#include <QGuiApplication>
#include <QListView>
#include <QVector>
#include <QPainter>
#include <QIcon>
#include <array>
#include <QMimeData>
#include <QDrag>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QGridLayout>
#include <QDragEnterEvent>
#include <QDropEvent>

#include <QDesktopServices>
#include <QSettings>
#include <QUrl>
#include <QToolButton>
#include <functional>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLocalSocket>
#include <QFrame>
#include <QSlider>
#include <QSpinBox>
#include <QLineEdit>
#include <QFile>
#include <QDir>
#include <QSet>
#include <QStandardPaths>
#include <QDebug>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QEvent>
#include <QFileInfo>
#include <QApplication>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QScrollBar>
#include <QDialog>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QSystemTrayIcon>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QSpacerItem>
#include <QFontMetrics>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDirIterator>
#include <QRegularExpression>
#include <algorithm>

static bool tileOrder(const TileData& a, const TileData& b)
{
    QDate da = a.effectiveDate(), db = b.effectiveDate();
    if (!da.isValid() && !db.isValid()) return false;
    if (!da.isValid()) return false;   // no-date tiles sort to end
    if (!db.isValid()) return true;
    if (da != db) return da < db;      // different dates: earlier first
    // Same date — break tie by time. Invalid time = midnight (00:00).
    QTime ta = a.effectiveTime().isValid() ? a.effectiveTime() : QTime(0, 0);
    QTime tb = b.effectiveTime().isValid() ? b.effectiveTime() : QTime(0, 0);
    return ta < tb;
}

// V5.4.11 — the bottom-left status line's ordinary colour. It was #666 on a
// #1a1a1a bar, which is only just above the background and was reported as
// hard to read. The size was already right, so only the colour changed. The
// semantic colours other messages use (green for done, red for an error,
// blue for in progress) are deliberately left alone — they carry meaning that
// white would throw away. This is set explicitly wherever a neutral message
// is written, because the label keeps whatever colour it was last given and
// "35 tiles ready" would otherwise inherit the red of an earlier error.
// Shared by both colour dialogs, so their buttons can't drift apart.
static const char* kPickerGhostBtnStyle =
    "QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
    "border-radius:4px; padding:8px 20px; font-size:13px; }"
    "QPushButton:hover { background:#333; color:#fff; }";
static const char* kPickerSaveBtnStyle =
    "QPushButton { background:#0078d4; color:#fff; border:none; "
    "border-radius:4px; padding:8px 28px; font-size:13px; font-weight:bold; }"
    "QPushButton:hover { background:#1a8de4; }";

static const char* kStatusStyleNeutral = "color:#ffffff; font-size:11px; background:transparent;";

// Tab bar style. (v3.1.6 briefly added a "shrink tabs to fit" feature here,
// which v3.1.92 reverted back to the default overflow scroll arrows since
// shrinking was cutting off tab labels.)
static const char* kTabBarBaseStyle = R"(
    QTabWidget::pane { border: none; background:#1a1a1a; }
    QTabBar::tab {
        background:#111111; color:#888888;
        padding:8px 10px; font-size:13px; min-height:18px;
        border-bottom:2px solid transparent;
    }
    QTabBar::tab:selected  { color:#ffffff; border-bottom:2px solid #0078d4; }
    QTabBar::tab:hover     { color:#cccccc; }
)";

// =============================================================================
//  v3.1.4 — Tab Layout metadata. Order here is also the checkbox order in
//  Settings and the default drag order on first run.
// =============================================================================
static const char* kKindKeys[MainWindow::NUM_KINDS] = {
    "countdowns", "released", "other", "favorite",
    "movie_countdowns", "show_countdowns", "game_countdowns",
    "special_countdowns", "custom_countdowns",
    "released_movie", "released_show", "released_game",
    "released_special", "released_custom"
};
static const char* kKindLabels[MainWindow::NUM_KINDS] = {
    "All (Countdowns)", "All (Released)", "Other", "Favorite",
    "Movie (Countdowns)", "Show (Countdowns)", "Game (Countdowns)",
    "Special (Countdowns)", "Custom (Countdowns)",
    "Movie (Released)", "Show (Released)", "Game (Released)",
    "Special (Released)", "Custom (Released)"
};

// =============================================================================
//  TabLayoutPresetCombo — v3.1.94 fix #1. A normal (non-editable, whole-box-
//  clickable) combo box whose dropdown list can never show "Custom" as a
//  clickable row, even though the closed box can still display "Custom" as
//  its current text. Previous versions dynamically removed/re-added the
//  item around each popup open/close, which proved fragile (reported not
//  sticking to "All"/"None" selections in some cases). This is simpler and
//  more robust: "Custom" stays a normal, permanent item in the model (so
//  setCurrentIndex()/currentText() always work normally), and Qt's own
//  row-hiding on the popup's view is used to keep it out of the visible
//  list — no signal-blocking, no add/remove dance, no restore-on-close logic.
// =============================================================================
class TabLayoutPresetCombo : public QComboBox
{
public:
    explicit TabLayoutPresetCombo(QWidget* parent = nullptr) : QComboBox(parent) {}

    // Call once after adding all items, with the index of the item that
    // should never appear as a clickable row (but can still be the
    // current/displayed value).
    void hideRowFromPopup(int row)
    {
        if (auto* lv = qobject_cast<QListView*>(view()))
            lv->setRowHidden(row, true);
    }
};

// =============================================================================
//  CenteredComboBox — v3.2.9 fix #2. Centers the box's own displayed text
//  by overriding paintEvent() and drawing it manually, instead of the
//  previous approach (an editable-but-readonly internal line edit), which
//  turned out to change how clicks are handled: editable combo boxes only
//  open their popup when the small drop-down arrow itself is clicked, not
//  anywhere on the box, which is what broke it as a working dropdown. This
//  approach doesn't touch editability or interaction at all — only how the
//  current text is painted — so normal click-anywhere-to-open behavior is
//  untouched.
// =============================================================================
class CenteredComboBox : public QComboBox
{
public:
    explicit CenteredComboBox(QWidget* parent = nullptr) : QComboBox(parent) {}

    // v3.2.92 fix #1 — shared with the external width calculation (see
    // applyCurrentTabGroup()) so the box is sized to leave EXACTLY this
    // much non-text space and nothing more: if both computations agree on
    // the same margins, the text area works out to precisely the text's
    // own needed width, with no slack space left over for the text to
    // look off-center within. Left = border + left padding; right =
    // border + right padding + the drop-down arrow's own width (18px, set
    // in the stylesheet below).
    static constexpr int kLeftMargin  = 9;
    static constexpr int kRightMargin = 27;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox opt;
        initStyleOption(&opt);
        QString text = opt.currentText;
        opt.currentText.clear();               // suppress the default (left-aligned) text draw
        painter.drawComplexControl(QStyle::CC_ComboBox, opt);
        QRect textRect = rect().adjusted(kLeftMargin, 0, -kRightMargin, 0);
        painter.drawItemText(textRect, Qt::AlignCenter, palette(), isEnabled(), text, QPalette::Text);

        // v3.2.96 fix #2 — draw the drop-down indicator arrow manually.
        // The default style's own arrow drawing wasn't showing up here
        // (left the reserved 18px arrow area empty), which is very likely
        // what was being seen as unexplained extra space on the right.
        // Drawing a small filled triangle directly guarantees it's always
        // visible, rather than depending on the current style/theme to
        // render one into that space.
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(isEnabled() ? QColor("#ddd") : QColor("#3a3a3a"));
        int cx = width() - (kRightMargin / 2) - 2;
        int cy = height() / 2;
        int aw = 5, ah = 4;
        QPolygon arrow;
        arrow << QPoint(cx - aw, cy - ah / 2)
              << QPoint(cx + aw, cy - ah / 2)
              << QPoint(cx, cy + ah);
        painter.drawPolygon(arrow);
    }
};

// =============================================================================
//  TabSlotBox — v3.2.0 fix #4. One draggable cell in the Manage Tabs grid.
//  Empty cells accept drops but aren't drag sources themselves. Dropping
//  one tab onto another SWAPS them (both ever end up moving); dropping
//  onto an empty cell just moves it there.
// =============================================================================
// =============================================================================
//  TabColorOverlay — V4.5. Draws a colored bottom border for any tab that
//  has a custom color, layered on top of the real QTabBar.
//
//  This is NOT a QProxyStyle override of CE_TabBarTabShape, on purpose:
//  once a stylesheet defines "QTabBar::tab {...}" rules (which this app's
//  tab bar already does, and needs to keep), Qt's internal QStyleSheetStyle
//  takes over that element's painting completely and never delegates back
//  to a wrapped/custom style's drawControl() for it — confirmed directly
//  before writing this, since a first attempt at exactly that silently
//  never painted anything. A transparent, click-through overlay widget
//  sitting on top of the tab bar sidesteps the whole style system instead.
// =============================================================================
class TabColorOverlay : public QWidget
{
public:
    explicit TabColorOverlay(QTabBar* bar) : QWidget(bar->parentWidget()), m_bar(bar)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }
    void setColors(const QHash<QString, QColor>& colors) { m_colors = colors; update(); }
    void syncGeometry() { setGeometry(m_bar->geometry()); raise(); update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        int current = m_bar->currentIndex();
        for (int i = 0; i < m_bar->count(); ++i) {
            if (i == current) continue;   // V4.6 — let the stylesheet's blue selected-indicator win here, so selection stays unambiguous even on a colored tab
            QColor c = m_colors.value(m_bar->tabText(i));
            if (!c.isValid()) continue;
            QRect r = m_bar->tabRect(i);
            p.fillRect(QRect(r.left(), r.bottom() - 2, r.width(), 3), c);
        }
    }

private:
    QTabBar* m_bar;
    QHash<QString, QColor> m_colors;
};


class TabSlotBox : public QFrame
{
    Q_OBJECT
public:
    TabSlotBox(int group, int slot, QWidget* parent = nullptr)
        : QFrame(parent), m_group(group), m_slot(slot)
    {
        // v3.2.7 fix #5 — doubled (was 96x52).
        setFixedSize(192, 104);
        setAcceptDrops(true);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 8, 8, 8);
        m_label = new QLabel(this);
        m_label->setWordWrap(true);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setStyleSheet("font-size:15px; color:#ddd; background:transparent;");
        lay->addWidget(m_label);
        setKind(-1, QString());
    }

    void setKind(int kind, const QString& label)
    {
        m_kind = kind;
        m_label->setText(label);
        setStyleSheet(kind >= 0
            ? "TabSlotBox { background:#2a2a2a; border:1px solid #444; border-radius:5px; }"
            : "TabSlotBox { background:#161616; border:1px dashed #333; border-radius:5px; }");
        setCursor(kind >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }
    int kind() const { return m_kind; }

    // V4.4 — shows a small colored dot in the corner when this slot's kind
    // has a custom color set; an invalid QColor means "no custom color",
    // matching MainWindow::m_tabColors' convention exactly.
    void setColor(const QColor& color)
    {
        m_color = color;
        update();
    }

signals:
    void dropped(int fromGroup, int fromSlot, int toGroup, int toSlot);
    // Only ever emitted for an occupied slot (m_kind >= 0) — right-clicking
    // an empty slot has nothing to act on. The actual menu (Color Picker/
    // Clear Color/Hide Tab) is built by MainWindow, not here, since only
    // MainWindow has access to the state (m_enabledKinds, m_tabColors)
    // needed to decide which items should even appear.
    void contextMenuRequestedForKind(int kind, QPoint globalPos);

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QFrame::paintEvent(event);
        if (!m_color.isValid()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(0, 0, 0, 120), 1));
        p.setBrush(m_color);
        p.drawEllipse(QPointF(width() - 14, 14), 6, 6);
    }
    void contextMenuEvent(QContextMenuEvent* event) override
    {
        if (m_kind < 0) return;   // nothing to act on for an empty slot
        emit contextMenuRequestedForKind(m_kind, event->globalPos());
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_kind >= 0) m_dragStart = event->pos();
        QFrame::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || m_kind < 0) return;
        if ((event->pos() - m_dragStart).manhattanLength() < QApplication::startDragDistance()) return;
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData();
        mime->setText(QString("%1,%2").arg(m_group).arg(m_slot));
        drag->setMimeData(mime);
        drag->exec(Qt::MoveAction);
    }
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasText()) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override
    {
        QStringList parts = event->mimeData()->text().split(',');
        if (parts.size() != 2) return;
        bool ok1 = false, ok2 = false;
        int fromGroup = parts[0].toInt(&ok1);
        int fromSlot  = parts[1].toInt(&ok2);
        if (!ok1 || !ok2) return;
        emit dropped(fromGroup, fromSlot, m_group, m_slot);
        event->acceptProposedAction();
    }

private:
    int m_group, m_slot, m_kind = -1;
    QLabel* m_label;
    QPoint m_dragStart;
    QColor m_color;   // V4.4 — invalid QColor() means no custom color set
};

// Default preset: only the 3 general kinds are enabled — reproduces the
// original (pre-3.0.0) 3-tab behavior exactly.
static QSet<int> defaultPresetKinds()
{
    return { MainWindow::K_COUNTDOWNS, MainWindow::K_RELEASED, MainWindow::K_OTHER, MainWindow::K_FAVORITE };
}
// Separate preset: one tab per media type (now including Special/Custom), plus Other.
static QSet<int> separatePresetKinds()
{
    return {
        MainWindow::K_MOVIE_COUNTDOWNS,  MainWindow::K_SHOW_COUNTDOWNS,   MainWindow::K_GAME_COUNTDOWNS,
        MainWindow::K_SPECIAL_COUNTDOWNS, MainWindow::K_CUSTOM_COUNTDOWNS,
        MainWindow::K_RELEASED_MOVIE,    MainWindow::K_RELEASED_SHOW,     MainWindow::K_RELEASED_GAME,
        MainWindow::K_RELEASED_SPECIAL,  MainWindow::K_RELEASED_CUSTOM,
        MainWindow::K_OTHER, MainWindow::K_FAVORITE
    };
}
// v3.1.93 fix #3 — "All": every tab kind enabled.
static QSet<int> allPresetKinds()
{
    QSet<int> all;
    for (int k = 0; k < MainWindow::NUM_KINDS; ++k) all.insert(k);
    return all;
}
// v3.1.93 fix #3 — "None": every tab kind disabled (refreshTabBar() already
// falls back to showing "All Countdowns" if this would leave zero visible
// tabs, so this can't result in a totally blank tab bar).
static QSet<int> nonePresetKinds()
{
    return {};
}

// =============================================================================
//  v3.0.1 — tabs are independent filters/views, not exclusive buckets: a tile
//  can (and should) match more than one enabled kind at once. "All Countdowns"
//  always matches every active tile regardless of media type, even when
//  "Show Countdowns" (etc.) is also enabled and separately matches it too.
// =============================================================================
bool MainWindow::tileMatchesKind(const TileData& td, int kind) const
{
    bool hasDate = td.hasDate();
    bool expired = hasDate && td.isExpired();

    // V5 — a month-only window that has run out belongs in Other, not
    // Released. "March 2027" was never a claim that the show came out on a
    // particular day; it was the only thing anyone had announced. Once that
    // month passes without a real date appearing, the honest state is "we
    // don't know when", which is exactly what Other is for — announcing it
    // as released would be asserting something no source ever said.
    // V5.4 — a year-only window ("2026") says even less and lapses the same
    // way, so both go through isWindowDate().
    //
    // Which tab depends on whether the title has a past. A returning show
    // falls back to the last episode that genuinely aired and belongs in
    // Released; something that has never come out has nothing to show and
    // belongs in Other. lapseWindowDate() rewrites the tile to match on the
    // next tick — this keeps the filter agreeing with it in the meantime.
    if (td.isWindowDate() && expired) {
        return td.lapsedWindowGoesToReleased() ? kind == K_RELEASED : kind == K_OTHER;
    }

    // v3.1.4 — "Special" (holiday/birthday) and plain "Custom" tiles both
    // carry mediaType=="custom"; presetType is what tells them apart (a
    // holiday name vs the literal "Custom"). Same list used in
    // customtiledialog.cpp/edittiledialog.cpp for the Type dropdown.
    static const QSet<QString> kHolidayPresets = {
        "Christmas","Easter","Halloween","Thanksgiving","New Year","April Fools",
        "Good Friday","Veterans Day","Independence Day","Birthday"
    };
    bool isCustomMedia = (td.mediaType == "custom");
    bool isSpecialDay  = isCustomMedia && kHolidayPresets.contains(td.presetType);
    bool isPlainCustom = isCustomMedia && !isSpecialDay;

    switch (kind) {
        case K_COUNTDOWNS:          return hasDate && !expired;
        case K_RELEASED:            return hasDate && expired;
        case K_OTHER:                return !hasDate;
        case K_FAVORITE:             return td.isFavorite;   // V4.12
        case K_MOVIE_COUNTDOWNS:    return hasDate && !expired && td.mediaType == "movie";
        case K_SHOW_COUNTDOWNS:     return hasDate && !expired && td.mediaType == "tv";
        case K_GAME_COUNTDOWNS:     return hasDate && !expired && td.mediaType == "game";
        case K_SPECIAL_COUNTDOWNS:  return hasDate && !expired && isSpecialDay;
        case K_CUSTOM_COUNTDOWNS:   return hasDate && !expired && isPlainCustom;
        case K_RELEASED_MOVIE:      return hasDate && expired  && td.mediaType == "movie";
        case K_RELEASED_SHOW:       return hasDate && expired  && td.mediaType == "tv";
        case K_RELEASED_GAME:       return hasDate && expired  && td.mediaType == "game";
        case K_RELEASED_SPECIAL:    return hasDate && expired  && isSpecialDay;
        case K_RELEASED_CUSTOM:     return hasDate && expired  && isPlainCustom;
        default:                    return false;
    }
}

// Does at least one current tile match this kind? Used for "Other"'s dynamic
// visibility — cheap existence check, no widget reparenting involved.
bool MainWindow::kindHasAnyTile(int kind) const
{
    for (TileWidget* tw : std::as_const(m_tileWidgets))
        if (tileMatchesKind(tw->tileData(), kind)) return true;
    return false;
}

// =============================================================================
//  makeGlyphIcon — v3.1.5 fix #5. Renders a unicode glyph (e.g. "🛈") to a
//  QIcon in a specific color. QPushButton can't mix colors within a single
//  plain-text string, so giving the icon its own color separate from the
//  button's text requires rendering it as an actual icon instead of just
//  putting it in the text string.
// =============================================================================
static QIcon makeGlyphIcon(const QString& glyph, const QColor& color, int pointSize = 13)
{
    QFont font;
    font.setPointSize(pointSize);
    QFontMetrics fm(font);
    QRect bounds = fm.boundingRect(glyph);
    int size = qMax(qMax(bounds.width(), bounds.height()) + 6, 16);

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, glyph);
    painter.end();
    return QIcon(pixmap);
}

// v3.3.16 — forward declaration; full definition is near onGlobalTick()
// below, but loadTiles() (earlier in this file) needs to call it too.
static bool tryLocalEpisodeAdvance(TileData& td);

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_scraper(new TmdbScraper(this))
    , m_igdbScraper(new IgdbScraper(this))
    , m_tvmaze(new TvmazeScraper(this))
{
    // V5.4.12 — from MC_APP_VERSION, not typed in. This said "V5.0.0" for
    // eleven releases, so the window was naming a build nobody was running.
    setWindowTitle(QString("Media Countdowns V%1").arg(MC_APP_VERSION));
    setMinimumSize(700, 500);

    connect(m_scraper, &TmdbScraper::searchResultsReady, this, &MainWindow::onSearchResultsReady);
    connect(m_scraper, &TmdbScraper::creditsReady,       this, &MainWindow::onCreditsReady);
    connect(m_scraper, &TmdbScraper::dataReady,          this, &MainWindow::onScraperDataReady);
    connect(m_scraper, &TmdbScraper::tileRefreshed,      this, &MainWindow::onTileRefreshed);
    connect(m_scraper, &TmdbScraper::posterReady,        this, &MainWindow::onPosterReady);
    connect(m_scraper, &TmdbScraper::scraperError,       this, &MainWindow::onScraperError);

    // v3.3.0 — IGDB (games) reuses the exact same slots as TMDB above; the
    // signal signatures are identical by design, so no separate handlers
    // are needed. Whichever scraper actually did the work is the one that
    // fires, and the shared slot doesn't need to know or care which.
    connect(m_igdbScraper, &IgdbScraper::searchResultsReady, this, &MainWindow::onSearchResultsReady);
    connect(m_igdbScraper, &IgdbScraper::dataReady,          this, &MainWindow::onScraperDataReady);
    connect(m_igdbScraper, &IgdbScraper::tileRefreshed,      this, &MainWindow::onTileRefreshed);
    connect(m_igdbScraper, &IgdbScraper::posterReady,        this, &MainWindow::onPosterReady);
    connect(m_igdbScraper, &IgdbScraper::scraperError,       this, &MainWindow::onScraperError);

    // V5 — TVmaze reuses the same shared slots for the same reason IGDB
    // does: identical signal signatures, so the slots don't care who
    // answered. tileNeedsConfirmation is the one genuinely new signal —
    // there is no TMDB→TVmaze id mapping, so an existing tile has to be
    // matched by title, and an uncertain match asks rather than guesses.
    connect(m_tvmaze, &TvmazeScraper::searchResultsReady, this, &MainWindow::onSearchResultsReady);
    connect(m_tvmaze, &TvmazeScraper::dataReady,          this, &MainWindow::onScraperDataReady);
    connect(m_tvmaze, &TvmazeScraper::tileRefreshed,      this, &MainWindow::onTileRefreshed);
    connect(m_tvmaze, &TvmazeScraper::posterReady,        this, &MainWindow::onPosterReady);
    connect(m_tvmaze, &TvmazeScraper::scraperError,       this, &MainWindow::onScraperError);
    connect(m_tvmaze, &TvmazeScraper::tileNeedsConfirmation,
            this, &MainWindow::onTvmazeNeedsConfirmation);

    auto* central = new QWidget(this);
    central->setStyleSheet("background-color:#1a1a1a;");
    setCentralWidget(central);

    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_tabs = new QTabWidget(central);
    m_tabs->setStyleSheet(kTabBarBaseStyle);
    // v3.0.0 — tabs can be dragged into any order; the new order is persisted.
    m_tabs->tabBar()->setMovable(true);
    // v3.2.3 fix #1 — QTabBar stretches visible tabs to fill the whole
    // available width by default (expanding=true), which is why a group
    // with only 1 tab was rendering it as wide as a group of 6 used to be.
    // Turning this off makes every tab size itself from its own label,
    // independent of how many other tabs happen to be visible right now.
    m_tabs->tabBar()->setExpanding(false);
    // V4.12 fix — without this, a tab whose available width gets squeezed
    // (see updateTabBarMaxWidth() below) would just clip its text abruptly
    // with no visual indicator, rather than truncating cleanly with "…".
    m_tabs->tabBar()->setElideMode(Qt::ElideRight);
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, &MainWindow::onTabMoved);

    // v3.1.95 fix #2 — group-based overflow instead of Qt's default scroll
    // buttons (which shifted the visible tabs a bit at a time) AND instead
    // of v3.1.94's width-measuring "sliding page" approach (which had a
    // real bug — see updateTabGroups()). Just two fixed groups: however
    // many tabs fit first (group 1), and everything else (group 2). Next
    // swaps to group 2, Prev swaps back — nothing more complicated than that.
    m_tabs->setUsesScrollButtons(false);

    // v3.0.0 — build a scroll area / grid for all 9 possible tab kinds up
    // front (cheap, empty widgets). Which ones are actually attached to
    // m_tabs — and in what order — is decided by refreshTabBar() below,
    // based on the Settings → Tab Layout preference loaded just above.
    loadTabLayoutSettings();
    // v3.3.38 — load the "Tile Size" (tiles-per-row) setting before the
    // grid gets built, so the very first layout already uses it.
    {
        QSettings prefs("HijackAssassin", "MediaCountdowns");
        m_tilesPerRow = qBound(2, prefs.value("tilesPerRow", 3).toInt(), 5);
    }
    for (int k = 0; k < NUM_KINDS; ++k) {
        auto* scroll = new QScrollArea;
        m_scrollAreas[k] = scroll;
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setStyleSheet(
            "QScrollArea { background:#1a1a1a; border:none; }"
            "QScrollBar:vertical { background:#1e1e1e; width:8px; }"
            "QScrollBar::handle:vertical { background:#444; border-radius:4px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }");

        m_tabContainers[k] = new QWidget;
        m_tabContainers[k]->setStyleSheet("background:#1a1a1a;");

        m_grids[k] = new QGridLayout(m_tabContainers[k]);
        m_grids[k]->setContentsMargins(0, 0, 0, 0);
        m_grids[k]->setSpacing(1);
        m_tabContainers[k]->setLayout(m_grids[k]);

        scroll->setWidget(m_tabContainers[k]);

        m_scrollTarget[k] = 0;
        m_scrollAnim[k] = new QVariantAnimation(this);
        m_scrollAnim[k]->setDuration(350);
        m_scrollAnim[k]->setEasingCurve(QEasingCurve::OutCubic);
        const int ki = k;
        connect(m_scrollAnim[k], &QVariantAnimation::valueChanged,
                this, [this, ki](const QVariant& val) {
            m_scrollAreas[ki]->verticalScrollBar()->setValue(val.toInt());
        });
        installSmoothScroll(scroll);
    }
    applyTilesPerRowStretch();   // v3.3.39 — see mainwindow.cpp for the full explanation
    loadTabColors();    // V4.4 — must happen before refreshTabBar() so its first icon-refresh has real data
    refreshTabBar();   // attaches the currently-enabled kinds to m_tabs, in order
    mainLayout->addWidget(m_tabs, 1);

    // V4.5 — right-click a tab in the top bar for the shared Color Picker /
    // Clear Color / Hide Tab menu (see showTabContextMenu).
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabs->tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint& pos) {
        int index = m_tabs->tabBar()->tabAt(pos);
        if (index < 0 || index >= m_visibleKindOrder.size()) return;
        int kind = m_visibleKindOrder[index];
        showTabContextMenu(kind, m_tabs->tabBar()->mapToGlobal(pos));
    });

    // V4.5 — colored tab outline. See TabColorOverlay's own comment for why
    // this is a separate overlay widget rather than a custom QStyle.
    m_tabColorOverlay = new TabColorOverlay(m_tabs->tabBar());
    m_tabColorOverlay->show();
    refreshTabColorIcons();   // now that the overlay exists, give it real data immediately

    // ── Import / Export corner buttons ────────────────────────────────────
    auto* cornerWidget = new QWidget(m_tabs);
    auto* cornerLayout = new QHBoxLayout(cornerWidget);
    cornerLayout->setContentsMargins(0, 2, 8, 0);
    cornerLayout->setSpacing(6);

    // v3.2.9 fix #1 — matches the 28px sibling button height. Growing the
    // box (tried in earlier rounds) made the whole row look vertically
    // misaligned, which mattered more than a bigger icon helped.
    // v3.2.95 — reverted back to plain "<" and ">" per request (the
    // box-drawing line design from last round wasn't liked). A single
    // character has more room to grow within the fixed 28px height, so
    // the font size is bigger than the multi-character versions used.
    // v3.2.96 fix #1 — nudged the text upward with a small asymmetric
    // bottom padding (shrinks the available space from below, shifting
    // where the centered text lands) without touching the button's own
    // size at all.
    const QString navBtnStyle =
        "QPushButton { background:#2a2a2a; color:#ddd; border:1px solid #444; "
        "border-radius:5px; font-size:20px; font-weight:bold; padding:0px; padding-bottom:4px; }"
        "QPushButton:hover:enabled { background:#383838; border-color:#555; }"
        "QPushButton:disabled { background:#161616; color:#3a3a3a; border-color:#222; }";
    m_tabPrevBtn = new QPushButton("<", cornerWidget);
    m_tabNextBtn = new QPushButton(">", cornerWidget);
    m_tabPrevBtn->setFixedSize(36, 28);
    m_tabNextBtn->setFixedSize(36, 28);
    m_tabPrevBtn->setStyleSheet(navBtnStyle);
    m_tabNextBtn->setStyleSheet(navBtnStyle);
    m_tabPrevBtn->setCursor(Qt::PointingHandCursor);
    m_tabNextBtn->setCursor(Qt::PointingHandCursor);
    // v3.2.0 fix #2 — group switches no longer touch which kind is
    // selected; applyCurrentTabGroup() restores it after updating
    // visibility, even if that kind's tab isn't in the group being shown.
    connect(m_tabPrevBtn, &QPushButton::clicked, this, [this]{
        m_currentTabGroup = prevNonEmptyTabGroup(m_currentTabGroup);
        applyCurrentTabGroup();
    });
    connect(m_tabNextBtn, &QPushButton::clicked, this, [this]{
        m_currentTabGroup = nextNonEmptyTabGroup(m_currentTabGroup);
        applyCurrentTabGroup();
    });
    cornerLayout->addWidget(m_tabPrevBtn);
    cornerLayout->addWidget(m_tabNextBtn);

    // v3.2.6 fix #2 — dropdown listing every group that currently has
    // tabs (e.g. just "Group 1" and "Group 3" if 2 is empty), letting you
    // jump straight to one. Rebuilt and re-selected every time
    // applyCurrentTabGroup() runs, so it always reflects the current row —
    // including when the arrows are what changed it.
    m_tabGroupDropdown = new CenteredComboBox(cornerWidget);
    // v3.2.7 fix #1 — the popup list wasn't styled at all before, leaving
    // it to Qt's default appearance, which didn't contrast well against
    // this dark theme. Styled explicitly now, same pattern used for every
    // other dropdown in this app (see kComboStyle in Settings).
    m_tabGroupDropdown->setStyleSheet(
        "QComboBox { background:#2a2a2a; color:#ddd; border:1px solid #444; "
        "border-radius:5px; font-size:12px; padding:4px 8px; }"
        "QComboBox:hover { background:#383838; }"
        "QComboBox::drop-down { border:none; width:18px; }"
        "QComboBox QAbstractItemView { background:#2a2a2a; color:#ddd; "
        "selection-background-color:#0078d4; selection-color:#fff; outline:none; }");
    // v3.2.9 fix #2 — centering is now handled by CenteredComboBox's own
    // paint override (see its class comment above) instead of an editable
    // line edit, which broke the normal "click anywhere to open" behavior
    // — editable combo boxes only open their popup from the small
    // drop-down arrow specifically.
    connect(m_tabGroupDropdown, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0) return;
        int selectedGroup = m_tabGroupDropdown->itemData(idx).toInt();
        if (selectedGroup == m_currentTabGroup) return;
        m_currentTabGroup = selectedGroup;
        applyCurrentTabGroup();
    });
    cornerLayout->addWidget(m_tabGroupDropdown);

    // v3.2.0 fix #4 — "Manage Tabs" opens a grid (6 rows x 8 slots, one row
    // per group) where tabs can be dragged to any slot, including swapping
    // with an occupied one.
    m_manageTabsBtn = new QPushButton("Manage Tabs", cornerWidget);
    m_manageTabsBtn->setFixedHeight(28);
    m_manageTabsBtn->setStyleSheet(
        "QPushButton { background:#2a2a3a; color:#9999cc; border:1px solid #3a3a5a; "
        "border-radius:5px; font-size:12px; padding:0 12px; }"
        "QPushButton:hover { background:#33334a; }");
    connect(m_manageTabsBtn, &QPushButton::clicked, this, &MainWindow::showManageTabsDialog);
    cornerLayout->addWidget(m_manageTabsBtn);

    // V5.4.22 — Recap sits where Export and Import used to. Those two are
    // things you do once in a while (moving to another machine, restoring a
    // backup) and they have moved to Settings; a list of what has actually
    // come out is something you look at, so it gets the top bar.
    // V5.4.26 — labelled with the name the dialog and its heading already
    // used. "Recap" alone read as the "While You Were Away" popup, which is a
    // different thing entirely (see showRecapDialog).
    auto* recapBtn = new QPushButton("Recap/History", cornerWidget);
    recapBtn->setToolTip("Everything that has released, by date");
    recapBtn->setFixedHeight(28);
    recapBtn->setStyleSheet(
        "QPushButton { background:#1e3a1e; color:#66cc66; border:1px solid #2a5a2a; "
        "border-radius:5px; font-size:12px; padding:0 14px; }"
        "QPushButton:hover { background:#2a4a2a; }");
    connect(recapBtn, &QPushButton::clicked, this, &MainWindow::showRecapDialog);
    cornerLayout->addWidget(recapBtn);

    auto* prefBtn = new QPushButton("\xe2\x9a\x99 Settings", cornerWidget);
    prefBtn->setFixedHeight(28);
    prefBtn->setToolTip("Settings");
    prefBtn->setStyleSheet(
        "QPushButton { background:#1e2a1e; color:#99cc99; border:1px solid #2a5a2a; "
        "border-radius:5px; font-size:12px; padding:0 12px; }"
        "QPushButton:hover { background:#2a3a2a; }");
    connect(prefBtn, &QPushButton::clicked, this, &MainWindow::showPreferencesDialog);
    cornerLayout->addWidget(prefBtn);

    // v3.1.4 — About button, to the right of Settings. v3.1.5 — the icon is
    // rendered as its own white icon (separate from the "About" text) rather
    // than embedded in the button's text, since a single QPushButton can't
    // mix colors within its plain text.
    auto* aboutBtn = new QPushButton("About", cornerWidget);
    aboutBtn->setIcon(makeGlyphIcon(QString::fromUtf8("\xf0\x9f\x9b\x88"), Qt::white, 17));
    aboutBtn->setIconSize(QSize(20, 20));
    aboutBtn->setFixedHeight(28);
    aboutBtn->setToolTip("About");
    // v3.2.7 fix #6 — text no longer needs to be pure white, just readable;
    // switched to a soft tint matching the button's own background, same
    // pattern Export/Import already use.
    aboutBtn->setStyleSheet(
        "QPushButton { background:#1e2233; color:#99aadd; border:1px solid #2a3a5a; "
        "border-radius:5px; font-size:12px; padding:0 12px; }"
        "QPushButton:hover { background:#2a3550; }");
    connect(aboutBtn, &QPushButton::clicked, this, &MainWindow::showAboutDialog);
    cornerLayout->addWidget(aboutBtn);

    // V4.6 — multi-select delete. Toggles select mode: while active, left-
    // clicking a tile selects it (blue border) instead of opening its edit
    // dialog, and right-clicking any tile shows a minimal "Delete All" menu
    // instead of the normal per-tile one.
    //
    // V4.10 — the clear-selection button sits directly to the LEFT of the
    // ⋮ button, in the same single row as every other corner button,
    // rather than stacked underneath it. A vertical stack looked squished
    // against the tab bar's own limited corner height; a normal row entry
    // that simply shows/hides (QHBoxLayout already reclaims its space when
    // hidden) reads much more cleanly.
    m_clearSelectionBtn = new QPushButton("\xe2\x9c\x95", cornerWidget);   // ✕
    m_clearSelectionBtn->setFixedSize(28, 28);
    m_clearSelectionBtn->setToolTip("Clear selection");
    m_clearSelectionBtn->setCursor(Qt::PointingHandCursor);
    m_clearSelectionBtn->setStyleSheet(
        "QPushButton { background:#3a1e1e; color:#e08080; border:1px solid #5a2a2a; "
        "border-radius:5px; font-size:13px; font-weight:bold; padding:0; }"
        "QPushButton:hover { background:#4a2626; }");
    connect(m_clearSelectionBtn, &QPushButton::clicked, this, &MainWindow::clearTileSelection);
    m_clearSelectionBtn->hide();   // nothing selected yet
    cornerLayout->addWidget(m_clearSelectionBtn);

    m_selectModeBtn = new QPushButton("\xe2\x8b\xae", cornerWidget);   // ⋮ (vertical ellipsis)
    m_selectModeBtn->setFixedSize(32, 28);
    m_selectModeBtn->setToolTip("Select tiles to delete");
    m_selectModeBtn->setCursor(Qt::PointingHandCursor);
    m_selectModeBtn->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:5px; font-size:16px; font-weight:bold; padding:0; }"
        "QPushButton:hover { background:#383838; }");
    connect(m_selectModeBtn, &QPushButton::clicked, this, &MainWindow::toggleSelectMode);
    cornerLayout->addWidget(m_selectModeBtn);

    m_tabsCornerWidget = cornerWidget;   // V4.12
    m_tabs->setCornerWidget(cornerWidget, Qt::TopRightCorner);

    // ── Jump-to-tab dropdown, left of the tab bar ──────────────────────────
    // v3.3.41 — a plain arrow with no visible button box (transparent
    // background, no border, matching the tab bar's own dark background)
    // that pops up a menu listing EVERY enabled tab regardless of which
    // page it's currently on, letting you jump straight to any of them —
    // unlike the group dropdown in the right corner, which only lists
    // pages, not individual tabs within them.
    m_jumpToTabBtn = new QToolButton(m_tabs);
    m_jumpToTabBtn->setText("\xe2\x96\xbc");   // ▼
    m_jumpToTabBtn->setPopupMode(QToolButton::InstantPopup);
    m_jumpToTabBtn->setCursor(Qt::PointingHandCursor);
    m_jumpToTabBtn->setToolTip("Jump to a tab");
    m_jumpToTabBtn->setStyleSheet(
        "QToolButton { background:transparent; border:none; color:#888888; "
        "font-size:11px; padding:8px 10px; }"
        "QToolButton:hover { color:#cccccc; }"
        "QToolButton::menu-indicator { image:none; width:0px; }");
    auto* jumpMenu = new QMenu(m_jumpToTabBtn);
    jumpMenu->setStyleSheet(
        "QMenu { background:#232323; color:#ddd; border:1px solid #3a3a3a; }");
    // V4.14 — a real QListWidget embedded via QWidgetAction, rather than
    // plain QAction entries, specifically so tabs can be dragged into a
    // new order right here — the same InternalMove mechanism used by
    // QListWidget everywhere else, not a custom-built drag implementation.
    m_jumpToTabList = new QListWidget(jumpMenu);
    m_jumpToTabList->setDragDropMode(QAbstractItemView::InternalMove);
    m_jumpToTabList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_jumpToTabList->setFocusPolicy(Qt::NoFocus);
    m_jumpToTabList->setIconSize(QSize(14, 14));
    m_jumpToTabList->setFrameShape(QFrame::NoFrame);
    m_jumpToTabList->setStyleSheet(
        "QListWidget { background:#232323; color:#ddd; border:none; outline:none; }"
        "QListWidget::item { padding:6px 10px; }"
        "QListWidget::item:selected { background:#0078d4; color:#fff; }"
        "QListWidget::item:hover:!selected { background:#2d2d2d; }");
    auto* jumpWidgetAction = new QWidgetAction(jumpMenu);
    jumpWidgetAction->setDefaultWidget(m_jumpToTabList);
    jumpMenu->addAction(jumpWidgetAction);
    connect(m_jumpToTabList, &QListWidget::itemClicked, this, &MainWindow::onJumpToTabItemClicked);
    connect(m_jumpToTabList->model(), &QAbstractItemModel::rowsMoved, this, &MainWindow::onJumpToTabReordered);
    m_jumpToTabBtn->setMenu(jumpMenu);
    m_tabs->setCornerWidget(m_jumpToTabBtn, Qt::TopLeftCorner);
    refreshJumpToTabMenu();   // v3.3.41 fix — see comment above for why this can't just rely on refreshTabBar()'s own call

    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int newTab) {
        populateActiveKindGrid();
        if (m_tabColorOverlay) m_tabColorOverlay->update();   // V4.6 — selected tab affects which one shows its custom color
        int kind = (newTab >= 0 && newTab < m_visibleKindOrder.size())
                   ? m_visibleKindOrder[newTab] : -1;
        for (TileWidget* tw : std::as_const(m_tileWidgets))
            if (kind >= 0 && tileMatchesKind(tw->tileData(), kind)) tw->tick(true);
    });

    m_bottomBar = new QWidget(central);
    m_bottomBar->setStyleSheet("background-color:#0d0d0d; border-top:1px solid #2a2a2a;");
    m_bottomBar->setFixedHeight(80);
    auto* bl = new QVBoxLayout(m_bottomBar);
    bl->setContentsMargins(14, 10, 14, 4);
    bl->setSpacing(4);

    auto* inputRow = new QHBoxLayout;
    inputRow->setSpacing(10);

    // v3.3.0 — toggles which API a search targets. Movie/TV routes to TMDB
    // (m_scraper) as before; Game routes to IGDB (m_igdbScraper) instead.
    m_searchModeBtn = new QPushButton("🎬  Movie/TV", m_bottomBar);
    m_searchModeBtn->setFixedSize(120, 48);
    m_searchModeBtn->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:6px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }");
    connect(m_searchModeBtn, &QPushButton::clicked, this, [this]{
        m_searchModeGame = !m_searchModeGame;
        m_searchModeBtn->setText(m_searchModeGame ? "🎮  Game" : "🎬  Movie/TV");
        m_searchEdit->setPlaceholderText(m_searchModeGame
            ? "Type a game name…   e.g.  Grand Theft Auto VI,  Hollow Knight Silksong"
            : "Type a movie or show name…   e.g.  Invincible,  Supergirl 2026,  The Batman 2022");
        m_searchBtn->setText(m_searchModeGame ? "🔍  Search Game" : "🔍  Search Media");
        hidePicker();
        m_searchEdit->clear();
    });
    inputRow->addWidget(m_searchModeBtn);

    m_searchEdit = new QLineEdit(m_bottomBar);
    m_searchEdit->setPlaceholderText(
        "Type a movie or show name…   e.g.  Invincible,  Supergirl 2026,  The Batman 2022");
    m_searchEdit->setMinimumHeight(48);
    m_searchEdit->setStyleSheet(
        "QLineEdit { background:#1e1e1e; color:#fff; border:1px solid #3a3a3a; "
        "border-radius:6px; padding:0 14px; font-size:16px; }"
        "QLineEdit:focus { border-color:#0078d4; }");
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearchClicked);
    connect(m_searchEdit, &QLineEdit::textEdited, this, [this](const QString& txt){
        if (txt.trimmed().isEmpty()) hidePicker();
    });
    inputRow->addWidget(m_searchEdit, 1);

    m_searchBtn = new QPushButton("🔍  Search Media", m_bottomBar);
    m_searchBtn->setFixedSize(170, 48);
    m_searchBtn->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; border-radius:6px; "
        "font-size:15px; }"
        "QPushButton:hover { background:#1a8de4; }"
        "QPushButton:disabled { background:#2a2a2a; color:#555; }");
    connect(m_searchBtn, &QPushButton::clicked, this, &MainWindow::onSearchClicked);
    inputRow->addWidget(m_searchBtn);

    auto* customBtn = new QPushButton("✦  Custom Tile", m_bottomBar);
    customBtn->setFixedSize(140, 48);
    customBtn->setStyleSheet(
        "QPushButton { background:#2a2a4a; color:#aaaaee; border:1px solid #4a4a7a; "
        "border-radius:6px; font-size:15px; }"
        "QPushButton:hover { background:#3a3a6a; }");
    connect(customBtn, &QPushButton::clicked, this, &MainWindow::onCustomTileClicked);
    inputRow->addWidget(customBtn);
    bl->addLayout(inputRow);

    m_statusLbl = new QLabel("", m_bottomBar);
    m_statusLbl->setStyleSheet(kStatusStyleNeutral);
    m_statusLbl->setFixedHeight(14);
    bl->addWidget(m_statusLbl);
    mainLayout->addWidget(m_bottomBar);

    // V4.8 — sits directly on top of m_bottomBar (same parent, same visual
    // style) while select mode is active, so there's no ambiguity about
    // being in it — the search bar and Custom Tile button are completely
    // covered rather than just slightly different-looking. Hidden by
    // default; toggleSelectMode() shows/hides and keeps it positioned.
    m_selectModeOverlay = new QWidget(central);
    m_selectModeOverlay->setStyleSheet("background-color:#0d0d0d; border-top:1px solid #2a2a2a;");
    auto* overlayLay = new QVBoxLayout(m_selectModeOverlay);
    overlayLay->setContentsMargins(0, 0, 0, 0);
    auto* overlayLbl = new QLabel("Currently In Multi-Select Mode", m_selectModeOverlay);
    overlayLbl->setAlignment(Qt::AlignCenter);
    overlayLbl->setStyleSheet("color:#0078d4; font-size:16px; font-weight:bold; background:transparent;");
    overlayLay->addWidget(overlayLbl);
    m_selectModeOverlay->hide();

    m_pickerFrame = new QWidget(central);
    m_pickerFrame->setStyleSheet(
        "QWidget { background:#1e1e1e; border:1px solid #3a3a3a; border-radius:6px; }");
    m_pickerFrame->hide();
    auto* pfl = new QVBoxLayout(m_pickerFrame);
    pfl->setContentsMargins(0,0,0,0);
    pfl->setSpacing(0);

    // Dismiss button — arrow pointing down, sits at the top of the popup
    auto* dismissBtn = new QPushButton("▼  Dismiss", m_pickerFrame);
    dismissBtn->setFixedHeight(28);
    dismissBtn->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#888888; border:none; "
        "border-bottom:1px solid #3a3a3a; font-size:12px; }"
        "QPushButton:hover { background:#333333; color:#aaaaaa; }");
    connect(dismissBtn, &QPushButton::clicked, this, &MainWindow::hidePicker);
    pfl->addWidget(dismissBtn);

    m_pickerList = new QListWidget(m_pickerFrame);
    m_pickerList->setStyleSheet(
        "QListWidget { background:#1e1e1e; border:none; color:#fff; font-size:13px; }"
        "QListWidget::item { padding:10px 14px; border-bottom:1px solid #2a2a2a; }"
        "QListWidget::item:selected, QListWidget::item:hover { background:#0078d4; }");
    m_pickerList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pickerList->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    m_pickerList->setWordWrap(true);
    m_pickerList->installEventFilter(this);
    connect(m_pickerList, &QListWidget::itemClicked, this, &MainWindow::onPickerItemActivated);
    pfl->addWidget(m_pickerList);
    central->installEventFilter(this);

    m_globalTick = new QTimer(this);
    m_globalTick->setInterval(1000);
    m_globalTick->setTimerType(Qt::PreciseTimer);
    connect(m_globalTick, &QTimer::timeout, this, &MainWindow::onGlobalTick);
    m_globalTick->start();

    setupDebugWindow();

    // Hidden tray icon used exclusively by fireDirectNotification() for test
    // notifications fired directly from the main app (no IPC to TrayApp).
    // NOTE: intentionally NOT calling show() here — we only show it briefly
    // when actually firing a notification, then hide it again. Showing it at
    // startup creates a phantom second tray icon alongside the real notifier.
    m_testTray = new QSystemTrayIcon(
        QApplication::style()->standardIcon(QStyle::SP_MediaPlay), this);

    loadTiles();   // V5 — captures the missed-release snapshot internally,
                   // before it kicks off the refresh that would overwrite it

    // v3.3.14 fix — previously refreshAllTiles() only ever ran once, here
    // at startup. If the app stays open for days without a restart, tiles
    // never learn about episodes that have since aired — this is very
    // likely why a tile could sit showing an episode that already
    // released as if it were still upcoming. Re-checks every 6 hours,
    // balancing staying current against not hammering TMDB/IGDB.
    // V5 — hand-entered air-time corrections from the relay, pulled on the
    // same cadence as the tile data they modify.
    // V5.4.8 — before anything talks to the relay, make sure this
    // installation has an id the relay will actually recognise.
    ensureRegisteredWithRelay();

    ShowOverrides::instance().refresh();
    connect(&ShowOverrides::instance(), &ShowOverrides::updated, this, [this]{
        bool any = false;
        for (TileData& td : m_tiles)
            if (ShowOverrides::instance().apply(td)) {
                any = true;
                for (TileWidget* w : std::as_const(m_tileWidgets))
                    if (w->tileData().id == td.id) { w->updateData(td); break; }
            }
        if (any) { APPLOG("ShowOverrides: applied relay air-time corrections"); saveTiles(); }
    });

    m_dataRefreshTimer = new QTimer(this);
    connect(m_dataRefreshTimer, &QTimer::timeout, this, [this]{
        ShowOverrides::instance().refresh();
        refreshAllTiles();
    });
    m_dataRefreshTimer->start(6 * 60 * 60 * 1000);
    // V5 — startup order, deliberately serial:
    //
    //   1. recap of what released while the app was closed
    //   2. update check
    //   3. the normal data refresh
    //
    // The recap goes first so it reports against the tiles exactly as they
    // were saved, with nothing having moved yet, and the update prompt waits
    // behind it so two dialogs can never stack. The refresh is last because
    // it is what advances episodes past the very releases being reported.
    //
    // A short delay lets the window finish showing first; the dialog is
    // otherwise parented to a window still being laid out.
    QTimer::singleShot(400, this, [this]{
        if (!showMissedReleases()) {
            // Nothing missed — the check passes silently and startup carries
            // straight on.
            checkForUpdates();
            runStartupRefresh();
        } else {
            // Dismissal chains the rest. This is only the backstop for a
            // recap left open: the data shouldn't go stale because a dialog
            // is sitting there unread.
            QTimer::singleShot(60000, this, [this]{ runStartupRefresh(); });
        }
    });
    // V5.4.12 — the message of the day is asked for once the tiles have
    // finished loading (see the refresh completion in onTileRefreshed), not on
    // a timer here. By then the recap and the update prompt have had their
    // turn, so it needs neither a delay nor anything to poll.
    // v3.3.35 — "What's New" / changelog feature removed entirely
    // (previously here and reachable from Settings), per request.

    // V4.13 fix — updateTabBarMaxWidth() gets called during construction
    // (via refreshTabBar()/applyCurrentTabGroup()), but at that point the
    // window may still be at whatever transient size it has before the
    // caller's resize()/show() actually take effect — this recalculates
    // once more after the event loop actually starts, by which point the
    // window has reached its real, final size, regardless of when the
    // caller resized/showed it relative to construction.
    QTimer::singleShot(0, this, [this]{ updateTabBarMaxWidth(); });
}

MainWindow::~MainWindow() { saveTiles(); }

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        // Picker list: always scroll exactly 1 item per wheel tick
        if (obj == m_pickerList->viewport()) {
            auto* we = static_cast<QWheelEvent*>(event);
            int dir = we->angleDelta().y() > 0 ? -1 : 1;
            int cur = m_pickerList->verticalScrollBar()->value();
            m_pickerList->verticalScrollBar()->setValue(cur + dir);
            return true;
        }
        for (int t = 0; t < NUM_KINDS; ++t) {
            if (m_scrollAreas[t] && obj == m_scrollAreas[t]->viewport()) {
                auto* we = static_cast<QWheelEvent*>(event);
                int pixels = -(we->angleDelta().y() * 120) / 120;
                smoothScrollBy(m_scrollAreas[t], pixels);
                return true;
            }
        }
    }
    if (event->type() == QEvent::MouseButtonPress && m_pickerFrame->isVisible()) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (!m_pickerFrame->geometry().contains(me->pos())) hidePicker();
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (m_pickerFrame->isVisible()) repositionPicker();
    if (m_tabColorOverlay) m_tabColorOverlay->syncGeometry();   // V4.5
    if (m_selectModeOverlay) m_selectModeOverlay->setGeometry(m_bottomBar->geometry());   // V4.8
    updateTabBarMaxWidth();   // V4.12
}

// =============================================================================
//  detectTabResolutionTier — v3.3.42. Picks how many tabs fit per group
//  (and how Manage Tabs visually arranges them) based on the monitor's
//  logical resolution — the same figures Patrick measured directly: 4
//  tabs fit at 720p, 8 at 1080p, 12 at 1440p, and (an educated guess,
//  since it hasn't been directly measured) 16 at 4K. Uses logical pixels
//  (QScreen::size(), which already accounts for OS-level display scaling)
//  rather than raw physical resolution, since that's what actually
//  determines how much room tab labels have to work with. Higher tab
//  counts split into multiple rows within Manage Tabs (2x6 at 1440p, 2x8
//  at 4K) and use fewer overall groups, since a group takes up more
//  dialog space once it's more than one row tall — 6 groups at 8-or-fewer
//  per row, 3 once a group becomes 2 rows.
// =============================================================================
void MainWindow::detectTabResolutionTier()
{
    int width = 1920;   // sensible fallback if no screen can be detected
    if (QScreen* screen = QGuiApplication::primaryScreen())
        width = screen->size().width();

    if (width < 1600) {          // ~720p tier
        m_tabsPerGroup = 4;  m_tabManagerRows = 1;  m_tabManagerCols = 4;  m_tabGroupCount = 6;
    } else if (width < 2200) {   // ~1080p tier (default/baseline)
        m_tabsPerGroup = 8;  m_tabManagerRows = 1;  m_tabManagerCols = 8;  m_tabGroupCount = 6;
    } else if (width < 3200) {   // ~1440p tier
        m_tabsPerGroup = 12; m_tabManagerRows = 2;  m_tabManagerCols = 6;  m_tabGroupCount = 3;
    } else {                     // ~4K tier and beyond
        m_tabsPerGroup = 16; m_tabManagerRows = 2;  m_tabManagerCols = 8;  m_tabGroupCount = 3;
    }
    APPLOG(QString("detectTabResolutionTier: screen width %1 -> %2 tabs/group, "
                    "%3 groups, Manage Tabs grid %4x%5")
           .arg(width).arg(m_tabsPerGroup).arg(m_tabGroupCount)
           .arg(m_tabManagerRows).arg(m_tabManagerCols));
}

// =============================================================================
//  Tab slot assignment — v3.2.0. Complete redesign: instead of an
//  automatic, width-computed grouping, tabs live in a persistent 6x8 grid
//  (6 groups/rows, 8 slots/columns each — see loadTabSlotAssignment,
//  reconcileTabSlots, showManageTabsDialog). Group 8-tab cap and layout are
//  now structural (a fixed-size grid), not computed.
// =============================================================================
void MainWindow::loadTabSlotAssignment()
{
    detectTabResolutionTier();   // v3.3.42 — must run first; sizes below depend on it
    m_tabSlots = QVector<QVector<int>>(m_tabGroupCount, QVector<int>(m_tabsPerGroup, -1));

    QSettings settings("HijackAssassin", "MediaCountdowns");
    QStringList saved = settings.value("tabSlotAssignment").toStringList();

    if (saved.size() == m_tabGroupCount * m_tabsPerGroup) {
        int idx = 0;
        for (int g = 0; g < m_tabGroupCount; ++g) {
            for (int s = 0; s < m_tabsPerGroup; ++s) {
                const QString& key = saved[idx++];
                // v3.2.1 — empty slots are saved as "_empty_", not "", since
                // some QSettings backends (the Windows registry one in
                // particular) can mishandle empty strings inside a
                // QStringList, which would silently corrupt this whole
                // round-trip. "_empty_" is never a valid kind key so this
                // is unambiguous either way.
                if (key.isEmpty() || key == "_empty_") continue;
                for (int k = 0; k < NUM_KINDS; ++k)
                    if (key == kKindKeys[k]) { m_tabSlots[g][s] = k; break; }
            }
        }
    } else {
        if (!saved.isEmpty())
            APPLOG(QString("loadTabSlotAssignment: saved list has %1 entries, expected %2 for the "
                            "current resolution tier — falling back to default")
                   .arg(saved.size()).arg(m_tabGroupCount * m_tabsPerGroup));
        // First run, upgrading from a version before this feature existed,
        // or the app is now running at a different resolution tier than
        // when it was last saved — derive a sensible default from the
        // existing tab order: fill group 1 first, then group 2, and so on.
        QList<int> order = loadTabOrder();
        int g = 0, s = 0;
        for (int k : order) {
            if (!m_enabledKinds.contains(k)) continue;
            if ((k == K_OTHER || k == K_FAVORITE) && !kindHasAnyTile(k)) continue;   // V4.12
            if (g >= m_tabGroupCount) break;
            m_tabSlots[g][s] = k;
            if (++s >= m_tabsPerGroup) { s = 0; ++g; }
        }
    }
}

void MainWindow::saveTabSlotAssignment()
{
    QStringList out;
    for (int g = 0; g < m_tabGroupCount; ++g)
        for (int s = 0; s < m_tabsPerGroup; ++s)
            out << (m_tabSlots[g][s] >= 0 ? kKindKeys[m_tabSlots[g][s]] : QString("_empty_"));
    QSettings settings("HijackAssassin", "MediaCountdowns");
    settings.setValue("tabSlotAssignment", out);
}

// Syncs the slot grid with the current enabled/disabled kind set: clears
// any slot holding a now-disabled kind (or "Other" with no tiles right
// now), then places any newly-valid kind not already somewhere in the
// grid into the first empty slot found (scanning group 1 slot 1 onward).
void MainWindow::reconcileTabSlots()
{
    for (int g = 0; g < m_tabGroupCount; ++g) {
        for (int s = 0; s < m_tabsPerGroup; ++s) {
            int k = m_tabSlots[g][s];
            if (k < 0) continue;
            bool stillValid = m_enabledKinds.contains(k) &&
                              ((k != K_OTHER && k != K_FAVORITE) || kindHasAnyTile(k));   // V4.12
            if (!stillValid) m_tabSlots[g][s] = -1;
        }
    }

    QSet<int> placed;
    for (int g = 0; g < m_tabGroupCount; ++g)
        for (int s = 0; s < m_tabsPerGroup; ++s)
            if (m_tabSlots[g][s] >= 0) placed.insert(m_tabSlots[g][s]);

    for (int k = 0; k < NUM_KINDS; ++k) {
        if (!m_enabledKinds.contains(k)) continue;
        if ((k == K_OTHER || k == K_FAVORITE) && !kindHasAnyTile(k)) continue;   // V4.12
        if (placed.contains(k)) continue;
        bool didPlace = false;
        for (int g = 0; g < m_tabGroupCount && !didPlace; ++g)
            for (int s = 0; s < m_tabsPerGroup && !didPlace; ++s)
                if (m_tabSlots[g][s] == -1) { m_tabSlots[g][s] = k; didPlace = true; }
    }
}

// v3.2.1 — defensive: dedupe and bounds-check, regardless of how m_tabSlots
// got into whatever state it's in. If the same kind somehow ends up in more
// than one slot (a settings round-trip issue, a stray write, anything), this
// stops it from producing duplicate tabs — each kind appears at most once,
// at its first occurrence in group-major/slot order, and anything outside
// the valid kind range is silently dropped rather than passed through.
QVector<int> MainWindow::flattenTabSlots() const
{
    QVector<int> result;
    QSet<int> seen;
    for (int g = 0; g < m_tabGroupCount; ++g) {
        for (int s = 0; s < m_tabsPerGroup; ++s) {
            int k = m_tabSlots[g][s];
            if (k < 0 || k >= NUM_KINDS) continue;
            if (seen.contains(k)) {
                APPLOG(QString("flattenTabSlots: kind %1 found more than once (at group %2 slot %3) — skipping duplicate")
                       .arg(k).arg(g).arg(s));
                continue;
            }
            seen.insert(k);
            result.append(k);
        }
    }
    return result;
}

bool MainWindow::tabGroupHasAnyTab(int group) const
{
    if (group < 0 || group >= m_tabSlots.size()) return false;
    for (int s = 0; s < m_tabsPerGroup; ++s)
        if (m_tabSlots[group][s] >= 0) return true;
    return false;
}

// v3.2.0 fix #4 — skips empty groups entirely (e.g. groups 1 and 6 occupied,
// 2-5 empty: Next from group 1 jumps straight to group 6). Does not wrap
// around; returns 'from' unchanged if there's no further non-empty group.
int MainWindow::nextNonEmptyTabGroup(int from) const
{
    for (int g = from + 1; g < m_tabGroupCount; ++g)
        if (tabGroupHasAnyTab(g)) return g;
    return from;
}
int MainWindow::prevNonEmptyTabGroup(int from) const
{
    for (int g = from - 1; g >= 0; --g)
        if (tabGroupHasAnyTab(g)) return g;
    return from;
}

// =============================================================================
//  applyCurrentTabGroup — v3.2.0 fix #2. Shows/hides tabs to match
//  m_currentTabGroup, then explicitly restores whichever tab was selected
//  beforehand — even if that tab is now hidden. Confirmed directly that Qt
//  allows setCurrentIndex() to target a hidden tab and still display its
//  content correctly; the previously-selected kind stays on screen across
//  group switches instead of jumping to whatever the new group's first tab
//  happens to be.
// =============================================================================
// =============================================================================
//  updateTabBarMaxWidth — V4.12 fix. Reported bug: tabs weren't shrinking
//  to fit before the group-switch arrows, so as the corner widget grew
//  wider over several rounds of feature additions (the arrows themselves,
//  the jump-to-tab dropdown, Import/Export/Manage Tabs/Settings/About, and
//  the select-mode ⋮/✕ buttons), tabs increasingly overflowed into it
//  instead of shrinking to make room. setExpanding(false) (see its own
//  comment above) means tabs never shrink on their own — this constrains
//  each visible tab's maximum width to whatever actually still fits,
//  combined with the elide mode set alongside setExpanding so anything
//  that gets truncated shows "…" rather than clipping abruptly.
//
//  Rebuilds from kTabBarBaseStyle each time rather than appending to
//  whatever the current stylesheet already is — appending on every call
//  (window resizes fire this often) would otherwise grow the stylesheet
//  string without bound.
// =============================================================================
void MainWindow::updateTabBarMaxWidth()
{
    if (!m_tabs || !m_tabsCornerWidget || !m_jumpToTabBtn) return;
    QTabBar* bar = m_tabs->tabBar();
    int visibleCount = 0;
    for (int i = 0; i < bar->count(); ++i)
        if (bar->isTabVisible(i)) ++visibleCount;
    if (visibleCount == 0) return;

    int reserved = m_tabsCornerWidget->sizeHint().width() + m_jumpToTabBtn->sizeHint().width() + 24;
    int available = m_tabs->width() - reserved;
    int perTab = available / visibleCount;
    // A floor so tabs never become unreadably tiny under extreme squeeze —
    // elideMode handles anything narrower than a label needs.
    perTab = qMax(perTab, 50);

    m_tabs->setStyleSheet(QString(kTabBarBaseStyle) +
                           QString(" QTabBar::tab { max-width: %1px; }").arg(perTab));
}

void MainWindow::applyCurrentTabGroup()
{
    if (!m_tabs) return;
    // v3.2.0 — refreshTabBar() is called once early in the constructor,
    // before the corner widget (which owns these buttons) is built; guard
    // against that ordering rather than relying on construction order,
    // since the buttons get their real state from the next refreshTabBar()
    // call anyway (right after the corner widget is set up).
    if (!m_tabPrevBtn || !m_tabNextBtn) return;
    QTabBar* bar = m_tabs->tabBar();
    int count = bar->count();
    if (count == 0) { m_tabPrevBtn->setVisible(false); m_tabNextBtn->setVisible(false); return; }

    // v3.2.5 fix #1 — if the current group has become empty (its only tab
    // just got moved elsewhere in Manage Tabs, for instance), move to the
    // nearest group that still has tabs instead of leaving the view on one
    // with nothing to show (previously this could blank the entire tab
    // bar until the app was restarted). Searches toward group 1 first —
    // e.g. group 3 emptying out moves to group 2, not group 4 — falling
    // back to searching forward only if nothing is found that way.
    if (!tabGroupHasAnyTab(m_currentTabGroup)) {
        int candidate = prevNonEmptyTabGroup(m_currentTabGroup);
        if (candidate == m_currentTabGroup)
            candidate = nextNonEmptyTabGroup(m_currentTabGroup);
        m_currentTabGroup = candidate;
    }

    int savedIndex = m_tabs->currentIndex();

    QSet<int> groupKinds;
    if (m_currentTabGroup >= 0 && m_currentTabGroup < m_tabSlots.size())
        for (int s = 0; s < m_tabsPerGroup; ++s)
            if (m_tabSlots[m_currentTabGroup][s] >= 0) groupKinds.insert(m_tabSlots[m_currentTabGroup][s]);

    // v3.2.2 — block signals while toggling visibility. Hiding the
    // currently-selected tab makes Qt transiently reassign the current
    // index mid-loop (once per tab that gets hidden), and each of those
    // would normally fire currentChanged — which runs the full detach/
    // reattach dance in populateActiveKindGrid() for every tile, every
    // time. Blocking here means that only runs once, from the explicit
    // setCurrentIndex() below, instead of potentially many times per
    // single arrow click.
    m_tabs->blockSignals(true);
    // v3.2.8 fix #1 — also clear the tab's label text when it's hidden
    // (restoring the correct label when it's visible again). Confirmed
    // directly that a tab which is both hidden AND still the "current" one
    // (our own design, so its content keeps showing per an earlier
    // request) gets a degenerate (0,0, 0x0) rect from Qt's own tabRect() —
    // a zero-size rectangle sitting at the top-left corner. That's
    // consistent with the reported "stray text above the first tile" only
    // showing up while viewing a group that doesn't contain the selected
    // tab: if Qt's paint code still tries to draw that tab's label using
    // stale/degenerate geometry, there's nothing to draw once the text
    // itself is empty.
    for (int i = 0; i < count; ++i) {
        int kind = (i < m_visibleKindOrder.size()) ? m_visibleKindOrder[i] : -1;
        bool visible = groupKinds.contains(kind);
        bar->setTabVisible(i, visible);
        bar->setTabText(i, visible && kind >= 0 ? kKindLabels[kind] : QString());
    }
    m_tabs->blockSignals(false);

    if (savedIndex >= 0 && savedIndex < count)
        m_tabs->setCurrentIndex(savedIndex);

    // v3.2.4 fix #1/#2 — force QTabBar to fully recompute its internal tab
    // layout after changing visibility. Without this, switching to a group
    // whose tab set differs from the previous one could leave stale
    // geometry behind: a leftover fragment of a previous tab's label still
    // rendering in the corner, or tabs seeming to vanish until something
    // else (like opening and closing a dialog) forces a fresh layout pass.
    // A momentary 1px resize and back reliably kicks QTabBar's layout
    // engine into recomputing everything from scratch, rather than relying
    // on setTabVisible() alone to trigger it.
    QSize barSize = bar->size();
    bar->resize(barSize.width() + 1, barSize.height());
    bar->resize(barSize);
    bar->updateGeometry();
    // v3.2.7 fix #4 — the stale-render artifact (reported as leftover text
    // appearing above the first tile) persisted even after v3.2.4's fix,
    // which only refreshed the tab bar itself. Widening this to repaint
    // the whole tab widget — not just the bar — and using an immediate
    // repaint() rather than a deferred update(), in case the artifact sits
    // right at the boundary between the bar and the content area below it.
    bar->repaint();
    m_tabs->update();

    bool canPrev = prevNonEmptyTabGroup(m_currentTabGroup) != m_currentTabGroup;
    bool canNext = nextNonEmptyTabGroup(m_currentTabGroup) != m_currentTabGroup;
    bool hasOverflow = canPrev || canNext;
    m_tabPrevBtn->setVisible(hasOverflow);
    m_tabNextBtn->setVisible(hasOverflow);
    m_tabPrevBtn->setEnabled(canPrev);
    m_tabNextBtn->setEnabled(canNext);
    // v3.2.92 fix #2 — no more icon-swapping needed now that these are
    // plain text buttons again; the QSS :disabled color rule applies
    // directly to button text, unlike a fixed-color pixmap icon.

    // v3.2.6 fix #2 — rebuild the dropdown's list of non-empty groups and
    // re-select whichever one matches m_currentTabGroup, so it always
    // reflects the current row regardless of what changed it (arrows,
    // picking a different item, Manage Tabs, etc).
    m_tabGroupDropdown->blockSignals(true);
    m_tabGroupDropdown->clear();
    int selectIdx = -1;
    for (int g = 0; g < m_tabGroupCount; ++g) {
        if (!tabGroupHasAnyTab(g)) continue;
        m_tabGroupDropdown->addItem(QString("Group %1").arg(g + 1), g);
        if (g == m_currentTabGroup) selectIdx = m_tabGroupDropdown->count() - 1;
    }
    if (selectIdx >= 0) m_tabGroupDropdown->setCurrentIndex(selectIdx);
    m_tabGroupDropdown->blockSignals(false);
    m_tabGroupDropdown->setVisible(m_tabGroupDropdown->count() > 1);

    // v3.2.8 fix #3 — AdjustToContents wasn't reliably sizing the box once
    // combined with the editable line-edit used for centering, leaving
    // text clipped/unreadable. Computing the needed width directly from
    // font metrics (using the widest label that could ever appear, "Group
    // N" for the highest possible group number, so it doesn't need to
    // resize every time the selection changes) is more reliable.
    // v3.2.93 — this used to query m_tabGroupDropdown->font() directly,
    // but that isn't guaranteed to reflect the QSS "font-size:12px" rule
    // reliably (QSS font propagation back into a widget's own font()
    // property can lag behind or not happen at the time this runs). If
    // this computed the needed width using a different, likely LARGER
    // font than what's actually rendered, the box would end up wider than
    // the real text needs — leaving slack space for it to drift off-
    // center within, which matches what's being seen. Explicitly matching
    // the exact pixel size from the stylesheet removes that ambiguity.
    QFont dropdownFont = m_tabGroupDropdown->font();
    dropdownFont.setPixelSize(12);
    QFontMetrics dropdownFm(dropdownFont);
    int neededTextWidth = dropdownFm.horizontalAdvance(QString("Group %1").arg(NUM_KINDS));
    // v3.2.92 fix #1 — uses the exact same margins as CenteredComboBox's
    // own paint logic (see its class comment), so the text area this
    // produces is precisely as wide as the text needs — no slack space
    // left over for it to look off-center within.
    m_tabGroupDropdown->setFixedWidth(neededTextWidth + CenteredComboBox::kLeftMargin + CenteredComboBox::kRightMargin);

    // V4.6 fix — reported bug: hiding the currently-selected tab could move
    // you to another tab whose tiles then simply never appeared, until
    // switching away and back. Root cause: QTabWidget::setCurrentIndex()
    // is a silent no-op (fires no currentChanged signal at all) whenever
    // the target index already equals the current one — which is exactly
    // what happens here, since addTab() during a rebuild already auto-
    // selects index 0, making the explicit setCurrentIndex(savedIndex)
    // above a no-op when savedIndex is also 0. Calling this directly,
    // unconditionally, removes any dependency on that signal actually
    // having fired — this function is the last thing that runs after any
    // tab-structure change, so guaranteeing the invariant here directly is
    // more robust than trying to reason about which specific
    // setCurrentIndex() call above did or didn't trigger it.
    populateActiveKindGrid();
    updateTabBarMaxWidth();   // V4.12
}

// =============================================================================
//  showManageTabsDialog — v3.2.0 fix #4. A 6-row x 8-slot grid (row = group,
//  matched top-to-bottom with group 1 through 6) where tabs can be dragged
//  to any slot, including swapping with an occupied one. Changes are only
//  committed if the dialog is accepted; Cancel/closing restores the
//  original arrangement.
// =============================================================================
void MainWindow::showManageTabsDialog()
{
    QVector<QVector<int>> backup = m_tabSlots;
    // v3.2.3 fix #2 — remember which row/group we were viewing before the
    // dialog opens. refreshTabBar() (called below) normally jumps to
    // whichever group contains the currently-selected tab — useful when
    // the enabled-tab set changes elsewhere, but not what's wanted here:
    // moving the selected tab to a different row in this dialog shouldn't
    // pull the view along with it.
    int groupBeforeDialog = m_currentTabGroup;

    QDialog dlg(this);
    dlg.setWindowTitle("Manage Tabs");
    dlg.setStyleSheet("QDialog { background:#1e1e1e; }");

    auto* vlay = new QVBoxLayout(&dlg);

    auto* info = new QLabel(
        "Drag a tab to move it, or drop it onto another to swap places. Each "
        "labeled block is a group — the tab arrows jump between groups that "
        "have at least one tab.", &dlg);
    info->setWordWrap(true);
    info->setStyleSheet("color:#aaa; font-size:12px;");
    vlay->addWidget(info);

    auto* grid = new QGridLayout();
    grid->setSpacing(6);
    QVector<QVector<TabSlotBox*>> boxes(m_tabGroupCount, QVector<TabSlotBox*>(m_tabsPerGroup, nullptr));

    auto refreshBox = [&](int g, int s) {
        int k = m_tabSlots[g][s];
        boxes[g][s]->setKind(k, k >= 0 ? kKindLabels[k] : QString());
        boxes[g][s]->setColor(k >= 0 ? m_tabColors[k] : QColor());
    };

    // v3.3.42 — each group's m_tabsPerGroup slots are arranged into
    // m_tabManagerRows visual rows of m_tabManagerCols columns (e.g. 2
    // rows of 6 at 1440p, instead of always a single row of 8) — slot
    // index s still means the same thing everywhere else (flattenTabSlots,
    // saveTabSlotAssignment, etc.), only its position within this dialog's
    // grid changes. One blank spacer row separates each group's block from
    // the next so multi-row groups don't visually run together.
    int gridRowsPerGroup = m_tabManagerRows + 1;   // +1 for the spacer row
    for (int g = 0; g < m_tabGroupCount; ++g) {
        int groupTopRow = g * gridRowsPerGroup;
        auto* rowLabel = new QLabel(QString("Group %1").arg(g + 1), &dlg);
        rowLabel->setStyleSheet("color:#888; font-size:11px;");
        grid->addWidget(rowLabel, groupTopRow, m_tabManagerCols, m_tabManagerRows, 1);
        for (int s = 0; s < m_tabsPerGroup; ++s) {
            int visualRow = s / m_tabManagerCols;
            int visualCol = s % m_tabManagerCols;
            auto* box = new TabSlotBox(g, s, &dlg);
            boxes[g][s] = box;
            grid->addWidget(box, groupTopRow + visualRow, visualCol);
            connect(box, &TabSlotBox::dropped, &dlg, [this, &boxes, refreshBox](int fg, int fs, int tg, int ts) {
                if (fg == tg && fs == ts) return;
                std::swap(m_tabSlots[fg][fs], m_tabSlots[tg][ts]);
                refreshBox(fg, fs);
                refreshBox(tg, ts);
            });
            connect(box, &TabSlotBox::contextMenuRequestedForKind, &dlg, [this, g, s, refreshBox](int kind, QPoint globalPos) {
                showTabContextMenu(kind, globalPos);
                refreshBox(g, s);   // this box's own color-dot may have just changed
            });
        }
        grid->setRowMinimumHeight(groupTopRow + m_tabManagerRows, 8);   // the spacer row
    }
    for (int g = 0; g < m_tabGroupCount; ++g)
        for (int s = 0; s < m_tabsPerGroup; ++s)
            refreshBox(g, s);

    vlay->addLayout(grid);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto* cancelBtn = new QPushButton("Cancel", &dlg);
    cancelBtn->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:4px; padding:8px 20px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }");
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    auto* doneBtn = new QPushButton("Done", &dlg);
    doneBtn->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; "
        "border-radius:4px; padding:8px 24px; font-size:13px; font-weight:bold; }"
        "QPushButton:hover { background:#1a8de4; }");
    connect(doneBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(doneBtn);
    vlay->addLayout(btnRow);

    if (dlg.exec() == QDialog::Accepted) {
        saveTabSlotAssignment();
    } else {
        m_tabSlots = backup;
    }
    refreshTabBar();
    // v3.2.3 fix #2 — stay on the same row regardless of where refreshTabBar's
    // own logic decided to jump to.
    m_currentTabGroup = groupBeforeDialog;
    applyCurrentTabGroup();
}

void MainWindow::repositionPicker()
{
    if (!m_pickerFrame || !m_bottomBar) return;
    static constexpr int VISIBLE = 6, DISMISS_H = 28;
    int itemH  = m_pickerList->count() > 0 ? m_pickerList->sizeHintForRow(0) : 44;
    int rows   = qMin(m_pickerList->count(), VISIBLE);
    if (rows < 1) rows = 1;
    int listH  = itemH * rows;
    int h      = listH + DISMISS_H + 2;
    int margin = 6;
    int w      = centralWidget()->width() - margin * 2;
    int y      = m_bottomBar->geometry().top() - h - 4;
    m_pickerFrame->setGeometry(margin, y, w, h);
    m_pickerList->setFixedHeight(listH);
}

void MainWindow::loadTiles()
{
    m_tiles = JsonManager::instance().loadTiles();
    APPLOG(QString("loadTiles: loaded %1 tile(s) from JSON").arg(m_tiles.size()));
    // V5.4.26 — before anything else touches the files: undo any image sharing
    // left behind by a tile duplicated before V5.4.23. Ahead of the cleanup
    // below because it adds files, and every one it adds is referenced.
    dedupeSharedImages();
    cleanupOrphanedFiles();

    // v3.3.16 fix #2 — startup catch-up: if the app was closed exactly
    // when a tile's episode expired, onGlobalTick() never got the chance
    // to run its own check for this (see tryLocalEpisodeAdvance above).
    // Run the same check here for anything already sitting expired, so a
    // tile doesn't have to wait for a live tick or a refresh to catch up
    // — this needs no new data, just what's already stored on the tile.
    int advancedCount = 0;
    for (TileData& td : m_tiles) {
        if (td.hasDate() && td.isExpired() && tryLocalEpisodeAdvance(td))
            ++advancedCount;
    }
    if (advancedCount > 0)
        APPLOG(QString("loadTiles: locally advanced %1 tile(s) on startup").arg(advancedCount));

    // v3.3.19 — read-only diagnostic only, no data is touched here. A
    // TV tile with a real tmdbId but seasonEpisodeCount still at 0 (or a
    // date sitting in the past despite noDateOverride being set, which
    // would make hasDate() report false and skip it above entirely) is
    // exactly the kind of state a tile added in an older version — before
    // some of these fields existed — could get stuck in. This just logs
    // it, so if it happens again there's something concrete in the debug
    // log to go on instead of guessing blind at what field is missing.
    for (const TileData& td : std::as_const(m_tiles)) {
        if (td.mediaType != "tv" || td.tmdbId <= 0) continue;
        // V5.4.12 — a window tile has no episode count to be missing. Its
        // season hasn't been scheduled, which is the whole reason it is a
        // window, so a count of 0 is the correct answer rather than the stuck
        // state this note is looking for. All five that were reporting here
        // were windows (Dexter, Daredevil, Invincible, X-Men '97, Spider-Man),
        // and a diagnostic that cries wolf on normal data is worse than none.
        if (td.seasonEpisodeCount <= 0 && !td.isWindowDate()) {
            APPLOG(QString("loadTiles: NOTE — '%1' has tmdbId but seasonEpisodeCount is 0; "
                            "a refresh should populate it, but if this tile's estimate still "
                            "looks stuck after that, this is worth reporting").arg(td.displayTitle()));
        }
        if (td.noDateOverride && td.targetDate.isValid() && td.targetDate < QDate::currentDate()) {
            APPLOG(QString("loadTiles: NOTE — '%1' has noDateOverride set with a past targetDate; "
                            "this tile won't be checked for expiry/estimates at all while that's set")
                            .arg(td.displayTitle()));
        }
    }

    std::sort(m_tiles.begin(), m_tiles.end(), tileOrder);
    for (const TileData& td : std::as_const(m_tiles))
        createTileWidgetNoRebuild(td);
    sortAndRebuildAllTabs();   // v3.3.36 — once, after all widgets exist, not once per tile
    // No ellipsis — updateRefreshStatus() animates the dots from here the
    // moment the refresh actually starts, and a "…" already sitting there
    // would read as a fourth, permanent dot.
    m_statusLbl->setStyleSheet(kStatusStyleNeutral);
    m_statusLbl->setText(refreshingStatusText());

    // V5 — snapshot BEFORE anything can advance a tile. A show is a single
    // tile across its whole run, so once a refresh moves one from the
    // episode that aired while the app was closed to the next upcoming
    // episode, the missed release is gone from the tile entirely and can
    // never be reported.
    //
    // The refresh itself is no longer started here: the constructor runs
    // the recap, then the update check, then the refresh, in that order.
    captureMissedReleases();
}

void MainWindow::saveTiles()
{
    // V5.4.22 — record anything that has released. Here because saving is what
    // every path that changes a tile ends with, so nothing has to remember to
    // call it; see releasehistory.h for why it is a sweep and not an event.
    ReleaseHistory::instance().sweep(m_tiles);

    JsonManager::instance().saveTiles(m_tiles);
    notifyTrayApp();
}

void MainWindow::refreshAllTiles()
{
    // Anything already showing a past date is recorded before the scrapers get
    // a chance to move it on. Cheap and idempotent; see releasehistory.h.
    ReleaseHistory::instance().sweep(m_tiles);

    m_refreshPending = 0;
    for (const TileData& td : std::as_const(m_tiles)) {
        // V5 — a TVmaze-sourced tile has no tmdbId, so the old guard would
        // have skipped it entirely.
        bool tvViaTvmaze = (td.mediaType == "tv" && useTvmazeForTv());
        if (td.tmdbId <= 0 && !(tvViaTvmaze && (td.tvmazeId > 0 || !td.title.isEmpty())))
            continue;
        ++m_refreshPending;
        // v3.3.0 — game tiles refresh via IGDB, everything else via TMDB.
        // V5 — TV goes to TVmaze unless the user opted back to TMDB.
        if (td.mediaType == "game")   m_igdbScraper->refreshTile(td);
        else if (tvViaTvmaze)         m_tvmaze->refreshTile(td);
        else                          m_scraper->refreshTile(td);
    }
    updateRefreshStatus();
    if (m_refreshPending == 0) {
        m_statusLbl->setStyleSheet(kStatusStyleNeutral);
        m_statusLbl->setText(
            QString("%1 tile%2 ready").arg(m_tiles.size()).arg(m_tiles.size()==1?"":"s"));
    } else {
        // v3.3.37 — safety net for the deferred rebuild: if some refresh never
        // calls back, m_refreshPending stays above zero forever and the rebuild
        // deferred until "the last one finishes" never runs.
        //
        // V5.4.12 — this was a flat 20 seconds from the START of the batch,
        // which is a deadline the app can outgrow. TVmaze is deliberately a
        // serial queue at 550ms with 2 attempts, so a TV-heavy library spends
        // longer than that simply doing the work correctly: ~9 TV tiles fit,
        // ~30 would not, and the net would fire mid-batch and declare a
        // half-finished refresh complete. It also fires on every ordinary
        // refresh where a tile FAILS, because a failed refresh never reports
        // back at all — onScraperError doesn't touch this counter, so those
        // batches only ever end here.
        //
        // It is now a watchdog on PROGRESS rather than a deadline on the
        // batch: it only gives up when nothing has arrived for 30 seconds. A
        // slow library keeps resetting the clock and is left alone; a batch
        // that has genuinely stalled is still caught, and caught no later than
        // it used to be.
        m_lastRefreshProgress.start();
        if (!m_refreshWatchdog) {
            m_refreshWatchdog = new QTimer(this);
            m_refreshWatchdog->setInterval(5000);
            connect(m_refreshWatchdog, &QTimer::timeout, this, [this]() {
                if (m_refreshPending <= 0) { m_refreshWatchdog->stop(); return; }
                if (m_lastRefreshProgress.elapsed() < kRefreshStallMs) return;
                APPLOG(QString("refreshAllTiles: no reply for %1s with %2 still outstanding "
                               "— forcing the deferred rebuild")
                           .arg(kRefreshStallMs / 1000).arg(m_refreshPending));
                m_refreshWatchdog->stop();
                m_refreshPending = 0;
                updateRefreshStatus();
                sortAndRebuildAllTabs();
                m_statusLbl->setStyleSheet(kStatusStyleNeutral);
                m_statusLbl->setText(
                    QString("%1 tile%2 ready").arg(m_tiles.size()).arg(m_tiles.size()==1?"":"s"));
                saveTiles();
            });
        }
        m_refreshWatchdog->start();
    }
}

// =============================================================================
//  The refreshing indicator — V5.4.11.
//
//  V5.4.10 put this in the middle of the grid as large text. It was tried and
//  rejected: the bottom-left status line already says the same thing, and a
//  second copy over the tiles is just something in the way. **Do not put it
//  back.** What survived is the part that was actually wanted — the trailing
//  dots cycling once a second (none → one → two → three → none), now animating
//  the status line itself so it reads as work in progress rather than a label
//  that might have stalled.
//
//  Driven purely by m_refreshPending, the counter that already exists to know
//  when a batch has finished, so there is no second notion of "busy" that could
//  disagree with the first and leave the dots ticking forever. Every site that
//  moves that counter calls this straight after — including the 20-second
//  safety net, which is the one path that resets it without a reply arriving.
// =============================================================================
QString MainWindow::refreshingStatusText() const
{
    return QString("%1 tile%2 — refreshing")
               .arg(m_tiles.size()).arg(m_tiles.size() == 1 ? "" : "s");
}

void MainWindow::updateRefreshStatus()
{
    if (m_refreshPending <= 0) {
        if (m_refreshDotsTimer) m_refreshDotsTimer->stop();
        // V5.4.13 — the tiles are loaded. That is one of the two things the
        // message of the day waits for; maybeCheckMotd() decides whether the
        // other one (the update prompt) has finished too.
        m_tilesLoaded = true;
        maybeCheckMotd();
        return;
    }

    if (!m_refreshDotsTimer) {
        m_refreshDotsTimer = new QTimer(this);
        m_refreshDotsTimer->setInterval(1000);
        connect(m_refreshDotsTimer, &QTimer::timeout, this, [this]() {
            m_refreshDots = (m_refreshDots + 1) % 4;
            m_statusLbl->setText(refreshingStatusText() + QString(m_refreshDots, '.'));
        });
    }

    if (m_refreshDotsTimer->isActive()) return;   // already counting — leave the cycle alone

    m_refreshDots = 0;
    m_statusLbl->setStyleSheet(kStatusStyleNeutral);
    m_statusLbl->setText(refreshingStatusText());
    m_refreshDotsTimer->start();
}

void MainWindow::appendTileWidget(const TileData& data)
{
    createTileWidgetNoRebuild(data);
    sortAndRebuildAllTabs();
}

// v3.3.36 — factored out of appendTileWidget so bulk-loading loops
// (startup, import) can create every widget first and rebuild the grid
// exactly once at the end, instead of once per tile. Previously, adding
// N tiles this way rebuilt the grid N times — each rebuild re-scanning
// however many tiles had been added so far — an O(N²) cost where O(N)
// is all that's needed. Single-tile call sites still go through
// appendTileWidget above, which is unchanged and still rebuilds
// immediately, exactly as before.
void MainWindow::createTileWidgetNoRebuild(const TileData& data)
{
    APPLOG(QString("appendTileWidget: '%1' | mediaType=%2 | date=%3 | notif=%4")
           .arg(data.displayTitle(), data.mediaType.isEmpty() ? "custom" : data.mediaType)
           .arg(data.effectiveDate().toString("yyyy-MM-dd"))
           .arg(data.notifStatus == NotifStatus::Active ? "Active"
              : data.notifStatus == NotifStatus::Ready  ? "Ready" : "Inactive"));
    auto* tw = new TileWidget(data, nullptr);

    // V4.12 fix — if the backdrop image finished downloading before this
    // tile even existed (the TV-show season-scan race described in
    // mainwindow.h), apply it now instead of it having been silently lost.
    auto pendingIt = m_pendingPosterUpdates.find(data.id);
    if (pendingIt != m_pendingPosterUpdates.end()) {
        const QString localPath = pendingIt->first;
        const bool makeActive   = pendingIt->second;
        m_pendingPosterUpdates.erase(pendingIt);

        TileData upd = tw->tileData();
        upd.fetchedImagePath = localPath;
        if (makeActive || upd.imagePath.isEmpty()) upd.imagePath = localPath;
        tw->updateData(upd);

        for (TileData& td : m_tiles) {
            if (td.id != data.id) continue;
            td.fetchedImagePath = localPath;
            if (makeActive || td.imagePath.isEmpty()) td.imagePath = localPath;
            break;
        }
        APPLOG(QString("createTileWidgetNoRebuild: applied buffered poster update for '%1'").arg(data.id));
    }

    connect(tw, &TileWidget::imageChanged,     this, &MainWindow::onImageChanged);
    connect(tw, &TileWidget::tileDataChanged,  this, &MainWindow::onTileDataChanged);
    connect(tw, &TileWidget::removeTile,       this, &MainWindow::onRemoveTile);
    connect(tw, &TileWidget::duplicateTile,    this, &MainWindow::onDuplicateTile);
    connect(tw, &TileWidget::refetchRequested, this, &MainWindow::onRefetchRequested);
    connect(tw, &TileWidget::forceImageRefetchRequested, this, &MainWindow::onForceImageRefetchRequested);
    connect(tw, &TileWidget::testNotification, this, &MainWindow::onTestNotification);
    connect(tw, &TileWidget::selectionChanged, this, &MainWindow::onTileSelectionChanged);
    connect(tw, &TileWidget::deleteAllSelectedRequested, this, &MainWindow::onDeleteAllSelectedRequested);
    connect(tw, &TileWidget::colorPickerRequested, this, &MainWindow::showTileColorPickerDialog);
    tw->setSelectMode(m_selectMode);   // V4.6 — respect select mode if it's already active
    tw->setAnySelected(m_selectedTileCount > 0);   // V4.12
    m_tileWidgets.append(tw);
}

// =============================================================================
//  sortAndRebuildAllTabs — refreshes which tabs are visible/ordered, then
//  (re)populates whichever tab is currently active. v3.0.1: tabs are
//  independent filters now (a tile can match more than one), so we don't
//  pre-sort every tile into one exclusive grid anymore — only the active
//  tab's grid ever holds widgets; switching tabs re-filters on demand.
// =============================================================================
void MainWindow::sortAndRebuildAllTabs()
{
    refreshTabBar();
    populateActiveKindGrid();
}

// =============================================================================
//  populateActiveKindGrid — (re)fills the currently-selected tab's grid with
//  every tile matching that kind's filter, sorted appropriately. All other
//  grids are left empty — a tile widget only ever has one parent at a time,
//  so it "belongs" to whichever tab you're currently looking at; switching
//  tabs simply re-filters the same underlying tile widgets into the newly
//  active grid. This is what lets a Show tile appear in both "All
//  Countdowns" and "Show Countdowns" without needing two copies of it.
//
//  KEY: clears ALL row stretch factors before setting the new bottom one.
//  This is what prevents ghost empty rows when tiles are deleted.
// =============================================================================
void MainWindow::populateActiveKindGrid()
{
    int idx = m_tabs->currentIndex();

    // Remember the previous kind's scroll position before we tear it down
    if (m_lastPopulatedKind >= 0 && m_lastPopulatedKind < NUM_KINDS)
        m_scrollTarget[m_lastPopulatedKind] = m_scrollAreas[m_lastPopulatedKind]->verticalScrollBar()->value();

    // v3.2.3 — only the PREVIOUSLY active kind's grid can possibly have any
    // content: this function is the only place that ever adds widgets to a
    // grid, and it always clears the old one first, every time. Looping
    // over all NUM_KINDS grids to detach widgets was therefore doing 12
    // guaranteed-empty passes for every real one — this clears just the
    // one grid that could actually have something in it.
    // v3.2.2 fix — hide() BEFORE detaching. A widget that loses its parent
    // while still marked visible can render as an orphaned top-level
    // window (its own OS-level window, complete with title bar) instead of
    // just quietly disappearing — this is exactly what was showing up as
    // separate "MediaCountdowns" windows on screen. hide() first ensures a
    // detached tile is never in a visible state to begin with.
    if (m_lastPopulatedKind >= 0 && m_lastPopulatedKind < NUM_KINDS) {
        QLayoutItem* item;
        while ((item = m_grids[m_lastPopulatedKind]->takeAt(0)) != nullptr) {
            if (item->widget()) {
                item->widget()->hide();
                item->widget()->setParent(nullptr);
            }
            delete item;
        }
    }

    // V4.6 — every tab can now be hidden (see showTabContextMenu's Hide
    // Tab), so idx == -1 is a real, allowed state, not just a transient
    // one. The detach above already ran unconditionally, so this really is
    // now a genuinely empty background rather than stale tiles left
    // attached to a container nothing points at anymore.
    if (idx < 0 || idx >= m_visibleKindOrder.size()) {
        m_lastPopulatedKind = -1;
        return;
    }
    int kind = m_visibleKindOrder[idx];

    QList<TileWidget*> matching;
    for (TileWidget* tw : std::as_const(m_tileWidgets))
        if (tileMatchesKind(tw->tileData(), kind)) matching.append(tw);

    std::sort(matching.begin(), matching.end(), [this, kind](TileWidget* a, TileWidget* b) {
        if (kind == K_OTHER) {
            // Preserve insertion order: tile added first appears first
            return m_tileWidgets.indexOf(a) < m_tileWidgets.indexOf(b);
        }
        bool released = (kind == K_RELEASED || kind == K_RELEASED_MOVIE ||
                          kind == K_RELEASED_SHOW || kind == K_RELEASED_GAME);
        if (released) return tileOrder(b->tileData(), a->tileData()); // newest first
        return tileOrder(a->tileData(), b->tileData());               // soonest first
    });

    for (int i = 0; i < matching.size(); ++i) {
        matching[i]->setParent(m_tabContainers[kind]);
        m_grids[kind]->addWidget(matching[i], i / m_tilesPerRow, i % m_tilesPerRow);
        matching[i]->show();
    }

    // Clear ALL previous row stretch factors, then set one below the last
    // row. Without this, Qt keeps allocating space for rows that no longer
    // have widgets — creating a ghost empty row after deletion.
    int totalRows = m_grids[kind]->rowCount();
    for (int r = 0; r < totalRows; ++r)
        m_grids[kind]->setRowStretch(r, 0);
    int lastRow = matching.isEmpty() ? 0 : (matching.size() - 1) / m_tilesPerRow;
    m_grids[kind]->setRowStretch(lastRow + 1, 1);

    m_lastPopulatedKind = kind;

    // Restore this kind's remembered scroll position (0 the first time)
    QScrollBar* sb = m_scrollAreas[kind]->verticalScrollBar();
    sb->setValue(qBound(sb->minimum(), m_scrollTarget[kind], sb->maximum()));
}

// =============================================================================
//  loadTabLayoutSettings — reads which of the 9 kinds are checked on in
//  Settings → Tab Layout into m_enabledKinds. Defaults to the "Default"
//  preset (Countdowns/Released/Other) so a fresh install — or an upgrade
//  from a pre-3.0.0 version — behaves exactly like the old fixed 3 tabs.
// =============================================================================
void MainWindow::loadTabLayoutSettings()
{
    QSettings s("HijackAssassin", "MediaCountdowns");
    if (!s.contains("tabEnabledKinds")) {
        m_enabledKinds = defaultPresetKinds();
    } else {
        QStringList saved = s.value("tabEnabledKinds").toStringList();
        m_enabledKinds.clear();
        for (int k = 0; k < NUM_KINDS; ++k)
            if (saved.contains(kKindKeys[k])) m_enabledKinds.insert(k);
        // V4.6 — no more "if empty, force defaults back" safety net here.
        // The !s.contains() branch above already correctly covers the
        // genuine first-run/pre-3.0.0-upgrade case; this one was instead
        // silently undoing a deliberate "Hide Tab" all the way down to
        // zero, every single time settings reloaded — including on the
        // very next app launch, since this runs at startup too.
    }
    loadTabSlotAssignment();   // v3.2.0 — load the persistent 6x8 group/slot grid
}

// =============================================================================
//  loadTabOrder — the user's preferred tab order (draggable), covering all 9
//  kinds. Any kind missing from the saved list is appended — "Other"
//  specifically goes last by default (its natural first-appearance spot);
//  everything else appends in canonical order. This is only a DEFAULT: once
//  the user actually drags a tab (including "Other"), onTabMoved persists
//  its exact position and it's found here from then on, skipping this
//  fallback entirely — it is not locked in place.
// =============================================================================
QList<int> MainWindow::loadTabOrder() const
{
    QSettings s("HijackAssassin", "MediaCountdowns");
    QStringList saved = s.value("tabOrder").toStringList();
    QList<int> order;
    for (const QString& key : saved) {
        for (int k = 0; k < NUM_KINDS; ++k)
            if (key == kKindKeys[k] && !order.contains(k)) { order.append(k); break; }
    }
    for (int k = 0; k < NUM_KINDS; ++k)
        if (k != K_OTHER && !order.contains(k)) order.append(k);
    if (!order.contains(K_OTHER)) order.append(K_OTHER);
    return order;
}

// =============================================================================
//  refreshTabBar — v3.2.0. Syncs the persistent slot grid with the current
//  enabled/disabled kind set (reconcileTabSlots), re-attaches every
//  assigned kind to m_tabs in group-major/slot order (so tab indices stay
//  stable across group switches — see applyCurrentTabGroup), then jumps to
//  whichever group contains the previously-selected kind and shows it.
// =============================================================================

// v3.3.39 fix (critical) — gives stretch ONLY to the columns actually in
// use (0 through m_tilesPerRow-1), explicitly zeroing stretch for every
// column beyond that. This is the actual fix for tiles being squeezed
// into a fraction of the window: giving all 20 possible columns equal
// stretch upfront meant unused "phantom" columns still claimed an equal
// share of the available width. Called once at startup (after all 9
// grids exist) and again whenever Settings is saved with a
// possibly-different Tile Size value.
void MainWindow::applyTilesPerRowStretch()
{
    for (int k = 0; k < NUM_KINDS; ++k) {
        if (!m_grids[k]) continue;
        for (int c = 0; c < MAX_COLS; ++c)
            m_grids[k]->setColumnStretch(c, c < m_tilesPerRow ? 1 : 0);
    }
}

// Forward declaration — actual definition (with colorwheel.h) is further
// down, alongside the rest of the tab-color feature; needed here since
// this file-scope helper is used before that point.
static QIcon coloredDotIcon(const QColor& color);
static QIcon blankDotIcon();   // V4.11

// v3.3.41 — rebuilds the left-corner "jump to any tab" menu to list every
// currently-enabled tab in the same order the tab bar itself uses.
// Picking one finds whichever dynamic page currently contains it, jumps
// there, then selects that specific tab — so it works regardless of
// which page happens to be showing when you open the menu.
void MainWindow::refreshJumpToTabMenu()
{
    if (!m_jumpToTabBtn || !m_jumpToTabList) return;
    // V4.14 — block signals while repopulating: clear()/addItem() below
    // would otherwise fire rowsMoved-adjacent model signals that
    // onJumpToTabReordered() would misinterpret as a user drag.
    m_jumpToTabList->blockSignals(true);
    m_jumpToTabList->clear();
    for (int k : m_visibleKindOrder) {
        auto* item = m_tabColors[k].isValid()
            ? new QListWidgetItem(coloredDotIcon(m_tabColors[k]), kKindLabels[k])
            : new QListWidgetItem(kKindLabels[k]);   // V4.15 fix — no icon at all when uncolored, rather
                                                       // than a blank one that just eats space invisibly
        item->setData(Qt::UserRole, k);
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);   // drop BETWEEN items, never ONTO one
        m_jumpToTabList->addItem(item);
    }
    m_jumpToTabList->blockSignals(false);
}

// =============================================================================
//  onJumpToTabItemClicked — V4.14. Jump to whichever tab was clicked (not
//  dragged), then close the dropdown — same behavior the old plain-QAction
//  menu had, now reimplemented against the QListWidget directly since a
//  QWidgetAction's embedded widget handles its own mouse events rather
//  than auto-closing the menu the way a normal QAction trigger would.
// =============================================================================
void MainWindow::onJumpToTabItemClicked(QListWidgetItem* item)
{
    if (!item) return;
    int k = item->data(Qt::UserRole).toInt();
    for (int g = 0; g < m_tabSlots.size(); ++g) {
        bool found = false;
        for (int s = 0; s < m_tabsPerGroup; ++s)
            if (m_tabSlots[g][s] == k) { found = true; break; }
        if (found) {
            m_currentTabGroup = g;
            applyCurrentTabGroup();
            break;
        }
    }
    int idx = m_visibleKindOrder.indexOf(k);
    if (idx >= 0) m_tabs->setCurrentIndex(idx);
    if (m_jumpToTabBtn->menu()) m_jumpToTabBtn->menu()->close();
}

// =============================================================================
//  onJumpToTabReordered — V4.14. Fires once a drag reorder within the
//  dropdown actually completes. Reads the list's new item order back out
//  as the new global kind sequence, then re-chunks it into m_tabSlots
//  group by group (m_tabsPerGroup kinds per group) — the same "groups are
//  just consecutive pages of one overall sequence" model
//  loadTabSlotAssignment() already uses to derive a first-run default, so
//  dragging a tab across what used to be a group boundary naturally moves
//  it into that neighboring group rather than being restricted to
//  reordering only within its own group.
// =============================================================================
void MainWindow::onJumpToTabReordered()
{
    QList<int> newOrder;
    for (int i = 0; i < m_jumpToTabList->count(); ++i)
        newOrder << m_jumpToTabList->item(i)->data(Qt::UserRole).toInt();
    if (newOrder.isEmpty() || newOrder == m_visibleKindOrder) return;

    // Deliberately NOT setting m_visibleKindOrder here — refreshTabBar()
    // below compares its own freshly-flattened order against the CURRENT
    // m_visibleKindOrder to decide whether the tab bar itself actually
    // needs rebuilding. Setting it here first would make that comparison
    // see no difference and skip the rebuild, leaving the visible tabs
    // showing the old order despite m_tabSlots already reflecting the new
    // one. Only m_tabSlots is updated here; refreshTabBar() derives
    // m_visibleKindOrder from it and rebuilds correctly.
    for (QVector<int>& group : m_tabSlots) group.fill(-1);
    int g = 0, s = 0;
    for (int k : std::as_const(newOrder)) {
        if (g >= m_tabSlots.size()) break;
        m_tabSlots[g][s] = k;
        if (++s >= m_tabsPerGroup) { s = 0; ++g; }
    }
    saveTabSlotAssignment();
    refreshTabBar();
}

void MainWindow::refreshTabBar()
{
    int currentKind = (m_tabs->currentIndex() >= 0 && m_tabs->currentIndex() < m_visibleKindOrder.size())
                       ? m_visibleKindOrder[m_tabs->currentIndex()] : K_COUNTDOWNS;

    reconcileTabSlots();
    saveTabSlotAssignment();

    QVector<int> flat = flattenTabSlots();
    // V4.6 — no more "if empty, force K_COUNTDOWNS back in" fallback here.
    // loadTabSlotAssignment() already seeds a sensible non-empty default
    // for the genuine first-run case; this one instead made it impossible
    // to ever actually reach zero tabs via Hide Tab, since it would
    // immediately re-add one right back within the very same call.
    // v3.2.1 — flattenTabSlots() is deduped and bounds-checked, so this can
    // never legitimately exceed NUM_KINDS; if it somehow does, that's a
    // sign of real data corruption upstream — log it loudly and truncate
    // rather than building however many tabs the corrupted list implies.
    if (flat.size() > NUM_KINDS) {
        APPLOG(QString("refreshTabBar: flattened slot list has %1 entries, more than NUM_KINDS (%2) — truncating")
               .arg(flat.size()).arg(NUM_KINDS));
        flat.resize(NUM_KINDS);
    }
    QList<int> newVisible(flat.begin(), flat.end());

    if (newVisible != m_visibleKindOrder) {
        m_tabs->blockSignals(true);
        m_tabs->clear();
        for (int k : newVisible)
            m_tabs->addTab(m_scrollAreas[k], kKindLabels[k]);
        m_visibleKindOrder = newVisible;
        m_tabs->blockSignals(false);

        int restoreIdx = m_visibleKindOrder.indexOf(currentKind);
        m_tabs->setCurrentIndex(restoreIdx >= 0 ? restoreIdx : 0);
        refreshJumpToTabMenu();   // v3.3.41
        refreshTabColorIcons();  // V4.4 — addTab() above just wiped any icons; re-apply them
    }

    // Jump to whichever group contains the kind we were just looking at,
    // so changing the enabled set doesn't leave the view somewhere odd.
    int targetKind = m_visibleKindOrder.contains(currentKind)
                      ? currentKind : (m_visibleKindOrder.isEmpty() ? -1 : m_visibleKindOrder[0]);
    for (int g = 0; g < m_tabSlots.size(); ++g) {
        bool found = false;
        for (int s = 0; s < m_tabsPerGroup; ++s)
            if (m_tabSlots[g][s] == targetKind) { found = true; break; }
        if (found) { m_currentTabGroup = g; break; }
    }

    applyCurrentTabGroup();
}

// =============================================================================
//  V4.4 — per-tab custom colors. Colors are keyed by TabKind's own stable
//  string key (kKindKeys[]), not by grid slot, so a color stays attached to
//  "the Movie tab" even if it gets dragged to a different slot/group later.
// =============================================================================
static QIcon coloredDotIcon(const QColor& color)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 0, 0, 140), 1));
    p.setBrush(color);
    p.drawEllipse(QRectF(1, 1, 12, 12));
    return QIcon(pixmap);
}

// =============================================================================
//  blankDotIcon — V4.11 fix. Same 14x14 dimensions as coloredDotIcon, just
//  fully transparent. Used for every tab that has NO color, instead of a
//  null QIcon(), so every tab always has an icon of identical size present
//  or absent uniformly. Some platform styles (notably Windows' native tab
//  style) reserve slightly different vertical space for a tab depending on
//  whether it has an icon at all — mixing icon/no-icon tabs in the same
//  bar could shift the bar's overall height depending on which tab has a
//  color, which in turn shifts the entire tile grid below it. Giving every
//  tab an icon of the same size, always, removes that inconsistency
//  regardless of the exact underlying platform-specific mechanism.
// =============================================================================
static QIcon blankDotIcon()
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    return QIcon(pixmap);
}

void MainWindow::loadTabColors()
{
    QSettings s("HijackAssassin", "MediaCountdowns");
    for (int k = 0; k < NUM_KINDS; ++k) {
        QString hex = s.value(QString("tabColor_%1").arg(kKindKeys[k])).toString();
        m_tabColors[k] = hex.isEmpty() ? QColor() : QColor(hex);
    }
}

void MainWindow::refreshTabColorIcons()
{
    QTabBar* bar = m_tabs->tabBar();
    QHash<QString, QColor> colorsByLabel;
    for (int i = 0; i < m_visibleKindOrder.size(); ++i) {
        int kind = m_visibleKindOrder[i];
        const QColor& c = m_tabColors[kind];
        bar->setTabIcon(i, c.isValid() ? coloredDotIcon(c) : QIcon());
        if (c.isValid()) colorsByLabel[kKindLabels[kind]] = c;
    }
    if (m_tabColorOverlay) {
        m_tabColorOverlay->setColors(colorsByLabel);
        m_tabColorOverlay->syncGeometry();   // tab count/widths may have just changed
    }
    // Consolidated here (rather than left to each individual call site to
    // remember) after Save's own handler turned out to be missing it —
    // this is the one place a color actually changing should always,
    // unconditionally, also refresh the jump-to-tab menu's icons.
    refreshJumpToTabMenu();
}

// =============================================================================
//  showTabContextMenu — V4.5. The shared "Color Picker / Clear Color /
//  Hide Tab" menu used by both right-click entry points (top bar and the
//  Manage Tabs grid). "Clear Color" only appears when this kind actually
//  has a custom color set; "Hide Tab" is omitted entirely if this is the
//  last remaining enabled kind, rather than letting someone hide every
//  tab and leave the app showing nothing at all.
// =============================================================================
bool MainWindow::showTabContextMenu(int kind, const QPoint& globalPos)
{
    if (kind < 0 || kind >= NUM_KINDS) return false;

    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background:#242424; color:#eee; border:1px solid #444; }"
        "QMenu::item:selected { background:#0078d4; }");

    QAction* colorAction = menu.addAction("Color Picker");
    QAction* clearAction = m_tabColors[kind].isValid() ? menu.addAction("Clear Color") : nullptr;
    QAction* hideAction   = menu.addAction("Hide Tab");   // V4.6 — hiding the last one is now allowed, leaving an empty background

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return false;

    if (chosen == colorAction) {
        showTabColorPickerDialog(kind);   // handles its own persistence + icon/menu refresh
        return false;
    }
    if (chosen == clearAction) {
        QSettings s("HijackAssassin", "MediaCountdowns");
        s.remove(QString("tabColor_%1").arg(kKindKeys[kind]));
        m_tabColors[kind] = QColor();
        refreshTabColorIcons();
        refreshJumpToTabMenu();
        return false;
    }
    if (chosen == hideAction) {
        m_enabledKinds.remove(kind);
        QStringList enabledKeys;
        for (int k = 0; k < NUM_KINDS; ++k)
            if (m_enabledKinds.contains(k)) enabledKeys << kKindKeys[k];
        QSettings s("HijackAssassin", "MediaCountdowns");
        s.setValue("tabEnabledKinds", enabledKeys);
        refreshTabBar();
        return true;   // the tab layout itself changed — Manage Tabs (if open) needs a full grid refresh
    }
    return false;
}


// =============================================================================
//  runColorPickerDialog — V4.7. A circular hue/saturation wheel (see
//  colorwheel.h) plus a brightness slider, 3 editable RGB values, a full
//  hex field that can be typed into or pasted, and a row of preset
//  swatches — all kept in sync with each other through one shared
//  applyColor() function, guarded by an "updating" flag so programmatic
//  updates never re-trigger each other in a loop.
//
//  Extracted from what was originally showTabColorPickerDialog()'s own
//  inline UI code, so this exact dialog can be reused for per-tile color
//  tags too, without duplicating the wheel/swatches/RGB/hex sync logic.
//  Knows nothing about tabs, tiles, kinds, or QSettings — the caller
//  handles persistence based on the returned outcome.
// =============================================================================
// =============================================================================
//  buildColorPane — V5.4.15. Everything the colour picker is, minus the window
//  around it: wheel, brightness, presets, RGB, hex, all kept in sync.
//
//  It exists because there are now two pickers on one dialog (Outline and
//  Text), and because they have to report every change as it happens so the
//  real tile can preview it. The single-colour dialog below is built from this
//  same pane, so the two can never drift into behaving differently.
//
//  onChanged fires for every adjustment, including the initial one.
// =============================================================================
MainWindow::ColorPane MainWindow::buildColorPane(QWidget* parent, const QColor& initialColor,
                                                 std::function<void(const QColor&)> onChanged)
{
    auto* pane = new QWidget(parent);
    QWidget& dlg = *pane;   // the original code addressed the dialog; same widget now

    auto* vlay = new QVBoxLayout(pane);
    vlay->setContentsMargins(16, 16, 16, 8);
    vlay->setSpacing(12);

    auto* wheelRow = new QHBoxLayout();
    wheelRow->setSpacing(16);
    auto* wheel = new ColorWheel(&dlg);
    wheelRow->addWidget(wheel);

    auto* valueSlider = new QSlider(Qt::Vertical, &dlg);
    valueSlider->setRange(0, 255);
    valueSlider->setFixedHeight(200);
    valueSlider->setStyleSheet(
        "QSlider::groove:vertical { width:8px; background:#333; border-radius:4px; }"
        "QSlider::handle:vertical { background:#0078d4; height:16px; margin:0 -6px; border-radius:8px; }"
        "QSlider::sub-page:vertical { background:#0078d4; border-radius:4px; }");
    wheelRow->addWidget(valueSlider);
    vlay->addLayout(wheelRow);

    // V4.5 — a row of preset swatches for one-click common colors, since
    // the wheel is precise but slower when you just want "make this blue".
    static const QList<QColor> kPresetColors = {
        QColor("#E53935"), QColor("#FB8C00"), QColor("#FDD835"), QColor("#43A047"),
        QColor("#00ACC1"), QColor("#1E88E5"), QColor("#8E24AA"), QColor("#D81B60"),
    };
    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(6);
    QList<QPushButton*> presetBtns;
    for (const QColor& preset : kPresetColors) {
        auto* btn = new QPushButton(&dlg);
        btn->setFixedSize(24, 24);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QString(
            "QPushButton { background:%1; border:1px solid #444; border-radius:4px; }"
            "QPushButton:hover { border:1px solid #fff; }").arg(preset.name()));
        presetRow->addWidget(btn);
        presetBtns.append(btn);
    }
    presetRow->addStretch();
    vlay->addLayout(presetRow);

    const char* kSpinStyle =
        "QSpinBox { background:#1e1e1e; color:#fff; border:1px solid #444; "
        "border-radius:4px; padding:5px; font-size:13px; }";

    auto* rgbRow = new QHBoxLayout();
    rgbRow->setSpacing(10);
    auto makeLabeledSpin = [&](const QString& label) -> QSpinBox* {
        auto* col = new QVBoxLayout();
        auto* lbl = new QLabel(label, &dlg);
        lbl->setStyleSheet("color:#888; font-size:11px; background:transparent;");
        lbl->setAlignment(Qt::AlignCenter);
        auto* spin = new QSpinBox(&dlg);
        spin->setRange(0, 255);
        spin->setAlignment(Qt::AlignCenter);
        spin->setStyleSheet(kSpinStyle);
        col->addWidget(lbl);
        col->addWidget(spin);
        rgbRow->addLayout(col);
        return spin;
    };
    QSpinBox* rSpin = makeLabeledSpin("R");
    QSpinBox* gSpin = makeLabeledSpin("G");
    QSpinBox* bSpin = makeLabeledSpin("B");
    vlay->addLayout(rgbRow);

    auto* hexRow = new QHBoxLayout();
    hexRow->setSpacing(10);
    auto* hexLbl = new QLabel("Hex:", &dlg);
    hexLbl->setStyleSheet("color:#aaa; font-size:12px; background:transparent;");
    auto* hexEdit = new QLineEdit(&dlg);
    hexEdit->setMaxLength(7);
    hexEdit->setStyleSheet(
        "QLineEdit { background:#1e1e1e; color:#fff; border:1px solid #444; "
        "border-radius:4px; padding:7px; font-size:13px; font-family:monospace; }"
        "QLineEdit:focus { border-color:#0078d4; }");
    auto* previewBox = new QFrame(&dlg);
    previewBox->setFixedSize(34, 34);
    hexRow->addWidget(hexLbl);
    hexRow->addWidget(hexEdit, 1);
    hexRow->addWidget(previewBox);
    vlay->addLayout(hexRow);

    // ── Sync logic: wheel <-> brightness slider <-> RGB spins <-> hex field ──
    auto current  = std::make_shared<QColor>(initialColor);
    auto updating = std::make_shared<bool>(false);
    // The colour a pane opens with is what the tile ALREADY looks like, so
    // reporting it as a change would preview a colour onto a tile that has
    // none the instant the dialog opens — and "Cancel adds nothing" has to be
    // true even if you only opened the dialog to look.
    auto announce = std::make_shared<bool>(false);
    auto applyColor = [=](const QColor& c) {
        *current = c;
        *updating = true;
        wheel->setHsv(c.hue() < 0 ? 0 : c.hue(), c.saturation(), c.value());
        valueSlider->blockSignals(true);  valueSlider->setValue(c.value()); valueSlider->blockSignals(false);
        rSpin->blockSignals(true);        rSpin->setValue(c.red());        rSpin->blockSignals(false);
        gSpin->blockSignals(true);        gSpin->setValue(c.green());      gSpin->blockSignals(false);
        bSpin->blockSignals(true);        bSpin->setValue(c.blue());       bSpin->blockSignals(false);
        hexEdit->blockSignals(true);      hexEdit->setText(c.name().toUpper()); hexEdit->blockSignals(false);
        previewBox->setStyleSheet(QString("background:%1; border:1px solid #444; border-radius:5px;").arg(c.name()));
        *updating = false;
        if (onChanged && *announce) onChanged(c);
    };

    applyColor(initialColor);
    *announce = true;   // from here on, every change is the user's doing

    for (int i = 0; i < presetBtns.size(); ++i) {
        QColor preset = kPresetColors[i];
        connect(presetBtns[i], &QPushButton::clicked, pane, [applyColor, preset]{ applyColor(preset); });
    }

    connect(wheel, &ColorWheel::hueSatChanged, pane, [=](int h, int s) {
        if (*updating) return;
        applyColor(QColor::fromHsv(h, s, valueSlider->value()));
    });
    connect(valueSlider, &QSlider::valueChanged, pane, [=](int v) {
        if (*updating) return;
        applyColor(QColor::fromHsv(wheel->hue(), wheel->sat(), v));
    });
    auto onRgbSpinChanged = [=](int) {
        if (*updating) return;
        applyColor(QColor(rSpin->value(), gSpin->value(), bSpin->value()));
    };
    connect(rSpin, QOverload<int>::of(&QSpinBox::valueChanged), pane, onRgbSpinChanged);
    connect(gSpin, QOverload<int>::of(&QSpinBox::valueChanged), pane, onRgbSpinChanged);
    connect(bSpin, QOverload<int>::of(&QSpinBox::valueChanged), pane, onRgbSpinChanged);
    connect(hexEdit, &QLineEdit::textEdited, pane, [=](const QString& text) {
        if (*updating) return;
        QString hex = text.trimmed();
        if (!hex.startsWith('#')) hex = "#" + hex;
        static const QRegularExpression hexRe("^#[0-9A-Fa-f]{6}$");
        if (hexRe.match(hex).hasMatch()) applyColor(QColor(hex));
    });

    return ColorPane{
        pane,
        [current]{ return *current; },
        applyColor,
        [=]{
            applyColor(QColor(Qt::white));
            hexEdit->blockSignals(true);
            hexEdit->clear();
            hexEdit->blockSignals(false);
        }};
}

// =============================================================================
//  runColorPickerDialog — the original single-colour dialog (tab colours still
//  use it), now just a window around one pane plus Clear/Cancel/Save.
// =============================================================================
MainWindow::ColorPickerOutcome MainWindow::runColorPickerDialog(const QString& title, const QColor& initialColor)
{
    QDialog dlg(this);
    dlg.setWindowTitle("Color Picker");
    dlg.setStyleSheet("QDialog { background:#1e1e1e; }");
    dlg.setModal(true);

    auto* vlay = new QVBoxLayout(&dlg);
    vlay->setContentsMargins(4, 4, 4, 16);
    vlay->setSpacing(4);

    auto* titleLbl = new QLabel(title, &dlg);
    titleLbl->setStyleSheet("color:#fff; font-size:14px; font-weight:bold; "
                            "background:transparent; padding:14px 16px 0 16px;");
    vlay->addWidget(titleLbl);

    ColorPane pane = buildColorPane(&dlg, initialColor, nullptr);
    vlay->addWidget(pane.widget);

    ColorPickerOutcome outcome{ColorPickerResult::Cancelled, QColor()};
    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(16, 0, 16, 0);
    btnRow->setSpacing(8);
    auto* clearBtn = new QPushButton("Clear Color", &dlg);
    clearBtn->setStyleSheet(kPickerGhostBtnStyle);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch();
    auto* cancelBtn = new QPushButton("Cancel", &dlg);
    cancelBtn->setStyleSheet(kPickerGhostBtnStyle);
    auto* saveBtn = new QPushButton("Save", &dlg);
    saveBtn->setStyleSheet(kPickerSaveBtnStyle);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    vlay->addLayout(btnRow);

    connect(clearBtn, &QPushButton::clicked, &dlg, [&]{
        outcome = {ColorPickerResult::Cleared, QColor()};
        dlg.reject();
    });
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, &dlg, [&]{
        outcome = {ColorPickerResult::Saved, pane.currentColor()};
        dlg.accept();
    });

    dlg.exec();
    return outcome;
}

void MainWindow::showTabColorPickerDialog(int kind)
{
    if (kind < 0 || kind >= NUM_KINDS) return;

    // Start from the tab's current custom color, or a neutral default (the
    // app's own accent blue) if none has been set yet.
    QColor initial = m_tabColors[kind].isValid() ? m_tabColors[kind] : QColor("#0078d4");
    ColorPickerOutcome outcome = runColorPickerDialog(QString("Color for \"%1\"").arg(kKindLabels[kind]), initial);

    if (outcome.result == ColorPickerResult::Cancelled) return;

    QSettings s("HijackAssassin", "MediaCountdowns");
    if (outcome.result == ColorPickerResult::Cleared) {
        s.remove(QString("tabColor_%1").arg(kKindKeys[kind]));
        m_tabColors[kind] = QColor();
    } else {
        s.setValue(QString("tabColor_%1").arg(kKindKeys[kind]), outcome.color.name());
        m_tabColors[kind] = outcome.color;
    }
    refreshTabColorIcons();
}

// =============================================================================
//  showTileColorPickerDialog — V5.4.15. "Customize Colors": one dialog, two
//  tabs, live preview on the real tile.
//
//  Outline is the tile's frame (what V4.7 called the colour tag); Text is its
//  countdown and title. Both preview as you move the wheel — on the actual
//  tile in the grid, not on a swatch, because a colour that looks right on a
//  32-pixel square regularly looks wrong over artwork. The dialog is
//  deliberately NOT modal-to-the-pixel about position: it can be dragged aside
//  to see the tile it is changing.
//
//  Cancel restores whatever the tile had when the dialog opened, including
//  "no colour at all" — previewing is done through TileWidget's preview
//  setters, which never touch the tile's data or save anything, so undoing is
//  simply previewing the original values back and closing.
// =============================================================================
void MainWindow::showTileColorPickerDialog(const QString& tileId)
{
    TileWidget* target = nullptr;
    for (TileWidget* tw : std::as_const(m_tileWidgets))
        if (tw->tileData().id == tileId) target = tw;
    if (!target) return;

    // What to put back if this is cancelled.
    const QColor originalOutline = target->tagColor();
    const QColor originalText    = target->textColor();

    QDialog dlg(this);
    dlg.setWindowTitle(QString("Customize \"%1\"").arg(target->tileData().displayTitle()));
    dlg.setStyleSheet("QDialog { background:#1e1e1e; }");
    // Modal, but movable — dragging it aside to watch the tile update is the
    // intended way to use it. (exec() is modal regardless of setModal(); the
    // grid behind still repaints, it just doesn't take clicks.)
    dlg.setModal(true);

    auto* vlay = new QVBoxLayout(&dlg);
    vlay->setContentsMargins(4, 12, 4, 16);
    vlay->setSpacing(8);

    auto* tabs = new QTabWidget(&dlg);
    tabs->setStyleSheet(
        "QTabWidget::pane { border:1px solid #333; border-radius:6px; background:#1e1e1e; }"
        "QTabBar::tab { background:#252525; color:#aaa; padding:8px 22px; font-size:13px;"
        "               border:1px solid #333; border-bottom:none;"
        "               border-top-left-radius:6px; border-top-right-radius:6px; margin-right:2px; }"
        "QTabBar::tab:selected { background:#1e1e1e; color:#fff; font-weight:bold; }"
        "QTabBar::tab:hover:!selected { background:#2e2e2e; color:#ddd; }");

    // Each tab is in one of three states when Save is pressed, and they mean
    // different things:
    //
    //   untouched  → leave that colour exactly as it was. This is the one that
    //                was missing: both panes open holding a default, and Save
    //                wrote BOTH panes' values, so changing only the text also
    //                stamped a white outline onto a tile that never had one.
    //                Opening the dialog must never change a colour you did not
    //                go near.
    //   cleared    → remove that colour.
    //   touched    → use whatever the wheel is showing.
    //
    // "Touched" is exactly "this pane reported a change", and since a pane no
    // longer reports the value it opens with, that is a true record of whether
    // anything was done on that tab. Cleared-ness survives switching tabs, and
    // is undone by simply moving the wheel again — pressing Clear and then
    // picking a colour obviously means you want the colour.
    auto outlineCleared = std::make_shared<bool>(false);
    auto textCleared    = std::make_shared<bool>(false);
    auto outlineTouched = std::make_shared<bool>(false);
    auto textTouched    = std::make_shared<bool>(false);

    // The starting point for a tile with nothing set, and where Clear puts the
    // controls back to. White for both tabs: it is the colour the text already
    // is when untouched, and for the outline it reads as "nothing chosen"
    // rather than a colour someone picked. One default for both means "Clear"
    // does the same visible thing wherever you press it.
    const QColor kDefaultColor = QColor("#ffffff");

    ColorPane outlinePane = buildColorPane(
        tabs, originalOutline.isValid() ? originalOutline : kDefaultColor,
        [target, outlineCleared, outlineTouched](const QColor& c) {
            *outlineCleared = false;   // picking a colour after Clear means you want it
            *outlineTouched = true;
            target->previewTagColor(c);
        });
    ColorPane textPane = buildColorPane(
        tabs, originalText.isValid() ? originalText : kDefaultColor,
        [target, textCleared, textTouched](const QColor& c) {
            *textCleared = false;
            *textTouched = true;
            target->previewTextColor(c);
        });

    tabs->addTab(outlinePane.widget, "Outline");
    tabs->addTab(textPane.widget, "Text");
    vlay->addWidget(tabs);

    // Clear applies to whichever tab is showing, so there is one obvious
    // meaning for it at any moment.
    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(16, 0, 16, 0);
    btnRow->setSpacing(8);
    auto* clearBtn = new QPushButton("Clear", &dlg);
    clearBtn->setStyleSheet(kPickerGhostBtnStyle);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch();
    auto* cancelBtn = new QPushButton("Cancel", &dlg);
    cancelBtn->setStyleSheet(kPickerGhostBtnStyle);
    auto* saveBtn = new QPushButton("Save", &dlg);
    saveBtn->setStyleSheet(kPickerSaveBtnStyle);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    vlay->addLayout(btnRow);

    // Clear applies to whichever tab is showing, so it has one obvious meaning
    // at any moment.
    connect(clearBtn, &QPushButton::clicked, &dlg, [=]() {
        // Order matters. setColor() drives every control back to the default —
        // wheel, brightness slider, RGB boxes and hex all at once — and fires
        // the pane's change callback on the way, which clears the flag. So the
        // flag is set AFTER, and the tile is previewed as having no colour at
        // all. Without the reset the controls kept showing the colour that had
        // just been removed, and reopening the dialog read that stale position
        // back as though it were still set.
        if (tabs->currentIndex() == 0) {
            outlinePane.showCleared();
            *outlineCleared = true;
            target->previewTagColor(QColor());
        } else {
            textPane.showCleared();
            *textCleared = true;
            target->previewTextColor(QColor());
        }
    });

    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    const int result = dlg.exec();

    if (result != QDialog::Accepted) {
        // Put the tile back exactly as it was found.
        target->previewTagColor(originalOutline);
        target->previewTextColor(originalText);
        return;
    }

    // An untouched tab keeps whatever the tile already had.
    if (*outlineCleared)      target->setTagColor(QColor());
    else if (*outlineTouched) target->setTagColor(outlinePane.currentColor());

    if (*textCleared)      target->setTextColor(QColor());
    else if (*textTouched) target->setTextColor(textPane.currentColor());
    saveTiles();
}

// =============================================================================
//  onTabMoved — user dragged a tab within the currently-visible group.
//  Rewrites that group's slots (compacted from slot 0) to match the new
//  relative order, rather than touching any other group.
// =============================================================================
void MainWindow::onTabMoved(int from, int to)
{
    if (from < 0 || from >= m_visibleKindOrder.size() ||
        to   < 0 || to   >= m_visibleKindOrder.size()) return;
    m_visibleKindOrder.move(from, to);

    QSet<int> groupKinds;
    for (int s = 0; s < m_tabsPerGroup; ++s)
        if (m_tabSlots[m_currentTabGroup][s] >= 0) groupKinds.insert(m_tabSlots[m_currentTabGroup][s]);

    int slot = 0;
    for (int k : std::as_const(m_visibleKindOrder)) {
        if (!groupKinds.contains(k)) continue;
        m_tabSlots[m_currentTabGroup][slot++] = k;
    }
    for (; slot < m_tabsPerGroup; ++slot) m_tabSlots[m_currentTabGroup][slot] = -1;

    saveTabSlotAssignment();
}

void MainWindow::onCustomTileClicked()
{
    CustomTileDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    TileData td = dlg.result();
    m_tiles.append(td);
    appendTileWidget(td);
    saveTiles();
    m_statusLbl->setStyleSheet("color:#66bb66; font-size:11px;");
    m_statusLbl->setText(QString("Custom tile created: %1").arg(td.title));
}

void MainWindow::onSearchClicked()
{
    QString q = m_searchEdit->text().trimmed();
    if (q.isEmpty()) {
        m_statusLbl->setStyleSheet("color:#cc5555; font-size:11px;");
        m_statusLbl->setText(m_searchModeGame ? "Type a game name first." : "Type a movie or show name first.");
        return;
    }
    hidePicker();
    setInputBusy(true);
    m_statusLbl->setStyleSheet("color:#6688cc; font-size:11px;");
    // v3.3.0 — routes to whichever scraper matches the current search mode.
    m_searchMerged.clear();
    m_lastSearchQuery = q;   // V5 — kept so results can be numeral-filtered

    // V5 — "gta 6" and "Grand Theft Auto VI" are the same thing to a person
    // but not to a search API, so a query naming a number is also sent in
    // the other notation. Only ever 1 or 2 requests: queryVariants returns
    // just the original when there's no number to swap.
    //
    // NOT applied to games: IgdbScraper has done this internally since
    // v3.3.43 (see generateNumeralAlternate there) and already fires three
    // parallel queries per search. Expanding here as well would make that
    // six, and IGDB is the strict provider — 4 requests/second, shared by
    // every installation through a single credential.
    const QStringList variants = m_searchModeGame ? QStringList{q}
                                                  : Numerals::queryVariants(q);
    m_searchPending = variants.size();

    // V5 safety net — nothing may leave the picker permanently unshown. If a
    // request is dropped outright (no reply, no error signal), this flushes
    // whatever arrived rather than leaving the search hanging forever.
    ++m_searchGeneration;
    int thisGeneration = m_searchGeneration;
    QTimer::singleShot(12000, this, [this, thisGeneration]{
        if (thisGeneration != m_searchGeneration) return;   // superseded
        if (m_searchPending <= 0) return;                   // already shown
        APPLOG("onSearchClicked: a search request never replied — showing partial results");
        m_searchPending = 1;
        onSearchResultsReady({});
    });

    if (m_searchModeGame) {
        m_statusLbl->setText("Searching IGDB…");
        for (const QString& v : variants) m_igdbScraper->searchMedia(v);
    } else {
        // V5 — search is TMDB's job again. It returns posters, cast and
        // director for the picker, which TVmaze and Wikipedia can't, and
        // with the app free (non-commercial) there's no reason to avoid it.
        // TVmaze is still the authority on EPISODE DATES — a tile added
        // here keeps its tmdbId and gets matched to TVmaze by title on its
        // first refresh (see TvmazeScraper::resolveTvmazeId).
        m_statusLbl->setText("Searching TMDB…");
        for (const QString& v : variants) m_scraper->searchMedia(v);
    }
}

// V5 — TVmaze is always the source of TV episode dates. This was briefly a
// user setting while the app had no API keys at all; it isn't one any more,
// because there's no good reason to opt out: TMDB leaves unaired episodes'
// dates null, which is exactly what left shows stranded in Released
// mid-season. Kept as a named function rather than inlining `true` so the
// intent stays legible at each call site, and so it's one edit if a real
// fallback is ever needed.
bool MainWindow::useTvmazeForTv()
{
    return true;
}

void MainWindow::onSearchResultsReady(const QList<SearchResult>& results)
{
    // V5 — a non-game search can be answered by two sources (TVmaze for
    // shows, TMDB for movies). Pool until every outstanding one has replied,
    // otherwise whichever returns first would wipe out the other's results.
    m_searchMerged += results;
    if (--m_searchPending > 0) return;

    // V5 — the numeral variants can return the same title twice, so
    // de-duplicate on (source, id) before anything else.
    QList<SearchResult> merged;
    QSet<QString> seen;
    for (const SearchResult& r : std::as_const(m_searchMerged)) {
        QString key = r.source + "/" + r.mediaType + "/" + QString::number(r.id);
        if (seen.contains(key)) continue;
        seen.insert(key);

        // V5 — drop results whose instalment number contradicts what was
        // typed. Server-side fuzzy matching returns neighbours: searching
        // "Black Ops 2" also comes back with "Black Ops III". A title with
        // no number at all is still a fair match and is kept.
        if (Numerals::rejectsQueryNumber(r.title, m_lastSearchQuery)) continue;
        merged.append(r);
    }

    // V5 — newest first. Release year is what people actually scan for when
    // picking between sequels, and it puts the entry someone is most likely
    // adding (an upcoming or recent title) at the top. Ties fall back to
    // popularity so same-year results stay sensibly ordered, and anything
    // with no year at all sinks rather than floating to the top.
    std::stable_sort(merged.begin(), merged.end(),
                     [](const SearchResult& a, const SearchResult& b) {
        if (a.year != b.year) {
            if (a.year == 0) return false;
            if (b.year == 0) return true;
            return a.year > b.year;
        }
        return a.popularity > b.popularity;
    });

    setInputBusy(false);
    m_statusLbl->setStyleSheet("color:#66aa66; font-size:11px;");
    m_statusLbl->setText(
        QString("Found %1 result%2 — pick one from above")
            .arg(merged.size()).arg(merged.size()==1?"":"s"));
    showPicker(merged);

    // v3.3.0 — credits (director/cast) are a TMDB-specific concept; skip
    // for IGDB game results, which don't have or need this.
    // V5 — and for TVmaze rows, which TMDB can't look up by a TVmaze id.
    QList<SearchResult> creditable;
    for (const SearchResult& r : std::as_const(merged))
        if (r.mediaType != "game" && r.source != "tvmaze") creditable.append(r);
    if (!creditable.isEmpty())
        m_scraper->fetchCreditsForResults(creditable);
}

void MainWindow::showPicker(const QList<SearchResult>& results)
{
    m_currentResults = results;
    m_pickerList->clear();
    for (const SearchResult& sr : results) {
        QString label;
        if (sr.mediaType == "game") {
            label = sr.title + "  •  Game";
            if (sr.year > 0) label += "  •  " + QString::number(sr.year);
        } else {
            label = sr.title + "  •  " + (sr.mediaType=="tv" ? "TV Show" : "Movie");
            if (!sr.director.isEmpty()) label += "  •  " + sr.director;
            if (!sr.castLine.isEmpty()) label += "  •  " + sr.castLine;
        }
        auto* item = new QListWidgetItem(label, m_pickerList);
        item->setData(Qt::UserRole, sr.id);
    }
    repositionPicker();
    m_pickerFrame->show();
    m_pickerFrame->raise();
}

void MainWindow::hidePicker()
{
    m_pickerFrame->hide();
    m_pickerList->clearSelection();
}

void MainWindow::onCreditsReady(int tmdbId, const QString& director, const QString& castLine)
{
    int idx = -1;
    for (int i = 0; i < m_currentResults.size(); ++i)
        if (m_currentResults[i].id == tmdbId) { idx = i; break; }
    if (idx < 0 || idx >= m_pickerList->count()) return;
    m_currentResults[idx].director = director;
    m_currentResults[idx].castLine = castLine;
    const SearchResult& sr = m_currentResults[idx];
    QString label = sr.title + "  •  " + (sr.mediaType=="tv"?"TV Show":"Movie");
    if (!sr.director.isEmpty()) label += "  •  " + sr.director;
    if (!sr.castLine.isEmpty()) label += "  •  " + sr.castLine;
    m_pickerList->item(idx)->setText(label);
}

void MainWindow::onPickerItemActivated(QListWidgetItem* item)
{
    int idx = m_pickerList->row(item);
    if (idx < 0 || idx >= m_currentResults.size()) return;
    hidePicker();
    const SearchResult& sr = m_currentResults[idx];
    for (const TileData& td : std::as_const(m_tiles)) {
        // v3.3.0 — also check mediaType, since a TMDB id and an IGDB id
        // can coincidentally be the same number for unrelated titles.
        if (td.tmdbId == sr.id && td.tmdbId > 0 && td.mediaType == sr.mediaType) {
            m_statusLbl->setStyleSheet("color:#cc8833; font-size:11px;");
            m_statusLbl->setText(QString("Already added: %1").arg(td.displayTitle()));
            m_searchEdit->clear();
            return;
        }
    }
    m_searchEdit->setText(sr.title);
    setInputBusy(true);
    m_statusLbl->setStyleSheet("color:#6688cc; font-size:11px;");
    m_statusLbl->setText(QString("Fetching details for %1…").arg(sr.title));
    // V5 — route the detail fetch back to whichever source produced this
    // result. sr.id is source-specific, so sending a TVmaze id to TMDB
    // would silently fetch a completely unrelated title.
    if (sr.source == "tvmaze")
        m_tvmaze->fetchDetails(sr.id, sr.mediaType, sr.posterPath);
    else if (sr.mediaType == "game")
        m_igdbScraper->fetchDetails(sr.id, sr.mediaType, sr.posterPath);
    else
        m_scraper->fetchDetails(sr.id, sr.mediaType, sr.posterPath);
}

void MainWindow::onScraperDataReady(const TileData& data)
{
    setInputBusy(false);
    m_searchEdit->clear();
    TileData td = data;
    // V5 — a brand-new tile's numbers came straight from the source, so
    // that's also its official baseline for a later Reset.
    td.officialStatusLabel        = td.statusLabel;
    td.officialSeasonEpisodeCount = td.seasonEpisodeCount;
    m_tiles.append(td);
    appendTileWidget(td);
    saveTiles();
    m_statusLbl->setStyleSheet("color:#66bb66; font-size:11px;");
    m_statusLbl->setText(QString("Added: %1").arg(td.title));
}

void MainWindow::onTileRefreshed(const TileData& updated)
{
    // v3.3.17 — was this refresh specifically the instant-refetch attempt
    // triggered right at episode expiry (see onGlobalTick())? If so and
    // the fresh data doesn't give us anything usable, fall back to the
    // local bump before finalizing below, instead of just sitting in
    // Released because TMDB didn't have anything new yet.
    bool wasPendingExpiry = m_pendingExpiryRefresh.remove(updated.id);

    for (TileData& td : m_tiles) {
        if (td.id != updated.id) continue;
        bool savedIsEstimated      = td.isEstimatedDate;
        QDate   savedTargetDateEarly = td.targetDate;

        // v3.3.22 fix — an already-valid estimate (not expired) being
        // recomputed into just another estimate isn't a meaningful
        // change; leave the tile completely untouched instead of routing
        // it through the widget update below at all. This is what was
        // causing the brief startup jolt (a tile flashing toward
        // Released and back to Countdowns) even though the end state
        // never actually changed — the tile doesn't need to be
        // "re-decided" every refresh, only checked for a newly-confirmed
        // date.
        bool wasValidEstimate = savedIsEstimated && savedTargetDateEarly.isValid()
            && !(savedTargetDateEarly < QDate::currentDate());
        if (wasValidEstimate && updated.isEstimatedDate) {
            break;
        }

        bool tmdbTitleChanged = (updated.title != td.title);
        QString savedCustomTitle   = td.customTitle;
        QDate   savedCustomDate    = td.customDate;
        QString savedCustomDateStr = td.customDateStr;
        QTime   savedCustomAirTime = td.customAirTime;
        QString savedImagePath     = td.imagePath;
        NotifStatus savedNotif     = td.notifStatus;
        QDate   savedTargetDate    = td.targetDate;   // v3.3.18 — captured before the overwrite below, so we can tell a genuinely later date from the same one coming back unchanged
        bool    savedNoDate        = td.noDateOverride;
        bool    savedIsLooped      = td.isLooped;
        bool    savedPendingLoop   = td.pendingLoopNotice;   // V5.4.3 — a scraper knows nothing about it
        QDate   savedLoopLastOcc   = td.loopLastOccurrence;  // V5.4.26 — nor about this
        QDateTime savedAirStamp    = td.airStampUtc;         // V5.4.28 — only TVmaze sets it
        QString savedPresetType    = td.presetType;
        QString savedLoopInterval  = td.loopInterval;
        int     savedLoopWeekday   = td.loopWeekday;
        int     savedLoopDayOfMonth= td.loopDayOfMonth;
        QDate   savedUnverifiedSince = td.unverifiedSince;   // V5 — wiped by the assignment below
        int     savedTvmazeId        = td.tvmazeId;          // V5
        int     savedTmdbId          = td.tmdbId;            // V5
        QString savedTmdbUrl         = td.tmdbUrl;           // V5
        QString savedTvmazeUrl       = td.tvmazeUrl;         // V5
        int     savedArtworkSeason    = td.artworkSeason;    // V5
        QDate   savedArtworkFetchedOn = td.artworkFetchedOn; // V5
        QDate   savedRecappedDate     = td.recappedDate;     // V5
        QString savedRecappedLabel    = td.recappedLabel;    // V5
        bool    savedEpisodeOverride  = td.episodeOverride;  // V5
        QString savedStatusLabel      = td.statusLabel;      // V5
        int     savedSeasonEpisodeCount = td.seasonEpisodeCount;   // V5
        const TileData beforeRefresh = td;
        // V5.4.23 - write down what this tile is showing RIGHT NOW, before the
        // refresh replaces it. The sweep on save can only see the tile's
        // current state, so an episode that aired while the app was closed
        // would be overwritten by the next one before anything recorded it:
        // the app would start, refresh, advance S04E02 to S04E03, and S04E02
        // would never have existed as far as the history was concerned. This
        // is the one moment the outgoing episode is still on the tile.
        ReleaseHistory::instance().sweep({beforeRefresh});
        td = updated;
        // V5.4.16 — the user's own visual settings, which no scraper returns.
        // See TileData::carryUserSettingsFrom for why this is one call rather
        // than three more lines in the list below.
        td.carryUserSettingsFrom(beforeRefresh);
        // V5 — a tile's source ids are permanent identity, not per-refresh
        // data. Whichever source answered only populates its OWN id, so
        // without this a TMDB refresh silently zeroes tvmazeId (and vice
        // versa), forcing an expensive title re-match on the next launch.
        // V5.4.28 — only TvmazeScraper produces an air-time instant, so a TMDB
        // refresh of a TV tile would otherwise blank it and the Time Zone
        // setting would stop reaching that show until TVmaze next answered.
        if (!td.airStampUtc.isValid()) td.airStampUtc = savedAirStamp;
        if (td.tvmazeId <= 0) td.tvmazeId = savedTvmazeId;
        if (td.tmdbId   <= 0) td.tmdbId   = savedTmdbId;
        // Each scraper only knows its OWN link, so neither may blank the other's.
        if (td.tmdbUrl.isEmpty())   td.tmdbUrl   = savedTmdbUrl;
        if (td.tvmazeUrl.isEmpty()) td.tvmazeUrl = savedTvmazeUrl;
        // V5 — same reasoning for artwork provenance: only the scraper that
        // actually downloaded an image sets these, so a refresh from the
        // OTHER source must not blank them and restart the 6-month clock.
        if (!td.artworkFetchedOn.isValid()) {
            td.artworkSeason    = savedArtworkSeason;
            td.artworkFetchedOn = savedArtworkFetchedOn;
        }
        // V5 — recap state is the app's own bookkeeping; no scraper ever
        // sets it, so a refresh would blank it and the same releases would
        // be reported as "missed" again on every single launch.
        td.recappedDate  = savedRecappedDate;
        td.recappedLabel = savedRecappedLabel;

        // V5 — a hand-typed season/episode survives refreshes until the
        // source actually has something NEW to say. statusLabel is scraped
        // data, so without this any refresh restored the official numbers —
        // and because moving a tile between Countdowns and Released
        // triggers a refresh, the override appeared to undo itself the
        // moment the date was edited. A genuinely different date means real
        // data has caught up, which is when the override is meant to go.
        // V5 — always record what the SOURCE said, even while an override is
        // in force. That's what Reset restores to, and it's the only copy of
        // the real numbers once statusLabel is holding a hand-typed value.
        if (!updated.statusLabel.isEmpty()) {
            td.officialStatusLabel        = updated.statusLabel;
            td.officialSeasonEpisodeCount = updated.seasonEpisodeCount;
        }

        if (savedEpisodeOverride) {
            bool sourceHasNewDate = updated.targetDate.isValid()
                                 && updated.targetDate != savedTargetDate;
            if (sourceHasNewDate) {
                td.episodeOverride = false;          // real data wins
                APPLOG(QString("onTileRefreshed: '%1' — new date from source, clearing manual episode override")
                           .arg(td.displayTitle()));
            } else {
                td.statusLabel        = savedStatusLabel;
                td.seasonEpisodeCount = savedSeasonEpisodeCount;
                td.episodeOverride    = true;
            }
        }
        // V5 — a genuinely verified date ends the unverified stretch and
        // clears any break state; another estimate coming back just means
        // we still don't know, so the existing clock keeps running rather
        // than resetting (which would postpone the break decision forever).
        if (updated.isEstimatedDate) {
            td.unverifiedSince = savedUnverifiedSince;
        } else {
            td.unverifiedSince  = QDate();
            td.inMidSeasonBreak = false;
        }
        // V5.4.4 — a custom date normally survives a refresh untouched, which
        // is the whole point of it. The one exception is a scraper deliberately
        // clearing a SPENT one: TvmazeScraper::fetchReturningWindow() drops a
        // custom date that is already in the past when the next season is
        // announced for the future, because effectiveDate() prefers it and it
        // would otherwise pin the tile to the old season's day beside the new
        // season's label (Invincible read "S05E01 · April 22, 2026" that way).
        //
        // Restoring it here unconditionally would put it straight back, so the
        // clear has to be recognised. It only counts as one when the tile HAD a
        // custom date and the refreshed copy doesn't — a scraper that simply
        // doesn't know about custom dates carries them through untouched
        // (TmdbScraper and IgdbScraper both copy them across explicitly), so
        // this can't be triggered accidentally.
        const bool customDateWasSpent = savedCustomDate.isValid()
                                     && !updated.customDate.isValid();
        if (customDateWasSpent) {
            APPLOG(QString("onTileRefreshed: '%1' — keeping the source's window, its "
                           "custom date had already passed").arg(td.displayTitle()));
            td.customDate    = QDate();
            td.customDateStr = QString();
            td.customAirTime = QTime();
        } else {
            td.customDate      = savedCustomDate;
            td.customDateStr   = savedCustomDateStr;
            td.customAirTime   = savedCustomAirTime;
        }
        td.imagePath       = savedImagePath;
        td.customTitle     = tmdbTitleChanged ? QString() : savedCustomTitle;
        td.noDateOverride  = savedNoDate;
        td.isLooped        = savedIsLooped;
        td.pendingLoopNotice = savedPendingLoop;
        td.loopLastOccurrence = savedLoopLastOcc;
        td.presetType      = savedPresetType;
        td.loopInterval    = savedLoopInterval;
        td.loopWeekday     = savedLoopWeekday;
        td.loopDayOfMonth  = savedLoopDayOfMonth;

        // v3.3.18 fix — a critical bug: "not less than today" is true even
        // when updated.targetDate is the EXACT SAME (already-expired) date
        // coming back unchanged from a refresh — e.g. the episode that
        // just aired, re-confirmed as-is because TMDB doesn't have a next
        // one yet. Combined with the v3.3.17 expiry-triggered refresh
        // (which sets notifStatus to Ready right before the refresh, so
        // the old "savedNotif == Ready" fallback here became unconditionally
        // true for that whole flow), this was being incorrectly treated as
        // "the date advanced" — flipping the tile back to Active with the
        // SAME expired date, which then immediately re-expired on the very
        // next tick. That's the flicker loop between Released and
        // Countdowns. Now requires the new date to be strictly later than
        // what the tile already had (or there was no valid previous date
        // at all), not just "not in the past."
        bool dateAdvanced = updated.targetDate.isValid() &&
                            !(updated.targetDate < QDate::currentDate()) &&
                            (!savedTargetDate.isValid() || updated.targetDate > savedTargetDate);

        if (dateAdvanced) {
            td.notifStatus = NotifStatus::Active;
            td.notified    = false;
        } else {
            // v3.3.17 — the instant refetch didn't produce a usable future
            // date (network hiccup, or TMDB genuinely has nothing new yet)
            // — fall back to the local bump right now instead of waiting.
            if (wasPendingExpiry && tryLocalEpisodeAdvance(td)) {
                APPLOG(QString("onTileRefreshed: instant refetch for '%1' didn't help — falling back to local bump").arg(td.displayTitle()));
            } else {
                td.notifStatus = savedNotif;
                // Keep notified=true only if the TMDB date hasn't changed
                td.notified    = (td.targetDate == updated.targetDate) && (savedNotif != NotifStatus::Active);
            }
        }

        // V5 — a published correction is applied last, so it wins over what
        // the source just supplied. It only ever writes airTime, so a user's
        // own customAirTime still takes precedence in effectiveTime().
        ShowOverrides::instance().apply(td);

        // V5 — TV tiles take dates from TVmaze and artwork from TMDB, so the
        // image has to be requested separately after the date refresh lands.
        // Only when it's actually needed: absent, or stale because the show
        // moved to a new season or the stored copy is approaching TMDB's
        // 6-month caching limit.
        if (td.mediaType == "tv" && td.tmdbId > 0 && useTvmazeForTv()) {
            bool missing = td.fetchedImagePath.isEmpty()
                           || !QFile::exists(td.fetchedImagePath);
            int season = parseEpisodeLabel(td.statusLabel).season;
            if (missing || td.artworkNeedsRefresh(season)) {
                // Never yank away a custom image the user chose themselves.
                bool makeActive = td.customImagePaths.isEmpty();
                m_scraper->fetchBackdropOnly(td.id, td.tmdbId, "tv", makeActive);
                td.artworkSeason    = season;
                td.artworkFetchedOn = QDate::currentDate();
            }
        }

        for (TileWidget* tw : std::as_const(m_tileWidgets)) {
            if (tw->tileData().id == td.id) {
                tw->updateData(td);
                break;
            }
        }
        break;
    }
    // v3.3.37 fix — previously sortAndRebuildAllTabs() ran on every single
    // refresh that came back, unconditionally. Since refreshAllTiles()
    // fires every tile's refresh in parallel at startup, this meant a
    // full grid rebuild for each one as responses trickled back in — the
    // same wasteful "rebuild N times instead of once" pattern already
    // fixed for the startup tile-loading loop, just triggered by network
    // responses instead. The per-tile widget update above still runs
    // immediately either way, so each tile's own display stays current;
    // only the full grid/tab rebuild is now deferred until the last
    // pending refresh finishes. A single manual refetch (not part of a
    // bulk batch) is unaffected — the counter reaches zero immediately
    // when it completes, so the rebuild still happens right away.
    // V5.4.12 — clamped, and it records that something arrived.
    //
    // This was a bare `--m_refreshPending`, which could take the counter
    // BELOW zero: the watchdog (and, before it, the 20-second net) sets the
    // count to 0 while replies are still in flight, and each straggler then
    // decremented from there. A counter at -3 means the next three tiles
    // refreshed after it show no "refreshing" indicator at all, and the batch
    // they belong to reports itself finished before it is — the count has to
    // climb back to zero before it means anything again. Clamping keeps
    // "outstanding replies" from ever being a negative number of replies.
    m_refreshPending = qMax(0, m_refreshPending - 1);
    m_lastRefreshProgress.restart();   // the watchdog only fires when this stops moving
    updateRefreshStatus();
    if (m_refreshPending <= 0) {
        if (m_refreshWatchdog) m_refreshWatchdog->stop();
        sortAndRebuildAllTabs();
        m_statusLbl->setStyleSheet(kStatusStyleNeutral);
        m_statusLbl->setText(
            QString("%1 tile%2 ready").arg(m_tiles.size()).arg(m_tiles.size()==1?"":"s"));
        saveTiles();
    }
}

void MainWindow::onPosterReady(const QString& tileId, const QString& localPath, bool makeActive)
{
    auto isOwned = [](const QString& p) {
        return !p.isEmpty() &&
               (p.contains("/fetched_images/") || p.contains("\\fetched_images\\") ||
                p.contains("/custom_images/")  || p.contains("\\custom_images\\"));
    };
    bool foundInTiles = false;
    for (TileData& td : m_tiles) {
        if (td.id != tileId) continue;
        foundInTiles = true;
        // Replace the fetched slot specifically — never touches custom images.
        if (!td.fetchedImagePath.isEmpty() && td.fetchedImagePath != localPath && isOwned(td.fetchedImagePath))
            QFile::remove(td.fetchedImagePath);
        td.fetchedImagePath = localPath;
        if (makeActive || td.imagePath.isEmpty()) td.imagePath = localPath;
        break;
    }
    bool foundInWidgets = false;
    for (TileWidget* tw : std::as_const(m_tileWidgets)) {
        if (tw->tileData().id == tileId) {
            foundInWidgets = true;
            TileData upd = tw->tileData();
            upd.fetchedImagePath = localPath;
            if (makeActive || upd.imagePath.isEmpty()) upd.imagePath = localPath;
            tw->updateData(upd);
            break;
        }
    }
    // V4.12 fix — the tile this belongs to doesn't exist yet (the season-
    // scan fetch that actually creates it, for TV shows that need one,
    // hasn't completed). Buffer it instead of losing it — see the member
    // declaration in mainwindow.h for the full race explanation.
    if (!foundInTiles && !foundInWidgets) {
        m_pendingPosterUpdates[tileId] = qMakePair(localPath, makeActive);
        APPLOG(QString("onPosterReady: no tile '%1' exists yet — buffered for when it's created").arg(tileId));
        return;   // nothing to save yet either
    }
    saveTiles();
}

void MainWindow::onScraperError(const QString& msg)
{
    // V5 — a search can now be answered by more than one request (the
    // roman/arabic numeral variants), and onSearchResultsReady only shows
    // the picker once every outstanding one has replied. An error reply
    // never reached that counter, so a single failed variant left the
    // count stuck above zero and the picker simply never appeared — the
    // results were fetched and then silently discarded. Count the failure
    // and show whatever did arrive.
    if (m_searchPending > 0 && --m_searchPending == 0 && !m_searchMerged.isEmpty()) {
        ++m_searchPending;              // onSearchResultsReady decrements again
        onSearchResultsReady({});
        return;
    }

    setInputBusy(false);
    m_statusLbl->setStyleSheet("color:#cc5555; font-size:11px;");
    m_statusLbl->setText("Error: " + msg.split('\n').first());
}

void MainWindow::setInputBusy(bool busy)
{
    m_searchEdit->setEnabled(!busy);
    m_searchBtn->setEnabled(!busy);
    m_searchBtn->setText(busy ? "Searching…" : "🔍  Search Media");
}

void MainWindow::onTileDataChanged(const QString& tileId)
{
    for (TileWidget* tw : std::as_const(m_tileWidgets)) {
        if (tw->tileData().id != tileId) continue;
        const TileData& w = tw->tileData();
        for (TileData& td : m_tiles) {
            if (td.id != tileId) continue;
            bool wasReleased = td.hasDate() && td.isExpired();
            td.customTitle    = w.customTitle;
            td.customDate     = w.customDate;
            td.customDateStr  = w.customDateStr;
            td.customAirTime  = w.customAirTime;
            td.imagePath      = w.imagePath;
            td.fetchedImagePath = w.fetchedImagePath;   // v3.1.0 — multi-image support
            td.customImagePaths = w.customImagePaths;   // v3.1.0 — multi-image support
            td.noDateOverride = w.noDateOverride;
            td.isLooped       = w.isLooped;
            td.loopInterval   = w.loopInterval;
            td.loopWeekday    = w.loopWeekday;
            td.loopDayOfMonth = w.loopDayOfMonth;
            td.presetType     = w.presetType;
            td.tagColor       = w.tagColor;   // V4.7 — per-tile color tag
            // V5.4.16 — textColor belongs here too, and its absence was the
            // fifth outing for this exact bug (see the season/episode note
            // below, which describes the same failure). TileWidget held the
            // new colour, so the tile LOOKED right and the preview worked,
            // but m_tiles is the copy that gets saved and rebuilt from — so
            // the moment anything rebuilt the grid, the old value came back.
            td.textColor      = w.textColor;
            td.isFavorite = w.isFavorite;   // V4.11
            td.mediaType      = w.mediaType;   // v3.0.0 — Type dropdown can now change this

            // V5 — the hand-typed Season/Episode/Total. These were missing
            // from this list, which is why the override "worked" visually
            // and then vanished: TileWidget held the new numbers, but this
            // is where m_tiles (the copy that actually gets saved and
            // refreshed) is updated, so the old values were what persisted.
            // Changing the date happens to trigger a rebuild from m_tiles,
            // which is why editing the date appeared to be what reset them.
            td.statusLabel        = w.statusLabel;
            td.seasonEpisodeCount = w.seasonEpisodeCount;
            td.episodeOverride    = w.episodeOverride;
            td.officialStatusLabel        = w.officialStatusLabel;
            td.officialSeasonEpisodeCount = w.officialSeasonEpisodeCount;

            bool nowReleased  = td.hasDate() && td.isExpired();

            // KEY FIX: when a tile moves from Released back to Active (user
            // set a future date), reset notifStatus to Active so onGlobalTick
            // will fire the transition and notification again when it expires.
            // Without this, notifStatus stays Ready and the tile never moves
            // tabs on its own.
            if (wasReleased && !nowReleased) {
                td.notifStatus = NotifStatus::Active;
                tw->updateData(td);
                APPLOG(QString("onTileDataChanged: '%1' released→active/other").arg(td.displayTitle()));
            }
            break;
        }
        break;
    }
    sortAndRebuildAllTabs();
    saveTiles();
}

void MainWindow::onImageChanged(const QString& tileId, const QString& /*path*/)
{
    auto isOwned = [](const QString& p) {
        return !p.isEmpty() &&
               (p.contains("/fetched_images/") || p.contains("\\fetched_images\\") ||
                p.contains("/custom_images/")  || p.contains("\\custom_images\\"));
    };
    for (TileWidget* tw : std::as_const(m_tileWidgets)) {
        if (tw->tileData().id != tileId) continue;
        const TileData& wData = tw->tileData();
        for (TileData& td : m_tiles) {
            if (td.id != tileId) continue;
            QString oldPath = td.imagePath;
            if (oldPath != wData.imagePath && isOwned(oldPath)) QFile::remove(oldPath);
            td.imagePath     = wData.imagePath;
            td.customTitle   = wData.customTitle;
            td.customDate    = wData.customDate;
            td.customDateStr = wData.customDateStr;
            td.customAirTime = wData.customAirTime;
            break;
        }
        break;
    }
    saveTiles();
}

// =============================================================================
//  onRemoveTile — removes tile from data, destroys widget synchronously,
//  then rebuilds all tabs so remaining tiles fill the gap immediately.
//
//  IMPORTANT: delete tw synchronously (not deleteLater) so the widget is
//  fully gone before sortAndRebuildAllTabs runs. deleteLater defers
//  destruction to the next event loop — the widget would still exist as a
//  hidden child of the container during the rebuild, causing crashes or
//  ghost gaps.
// =============================================================================
void MainWindow::onRemoveTile(const QString& tileId)
{
    APPLOG(QString("onRemoveTile: id=%1").arg(tileId));
    QString imgToDelete;
    for (const TileData& td : std::as_const(m_tiles)) {
        if (td.id == tileId) {
            if (!td.imagePath.isEmpty() &&
                (td.imagePath.contains("/fetched_images/") || td.imagePath.contains("\\fetched_images\\") ||
                 td.imagePath.contains("/custom_images/")  || td.imagePath.contains("\\custom_images\\")))
                imgToDelete = td.imagePath;
            break;
        }
    }
    m_tiles.removeIf([&](const TileData& td){ return td.id == tileId; });

    for (int i = 0; i < m_tileWidgets.size(); ++i) {
        if (m_tileWidgets[i]->tileData().id == tileId) {
            TileWidget* tw = m_tileWidgets.takeAt(i);
            APPLOG(QString("onRemoveTile: deleting widget, rebuilding"));
            tw->hide();               // v3.2.2 — hide before detaching, same reasoning as populateActiveKindGrid()
            tw->setParent(nullptr);   // detach — Qt auto-cleans any layout reference
            delete tw;                // synchronous — gone before rebuild
            sortAndRebuildAllTabs();  // reflow remaining tiles
            break;
        }
    }

    if (!imgToDelete.isEmpty()) QFile::remove(imgToDelete);
    saveTiles();
    m_statusLbl->setStyleSheet("color:#888; font-size:11px;");
    m_statusLbl->setText("Tile removed.");
}

// =============================================================================
//  V4.6 — multi-select delete. toggleSelectMode() flips select mode for
//  EVERY tile (not just the ones on the currently-visible tab), so
//  switching tabs mid-selection doesn't leave some tiles still in normal
//  click-to-edit mode. Exiting select mode (either via the button again, or
//  automatically once Delete All finishes) clears any existing selection.
// =============================================================================
void MainWindow::toggleSelectMode()
{
    m_selectMode = !m_selectMode;
    m_selectedTileCount = 0;
    for (TileWidget* tw : std::as_const(m_tileWidgets)) {
        tw->setSelectMode(m_selectMode);
        tw->setAnySelected(false);   // V4.12
    }

    m_selectModeBtn->setStyleSheet(m_selectMode
        ? "QPushButton { background:#0078d4; color:#fff; border:1px solid #0078d4; "
          "border-radius:5px; font-size:16px; font-weight:bold; padding:0; }"
          "QPushButton:hover { background:#1a8de4; }"
        : "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
          "border-radius:5px; font-size:16px; font-weight:bold; padding:0; }"
          "QPushButton:hover { background:#383838; }");
    m_selectModeBtn->setToolTip(m_selectMode ? "Exit select mode" : "Select tiles to delete");
    m_clearSelectionBtn->hide();   // V4.8 — selection always resets to 0 on toggle, either direction

    // V4.8 — cover the search/custom-tile bar for as long as select mode
    // is active, so it's never ambiguous which mode you're in.
    if (m_selectMode) {
        m_selectModeOverlay->setGeometry(m_bottomBar->geometry());
        m_selectModeOverlay->show();
        m_selectModeOverlay->raise();
    } else {
        m_selectModeOverlay->hide();
    }
}

void MainWindow::onTileSelectionChanged(const QString&, bool selected)
{
    m_selectedTileCount += selected ? 1 : -1;
    m_clearSelectionBtn->setVisible(m_selectedTileCount > 0);   // V4.8
    bool any = m_selectedTileCount > 0;   // V4.12
    for (TileWidget* tw : std::as_const(m_tileWidgets))
        tw->setAnySelected(any);
}

void MainWindow::clearTileSelection()
{
    for (TileWidget* tw : std::as_const(m_tileWidgets)) {
        if (tw->isSelected()) tw->setSelected(false);
        tw->setAnySelected(false);   // V4.12
    }
    m_selectedTileCount = 0;
    m_clearSelectionBtn->hide();
}

void MainWindow::onDeleteAllSelectedRequested()
{
    QStringList ids;
    for (TileWidget* tw : std::as_const(m_tileWidgets))
        if (tw->isSelected()) ids << tw->tileData().id;

    if (!ids.isEmpty()) onRemoveMultipleTiles(ids);

    if (m_selectMode) toggleSelectMode();   // the action is complete — exit select mode
}

// =============================================================================
//  onRemoveMultipleTiles — batch version of onRemoveTile(). Removes every
//  matching tile from m_tiles/m_tileWidgets and deletes any owned image
//  files, then calls sortAndRebuildAllTabs() and saveTiles() ONCE at the
//  end rather than once per tile, which would otherwise rebuild the whole
//  tab layout and hit disk repeatedly for what's really one user action.
// =============================================================================
void MainWindow::onRemoveMultipleTiles(const QStringList& ids)
{
    if (ids.isEmpty()) return;
    QSet<QString> idSet(ids.begin(), ids.end());
    APPLOG(QString("onRemoveMultipleTiles: removing %1 tile(s)").arg(idSet.size()));

    QStringList imagesToDelete;
    for (const TileData& td : std::as_const(m_tiles)) {
        if (!idSet.contains(td.id)) continue;
        if (!td.imagePath.isEmpty() &&
            (td.imagePath.contains("/fetched_images/") || td.imagePath.contains("\\fetched_images\\") ||
             td.imagePath.contains("/custom_images/")  || td.imagePath.contains("\\custom_images\\")))
            imagesToDelete << td.imagePath;
    }
    m_tiles.removeIf([&](const TileData& td){ return idSet.contains(td.id); });

    for (int i = m_tileWidgets.size() - 1; i >= 0; --i) {
        if (!idSet.contains(m_tileWidgets[i]->tileData().id)) continue;
        TileWidget* tw = m_tileWidgets.takeAt(i);
        tw->hide();
        tw->setParent(nullptr);
        delete tw;
    }

    for (const QString& img : std::as_const(imagesToDelete)) QFile::remove(img);

    sortAndRebuildAllTabs();
    saveTiles();
    m_statusLbl->setStyleSheet("color:#888; font-size:11px;");
    m_statusLbl->setText(QString("%1 tile(s) removed.").arg(idSet.size()));
}

void MainWindow::onDuplicateTile(const QString& tileId)
{
    // Find the source tile
    TileData src;
    bool found = false;
    for (const TileData& td : std::as_const(m_tiles)) {
        if (td.id == tileId) { src = td; found = true; break; }
    }
    if (!found) return;

    // Give the duplicate a fresh id and reset notification state
    const QString oldId = src.id;
    src.id         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    src.notifStatus = NotifStatus::Active;
    src.notified   = false;

    // V5.4.23 - and give it its own COPIES of the images.
    //
    // This used to copy the paths, so both tiles pointed at the same files on
    // disk. Deleting either one then deleted the picture out from under the
    // other, which is why a duplicated tile went black after its twin was
    // removed: deleteBackdropIfOwned() sees a path inside the app's own image
    // folder and removes the file, with no idea another tile is using it.
    // Removing a tile must never change how a different tile looks.
    {
        auto copyBeside = [&oldId, &src](const QString& path) -> QString {
            if (path.isEmpty() || !QFile::exists(path)) return path;
            QFileInfo fi(path);
            // Image files are named after the tile that owns them, so the copy
            // takes the new id; anything else keeps its name with the id
            // swapped, which keeps "<id>_custom.png" style names intact.
            QString name = fi.fileName();
            name = name.contains(oldId) ? QString(name).replace(oldId, src.id)
                                        : src.id + "_" + name;
            const QString dest = fi.absolutePath() + "/" + name;
            if (dest == path) return path;             // nothing to do
            QFile::remove(dest);                        // a stale leftover, if any
            return QFile::copy(path, dest) ? dest : path;
        };

        const QString oldActive = src.imagePath;
        QStringList copiedCustoms;
        for (const QString& c : std::as_const(src.customImagePaths))
            copiedCustoms << copyBeside(c);
        const QString copiedFetched = copyBeside(src.fetchedImagePath);

        // Keep whichever image was showing, now pointing at this tile's copy.
        QString newActive = copiedFetched;
        for (int i = 0; i < src.customImagePaths.size() && i < copiedCustoms.size(); ++i)
            if (src.customImagePaths[i] == oldActive) newActive = copiedCustoms[i];
        if (oldActive == src.fetchedImagePath) newActive = copiedFetched;

        src.customImagePaths = copiedCustoms;
        src.fetchedImagePath = copiedFetched;
        src.imagePath        = newActive.isEmpty() ? oldActive : newActive;
    }

    m_tiles.append(src);
    appendTileWidget(src);
    saveTiles();
    APPLOG(QString("onDuplicateTile: duplicated '%1' → new id %2").arg(src.displayTitle(), src.id));
    m_statusLbl->setStyleSheet("color:#66bb66; font-size:11px;");
    m_statusLbl->setText(QString("Duplicated: %1").arg(src.displayTitle()));
}

// =============================================================================
//  onGlobalTick — fires every second.
//
//  1. Ticks countdowns on the visible tab.
//  2. Detects Active tiles that just expired → moves them to Released tab
//     immediately (no restart required) + marks Ready for notifier.
// =============================================================================
// =============================================================================
//  tryLocalEpisodeAdvance — v3.3.16. Locally advances a TV show tile's
//  episode number and date the moment its currently-confirmed episode's
//  date passes, WITHOUT any API call — using only data already stored on
//  the tile from a past fetch (seasonEpisodeCount, statusLabel). This is a
//  different mechanism from the refresh-based estimate logic in
//  TmdbScraper: that logic runs when NEW data comes back from an API
//  call; this runs purely on the app's own internal state, the instant a
//  tile would otherwise sit in Released despite the season's own episode
//  count (already known) being higher than the episode that just aired.
//  No date needs to be fetched to know that — the season total was
//  already on hand.
// =============================================================================
static bool tryLocalEpisodeAdvance(TileData& td)
{
    if (td.mediaType != "tv") return false;
    // V5 — never guess on top of hand-typed numbers. If the user has set a
    // specific season/episode/total, that's a deliberate statement about
    // where the show is, and quietly advancing it to the "next" episode a
    // week later overwrites exactly what they entered.
    if (td.episodeOverride) return false;
    // V5 — a month-only date is an announced window, not an episode on a
    // weekly cadence. Rolling it forward by seven days would turn "March
    // 2027" into a specific invented date. V5.4 — same for a year window.
    if (td.isWindowDate()) return false;
    if (td.seasonEpisodeCount <= 0) return false;

    // V5 — shared parser, so a multi-episode label ("S04E01+E02+E03") advances
    // off its LAST episode instead of failing to parse and stranding the tile
    // in Released forever.
    EpisodeLabel el = parseEpisodeLabel(td.statusLabel);
    if (!el.valid) return false;

    int season = el.season;
    int epNum  = el.lastEpisode;
    if (epNum <= 0 || epNum >= td.seasonEpisodeCount)
        return false;   // season genuinely complete (or count unknown) — leave as Released

    QDate base = td.targetDate.isValid() ? td.targetDate : QDate::currentDate();
    QDate estimate = base.addDays(7);
    QDate today = QDate::currentDate();
    while (estimate < today) estimate = estimate.addDays(7);

    // V5 — only anchor when advancing off a REAL episode. Advancing off an
    // estimate (a lapsed guess rolling forward another week) must keep the
    // original verified anchor, otherwise each roll-forward would overwrite
    // it with the previous guess and a later Mid-Season Break would revert
    // to a date that was itself never confirmed.
    if (!td.isEstimatedDate) {
        td.lastVerifiedDate        = base;
        td.lastVerifiedStatusLabel = td.statusLabel;
    }
    td.targetDate       = estimate;
    td.dateDisplay      = estimate.toString("MMMM d, yyyy");
    td.statusLabel      = QString("S%1E%2")
                              .arg(season,      2, 10, QChar('0'))
                              .arg(epNum + 1,   2, 10, QChar('0'));
    td.isEstimatedDate  = true;
    td.inMidSeasonBreak = false;
    td.notifStatus      = NotifStatus::Active;
    td.notified         = false;
    return true;
}

// =============================================================================
//  lapseWindowDate — V5.4. An announced month/year window has passed without
//  a real date ever appearing.
//
//  Nothing released: "2026" was never a claim that the show came out on
//  December 31, it was the only thing anyone had said. So the window is
//  dropped and the tile falls back to whatever IS true, which is one of two
//  things:
//
//   • The show has aired before. The last episode that genuinely aired is
//     still the most recent real thing about it, so the tile returns to
//     exactly that — the verified date and its episode label — and sits in
//     Released, where it was before the window was announced.
//
//   • The show has never aired. There is nothing to fall back to, so it
//     keeps no date at all and moves to Other.
//
//  The date is cleared by invalidating targetDate rather than by setting
//  noDateOverride. They look identical on the tile, but noDateOverride is the
//  user's own "remove the date" switch and is deliberately preserved across
//  refreshes — setting it here would mean that once a window lapsed, a real
//  date published later could never appear on that tile again.
//
//  Returns true when anything changed.
// =============================================================================
static bool lapseWindowDate(TileData& td)
{
    if (!td.isWindowDate()) return false;

    const QDate   oldDate  = td.targetDate;
    const QString oldLabel = td.statusLabel;

    td.isMonthOnlyDate = false;
    td.isYearOnlyDate  = false;

    if (td.hasAiredBefore() && td.lastVerifiedDate.isValid()) {
        // Back to the last episode that actually aired.
        td.targetDate  = td.lastVerifiedDate;
        td.dateDisplay = td.lastVerifiedDate.toString("MMMM d, yyyy");
        if (!td.lastVerifiedStatusLabel.isEmpty())
            td.statusLabel = td.lastVerifiedStatusLabel;
        APPLOG(QString("lapseWindowDate: '%1' window %2 lapsed — restoring last aired %3 on %4")
                   .arg(td.displayTitle(), oldDate.toString(Qt::ISODate),
                        td.statusLabel, td.targetDate.toString(Qt::ISODate)));
    } else {
        // Nothing has ever come out under this title.
        td.targetDate  = QDate();
        td.dateDisplay = "No Release Date Yet";
        td.airTime     = QTime();
        APPLOG(QString("lapseWindowDate: '%1' window %2 lapsed with nothing ever aired — no date, moving to Other")
                   .arg(td.displayTitle(), oldDate.toString(Qt::ISODate)));
    }

    // A window is not a release, so it must never leave a pending
    // notification behind — the tile did not come out.
    td.notifStatus     = NotifStatus::Inactive;
    td.notified        = false;
    td.isEstimatedDate = false;
    td.inMidSeasonBreak = false;
    td.unverifiedSince  = QDate();

    return td.targetDate != oldDate || td.statusLabel != oldLabel;
}

void MainWindow::onGlobalTick()
{
    int currentTabIdx = m_tabs->currentIndex();
    int currentKind = (currentTabIdx >= 0 && currentTabIdx < m_visibleKindOrder.size())
                      ? m_visibleKindOrder[currentTabIdx] : -1;
    bool anyExpired = false;

    for (TileWidget* tw : std::as_const(m_tileWidgets)) {
        bool onVisibleTab = (currentKind >= 0 && tileMatchesKind(tw->tileData(), currentKind));
        tw->tick(onVisibleTab);

        // Detect Active → expired transition
        const TileData& td = tw->tileData();
        if (td.notifStatus == NotifStatus::Active && td.hasDate() && td.isExpired()) {
            for (TileData& mtd : m_tiles) {
                if (mtd.id != td.id) continue;

                // Mark as notified — TrayApp will fire the actual notification
                // when it receives the REFRESH signal at the end of this tick.

                if (mtd.isLooped) {
                    APPLOG(QString("onGlobalTick: looped tile '%1' expired — advancing (%2)").arg(mtd.displayTitle(), mtd.loopInterval));
                    // V5.4.2 — the rule now lives in loopschedule.h so the
                    // notifier can apply the identical one. It is the only
                    // thing running when this app is closed, and a looped
                    // tile it notified without advancing stayed Inactive and
                    // never rolled again.
                    QDate next = LoopSchedule::nextOccurrence(
                        mtd.loopInterval, mtd.presetType,
                        mtd.loopWeekday, mtd.loopDayOfMonth,
                        mtd.targetDate, mtd.customDate);
                    if (next.isValid()) {
                        // V5.4.26 — what this tile is advancing AWAY from, kept
                        // before it is overwritten. It is the only record that
                        // the occurrence happened at all: a second from now the
                        // tile shows next year's date, and the startup recap and
                        // Recap/History would both have nothing to report.
                        mtd.loopLastOccurrence = mtd.effectiveDate();
                        mtd.targetDate    = next;
                        mtd.customDate    = QDate();
                        mtd.dateDisplay   = next.toString("MMMM d, yyyy");
                        mtd.customDateStr = "";
                        mtd.notifStatus   = NotifStatus::Active;
                        mtd.notified      = false;
                        // V5.4.3 — the occurrence that just arrived still owes
                        // a notification. Advancing happens here within a
                        // second of expiry, which left the notifier no window
                        // to see it, so a birthday arriving while this app was
                        // open was announced by nobody. Flagging it rather
                        // than notifying from here keeps ONE program doing the
                        // announcing, so the two can't both fire.
                        mtd.pendingLoopNotice = true;
                        tw->updateData(mtd);
                        anyExpired = true; // triggers a full tab rebuild so display refreshes
                    }
                } else if (mtd.isWindowDate()) {
                    // V5.4 — an announced window ("March 2027", "2026") has
                    // run out with no real date ever published. The window
                    // was only ever a countdown bound, so nothing released
                    // here — but what the tile should say next depends on
                    // whether this title has a past at all.
                    if (lapseWindowDate(mtd)) {
                        tw->updateData(mtd);
                        anyExpired = true;
                    }
                } else if (mtd.isEstimatedDate) {
                    // V5 — an estimated (unverified) episode date expired
                    // without ever being confirmed. This used to revert to
                    // the last verified episode and declare a Mid-Season
                    // Break immediately, which is backwards for a weekly
                    // show: one unconfirmed guess is not evidence of a
                    // hiatus, it's just TMDB not having filled the date in
                    // yet. Roll FORWARD another week instead, and let the
                    // 5-day rule below decide if this is a real break.
                    if (!mtd.unverifiedSince.isValid())
                        mtd.unverifiedSince = mtd.targetDate;   // start the clock at the first lapse

                    if (tryLocalEpisodeAdvance(mtd)) {
                        APPLOG(QString("onGlobalTick: '%1' estimate lapsed unverified — rolling forward to %2 on %3 (unverified since %4)")
                                   .arg(mtd.displayTitle(), mtd.statusLabel,
                                        mtd.targetDate.toString(Qt::ISODate),
                                        mtd.unverifiedSince.toString(Qt::ISODate)));
                    } else {
                        // Nothing left to roll into — the guess was for the
                        // season's final episode. The season really is over,
                        // so just let it sit as released. Not a break.
                        APPLOG(QString("onGlobalTick: '%1' estimate lapsed on the season finale — season complete").arg(mtd.displayTitle()));
                        mtd.isEstimatedDate = false;
                        mtd.unverifiedSince = QDate();
                        mtd.notifStatus     = NotifStatus::Inactive;   // still a guess — never notify off one
                    }
                    tw->updateData(mtd);
                    anyExpired = true;
                } else {
                    // v3.3.17 fix — try an instant refetch first; only
                    // fall back to the local bump if that doesn't produce
                    // anything useful. Games don't have an "episode"
                    // concept, so this only applies to TV.
                    if (mtd.mediaType == "tv" && mtd.tmdbId > 0) {
                        // Marks this Ready (not Active) so the outer
                        // isExpired() check above doesn't keep re-firing
                        // this same branch every second while we wait on
                        // the network — the pending set below is what
                        // actually tracks "still waiting on a refetch".
                        mtd.notifStatus = NotifStatus::Ready;
                        tw->updateData(mtd);
                        QString tileId = mtd.id;
                        m_pendingExpiryRefresh.insert(tileId);
                        // V5 — must use the same source as every other
                        // refresh path. Sending this to TMDB while the tile
                        // is TVmaze-backed was wiping tvmazeId (TMDB builds
                        // a fresh TileData that has none), so the tile fell
                        // back to local guessing and re-resolved from
                        // scratch on every launch.
                        if (useTvmazeForTv()) m_tvmaze->refreshTile(mtd);
                        else                  m_scraper->refreshTile(mtd);
                        APPLOG(QString("onGlobalTick: '%1' expired — attempting instant refetch before falling back to a local estimate").arg(mtd.displayTitle()));
                        // Safety net — refreshTile() emits nothing at all
                        // on a network failure (confirmed by reading its
                        // own error-handling), so without this a failed
                        // fetch would leave the tile stuck indefinitely
                        // instead of falling back to the local bump.
                        QTimer::singleShot(8000, this, [this, tileId]{
                            if (!m_pendingExpiryRefresh.remove(tileId)) return; // already resolved via onTileRefreshed
                            for (TileData& t : m_tiles) {
                                if (t.id != tileId) continue;
                                if (tryLocalEpisodeAdvance(t)) {
                                    APPLOG(QString("onGlobalTick: expiry refetch for '%1' timed out — falling back to local bump").arg(t.displayTitle()));
                                } else {
                                    t.notifStatus = NotifStatus::Ready;
                                }
                                for (TileWidget* w : std::as_const(m_tileWidgets))
                                    if (w->tileData().id == tileId) { w->updateData(t); break; }
                                break;
                            }
                            sortAndRebuildAllTabs();
                            saveTiles();
                            notifyTrayApp();
                        });
                    } else {
                        APPLOG(QString("onGlobalTick: tile '%1' just expired — moving to Released tab").arg(mtd.displayTitle()));
                        mtd.notifStatus = NotifStatus::Ready;
                        tw->updateData(mtd);
                    }
                    anyExpired = true;
                }
                break;
            }
        }
    }

    // V5 — the Mid-Season Break decision, now time-based rather than fired
    // off a single lapsed guess. A real air date reliably shows up at least
    // a couple of days before the episode; if 5 days have passed since the
    // first estimate lapsed and nothing has been verified in all that time,
    // the show genuinely is on a break. Runs outside the expiry loop above
    // because the tile being checked is mid-countdown toward its NEXT
    // rolled-forward guess, not expiring right now.
    for (TileData& mtd : m_tiles) {
        if (!mtd.isEstimatedDate || !mtd.unverifiedSince.isValid()) continue;
        if (mtd.unverifiedSince.daysTo(QDate::currentDate()) < 5) continue;

        APPLOG(QString("onGlobalTick: '%1' unverified since %2 (5+ days) — declaring Mid-Season Break")
                   .arg(mtd.displayTitle(), mtd.unverifiedSince.toString(Qt::ISODate)));
        mtd.targetDate  = mtd.lastVerifiedDate;
        mtd.dateDisplay = mtd.lastVerifiedDate.isValid()
            ? mtd.lastVerifiedDate.toString("MMMM d, yyyy") : mtd.dateDisplay;
        mtd.statusLabel      = mtd.lastVerifiedStatusLabel;
        mtd.isEstimatedDate  = false;
        mtd.inMidSeasonBreak = true;
        mtd.unverifiedSince  = QDate();          // decided — stop re-evaluating
        mtd.notifStatus      = NotifStatus::Inactive;   // no notification for a guess that didn't pan out

        for (TileWidget* w : std::as_const(m_tileWidgets))
            if (w->tileData().id == mtd.id) { w->updateData(mtd); break; }
        anyExpired = true;
    }

    if (anyExpired) {
        // Rebuild so tiles visually move between kinds (e.g. Countdowns → Released)
        sortAndRebuildAllTabs();
        saveTiles();
        notifyTrayApp();
    }
}

// =============================================================================
//  onTvmazeNeedsConfirmation — V5 migration prompt.
//
//  Existing tiles were created against TMDB and carry no TVmaze id, and
//  there is no id mapping between the two, so the match has to be made on
//  title. Where TVmaze's own relevance score and the premiere year agree,
//  that happens silently. Where they don't, this asks — because binding a
//  tile to the wrong series would quietly show wrong dates forever, which
//  is far worse than one dialog. (Hand-picking an id during testing landed
//  on "Gino's Italian Express" instead of "Reacher"; this is that failure
//  mode, made visible.)
// =============================================================================
void MainWindow::onTvmazeNeedsConfirmation(const QString& tileId,
                                            const QList<SearchResult>& candidates)
{
    TileData* target = nullptr;
    for (TileData& t : m_tiles)
        if (t.id == tileId) { target = &t; break; }
    if (!target || candidates.isEmpty()) return;

    QStringList labels;
    for (const SearchResult& c : candidates) {
        QString l = c.title;
        if (c.year > 0)            l += QString(" (%1)").arg(c.year);
        if (!c.castLine.isEmpty()) l += "  •  " + c.castLine;
        labels << l;
    }
    labels << "Skip — leave this tile on TMDB";

    bool ok = false;
    QString chosen = QInputDialog::getItem(
        this, "Match show to TVmaze",
        QString("Which show is \"%1\"?\n\nTVmaze doesn't share IDs with TMDB, so this\n"
                "has to be matched once by name.").arg(target->displayTitle()),
        labels, 0, false, &ok);
    if (!ok || chosen == labels.last()) return;

    int idx = labels.indexOf(chosen);
    if (idx < 0 || idx >= candidates.size()) return;

    target->tvmazeId = candidates[idx].id;
    APPLOG(QString("TVmaze migration: '%1' matched to '%2' (id %3)")
               .arg(target->displayTitle(), candidates[idx].title)
               .arg(candidates[idx].id));
    saveTiles();
    m_tvmaze->refreshTile(*target);
}

void MainWindow::onRefetchRequested(const QString& tileId)
{
    for (const TileData& td : std::as_const(m_tiles))
        if (td.id == tileId) {
            ++m_refreshPending;
            updateRefreshStatus();
            // v3.3.0 — route based on which API this tile's data came from.
            if (td.mediaType == "game") m_igdbScraper->refreshTile(td);
            else if (td.mediaType == "tv" && useTvmazeForTv()) m_tvmaze->refreshTile(td);   // V5
            else m_scraper->refreshTile(td);
            return;
        }
}

// v3.1.0 — "Refetch Image" button: forces a fresh backdrop download
// regardless of whether one is already present, and makes it the active image.
void MainWindow::onForceImageRefetchRequested(const QString& tileId)
{
    for (const TileData& td : std::as_const(m_tiles))
        if (td.id == tileId) {
            ++m_refreshPending;
            updateRefreshStatus();
            if (td.mediaType == "game") m_igdbScraper->refreshTile(td, /*forceCoverRefetch=*/true);
            else if (td.mediaType == "tv" && useTvmazeForTv())
                m_tvmaze->refreshTile(td, /*forceBackdropRefetch=*/true);   // V5
            else m_scraper->refreshTile(td, /*forceBackdropRefetch=*/true);
            return;
        }
}

// =============================================================================
//  onTestNotification — fires a Windows notification directly from the main
//  app via m_testTray (a hidden QSystemTrayIcon).
//
//  This deliberately bypasses the TrayApp IPC so we can independently verify:
//    (a) whether QSystemTrayIcon::showMessage works at all on this machine, and
//    (b) whether any failure is in the notification method vs. the IPC channel.
// =============================================================================
void MainWindow::onTestNotification(const QString& tileId)
{
    APPLOG(QString("onTestNotification: tileId=%1 — firing DIRECT notification from main app").arg(tileId));

    // Find the tile data
    const TileData* found = nullptr;
    for (const TileData& td : std::as_const(m_tiles)) {
        if (td.id == tileId) { found = &td; break; }
    }
    if (!found) {
        APPLOG("onTestNotification: tile not found in m_tiles!");
        m_statusLbl->setStyleSheet("color:#cc5555; font-size:11px;");
        m_statusLbl->setText("Test notification failed — tile not found.");
        return;
    }

    fireDirectNotification(*found);

    m_statusLbl->setStyleSheet("color:#66aa66; font-size:11px;");
    m_statusLbl->setText("Test notification fired directly from main app.");
}

// =============================================================================
//  fireDirectNotification — fires a QSystemTrayIcon::showMessage notification
//  from the main app's own hidden tray icon (m_testTray).
//  Same logic as TrayApp::sendNotification so results are directly comparable.
// =============================================================================
void MainWindow::fireDirectNotification(const TileData& td)
{
    // Strip any "  •  N Seasons" suffix from the title, if present.
    // v3.1.7 — new tiles no longer get this suffix at all (removed at the
    // source since it never displayed anywhere), but this stays as a
    // defensive no-op for tiles saved before that fix.
    // For notifications we only want the bare show/movie name.
    QString title = td.displayTitle();
    int bullet = title.indexOf(QChar(0x2022));  // Unicode bullet •
    if (bullet >= 0) title = title.left(bullet).trimmed();

    // Build the notification body — show name is already the toast title,
    // so the body only contains the episode/status info.
    //
    //  statusLabel   →  body
    //  ""            →  "Now available!"
    //  "Releases"    →  "Now available!"
    //  "Released"    →  "Now available!"
    //  "S02E01"      →  "S02E01 is out!"
    //  "Last Episode"→  "Last Episode is out!"
    //  "S02E01+E02"  →  "S02E01+E02 is out!"
    QString body;
    if (td.statusLabel.isEmpty()
        || td.statusLabel == "Releases"
        || td.statusLabel == "Released"
        || td.statusLabel == "No Release Date Yet")
    {
        body = "Now available!";
    } else {
        body = QString("%1 is out!").arg(td.statusLabel);
    }

    APPLOG(QString("fireDirectNotification: title='%1' body='%2'").arg(title, body));

    // Build icon from tile's backdrop image (same as TrayApp does)
    QIcon notifIcon;
    if (!td.imagePath.isEmpty() && QFile::exists(td.imagePath)) {
        QPixmap px(td.imagePath);
        if (!px.isNull()) {
            notifIcon = QIcon(px.scaled(256, 144, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            APPLOG("fireDirectNotification: using tile backdrop as icon");
        }
    }
    if (notifIcon.isNull()) {
        notifIcon = QApplication::style()->standardIcon(QStyle::SP_MediaPlay);
        APPLOG("fireDirectNotification: using default icon (no backdrop)");
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        APPLOG("fireDirectNotification: ERROR — system tray is NOT available on this system");
        m_statusLbl->setStyleSheet("color:#cc5555; font-size:11px;");
        m_statusLbl->setText("System tray not available — cannot show notification.");
        return;
    }
    if (!QSystemTrayIcon::supportsMessages()) {
        APPLOG("fireDirectNotification: ERROR — system tray does NOT support messages on this system");
        m_statusLbl->setStyleSheet("color:#cc5555; font-size:11px;");
        m_statusLbl->setText("System tray does not support notifications on this system.");
        return;
    }

    // Swap icon temporarily so it shows as the notification icon
    m_testTray->setIcon(notifIcon);
    // show() is required for showMessage() to work; we hide after the
    // notification duration so no persistent extra tray icon is visible.
    m_testTray->show();
    m_testTray->showMessage(title, body, notifIcon, 10000);
    APPLOG("fireDirectNotification: showMessage() called — notification should appear");

    // Hide and restore after notification timeout
    QTimer::singleShot(12000, this, [this]() {
        if (m_testTray) {
            m_testTray->hide();
            m_testTray->setIcon(
                QApplication::style()->standardIcon(QStyle::SP_MediaPlay));
        }
    });
}

// =============================================================================
//  onExportClicked — packages tiles.json + fetched_images + custom_images
//  into a zip and asks the user where to save it.
//
//  Uses PowerShell's Compress-Archive (Windows built-in, no extra deps).
// =============================================================================
void MainWindow::onExportClicked()
{
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    // Ask where to save
    QString dest = QFileDialog::getSaveFileName(
        this, "Export Tiles", QDir::homePath() + "/MediaCountdowns_export.zip",
        "Zip Archive (*.zip)");
    if (dest.isEmpty()) return;

    // Remove existing file at dest so PowerShell doesn't complain
    QFile::remove(dest);

    // Build list of items to include — tiles.json plus both image folders
    QStringList items;
    QString jsonPath = appData + "/tiles.json";
    if (QFile::exists(jsonPath))
        items << jsonPath;
    for (const QString& folder : {QString("fetched_images"), QString("custom_images")}) {
        QString p = appData + "/" + folder;
        if (QDir(p).exists()) items << p;
    }

    if (items.isEmpty()) {
        QMessageBox::information(this, "Export", "Nothing to export — no tiles found.");
        return;
    }

    // Build PowerShell command: Compress-Archive -Path a,b,c -DestinationPath dest
    QStringList psItems;
    for (const QString& it : items)
        psItems << "\"" + QDir::toNativeSeparators(it) + "\"";

    QString psCmd = QString(
        "Compress-Archive -Path %1 -DestinationPath \"%2\"")
        .arg(psItems.join(","))
        .arg(QDir::toNativeSeparators(dest));

    int ret = QProcess::execute("powershell",
        QStringList() << "-NoProfile" << "-Command" << psCmd);

    if (ret == 0 && QFile::exists(dest)) {
        m_statusLbl->setStyleSheet("color:#66bb66; font-size:11px;");
        m_statusLbl->setText(QString("Exported to: %1").arg(QFileInfo(dest).fileName()));
    } else {
        QMessageBox::warning(this, "Export Failed",
            "Could not create the zip file.\nMake sure PowerShell is available.");
    }
}

// =============================================================================
//  onImportClicked — merges tiles from a previously exported zip.
//
//  Rules:
//   • Tiles whose ID already exists in m_tiles are SKIPPED (no overwrite).
//   • Images are copied into the local fetched_images / custom_images folders.
//   • tiles.json from the zip is merged, not replaced.
// =============================================================================
void MainWindow::onImportClicked()
{
    QString zipPath = QFileDialog::getOpenFileName(
        this, "Import Tiles", QDir::homePath(),
        "Zip Archive (*.zip)");
    if (zipPath.isEmpty()) return;

    QString appData  = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString tempDir  = appData + "/import_temp";

    // Clean up any previous temp extract
    QDir(tempDir).removeRecursively();
    QDir().mkpath(tempDir);

    // Extract zip via PowerShell
    QString psCmd = QString(
        "Expand-Archive -Path \"%1\" -DestinationPath \"%2\" -Force")
        .arg(QDir::toNativeSeparators(zipPath))
        .arg(QDir::toNativeSeparators(tempDir));

    int ret = QProcess::execute("powershell",
        QStringList() << "-NoProfile" << "-Command" << psCmd);

    if (ret != 0) {
        QDir(tempDir).removeRecursively();
        QMessageBox::warning(this, "Import Failed",
            "Could not extract the zip file.");
        return;
    }

    // Find tiles.json inside extracted folder (may be nested one level)
    QString importedJson;
    for (const QString& candidate : {
        tempDir + "/tiles.json",
        tempDir + "/" + QFileInfo(appData).fileName() + "/tiles.json"
    }) {
        if (QFile::exists(candidate)) { importedJson = candidate; break; }
    }
    // Fallback: search recursively
    if (importedJson.isEmpty()) {
        QDirIterator it(tempDir, QStringList() << "tiles.json",
                        QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) importedJson = it.next();
    }

    if (importedJson.isEmpty()) {
        QDir(tempDir).removeRecursively();
        QMessageBox::warning(this, "Import Failed",
            "No tiles.json found inside the zip.");
        return;
    }

    // Parse the imported JSON
    QFile jf(importedJson);
    if (!jf.open(QIODevice::ReadOnly)) {
        QDir(tempDir).removeRecursively();
        QMessageBox::warning(this, "Import Failed", "Could not read tiles.json.");
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(jf.readAll());
    jf.close();
    if (!doc.isArray()) {
        QDir(tempDir).removeRecursively();
        QMessageBox::warning(this, "Import Failed", "tiles.json is not valid.");
        return;
    }

    // Build set of existing IDs for duplicate check
    QSet<QString> existingIds;
    for (const TileData& td : std::as_const(m_tiles))
        existingIds.insert(td.id);

    QString importedJsonDir = QFileInfo(importedJson).absolutePath();
    int added = 0, skipped = 0;

    for (const QJsonValue& v : doc.array()) {
        QJsonObject o = v.toObject();
        QString id = o["id"].toString();
        if (id.isEmpty() || existingIds.contains(id)) { ++skipped; continue; }

        // Copy image file if it exists in the extracted folder
        QString imgPath = o["imagePath"].toString();
        if (!imgPath.isEmpty()) {
            QString imgFilename = QFileInfo(imgPath).fileName();
            // Determine destination subfolder from original path
            QString subfolder = imgPath.contains("custom_images") ? "custom_images" : "fetched_images";
            QString destFolder = appData + "/" + subfolder;
            QDir().mkpath(destFolder);
            QString destPath = destFolder + "/" + imgFilename;

            // Try to find the image in extracted temp dir
            bool copied = false;
            for (const QString& sf : {QString("fetched_images"), QString("custom_images")}) {
                QString src = importedJsonDir + "/../" + sf + "/" + imgFilename;
                if (!QFile::exists(src))
                    src = importedJsonDir + "/" + sf + "/" + imgFilename;
                if (QFile::exists(src)) {
                    QFile::copy(src, destPath);
                    copied = true;
                    break;
                }
            }
            // Also try a flat search in tempDir
            if (!copied) {
                QDirIterator imgIt(tempDir, QStringList() << imgFilename,
                                   QDir::Files, QDirIterator::Subdirectories);
                if (imgIt.hasNext()) QFile::copy(imgIt.next(), destPath);
            }
            o["imagePath"] = destPath;
        }

        // Re-parse the (possibly patched) tile and add it.
        // V5 — was a hand-copied duplicate of JsonManager's loader that had
        // drifted out of sync, silently dropping an imported tile's loop
        // schedule, preset type, tag colour, favourite star and
        // predict-finale flag. Shares the one parser now.
        TileData td = JsonManager::tileFromJson(o);
        td.id = id;   // the de-duplicated id resolved above, not the file's

        m_tiles.append(td);
        existingIds.insert(id);
        createTileWidgetNoRebuild(td);
        ++added;
    }
    if (added > 0)
        sortAndRebuildAllTabs();   // v3.3.36 — once, after all imported widgets exist

    QDir(tempDir).removeRecursively();
    saveTiles();

    m_statusLbl->setStyleSheet("color:#66bb66; font-size:11px;");
    m_statusLbl->setText(QString("Import complete — %1 added, %2 skipped (duplicates).")
                         .arg(added).arg(skipped));

    if (added == 0)
        QMessageBox::information(this, "Import",
            QString("No new tiles were added.\n%1 tile(s) already existed.").arg(skipped));
}

void MainWindow::notifyTrayApp()
{
    QLocalSocket sock;
    sock.connectToServer("MediaCountdownsTray");
    if (sock.waitForConnected(300)) {
        sock.write("REFRESH\n");
        sock.waitForBytesWritten(300);
        sock.disconnectFromServer();
    }
}

void MainWindow::installSmoothScroll(QScrollArea* area)
{
    area->viewport()->installEventFilter(this);
}

void MainWindow::smoothScrollBy(QScrollArea* area, int pixelDelta)
{
    int tab = -1;
    for (int t = 0; t < NUM_KINDS; ++t)
        if (m_scrollAreas[t] == area) { tab = t; break; }
    if (tab < 0) return;

    QScrollBar* sb = area->verticalScrollBar();
    m_scrollTarget[tab] = qBound(sb->minimum(),
                                  m_scrollTarget[tab] + pixelDelta,
                                  sb->maximum());
    QVariantAnimation* anim = m_scrollAnim[tab];
    anim->stop();
    anim->setStartValue(sb->value());
    anim->setEndValue(m_scrollTarget[tab]);
    anim->start();
}

// =============================================================================
//  dedupeSharedImages — V5.4.26. One tile, one copy of its own picture.
//
//  Duplicating a tile used to copy the image PATHS, so both tiles pointed at
//  the same file on disk. V5.4.23 made a duplicate take its own copies, and
//  V5.4.25 taught deleteBackdropIfOwned() to refuse to remove a file another
//  tile still lists — but neither of those helps a pair created BEFORE them,
//  which shares a file to this day. That sharing is invisible until something
//  removes one of the two and the other goes black, which is exactly how the
//  bug was found ("Avengers: Doomsday went black").
//
//  Guarding each delete site is the weaker fix, because it is a list of call
//  sites — the pattern that has caused six bugs here already, and there is a
//  second one: onDeleteTiles() removes imagePath directly with QFile::remove.
//  So the sharing itself is undone instead. Any app-owned file listed by more
//  than one tile is copied, so every tile after the first owns its own, and
//  after one launch nothing shares anything.
//
//  A scan rather than a one-time migration with a flag, for the same reason
//  ReleaseHistory::sweep() is one: importing an old backup can reintroduce a
//  shared path at any time, and when nothing is shared this does nothing at
//  all. Files OUTSIDE the app's own image folders are left alone — nothing
//  ever deletes those, so two tiles pointing at the same picture in the user's
//  own Pictures folder is not a problem to fix.
// =============================================================================
static bool isAppOwnedImage(const QString& p)
{
    return p.contains("/fetched_images/") || p.contains("\\fetched_images\\")
        || p.contains("/custom_images/")  || p.contains("\\custom_images\\");
}

void MainWindow::dedupeSharedImages()
{
    QHash<QString, QString> owner;   // image file → the tile id that keeps it
    int copies = 0;

    for (TileData& td : m_tiles) {
        // Hands back the path this tile should be using: the original when it
        // is the first tile to claim the file, otherwise a copy of its own.
        auto claim = [&](QString& path) {
            if (path.isEmpty() || !isAppOwnedImage(path)) return;
            const QString key = QFileInfo(path).absoluteFilePath();
            auto it = owner.constFind(key);
            if (it == owner.constEnd()) { owner.insert(key, td.id); return; }
            if (*it == td.id) return;                  // the same tile listing it twice
            if (!QFile::exists(path)) return;          // nothing there to copy

            QFileInfo fi(path);
            const QString suffix = fi.suffix().isEmpty() ? QString() : "." + fi.suffix();
            const QString dest   = fi.absolutePath() + "/" + td.id + "_"
                                 + fi.completeBaseName() + suffix;
            // If the copy is somehow already there, take it; a failed copy
            // leaves the tile sharing rather than pointing at nothing.
            if (!QFile::exists(dest) && !QFile::copy(path, dest)) return;
            path = dest;
            owner.insert(QFileInfo(dest).absoluteFilePath(), td.id);
            ++copies;
        };

        const QString  wasActive  = td.imagePath;
        const QStringList oldCustoms = td.customImagePaths;
        QStringList customs = td.customImagePaths;
        for (QString& c : customs) claim(c);
        QString fetched = td.fetchedImagePath;
        claim(fetched);

        // Keep whatever was on screen pointing at this tile's own copy.
        QString active = wasActive;
        for (int i = 0; i < customs.size() && i < oldCustoms.size(); ++i)
            if (oldCustoms[i] == wasActive) active = customs[i];
        if (wasActive == td.fetchedImagePath) active = fetched;
        // A displayed path that is in neither list is a stray, but it is still
        // a file something could delete, so it gets claimed on its own.
        if (active == wasActive) claim(active);

        td.customImagePaths = customs;
        td.fetchedImagePath = fetched;
        td.imagePath        = active;
    }

    if (copies > 0) {
        APPLOG(QString("dedupeSharedImages: %1 image(s) were shared between tiles — "
                       "each tile now has its own copy").arg(copies));
        // Written straight through JsonManager: this runs during loadTiles(),
        // before the recap snapshot, and has nothing to tell the notifier.
        JsonManager::instance().saveTiles(m_tiles);
    }
}

// =============================================================================
//  retimeTilesForZone — V5.4.28. Make the Time Zone setting mean something
//  for TV shows.
//
//  A movie's time was always live: TmdbScraper deliberately leaves airTime
//  invalid and effectiveTime() asks TimeZoneUtil, so changing the setting moved
//  every movie immediately. TV was the opposite. TvmazeScraper converts
//  TVmaze's airstamp into wall-clock date and time using whatever zone was set
//  AT THAT MOMENT and stores the result, so changing the setting afterwards
//  changed nothing at all — a show fetched under Eastern kept reading 3 AM
//  after switching to Pacific, where it should read midnight.
//
//  Three reported symptoms, one cause. The Edit dialog showing an unchanged
//  air time, the tile countdown targeting the wrong instant, and the same show
//  recapping as "9 PM" one launch and "3 AM" the next — that last one is two
//  tiles fetched under different settings sitting side by side, because 9 PM
//  Pacific and midnight Eastern are the same instant, as are midnight Pacific
//  and 3 AM Eastern.
//
//  Recomputing from the stored instant rather than shifting the stored time by
//  the difference between two zones: shifting is arithmetic on a value that has
//  already been shifted once, so running it twice, or on a tile that never came
//  from an airstamp, silently corrupts. Recomputing from the instant is
//  idempotent — it can run any number of times and always lands on the same
//  answer.
//
//  Note the DATE moves too, not just the time. Midnight Eastern is 9 PM on the
//  PREVIOUS day in Pacific, so a tile can legitimately change which day it is
//  counting down to. That is correct, and it is why this writes dateDisplay as
//  well.
//
//  Returns how many tiles changed, for the status line.
// =============================================================================
int MainWindow::retimeTilesForZone()
{
    QTimeZone zone(TimeZoneUtil::currentZoneId().toUtf8());
    if (!zone.isValid()) return 0;

    int changed = 0;
    for (TileData& td : m_tiles) {
        // Only tiles whose time came from a published airstamp. Everything
        // else either has no stored time (movies, games, streaming shows with
        // no published slot) and so already follows the setting live, or is
        // not ours to move.
        if (!td.airStampUtc.isValid()) continue;
        // A window is a countdown bound, not a real day — nothing may derive
        // day-level precision from one (see TileData::isWindowDate).
        if (td.isWindowDate()) continue;
        // A relay correction owns this tile's air time; re-deriving here would
        // undo it on the next tick. ShowOverrides::apply() handles the zone for
        // corrections itself.
        if (!td.overrideKey.isEmpty()) continue;

        const QDateTime local = td.airStampUtc.toTimeZone(zone);
        if (!local.isValid()) continue;
        if (td.targetDate == local.date() && td.airTime == local.time()) continue;

        td.targetDate  = local.date();
        td.airTime     = local.time();
        td.dateDisplay = local.date().toString("MMMM d, yyyy");
        ++changed;
    }
    return changed;
}

void MainWindow::cleanupOrphanedFiles()
{
    QSet<QString> inUse;
    for (const TileData& td : std::as_const(m_tiles)) {
        if (!td.imagePath.isEmpty())        inUse.insert(QFileInfo(td.imagePath).fileName());
        if (!td.fetchedImagePath.isEmpty()) inUse.insert(QFileInfo(td.fetchedImagePath).fileName());
        for (const QString& p : td.customImagePaths)
            if (!p.isEmpty()) inUse.insert(QFileInfo(p).fileName());
    }

    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    for (const QString& folder : {QString("fetched_images"), QString("custom_images")}) {
        QDir dir(appData + "/" + folder);
        if (!dir.exists()) continue;
        for (const QString& file : dir.entryList(QDir::Files))
            if (!inUse.contains(file)) dir.remove(file);
    }
}

// =============================================================================
//  Debug window — Ctrl+Shift+D toggles a floating log window.
//  AppLogger::instance().log(msg) feeds into this window from anywhere in the app.
// =============================================================================
void MainWindow::setupDebugWindow()
{
    m_debugWindow = new QDialog(this,
        Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    m_debugWindow->setWindowTitle(
        QString("Debug Log — Media Countdowns  v%1").arg(MC_APP_VERSION));
    m_debugWindow->resize(860, 480);
    m_debugWindow->setStyleSheet("background:#111; color:#ccc;");

    auto* vlay = new QVBoxLayout(m_debugWindow);
    vlay->setContentsMargins(4, 4, 4, 4);

    m_debugLog = new QPlainTextEdit(m_debugWindow);
    m_debugLog->setReadOnly(true);
    m_debugLog->setMaximumBlockCount(2000);
    m_debugLog->setStyleSheet(
        "QPlainTextEdit { background:#0a0a0a; color:#b0ffb0; "
        "font-family:'Consolas','Courier New',monospace; font-size:11px; "
        "border:1px solid #222; }");
    vlay->addWidget(m_debugLog);

    auto* btnRow = new QHBoxLayout;
    auto* clearBtn = new QPushButton("Clear", m_debugWindow);
    clearBtn->setStyleSheet(
        "QPushButton { background:#222; color:#aaa; border:1px solid #444; "
        "border-radius:4px; padding:4px 14px; } "
        "QPushButton:hover { background:#333; }");
    connect(clearBtn, &QPushButton::clicked, m_debugLog, &QPlainTextEdit::clear);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch();
    auto* hintLbl = new QLabel("  Press  Ctrl+Shift+D  to toggle this window  ", m_debugWindow);
    hintLbl->setStyleSheet("color:#555; font-size:10px;");
    btnRow->addWidget(hintLbl);
    vlay->addLayout(btnRow);

    connect(&AppLogger::instance(), &AppLogger::newEntry,
            this, [this](const QString& entry) {
        if (m_debugLog) {
            m_debugLog->appendPlainText(entry);
            QTextCursor c = m_debugLog->textCursor();
            c.movePosition(QTextCursor::End);
            m_debugLog->setTextCursor(c);
        }
    });

    auto* sc = new QShortcut(QKeySequence("Ctrl+Shift+D"), this);
    sc->setContext(Qt::ApplicationShortcut);
    connect(sc, &QShortcut::activated, this, &MainWindow::showDebugWindow);

    auto* scApi = new QShortcut(QKeySequence("Ctrl+Shift+P"), this);
    scApi->setContext(Qt::ApplicationShortcut);
    connect(scApi, &QShortcut::activated, this, &MainWindow::showApiDialog);

    APPLOG("=== Debug log started ===");
    APPLOG(QString("App version: 5.0.0"));
    APPLOG(QString("Qt version: %1").arg(qVersion()));
}

void MainWindow::showDebugWindow()
{
    if (!m_debugWindow) return;
    if (m_debugWindow->isVisible()) {
        m_debugWindow->hide();
    } else {
        m_debugWindow->show();
        m_debugWindow->raise();
        m_debugWindow->activateWindow();
    }
}

// =============================================================================
// =============================================================================
//  showMissedReleases — V5.
//
//  The notifier process runs all the time and fires as things come out, but
//  the main app doesn't — so anything that released while it was closed is
//  simply never surfaced. This is the catch-up: one list, newest first, of
//  everything whose date has passed and hasn't been recapped before.
//
//  Dismissing it marks those tiles seenAfterPassed, so the same releases
//  never show up twice. When nothing was missed there's no dialog at all —
//  the check just passes silently and the update check carries on.
// =============================================================================
// V5 — the one place the startup refresh is triggered, so it can be called
// from whichever path finishes first without ever running twice. A recap
// left open would otherwise hold the refresh off indefinitely; the timeout
// below means the tiles still update while the dialog waits.
void MainWindow::runStartupRefresh()
{
    if (m_startupRefreshDone) return;
    m_startupRefreshDone = true;
    refreshAllTiles();
}

void MainWindow::captureMissedReleases()
{
    // Snapshotting has to happen BEFORE refreshAllTiles() runs, not when the
    // dialog is shown. A show is one tile: the moment the startup refresh
    // lands, a tile sitting on episode 2 (aired while the app was shut)
    // advances to episode 3 with a future date — and episode 2, the thing
    // actually missed, is gone from the tile entirely. So the list is taken
    // from the tiles as loaded from disk, then displayed a moment later.
    m_missedAtStartup.clear();

    // How far back counts as "while you were away". Without this the very
    // first run recaps everything ever added that happens to have a past
    // date — a 2004 birthday and a 2024 film are not things missed since
    // last launch. Older releases are still marked recapped on dismissal,
    // so they retire quietly instead of resurfacing later.
    static constexpr int kRecapWindowDays = 45;
    const QDate cutoff = QDate::currentDate().addDays(-kRecapWindowDays);

    for (const TileData& td : std::as_const(m_tiles)) {
        if (td.alreadyRecapped()) continue;
        if (!td.hasDate()) continue;
        // V5.4.26 — looped tiles are recapped like anything else now.
        //
        // They used to be skipped on the reasoning that a birthday "rolls
        // forward rather than releasing, so there's nothing to catch up on".
        // That was wrong from the user's side: the day happened, and the whole
        // point of this popup is to be told what happened while the app was
        // shut. The reason it took a change to fix rather than deleting a line
        // is that a looped tile is advanced within a second of its occurrence
        // arriving — by the notifier when the app is closed, which is exactly
        // when this popup matters — so by the time we look, the tile is
        // already showing next year. recapCandidateDate() is what catches the
        // occurrence before that change, the same way an episode is caught
        // before its successor overwrites it.
        const QDate when = td.recapCandidateDate();
        if (!td.recapCandidateArrived()) continue;
        // V5.4 — a month/year window that has lapsed has NOT released; it
        // moves to Other precisely because nobody ever said it came out.
        // Recapping it would announce a release on December 31 that no
        // source claimed, with a time attached to make it worse.
        if (td.isWindowDate()) continue;
        if (when < cutoff) continue;

        QString line = td.displayTitle();
        EpisodeLabel el = parseEpisodeLabel(td.statusLabel);
        if (el.valid) {
            QString ep = QString("S%1E%2").arg(el.season, 2, 10, QChar('0'))
                                          .arg(el.firstEpisode, 2, 10, QChar('0'));
            if (el.lastEpisode != el.firstEpisode)
                ep += QString("-E%1").arg(el.lastEpisode, 2, 10, QChar('0'));
            if (td.seasonEpisodeCount > 0)
                ep += QString("/%1").arg(td.seasonEpisodeCount, 2, 10, QChar('0'));
            line += " " + ep;
        }
        // Same bullet separator the tiles themselves use, so the recap reads
        // like part of the app rather than a different program. The time
        // comes from effectiveTime(), which is the same clock the countdown
        // targeted — a real broadcast slot when one was published, otherwise
        // the Time Zone default.
        //
        // V5.4.26 — written "@4:00PM" rather than "at 4:00 PM", which is the
        // form Recap/History uses too, so the two read the same way.
        const QString dot = "  \xe2\x80\xa2  ";
        line += dot + when.toString("MMMM d, yyyy")
              + " @" + td.effectiveTime().toString("h:mmAP");

        m_missedAtStartup.append({td.id, line, when,
                                  td.effectiveTime(), td.statusLabel});
    }

    std::sort(m_missedAtStartup.begin(), m_missedAtStartup.end(),
              [](const MissedRelease& a, const MissedRelease& b) {
        if (a.date != b.date) return a.date > b.date;
        return a.time > b.time;                 // same day: latest first
    });
}

bool MainWindow::showMissedReleases()
{
    const QList<MissedRelease>& missed = m_missedAtStartup;
    if (missed.isEmpty()) return false;   // nothing missed — check passes silently

    QString body;
    for (const MissedRelease& m : missed)
        body += m.line + "\n";

    APPLOG(QString("showMissedReleases: %1 release(s) happened while the app was closed").arg(missed.size()));

    auto* box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle("While You Were Away");
    box->setIcon(QMessageBox::Information);
    box->setText(QString("<b>%1 release%2 since you last opened the app</b>")
                     .arg(missed.size()).arg(missed.size() == 1 ? "" : "s"));
    box->setInformativeText(body.trimmed());
    box->setStandardButtons(QMessageBox::Ok);
    // V5 — QMessageBox sizes itself from its own heuristics and happily
    // wraps a line mid-entry, which put "at 12:00 PM" on its own row and
    // looked broken. Measure the longest line in the app's real font and
    // force the dialog at least that wide, so every entry stays on one
    // line however long the show's name is. Capped so a pathological title
    // can't produce a dialog wider than the screen — that one may wrap.
    int widest = 0;
    {
        QFontMetrics fm(box->font());
        for (const MissedRelease& m : missed)
            widest = qMax(widest, fm.horizontalAdvance(m.line));
    }
    const int screenLimit = screen() ? int(screen()->availableGeometry().width() * 0.9) : 1400;
    const int wanted = qMin(widest + 120, screenLimit);   // + icon, margins, padding

    box->setStyleSheet(
        "QMessageBox { background:#1e1e1e; }"
        "QLabel { color:#ddd; font-size:13px; }"
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:4px; padding:6px 20px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }");

    // QMessageBox ignores setFixedWidth/resize on its own, but its layout
    // honours a zero-height spacer in the grid's last row — the documented
    // way to widen one.
    if (auto* grid = qobject_cast<QGridLayout*>(box->layout())) {
        auto* spacer = new QSpacerItem(wanted, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
        grid->addItem(spacer, grid->rowCount(), 0, 1, grid->columnCount());
    }

    connect(box, &QMessageBox::finished, this, [this]{
        // The check completes only once it's actually been seen. Every
        // expired release is marked — including ones older than the display
        // window — so nothing that was skipped for being ancient can
        // resurface later. Recording the exact date+label (rather than a
        // flag) is what lets the SAME tile be recapped again for its next
        // episode.
        // Stamp what was SHOWN, from the snapshot — not the tile's current
        // state. The startup refresh runs while this dialog is open, so a
        // tile can already have advanced; stamping its new state would mark
        // a release as seen that was never displayed, and that release
        // would then never be reported.
        QHash<QString, const MissedRelease*> shownFor;
        for (const MissedRelease& m : std::as_const(m_missedAtStartup))
            shownFor.insert(m.tileId, &m);

        for (TileData& td : m_tiles) {
            auto it = shownFor.constFind(td.id);
            if (it != shownFor.constEnd()) {
                td.recappedDate  = (*it)->date;
                td.recappedLabel = (*it)->label;
                continue;
            }
            // Anything expired but not shown (older than the display window)
            // is retired quietly so it can't surface later.
            if (!td.hasDate() || !td.recapCandidateArrived()) continue;
            if (!td.alreadyRecapped()) {
                td.recappedDate  = td.recapCandidateDate();
                td.recappedLabel = td.statusLabel;
            }
        }
        m_missedAtStartup.clear();
        saveTiles();
        // Now that the recap has been seen: update prompt, then the refresh
        // that will move these tiles on to their next episodes.
        checkForUpdates();
        runStartupRefresh();
    });
    box->open();   // non-modal, so startup isn't blocked waiting on it
    return true;
}

// =============================================================================
//  showFeedbackDialog — V5.4.3. A box to type in, and a Send button.
//
//  One function for both kinds, because the only differences are the words:
//  the same box, the same limit, the same send path. Two copies would drift.
//
//  The result is always reported back. Someone who wrote three paragraphs
//  about a bug deserves to know if it failed to send rather than assuming it
//  arrived, so a failure keeps the dialog open with the text intact — closing
//  it and losing what they wrote would be the worst possible response.
// =============================================================================
void MainWindow::showFeedbackDialog(QWidget* parent, bool isBug)
{
    auto* dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(isBug ? "Report a Problem" : "Share an Idea");
    dlg->setModal(true);
    dlg->setStyleSheet("QDialog { background:#1e1e1e; }");
    dlg->setMinimumWidth(460);

    auto* lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(18, 18, 18, 18);
    lay->setSpacing(10);

    auto* title = new QLabel(isBug ? "What went wrong?" : "What would you like to see?", dlg);
    title->setStyleSheet("color:#eee; font-size:15px; font-weight:bold; background:transparent;");
    lay->addWidget(title);

    auto* hint = new QLabel(
        isBug ? "What were you doing, and what happened instead? Which tile it "
                "involved helps too, if it was a particular one."
              : "Describe it however you like — rough ideas are welcome.", dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#888; font-size:12px; background:transparent;");
    lay->addWidget(hint);

    auto* edit = new QPlainTextEdit(dlg);
    edit->setStyleSheet(
        "QPlainTextEdit { background:#141414; color:#eee; border:1px solid #3a3a3a; "
        "border-radius:5px; padding:8px; font-size:13px; }");
    edit->setMinimumHeight(170);
    lay->addWidget(edit);

    auto* status = new QLabel(dlg);
    status->setWordWrap(true);
    status->setStyleSheet("font-size:11px; background:transparent;");
    lay->addWidget(status);

    auto* row = new QHBoxLayout;
    auto* counter = new QLabel(dlg);
    counter->setStyleSheet("color:#666; font-size:11px; background:transparent;");
    row->addWidget(counter);
    row->addStretch();

    auto* cancel = new QPushButton("Cancel", dlg);
    cancel->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#bbb; border:1px solid #444; "
        "border-radius:4px; padding:7px 16px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }");
    auto* send = new QPushButton("Send", dlg);
    send->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; border-radius:4px; "
        "padding:7px 20px; font-size:13px; font-weight:bold; }"
        "QPushButton:hover { background:#1a8de4; }"
        "QPushButton:disabled { background:#333; color:#777; }");
    row->addWidget(cancel);
    row->addWidget(send);
    lay->addLayout(row);

    // Nothing to send until something is typed, and the count only appears
    // once it's worth worrying about.
    auto syncCounter = [edit, counter, send] {
        int n = edit->toPlainText().trimmed().size();
        send->setEnabled(n > 0 && n <= Feedback::kMaxLength);
        if (n > Feedback::kMaxLength * 3 / 4)
            counter->setText(QString("%1 / %2").arg(n).arg(Feedback::kMaxLength));
        else
            counter->clear();
    };
    syncCounter();
    connect(edit, &QPlainTextEdit::textChanged, dlg, syncCounter);
    connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);

    connect(send, &QPushButton::clicked, dlg, [=] {
        send->setEnabled(false);
        send->setText("Sending…");
        status->setStyleSheet("color:#888; font-size:11px; background:transparent;");
        status->setText("Sending…");

        QPointer<QDialog> guard(dlg);
        Feedback::instance().send(
            isBug ? Feedback::Kind::Bug : Feedback::Kind::Request,
            edit->toPlainText(), MC_APP_VERSION,
            [=](bool ok, const QString& message) {
                if (!guard) return;             // dialog closed while in flight
                if (ok) {
                    APPLOG(QString("Feedback sent (%1)").arg(isBug ? "bug" : "request"));
                    m_statusLbl->setStyleSheet("color:#66aa66; font-size:11px;");
                    m_statusLbl->setText(isBug ? "Bug report sent — thank you."
                                               : "Idea sent — thank you.");
                    guard->accept();
                    return;
                }
                // Keep the dialog and the text; let them fix it or retry.
                status->setStyleSheet("color:#cc5555; font-size:11px; background:transparent;");
                status->setText(message);
                send->setText("Send");
                send->setEnabled(true);
            });
    });

    dlg->exec();
}

// =============================================================================
//  ensureRegisteredWithRelay — V5.4.8. Collect an installation id, once ever.
//
//  The relay issues and signs these now, so the app can't just invent one:
//  an unsigned id is still served, it simply never counts as an installation.
//  That is what fixes a dashboard reading 22 installations when one person
//  had ever run the app — every throwaway X-Client-ID used while debugging
//  had been registering as a new one.
//
//  Runs only when no ISSUED id is stored yet, which after a successful
//  registration is never. V5.4.9 — it used to run only when nothing at all was
//  stored, which meant it never ran on any machine that had used the app
//  before V5.4.8: those still had the old self-made UUID, which the relay
//  cannot verify, so the count stayed at zero and every request logged as an
//  unregistered client. Failure is silent and harmless: requests still work,
//  this simply tries again next launch rather than nagging about a number the
//  user has no reason to care about.
// =============================================================================
void MainWindow::ensureRegisteredWithRelay()
{
    if (!RelayConfig::isConfigured()) return;            // V5.4.26 — no relay to register with
    if (!RelayConfig::relayCheckboxValue()) return;      // offline mode — nothing to ask
    if (!RelayConfig::needsRegistration()) return;

    auto* nam = new QNetworkAccessManager(this);
    QNetworkRequest req{QUrl(RelayConfig::baseUrl() + "/register")};
    req.setRawHeader("Authorization", ("Bearer " + RelayConfig::sharedSecret()).toUtf8());
    req.setRawHeader("Content-Type", "application/json");

    QNetworkReply* r = nam->post(req, QByteArray("{}"));
    connect(r, &QNetworkReply::finished, this, [r, nam]() {
        r->deleteLater();
        nam->deleteLater();
        if (r->error() != QNetworkReply::NoError) return;
        QString id = QJsonDocument::fromJson(r->readAll()).object()["client_id"].toString();
        if (id.isEmpty()) return;
        RelayConfig::setInstallationId(id);
        APPLOG("Registered with the relay and stored this installation's id");

        // The relay only counts an installation once a real request arrives
        // carrying the new id — deliberately, so an id handed out and never
        // used isn't recorded as an installation that never existed. But
        // startup fires its requests while this one is still in flight, so
        // they all go out under the old id and the whole first session goes
        // uncounted. Asking for the corrections again is the app's own
        // ordinary request, costs a few hundred bytes, and means the first
        // launch counts as the first launch.
        ShowOverrides::instance().refresh();
    });
}

// =============================================================================
//  Where a build comes from.
//
//  The app's page on teamwolfgamers.online. Its "Latest Version" and Download
//  button are filled in from the installer's file name every time the website
//  is published ("push to website.bat" -> NAS-Website\update-versions.ps1), so
//  a build exists the instant it is on the website, with no separate step to
//  remember. The duckdns name is asked second, in case the domain ever lapses.
//
//  Until V5.4.28 this asked the public GitHub releases repo, which no longer
//  exists — the check had been 404ing, silently by design, ever since.
//
//  Nothing here is authenticated: the page is public, like the download.
// =============================================================================
static const char* kUpdatePages[] = {
    "https://teamwolfgamers.online/media-countdowns",
    "https://pattywatty.duckdns.org/media-countdowns",
};
static const int kUpdatePageCount = int(sizeof(kUpdatePages) / sizeof(kUpdatePages[0]));

void MainWindow::checkForUpdates()
{
    // Offline mode — nothing to check, so the message of the day is free to
    // go as soon as the tiles are in.
    if (!RelayConfig::relayCheckboxValue()) { m_updateCheckDone = true; maybeCheckMotd(); return; }

    static const QString kCurrentVersion = MC_APP_VERSION;

    // What the page offered — empty if no address answered with a version —
    // and where its Download button points.
    auto decide = [this](QString latest, const QString& download) {
        // Each of these ends the update check with no prompt, so each one
        // releases the message of the day on its way out.
        auto finished = [this]() { m_updateCheckDone = true; maybeCheckMotd(); };

        if (latest.isEmpty()) { finished(); return; }   // unreachable — fail silently, no nagging

        // The page's version, however it was written: "V5.4.29", "5.4.29",
        // "V5.4". Take the first number with two or three parts and ignore the
        // rest, so a version with a name on the end still compares.
        static const QRegularExpression verRe(R"((\d+)\.(\d+)(?:\.(\d+))?)");
        QRegularExpressionMatch m = verRe.match(latest);
        latest = m.hasMatch()
                     ? QString("%1.%2.%3").arg(m.captured(1), m.captured(2),
                                               m.captured(3).isEmpty() ? "0" : m.captured(3))
                     : QString();
        if (latest.isEmpty()) { finished(); return; }

        auto parseVer = [](const QString& v) -> QList<int> {
            QList<int> parts;
            for (const QString& p : v.split('.')) parts << p.toInt();
            while (parts.size() < 3) parts << 0;
            return parts;
        };
        // Newer, not merely different. "Different" would be the literal reading
        // of the rule, but the one person who routinely runs a version the
        // website has never heard of is whoever is building it — every
        // local build would prompt to "update" to an older release. A user can
        // only ever be behind, so for them the two readings are the same thing.
        if (parseVer(latest) <= parseVer(kCurrentVersion)) { finished(); return; }   // already up to date

        QSettings settings("HijackAssassin", "MediaCountdowns");
        QStringList skipped = settings.value("skippedUpdates").toStringList();
        if (skipped.contains(latest)) { finished(); return; }

        auto* dlg = new QDialog(this, Qt::Dialog);
        dlg->setWindowTitle("Update Available");
        // Width is set AFTER the buttons exist, from what they actually need —
        // see the note below the button row. It used to be a fixed 400px.
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setStyleSheet("QDialog { background:#1e1e1e; }");

        auto* vlay = new QVBoxLayout(dlg);
        vlay->setContentsMargins(24, 24, 24, 24);
        vlay->setSpacing(12);

        auto* title = new QLabel(QString("A new version is available: <b>%1</b>").arg(latest), dlg);
        title->setStyleSheet("font-size:14px; color:#ffffff; background:transparent;");
        title->setWordWrap(true);
        vlay->addWidget(title);

        auto* sub = new QLabel(QString("You're currently on %1.").arg(kCurrentVersion), dlg);
        sub->setStyleSheet("font-size:12px; color:#aaaaaa; background:transparent;");
        vlay->addWidget(sub);

        auto* btnRow = new QHBoxLayout;
        btnRow->setSpacing(8);

        auto* okBtn = new QPushButton("Download", dlg);
        okBtn->setStyleSheet(
            "QPushButton { background:#0078d4; color:#fff; border:none; "
            "border-radius:4px; padding:8px 20px; font-size:13px; font-weight:bold; }"
            "QPushButton:hover { background:#1a8de4; }");
        // The installer itself (the page's Download button), straight to the browser.
        connect(okBtn, &QPushButton::clicked, dlg, [dlg, download]() {
            QDesktopServices::openUrl(QUrl(download));
            dlg->accept();
        });

        auto* laterBtn = new QPushButton("Later", dlg);
        laterBtn->setStyleSheet(
            "QPushButton { background:#2a2a2a; color:#aaa; border:1px solid #444; "
            "border-radius:4px; padding:8px 20px; font-size:13px; }"
            "QPushButton:hover { background:#333; color:#fff; }");
        connect(laterBtn, &QPushButton::clicked, dlg, &QDialog::accept);

        auto* skipBtn = new QPushButton("Don't tell me again for this version", dlg);
        skipBtn->setStyleSheet(
            "QPushButton { background:#2a2a2a; color:#aaa; border:1px solid #444; "
            "border-radius:4px; padding:8px 20px; font-size:13px; }"
            "QPushButton:hover { background:#333; color:#fff; }");
        connect(skipBtn, &QPushButton::clicked, dlg, [dlg, latest]() {
            QSettings settings("HijackAssassin", "MediaCountdowns");
            QStringList skipped = settings.value("skippedUpdates").toStringList();
            if (!skipped.contains(latest)) {
                skipped << latest;
                settings.setValue("skippedUpdates", skipped);
            }
            dlg->reject();
        });

        btnRow->addWidget(okBtn);
        btnRow->addWidget(laterBtn);
        btnRow->addWidget(skipBtn);
        btnRow->addStretch();
        vlay->addLayout(btnRow);

        // V5.4.27 — the dialog is sized to its buttons, instead of the buttons
        // being crushed to fit the dialog.
        //
        // Three buttons on one row, one of them a whole sentence, need roughly
        // 530px. The dialog was pinned at 400, and Qt resolves that by eliding
        // the labels — so the third button read as "Don't tell me again f…"
        // and there was genuinely no way to tell what it did.
        //
        // Measured rather than replaced with a bigger number, because a bigger
        // number is the same bug waiting on a font change, a display scale or a
        // longer version string. This is the approach showMissedReleases()
        // already uses for its longest line: ask the widgets what they need.
        //
        // ensurePolished() first — a button's sizeHint only accounts for the
        // stylesheet padding set above once the style has actually been applied
        // to it, and without this the hints come back too small.
        for (QPushButton* b : {okBtn, laterBtn, skipBtn}) b->ensurePolished();
        const QMargins mg = vlay->contentsMargins();
        const int needed = mg.left() + mg.right()
                         + okBtn->sizeHint().width()
                         + laterBtn->sizeHint().width()
                         + skipBtn->sizeHint().width()
                         + btnRow->spacing() * 2;
        // Minimum, not fixed: the title label may want more than the buttons do
        // for a long version string, and it should be allowed to have it.
        // Capped so a pathological font can't produce a dialog wider than the
        // screen — at that point the labels would wrap rather than be cut.
        const int screenLimit = screen() ? int(screen()->availableGeometry().width() * 0.9) : 1400;
        dlg->setMinimumWidth(qMin(qMax(400, needed), screenLimit));

        // Blocks here until the prompt is dismissed, which is exactly the
        // point: the message of the day is not allowed out until it is.
        dlg->exec();
        finished();
    };

    // Each address in turn; the first page that carries a version decides.
    // `self` is this lambda, so a failed address can hand over to the next.
    auto* nam = new QNetworkAccessManager(this);
    auto ask = [this, nam, decide](auto self, int page) -> void {
        QNetworkRequest req{QUrl(kUpdatePages[page])};
        req.setRawHeader("User-Agent", QByteArray("MediaCountdowns/") + MC_APP_VERSION);
        req.setTransferTimeout(15000);
        QNetworkReply* reply = nam->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, nam, decide, self, page]() {
            reply->deleteLater();
            QString latest, download;
            if (reply->error() == QNetworkReply::NoError) {
                const QString html = QString::fromUtf8(reply->readAll());
                static const QRegularExpression versionRe(
                    R"re(data-version="media-countdowns">\s*V?([^<\s]+)\s*<)re");
                static const QRegularExpression downloadRe(
                    R"re(data-download="media-countdowns" href="([^"]+)")re");
                const QRegularExpressionMatch v = versionRe.match(html);
                if (v.hasMatch()) {
                    latest = v.captured(1);
                    const QRegularExpressionMatch d = downloadRe.match(html);
                    // A page with no file yet links the bare downloads folder: offer the page instead.
                    download = d.hasMatch() && !d.captured(1).endsWith("downloads/")
                                   ? reply->url().resolved(QUrl(d.captured(1))).toString()
                                   : reply->url().toString();
                }
            }
            if (latest.isEmpty() && page + 1 < kUpdatePageCount) { self(self, page + 1); return; }
            nam->deleteLater();
            decide(latest, download);
        });
    };
    ask(ask, 0);
}

// =============================================================================
//  maybeCheckMotd — V5.4.13. The message of the day goes last, deliberately.
//
//  The startup order is: check for an update, load the tiles, then show the
//  message. The first two run at the same time — the update check is a single
//  request while the refresh is dozens, so making them literally sequential
//  would just make startup slower for no gain — but the MESSAGE waits for
//  both. Whichever finishes second calls this and it fires; before that it
//  does nothing. That is what stops the message landing on top of the update
//  prompt, which is the thing a fixed delay was always failing to guarantee.
//
//  "The update check finished" includes every way it can end without a prompt:
//  offline, unreachable, no answer, already up to date, or a version the user
//  has skipped. Otherwise a launch with no update available would never show
//  a message at all.
// =============================================================================
void MainWindow::maybeCheckMotd()
{
    if (m_motdShown) return;                       // once per launch
    if (!m_updateCheckDone || !m_tilesLoaded) return;
    m_motdShown = true;
    checkMotd();
}

// =============================================================================
//  checkMotd — V4.5. Same shape as checkForUpdates() (unauthenticated GET,
//  gated on the Media Countdowns Server connection, fails silently if
//  unreachable), but for the admin-settable "message of the day" instead.
//  Tracks the last message actually shown (not just fetched) so the same
//  message doesn't nag on every single launch — only a genuinely new or
//  changed one does.
// =============================================================================
void MainWindow::checkMotd()
{
    if (!RelayConfig::isConfigured()) return;   // V5.4.26 — no relay, no message
    if (!RelayConfig::relayCheckboxValue()) return;

    auto* nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(RelayConfig::baseUrl() + "/motd"));
    QNetworkReply* reply = nam->get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply, nam]() {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;

        QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        QString message = obj["message"].toString().trimmed();
        if (message.isEmpty()) return;

        QSettings settings("HijackAssassin", "MediaCountdowns");
        if (settings.value("lastSeenMotd").toString() == message) return;

        showMotdDialog(message);
    });
}

// =============================================================================
//  showMotdDialog — V5.4.12. Puts the message on screen. Nothing clever.
//
//  Two previous versions of this were both more complicated than the job: a
//  flat 4-second timer every launch (V4.5), then a re-check every second until
//  no modal dialog was open (V5.4.10). The answer is simply to call it once,
//  when the tiles have finished loading — the recap and the update prompt are
//  done by then, so there is nothing to wait for and nothing to poll.
// =============================================================================
void MainWindow::showMotdDialog(const QString& message)
{
    auto* dlg = new QDialog(this, Qt::Dialog);
    dlg->setWindowTitle("Message of the day");
    dlg->setFixedWidth(400);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setStyleSheet("QDialog { background:#1e1e1e; }");

    auto* vlay = new QVBoxLayout(dlg);
    vlay->setContentsMargins(24, 24, 24, 24);
    vlay->setSpacing(12);

    auto* msgLbl = new QLabel(message, dlg);
    msgLbl->setStyleSheet("font-size:13px; color:#eee; background:transparent;");
    msgLbl->setWordWrap(true);
    vlay->addWidget(msgLbl);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto* okBtn = new QPushButton("OK", dlg);
    okBtn->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; "
        "border-radius:4px; padding:8px 24px; font-size:13px; font-weight:bold; }"
        "QPushButton:hover { background:#1a8de4; }");
    connect(okBtn, &QPushButton::clicked, dlg, [dlg, message]() {
        QSettings settings("HijackAssassin", "MediaCountdowns");
        settings.setValue("lastSeenMotd", message);
        dlg->accept();
    });
    btnRow->addWidget(okBtn);
    vlay->addLayout(btnRow);

    dlg->exec();
}


// =============================================================================
//  showRelayKeyDialog — V5.4.18. The server URL and its key, for a build that
//  didn't ship with one.
//
//  Both values have been readable from QSettings since V4 (see RelayConfig),
//  but nothing ever let a user set them — relayconfig.h claimed they were
//  "user-overridable via Settings" and that simply wasn't true. This is that
//  UI, and it matters most for a build made from the public source, where both
//  slots ship empty on purpose: without it, such a build could only reach a
//  relay by editing the source and recompiling.
//
//  The key is shown as typed rather than masked. It is a shared secret for a
//  service the person is choosing to point at, usually their own, and hiding
//  it would only make a typo harder to spot.
// =============================================================================
void MainWindow::showRelayKeyDialog()
{
    QSettings settings("HijackAssassin", "MediaCountdowns");

    auto* dlg = new QDialog(this, Qt::Dialog);
    dlg->setWindowTitle("Relay Key");
    dlg->setFixedWidth(460);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setModal(true);
    dlg->setStyleSheet("QDialog { background:#1e1e1e; }");

    auto* vlay = new QVBoxLayout(dlg);
    vlay->setContentsMargins(20, 20, 20, 20);
    vlay->setSpacing(10);

    auto* intro = new QLabel(
        "Point this copy at your own Media Countdowns relay. Leave both blank "
        "to use whatever this build was made with.", dlg);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#888; font-size:11px; background:transparent;");
    vlay->addWidget(intro);

    const char* kFieldStyle =
        "QLineEdit { background:#1e1e1e; color:#fff; border:1px solid #3a3a3a; "
        "border-radius:4px; padding:8px 10px; font-size:13px; font-family:monospace; }"
        "QLineEdit:focus { border-color:#0078d4; }";

    auto* urlLbl = new QLabel("Server URL:", dlg);
    urlLbl->setStyleSheet("color:#888; font-size:11px; margin-top:6px; background:transparent;");
    vlay->addWidget(urlLbl);
    auto* urlEdit = new QLineEdit(dlg);
    urlEdit->setStyleSheet(kFieldStyle);
    // Deliberately NOT the built-in URL: a placeholder is visible to anyone
    // who opens Settings, and this box exists precisely so somebody can point
    // the app at their own server. Showing where THIS copy connects is not
    // information the box needs to do its job.
    urlEdit->setPlaceholderText("https://your-server-address");
    urlEdit->setText(settings.value("relayBaseUrl").toString());
    vlay->addWidget(urlEdit);

    // V5.4.26 — spell out that a domain is only one of the options.
    //
    // The placeholder above reads like a public hostname is required, and the
    // setup notes lead with DuckDNS, so it looked as though running your own
    // relay meant registering a domain and forwarding a port. It doesn't: the
    // common cases are the relay on this same machine, or on another machine in
    // the house. Those need no domain, no certificate and nothing exposed to
    // the internet, and they are what most people should use. Anything a URL
    // can express works here — the app only ever concatenates a path onto it.
    // Rich text rather than spaces-as-columns: this label is in the app's
    // proportional UI font, where padded columns come out ragged. A tiny table
    // lines them up whatever the font metrics turn out to be.
    auto* urlHint = new QLabel(
        "A domain is only needed to reach the server over the internet."
        "<table style='margin-top:4px;' cellpadding='0' cellspacing='0'>"
        "<tr><td>On this machine&nbsp;&nbsp;&nbsp;</td>"
            "<td><code>http://localhost:8080</code></td></tr>"
        "<tr><td>On your network&nbsp;&nbsp;&nbsp;</td>"
            "<td><code>http://192.168.1.20:8080</code></td></tr>"
        "<tr><td>Over the internet&nbsp;&nbsp;&nbsp;</td>"
            "<td><code>https://your-name.duckdns.org</code></td></tr>"
        "</table>", dlg);
    urlHint->setTextFormat(Qt::RichText);
    urlHint->setWordWrap(true);
    urlHint->setStyleSheet("color:#777; font-size:11px; background:transparent;");
    vlay->addWidget(urlHint);

    auto* keyLbl = new QLabel("Relay Key:", dlg);
    keyLbl->setStyleSheet("color:#888; font-size:11px; margin-top:6px; background:transparent;");
    vlay->addWidget(keyLbl);
    auto* keyEdit = new QLineEdit(dlg);
    keyEdit->setStyleSheet(kFieldStyle);
    keyEdit->setPlaceholderText("Paste your relay key here…");
    keyEdit->setText(settings.value("relaySharedSecret").toString());
    vlay->addWidget(keyEdit);

    auto* note = new QLabel("Changes take effect on the next refresh.", dlg);
    note->setWordWrap(true);
    note->setStyleSheet("color:#777; font-size:11px; margin-top:4px; background:transparent;");
    vlay->addWidget(note);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    auto* resetBtn = new QPushButton("Reset to Default", dlg);
    resetBtn->setStyleSheet(kPickerGhostBtnStyle);
    connect(resetBtn, &QPushButton::clicked, dlg, [urlEdit, keyEdit]() {
        urlEdit->clear();
        keyEdit->clear();
    });
    btnRow->addWidget(resetBtn);
    btnRow->addStretch();
    auto* cancelBtn = new QPushButton("Cancel", dlg);
    cancelBtn->setStyleSheet(kPickerGhostBtnStyle);
    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::reject);
    auto* saveBtn = new QPushButton("Save", dlg);
    saveBtn->setStyleSheet(kPickerSaveBtnStyle);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    vlay->addLayout(btnRow);

    connect(saveBtn, &QPushButton::clicked, dlg, [dlg, urlEdit, keyEdit]() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        const QString url = urlEdit->text().trimmed();
        const QString key = keyEdit->text().trimmed();
        // Empty means "fall back to whatever this build ships with", which is
        // exactly what removing the setting does — storing "" would be a
        // deliberate empty key, which is a different thing.
        if (url.isEmpty()) s.remove("relayBaseUrl");   else s.setValue("relayBaseUrl", url);
        if (key.isEmpty()) s.remove("relaySharedSecret"); else s.setValue("relaySharedSecret", key);
        APPLOG("Relay settings changed — new requests will use them");
        dlg->accept();
    });

    dlg->exec();
}


// =============================================================================
//  showRecapDialog — V5.4.22. Everything that has come out, newest first.
//
//  One year per page, chosen from the dropdown in the corner, because a list
//  of several years is a scroll rather than a read. The dropdown only becomes
//  useful once there is more than one year to choose between, so with a single
//  year it is disabled rather than hidden — a control that appears out of
//  nowhere the first time a year rolls over is more surprising than one that
//  was always there.
//
//  The "/E08" total follows the Tile Display setting for Total Episodes, and
//  nothing else here is conditional: this is a record of what happened, not a
//  second place to configure how tiles look.
// =============================================================================
void MainWindow::showRecapDialog()
{
    // Catch anything that has released since the last save before showing the
    // list, so opening this straight after a release doesn't look stale.
    ReleaseHistory::instance().sweep(m_tiles);

    auto* dlg = new QDialog(this, Qt::Dialog);
    // "Recap/History" so it is not mistaken for the "While You Were Away"
    // popup that appears at startup. That one reports what was missed since the
    // app was last open; this is the permanent record of everything.
    dlg->setWindowTitle("Recap/History");
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setStyleSheet("QDialog { background:#1a1a1a; }");
    dlg->resize(560, 640);

    auto* vlay = new QVBoxLayout(dlg);
    vlay->setContentsMargins(20, 18, 20, 18);
    vlay->setSpacing(12);

    auto* headRow = new QHBoxLayout;
    auto* heading = new QLabel("Recap/History", dlg);
    heading->setStyleSheet("color:#fff; font-size:20px; font-weight:bold; background:transparent;");
    headRow->addWidget(heading);
    headRow->addStretch();

    auto* yearCombo = new QComboBox(dlg);
    yearCombo->setStyleSheet(
        "QComboBox { background:#242424; color:#eee; border:1px solid #3a3a3a; "
        "border-radius:5px; padding:5px 10px; font-size:13px; min-width:90px; }"
        "QComboBox QAbstractItemView { background:#242424; color:#eee; "
        "selection-background-color:#0078d4; }");
    headRow->addWidget(yearCombo);
    vlay->addLayout(headRow);

    auto* scroll = new QScrollArea(dlg);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background:transparent; border:none; }");
    auto* listHost = new QWidget(scroll);
    listHost->setStyleSheet("background:transparent;");
    auto* listLay = new QVBoxLayout(listHost);
    listLay->setContentsMargins(0, 0, 8, 0);
    listLay->setSpacing(0);
    scroll->setWidget(listHost);
    vlay->addWidget(scroll, 1);

    const QList<int> years = ReleaseHistory::instance().years();
    for (int y : years) yearCombo->addItem(QString::number(y), y);
    // Nothing to switch between with one year — left visible but inert, so the
    // page looks the same before and after a year rolls over.
    yearCombo->setEnabled(years.size() > 1);
    if (years.isEmpty()) yearCombo->addItem(QString::number(QDate::currentDate().year()));

    std::function<void(int)> populate;
    populate = [listLay, listHost, &populate](int year) {
        // Clear whatever the previous year left behind.
        while (QLayoutItem* item = listLay->takeAt(0)) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }

        const QList<ReleaseHistory::Entry> entries =
            ReleaseHistory::instance().forYear(year);

        if (entries.isEmpty()) {
            auto* empty = new QLabel(
                QString("Nothing recorded for this year yet.%1%1"
                        "Releases are written down as they happen, so this "
                        "fills in from here on.").arg(QChar('\n')), listHost);
            empty->setWordWrap(true);
            empty->setStyleSheet("color:#777; font-size:13px; background:transparent; padding:12px 0;");
            listLay->addWidget(empty);
            listLay->addStretch();
            return;
        }

        const bool showTotals = TileDisplayPrefs::showTotalEpisodes();
        int lastMonth = -1;
        for (const ReleaseHistory::Entry& e : entries) {
            if (e.date.month() != lastMonth) {
                lastMonth = e.date.month();
                auto* monthLbl = new QLabel(
                    e.date.toString("MMMM yyyy"), listHost);
                monthLbl->setStyleSheet(
                    "color:#fff; font-size:15px; font-weight:bold; background:transparent; "
                    "padding:16px 0 6px 0;");
                listLay->addWidget(monthLbl);
            }

            // "August 4 @4:00PM  -  Invincible S04E04/E08"
            //
            // V5.4.26 — the time is the same one the countdown was aimed at
            // and the same one the startup recap prints. Entries recorded
            // before times were kept simply show the date; sweep() fills those
            // in where the tile can still supply one.
            QString line = e.date.toString("MMMM d");
            if (e.time.isValid()) line += " @" + e.time.toString("h:mmAP");
            line += "  -  " + e.title;
            if (!e.statusLabel.isEmpty()) {
                line += " " + e.statusLabel;
                if (showTotals && e.episodeTotal > 0)
                    line += QString("/E%1").arg(e.episodeTotal, 2, 10, QChar('0'));
            }
            // Each row carries its own delete, on the right where the eye
            // ends up after reading the line. Dim red until hovered, so a list
            // being read is a list of dates rather than a column of crosses.
            auto* rowWidget = new QWidget(listHost);
            rowWidget->setStyleSheet("background:transparent;");
            auto* rowLay = new QHBoxLayout(rowWidget);
            rowLay->setContentsMargins(4, 0, 0, 0);
            rowLay->setSpacing(6);

            auto* row = new QLabel(line, rowWidget);
            row->setWordWrap(true);
            row->setStyleSheet("color:#ccc; font-size:13px; background:transparent; padding:3px 0;");
            rowLay->addWidget(row, 1);

            auto* delBtn = new QToolButton(rowWidget);
            delBtn->setText(QString::fromUtf8("\xc3\x97"));   // multiplication sign, not the letter x
            delBtn->setCursor(Qt::PointingHandCursor);
            delBtn->setToolTip("Remove from history");
            delBtn->setStyleSheet(
                "QToolButton { background:transparent; color:#6a3b3b; border:none; "
                "font-size:15px; font-weight:bold; padding:0 8px; }"
                "QToolButton:hover { color:#ff6b6b; }");
            rowLay->addWidget(delBtn, 0);

            const QString entryKey = e.key();
            const int shownYear = year;
            QObject::connect(delBtn, &QToolButton::clicked, rowWidget,
                             [entryKey, shownYear, &populate]() {
                                 ReleaseHistory::instance().remove(entryKey);
                                 populate(shownYear);   // redraw the page we are on
                             });

            listLay->addWidget(rowWidget);
        }
        listLay->addStretch();
    };

    connect(yearCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), dlg,
            [populate, yearCombo](int) {
                populate(yearCombo->currentData().toInt());
            });
    populate(years.isEmpty() ? QDate::currentDate().year() : years.first());

    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch();
    auto* closeBtn = new QPushButton("Close", dlg);
    closeBtn->setStyleSheet(kPickerSaveBtnStyle);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);
    closeRow->addWidget(closeBtn);
    vlay->addLayout(closeRow);

    dlg->exec();
}

// =============================================================================
void MainWindow::showApiDialog()
{
    QSettings settings("HijackAssassin", "MediaCountdowns");
    QString current = settings.value("tmdbApiKey", TmdbScraper::DEFAULT_API_KEY).toString();

    auto* dlg = new QDialog(this, Qt::Dialog);
    dlg->setWindowTitle("TMDB API Key");
    dlg->setFixedWidth(460);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setModal(true);

    auto* vlay = new QVBoxLayout(dlg);
    vlay->setContentsMargins(20, 20, 20, 20);
    vlay->setSpacing(10);

    auto* curLbl = new QLabel("Current API Key:", dlg);
    curLbl->setStyleSheet("color:#888; font-size:11px;");
    vlay->addWidget(curLbl);

    auto* curVal = new QLabel(current, dlg);
    curVal->setStyleSheet("color:#555; font-size:11px; font-family:monospace;");
    curVal->setWordWrap(true);
    vlay->addWidget(curVal);

    auto* newLbl = new QLabel("New API Key:", dlg);
    newLbl->setStyleSheet("color:#888; font-size:11px; margin-top:6px;");
    vlay->addWidget(newLbl);

    auto* edit = new QLineEdit(dlg);
    edit->setStyleSheet(
        "QLineEdit { background:#1e1e1e; color:#fff; border:1px solid #3a3a3a; "
        "border-radius:4px; padding:8px 10px; font-size:13px; font-family:monospace; }"
        "QLineEdit:focus { border-color:#0078d4; }");
    edit->setPlaceholderText("Paste new API key here…");
    vlay->addWidget(edit);

    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);

    auto* resetBtn = new QPushButton("Reset to Default", dlg);
    resetBtn->setStyleSheet(
        "QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#333; color:#fff; }");
    connect(resetBtn, &QPushButton::clicked, dlg, [dlg, edit, curVal]() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        s.remove("tmdbApiKey");
        edit->clear();
        curVal->setText(TmdbScraper::DEFAULT_API_KEY);
    });
    btnRow->addWidget(resetBtn);
    btnRow->addStretch();

    auto* cancelBtn = new QPushButton("Cancel", dlg);
    cancelBtn->setStyleSheet(
        "QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
        "border-radius:4px; padding:8px 20px; font-size:13px; }"
        "QPushButton:hover { background:#333; }");
    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    auto* saveBtn = new QPushButton("Save", dlg);
    saveBtn->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; "
        "border-radius:4px; padding:8px 28px; font-size:13px; font-weight:bold; }"
        "QPushButton:hover { background:#1a8de4; }");
    connect(saveBtn, &QPushButton::clicked, dlg, [dlg, edit]() {
        QString key = edit->text().trimmed();
        if (key.isEmpty()) return;
        QSettings s("HijackAssassin", "MediaCountdowns");
        s.setValue("tmdbApiKey", key);
        dlg->accept();
    });
    btnRow->addWidget(saveBtn);
    vlay->addLayout(btnRow);

    dlg->exec();
}

// =============================================================================
//  showIgdbCredentialsDialog — v3.3.0. Same pattern as showApiDialog() above,
//  but two fields instead of one, since IGDB's OAuth flow needs both a
//  Client ID and a Client Secret. The secret is shown masked (like a
//  password field) rather than in plain text, since it's meaningfully more
//  sensitive than TMDB's read-only API key — it's what proves this app's
//  identity to Twitch's OAuth server, closer to a password than a key.
// =============================================================================
void MainWindow::showIgdbCredentialsDialog()
{
    QSettings settings("HijackAssassin", "MediaCountdowns");
    QString currentId = settings.value("igdbClientId").toString();

    auto* dlg = new QDialog(this, Qt::Dialog);
    dlg->setWindowTitle("IGDB API Credentials");
    dlg->setFixedWidth(460);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setModal(true);

    auto* vlay = new QVBoxLayout(dlg);
    vlay->setContentsMargins(20, 20, 20, 20);
    vlay->setSpacing(10);

    auto* hint = new QLabel(
        "From your Twitch Developer Console app. Both are stored locally on "
        "this machine only — never bundled with the app or shared anywhere.", dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#777; font-size:11px;");
    vlay->addWidget(hint);

    auto* curLbl = new QLabel("Current Client ID:", dlg);
    curLbl->setStyleSheet("color:#888; font-size:11px; margin-top:6px;");
    vlay->addWidget(curLbl);

    auto* curVal = new QLabel(currentId.isEmpty() ? "(not set)" : currentId, dlg);
    curVal->setStyleSheet("color:#555; font-size:11px; font-family:monospace;");
    curVal->setWordWrap(true);
    vlay->addWidget(curVal);

    auto* idLbl = new QLabel("New Client ID:", dlg);
    idLbl->setStyleSheet("color:#888; font-size:11px; margin-top:6px;");
    vlay->addWidget(idLbl);

    auto* idEdit = new QLineEdit(dlg);
    idEdit->setStyleSheet(
        "QLineEdit { background:#1e1e1e; color:#fff; border:1px solid #3a3a3a; "
        "border-radius:4px; padding:8px 10px; font-size:13px; font-family:monospace; }"
        "QLineEdit:focus { border-color:#0078d4; }");
    idEdit->setPlaceholderText("Paste Client ID here…");
    vlay->addWidget(idEdit);

    auto* secretLbl = new QLabel("New Client Secret:", dlg);
    secretLbl->setStyleSheet("color:#888; font-size:11px; margin-top:6px;");
    vlay->addWidget(secretLbl);

    auto* secretEdit = new QLineEdit(dlg);
    secretEdit->setEchoMode(QLineEdit::Password);
    secretEdit->setStyleSheet(
        "QLineEdit { background:#1e1e1e; color:#fff; border:1px solid #3a3a3a; "
        "border-radius:4px; padding:8px 10px; font-size:13px; font-family:monospace; }"
        "QLineEdit:focus { border-color:#0078d4; }");
    secretEdit->setPlaceholderText(
        settings.value("igdbClientSecret").toString().isEmpty()
            ? "Paste Client Secret here…" : "(already set — leave blank to keep it)");
    vlay->addWidget(secretEdit);

    auto* btnRow2 = new QHBoxLayout;
    btnRow2->setSpacing(8);

    auto* clearBtn = new QPushButton("Clear Both", dlg);
    clearBtn->setStyleSheet(
        "QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#333; color:#fff; }");
    connect(clearBtn, &QPushButton::clicked, dlg, [dlg, idEdit, secretEdit, curVal]() {
        QSettings s("HijackAssassin", "MediaCountdowns");
        s.remove("igdbClientId");
        s.remove("igdbClientSecret");
        idEdit->clear();
        secretEdit->clear();
        curVal->setText("(not set)");
    });
    btnRow2->addWidget(clearBtn);
    btnRow2->addStretch();

    auto* cancelBtn2 = new QPushButton("Cancel", dlg);
    cancelBtn2->setStyleSheet(
        "QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
        "border-radius:4px; padding:8px 20px; font-size:13px; }"
        "QPushButton:hover { background:#333; }");
    connect(cancelBtn2, &QPushButton::clicked, dlg, &QDialog::reject);
    btnRow2->addWidget(cancelBtn2);

    auto* saveBtn2 = new QPushButton("Save", dlg);
    saveBtn2->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; "
        "border-radius:4px; padding:8px 28px; font-size:13px; font-weight:bold; }"
        "QPushButton:hover { background:#1a8de4; }");
    connect(saveBtn2, &QPushButton::clicked, dlg, [dlg, idEdit, secretEdit]() {
        QString id     = idEdit->text().trimmed();
        QString secret = secretEdit->text().trimmed();
        QSettings s("HijackAssassin", "MediaCountdowns");
        if (!id.isEmpty())     s.setValue("igdbClientId", id);
        if (!secret.isEmpty()) s.setValue("igdbClientSecret", secret);
        dlg->accept();
    });
    btnRow2->addWidget(saveBtn2);
    vlay->addLayout(btnRow2);

    dlg->exec();
}

// v3.1.4 — simple wrapper; all the real work lives in AboutDialog.
void MainWindow::showAboutDialog()
{
    AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::showPreferencesDialog()
{
    QSettings prefs("HijackAssassin", "MediaCountdowns");

    auto* dlg = new QDialog(this, Qt::Dialog);
    dlg->setWindowTitle("Settings");
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setModal(true);
    dlg->setStyleSheet("QDialog { background:#1e1e1e; }");

    auto* outerLay = new QVBoxLayout(dlg);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    // v3.0.2 fix #5 — categories now flow left-to-right in a grid (4 per row)
    // instead of stacking vertically, so most screens see everything without
    // scrolling. The scroll area stays as a safety net for smaller windows.
    // V5.4.23 - the cards are grouped into tabs rather than flowed into one
    // long grid. Nothing about any card changed; only which page it lands on.
    // Thirteen cards in a single grid meant scrolling past network settings to
    // reach a checkbox about tile size.
    auto* tabs = new QTabWidget(dlg);
    tabs->setStyleSheet(
        "QTabWidget::pane { border:none; background:#1e1e1e; }"
        "QTabBar { background:#1e1e1e; }"
        "QTabBar::tab { background:#252525; color:#aaa; padding:9px 20px; font-size:13px;"
        "               border:1px solid #333; border-bottom:none;"
        "               border-top-left-radius:6px; border-top-right-radius:6px;"
        "               margin-right:2px; }"
        "QTabBar::tab:selected { background:#1e1e1e; color:#fff; font-weight:bold; }"
        "QTabBar::tab:hover { color:#ddd; }");

    // One page per tab, each with its own scroll area and grid, declared in the
    // order they appear along the top.
    struct SettingsPage { QGridLayout* grid = nullptr; int row = 0; int col = 0; int lastRow = 0; };
    QMap<QString, SettingsPage> pages;
    const QStringList kTabOrder = {"Preferences", "Backup", "Region", "Network", "Feedback"};
    for (const QString& name : kTabOrder) {
        auto* pageScroll = new QScrollArea(tabs);
        pageScroll->setWidgetResizable(true);
        pageScroll->setFrameShape(QFrame::NoFrame);
        pageScroll->setStyleSheet(
            "QScrollArea { background:#1e1e1e; border:none; }"
            "QScrollBar:vertical { background:#1e1e1e; width:8px; }"
            "QScrollBar:horizontal { background:#1e1e1e; height:8px; }"
            "QScrollBar::handle { background:#444; border-radius:4px; }");
        auto* pageContent = new QWidget;
        pageContent->setStyleSheet("background:#1e1e1e;");
        auto* pageGrid = new QGridLayout(pageContent);
        pageGrid->setContentsMargins(20, 20, 20, 20);
        pageGrid->setHorizontalSpacing(14);
        pageGrid->setVerticalSpacing(14);
        pageScroll->setWidget(pageContent);
        tabs->addTab(pageScroll, name);
        pages[name].grid = pageGrid;
    }

    // Everything below still builds its widgets with `content` as the parent,
    // which is now only an owner - beginCard puts each finished card into the
    // grid of whichever page it names, and the widgets follow their card.
    auto* content = new QWidget(dlg);
    content->setStyleSheet("background:#1e1e1e;");
    // Hidden, and that matters. This widget exists only to own the controls
    // while they are being built - beginCard moves each finished card onto its
    // page. Left visible it is a child of the dialog with no layout, so Qt
    // parks it at the top-left at its default size, sitting ON TOP of the tab
    // bar: the first tab could not be seen or clicked, and once another tab was
    // selected there was no way back to it.
    content->hide();

    // v3.0.2 fix #1 — dropdown popups now always include explicit text/
    // background colors, so the open list is never dark-on-dark.
    const QString kComboStyle =
        "QComboBox { background:#252525; color:#ddd; border:1px solid #3a3a3a; "
        "border-radius:4px; padding:6px 8px; font-size:13px; }"
        "QComboBox QAbstractItemView { background:#252525; color:#ddd; "
        "selection-background-color:#0078d4; selection-color:#fff; "
        "outline:none; }";
    const QString kCardStyle = "background:#232323; border:1px solid #333333; border-radius:8px;";
    const QString kCheckStyle =
        "QCheckBox { color:#cccccc; font-size:13px; } "
        "QCheckBox:disabled { color:#666666; }";

    // Two per row now that each page holds only a few cards; three left a
    // lonely single card on a second row for most tabs.
    const int kColsPerRow = 2;
    for (const QString& name : kTabOrder)
        for (int c = 0; c < kColsPerRow; ++c) pages[name].grid->setColumnStretch(c, 1);

    // Starts a new card (heading + its own vertical layout) and places it in
    // the grid, wrapping to a new row every kColsPerRow cards — so any
    // categories added later automatically flow onto the next row.
    // slot is an explicit position on the page (0 = top-left, reading order).
    // Left at -1 a card simply takes the next free spot, which is how almost
    // every card is placed; it exists so a page can be ordered by importance
    // without moving hundreds of lines of card-building code around.
    auto beginCard = [&](const QString& heading, const QString& tabName,
                         int slot = -1) -> QVBoxLayout* {
        auto* card = new QWidget(content);
        card->setStyleSheet(kCardStyle);
        auto* cardLay = new QVBoxLayout(card);
        cardLay->setContentsMargins(14, 14, 14, 14);
        cardLay->setSpacing(6);
        auto* lbl = new QLabel(heading, card);
        lbl->setWordWrap(true);
        lbl->setStyleSheet("color:#aaa; font-size:12px; font-weight:bold; "
                            "background:transparent; border:none;");
        cardLay->addWidget(lbl);

        SettingsPage& page = pages[tabName];
        int r, c;
        if (slot >= 0) {
            r = slot / kColsPerRow;
            c = slot % kColsPerRow;
        } else {
            r = page.row;
            c = page.col;
            if (++page.col >= kColsPerRow) { page.col = 0; ++page.row; }
        }
        page.grid->addWidget(card, r, c, Qt::AlignTop);
        page.lastRow = qMax(page.lastRow, r);
        return cardLay;
    };

    auto makeCheck = [&](QVBoxLayout* card, const QString& label,
                         const QString& key, bool defaultVal) -> QCheckBox* {
        auto* cb = new QCheckBox(label, content);
        cb->setChecked(prefs.value(key, defaultVal).toBool());
        cb->setStyleSheet(kCheckStyle);
        card->addWidget(cb);
        return cb;
    };

    // V4.3 — the Relay Server checkbox (built last, in its own card
    // further down) needs to be re-evaluated the instant a custom TMDB
    // key or IGDB credential is set or cleared via the buttons above it
    // in the new row order — declared here so both sides can reference
    // it, assigned once its own card actually creates the checkbox.
    //
    // refreshRelayCheckDisplay() only ever reflects current state (does
    // NOT decide auto-uncheck/re-check transitions itself) — that
    // one-time transition logic lives in the TMDB/IGDB button handlers
    // below, since only they know whether a change just happened. This
    // split matters: without it, simply reopening this dialog or
    // clicking either button again would re-force the checkbox every
    // time, silently overwriting a deliberate manual re-check (see the
    // "both configured, relay checked back on" case in RelayConfig).
    //
    // V4.5 — always enabled/toggleable now, even with zero custom
    // credentials configured. Previously this force-checked and disabled
    // itself in that case (relay being the only way to get any data at
    // all) — but that also meant there was no way to go fully offline
    // without first setting up a credential you might not even want.
    // Someone who deliberately unchecks this with nothing else
    // configured has chosen for both TMDB and IGDB to simply have
    // nothing to fall back on — an intentional, honored choice, not an
    // accident to guard against.
    QCheckBox* useRelayCheck = nullptr;
    auto refreshRelayCheckDisplay = [&]() {
        if (!useRelayCheck) return;
        useRelayCheck->setEnabled(true);
        QSettings s("HijackAssassin", "MediaCountdowns");
        useRelayCheck->setChecked(s.value("useRelay", true).toBool());
    };
    // One-time transition handling: call right after either TMDB or IGDB
    // credentials might have just changed. If that change completed the
    // pair (0/1 -> both configured), auto-turn relay off for both, since
    // neither needs it anymore. If it broke the pair (both -> 0/1), turn
    // relay back on so whichever service lost its credential still has
    // the relay to fall back on. Otherwise (no change in "both configured"
    // status), leave the persisted useRelay value alone entirely — this
    // is what protects a manual re-check from being stomped on.
    auto handlePossibleCredentialTransition = [&](bool wasBothConfigured) {
        bool isBothConfigured = RelayConfig::hasCustomTmdbKey() && RelayConfig::hasCustomIgdbCredentials();
        if (isBothConfigured != wasBothConfigured) {
            QSettings s("HijackAssassin", "MediaCountdowns");
            s.setValue("useRelay", !isBothConfigured);
        }
        refreshRelayCheckDisplay();
    };

    // ── Card: Time Zone ───────────────────────────────────────────────────────
    QVBoxLayout* tzCard = beginCard("Time Zone", "Region");
    auto* tzCombo = new QComboBox(content);
    tzCombo->setStyleSheet(kComboStyle);
    for (const QString& label : TimeZoneUtil::labels()) tzCombo->addItem(label);
    {
        int idx = TimeZoneUtil::ids().indexOf(TimeZoneUtil::currentZoneId());
        tzCombo->setCurrentIndex(idx < 0 ? 0 : idx);
    }
    tzCard->addWidget(tzCombo);
    auto* tzHint = new QLabel(
        "Affects the default time for movies & shows. Digital releases line up "
        "with midnight Pacific — this shifts that to your zone. Theatrical "
        "movies default to noon local; games default to local midnight.", content);
    tzHint->setWordWrap(true);
    tzHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    tzCard->addWidget(tzHint);
    tzCard->addStretch();

    // ── Card: Language  (v3.1.2) ──────────────────────────────────────────────
    QVBoxLayout* langCard = beginCard("Language", "Region");
    auto* langCombo = new QComboBox(content);
    langCombo->setStyleSheet(kComboStyle);
    for (const QString& label : LanguageUtil::labels()) langCombo->addItem(label);
    {
        int idx = LanguageUtil::codes().indexOf(LanguageUtil::currentCode());
        langCombo->setCurrentIndex(idx < 0 ? 0 : idx);
    }
    langCard->addWidget(langCombo);
    auto* langHint = new QLabel(
        "Determines which language's backdrop images get pulled from TMDB. "
        "Auto-detected from your system where possible.", content);
    langHint->setWordWrap(true);
    langHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    langCard->addWidget(langHint);
    langCard->addStretch();

    // ── Card: Tile Size  (v3.3.40) ───────────────────────────────────────────
    QVBoxLayout* prefsCard = beginCard("Tile Size", "Preferences", 2);
    auto* tileSizeSlider = new QSlider(Qt::Horizontal, content);
    tileSizeSlider->setMinimum(2);
    tileSizeSlider->setMaximum(5);
    tileSizeSlider->setValue(prefs.value("tilesPerRow", 3).toInt());
    tileSizeSlider->setStyleSheet(
        "QSlider::groove:horizontal { background:#333; height:4px; border-radius:2px; }"
        "QSlider::handle:horizontal { background:#0078d4; width:14px; height:14px; "
        "margin:-5px 0; border-radius:7px; }"
        "QSlider::handle:horizontal:hover { background:#1a8de4; }");
    prefsCard->addWidget(tileSizeSlider);

    auto tileSizeLabelText = [](int v) {
        return v == 3 ? QString("%1 per row (Default)").arg(v) : QString("%1 per row").arg(v);
    };
    auto* tileSizeValueLbl = new QLabel(tileSizeLabelText(tileSizeSlider->value()), content);
    tileSizeValueLbl->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    prefsCard->addWidget(tileSizeValueLbl);
    connect(tileSizeSlider, &QSlider::valueChanged, dlg, [=](int v) {
        tileSizeValueLbl->setText(tileSizeLabelText(v));
    });
    prefsCard->addStretch();

    // ── Card: Show in search results ─────────────────────────────────────────
    QVBoxLayout* searchCard = beginCard("Show in search results", "Preferences", 3);
    auto* cbMovies  = makeCheck(searchCard, "Movies",                "pref_movies",  true);
    auto* cbShows   = makeCheck(searchCard, "Shows",                 "pref_shows",   true);
    auto* cbReality = makeCheck(searchCard, "Reality TV",            "pref_reality", false);
    auto* cbDocs    = makeCheck(searchCard, "Documentaries",         "pref_docs",    false);
    auto* cbTalk    = makeCheck(searchCard, "Talk Shows",            "pref_talk",    false);
    auto* cbForeign = makeCheck(searchCard, "Foreign (non-English)", "pref_foreign", false);
    searchCard->addStretch();

    // Mutual lock: at least one of Movies/Shows must stay checked
    auto enforceLock = [=]() {
        if (!cbMovies->isChecked()) {
            cbShows->setChecked(true);
            cbShows->setEnabled(false);
        } else {
            cbShows->setEnabled(true);
        }
        if (!cbShows->isChecked()) {
            cbMovies->setChecked(true);
            cbMovies->setEnabled(false);
        } else {
            cbMovies->setEnabled(true);
        }
    };
    enforceLock();
    connect(cbMovies, &QCheckBox::toggled, dlg, [=]{ enforceLock(); });
    connect(cbShows,  &QCheckBox::toggled, dlg, [=]{ enforceLock(); });

    // ── Card: Tile Display  (v3.0.2 fix #4) ──────────────────────────────────
    QVBoxLayout* tileCard = beginCard("Tile Display", "Preferences", 0);
    auto* cbTitle    = makeCheck(tileCard, "Title",          "tileShowTitle",         true);
    auto* cbYear     = makeCheck(tileCard, "Year",           "tileShowYear",          false);
    auto* cbSeason   = makeCheck(tileCard, "Season",         "tileShowSeason",        true);
    auto* cbEpisode  = makeCheck(tileCard, "Episode",        "tileShowEpisode",       true);
    auto* cbTotalEp  = makeCheck(tileCard, "Total Episodes", "tileShowTotalEpisodes", false);
    auto* cbWeekday  = makeCheck(tileCard, "Weekday",        "tileShowWeekday",       false);
    auto* cbDate     = makeCheck(tileCard, "Date",           "tileShowDate",          true);
    auto* cbTime     = makeCheck(tileCard, "Time",           "tileShowTime",          false);   // V5.4
    tileCard->addStretch();

    // Total Episodes only makes sense alongside Episode — grey it out (but
    // keep its checked state remembered) whenever Episode is off.
    auto syncTotalEpEnabled = [=] { cbTotalEp->setEnabled(cbEpisode->isChecked()); };
    syncTotalEpEnabled();
    connect(cbEpisode, &QCheckBox::toggled, dlg, [=]{ syncTotalEpEnabled(); });

    // ── Card: Tab Layout ──────────────────────────────────────────────────────
    QVBoxLayout* tabCard = beginCard("Tab Layout", "Preferences", 1);

    auto* presetLbl = new QLabel("Presets", content);
    presetLbl->setStyleSheet("color:#cccccc; font-size:13px; background:transparent; border:none;");
    tabCard->addWidget(presetLbl);

    auto* presetCombo = new TabLayoutPresetCombo(content);
    presetCombo->setStyleSheet(kComboStyle);
    presetCombo->setMinimumHeight(30);   // room for full text height (avoids clipping descenders)
    presetCombo->addItem("Default");
    presetCombo->addItem("Separate");
    presetCombo->addItem("All");
    presetCombo->addItem("None");
    presetCombo->addItem("Custom");   // never clickable in the popup — see TabLayoutPresetCombo above
    presetCombo->hideRowFromPopup(4);
    tabCard->addWidget(presetCombo);

    tabCard->addSpacing(2);

    std::array<QCheckBox*, NUM_KINDS> kindChecks{};
    for (int k = 0; k < NUM_KINDS; ++k) {
        auto* cb = new QCheckBox(kKindLabels[k], content);
        cb->setChecked(m_enabledKinds.contains(k));
        cb->setStyleSheet(kCheckStyle);
        tabCard->addWidget(cb);
        kindChecks[k] = cb;
    }
    tabCard->addStretch();

    auto syncPresetFromChecks = [=]() {
        QSet<int> checked;
        for (int k = 0; k < NUM_KINDS; ++k)
            if (kindChecks[k]->isChecked()) checked.insert(k);
        presetCombo->blockSignals(true);
        if (checked == defaultPresetKinds()) {
            presetCombo->setCurrentIndex(0);
        } else if (checked == separatePresetKinds()) {
            presetCombo->setCurrentIndex(1);
        } else if (checked == allPresetKinds()) {
            presetCombo->setCurrentIndex(2);
        } else if (checked == nonePresetKinds()) {
            presetCombo->setCurrentIndex(3);
        } else {
            presetCombo->setCurrentIndex(4); // "Custom" — display only, never a popup choice
        }
        presetCombo->blockSignals(false);
    };
    syncPresetFromChecks();

    for (int k = 0; k < NUM_KINDS; ++k)
        connect(kindChecks[k], &QCheckBox::toggled, dlg, [=]{ syncPresetFromChecks(); });

    connect(presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), dlg, [=](int idx) {
        QSet<int> want;
        if (idx == 0)      want = defaultPresetKinds();
        else if (idx == 1) want = separatePresetKinds();
        else if (idx == 2) want = allPresetKinds();
        else if (idx == 3) want = nonePresetKinds();
        else               return; // "Custom"/transient state — never reachable by user pick
        for (int k = 0; k < NUM_KINDS; ++k) {
            kindChecks[k]->blockSignals(true);
            kindChecks[k]->setChecked(want.contains(k));
            kindChecks[k]->blockSignals(false);
        }
    });

    // ── Card: TMDB API Key (Optional)  (v3.1.2) ──────────────────────────────
    QVBoxLayout* apiCard = beginCard("TMDB API Key (Optional)", "Network");
    auto* apiHint = new QLabel(
        "Use your own API Key from TMDB instead of using the app's server", content);
    apiHint->setWordWrap(true);
    apiHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    apiCard->addWidget(apiHint);
    auto* apiBtn = new QPushButton("Set Custom API Key", content);
    apiBtn->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }");
    connect(apiBtn, &QPushButton::clicked, dlg, [this, &handlePossibleCredentialTransition]{
        bool wasBothConfigured = RelayConfig::hasCustomTmdbKey() && RelayConfig::hasCustomIgdbCredentials();
        showApiDialog();
        handlePossibleCredentialTransition(wasBothConfigured);
    });
    apiCard->addWidget(apiBtn);
    apiCard->addStretch();

    // ── Card: IGDB API Credentials (Optional)  (v3.3.0) ───────────────────────
    QVBoxLayout* igdbCard = beginCard("IGDB API Credentials (Optional)", "Network");
    auto* igdbHint = new QLabel(
        "Use your own API Credentials from IGDB instead of using the app's server", content);
    igdbHint->setWordWrap(true);
    igdbHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    igdbCard->addWidget(igdbHint);
    auto* igdbBtn = new QPushButton("Set Custom API Credentials", content);
    igdbBtn->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }");
    connect(igdbBtn, &QPushButton::clicked, dlg, [this, &handlePossibleCredentialTransition]{
        bool wasBothConfigured = RelayConfig::hasCustomTmdbKey() && RelayConfig::hasCustomIgdbCredentials();
        showIgdbCredentialsDialog();
        handlePossibleCredentialTransition(wasBothConfigured);
    });
    igdbCard->addWidget(igdbBtn);
    igdbCard->addStretch();

    // ── Card: Backup  (V5.4.22 — moved out of the top bar) ───────────────────
    QVBoxLayout* backupCard = beginCard("Backup", "Backup");
    auto* backupHint = new QLabel(
        "Save every tile to a file, or bring them back from one.", content);
    backupHint->setWordWrap(true);
    backupHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    backupCard->addWidget(backupHint);
    {
        const char* kBackupBtnStyle =
            "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
            "border-radius:4px; padding:8px 16px; font-size:13px; }"
            "QPushButton:hover { background:#383838; }";
        auto* exportBtn = new QPushButton("Export Tiles…", content);
        exportBtn->setStyleSheet(kBackupBtnStyle);
        connect(exportBtn, &QPushButton::clicked, dlg, [this]{ onExportClicked(); });
        backupCard->addWidget(exportBtn);
        auto* importBtn = new QPushButton("Import Tiles…", content);
        importBtn->setStyleSheet(kBackupBtnStyle);
        connect(importBtn, &QPushButton::clicked, dlg, [this]{ onImportClicked(); });
        backupCard->addWidget(importBtn);
    }
    backupCard->addStretch();

    // ── Card: Relay Key (Optional)  (V5.4.18) ────────────────────────────────
    //
    // Third on the same row as the two above, and for the same reason: all
    // three are "use your own credential instead of the one this build ships
    // with". This one matters for a build made from the public source, which
    // ships with the slot empty — without somewhere to type a key, such a
    // build can only talk to a relay the person runs themselves after editing
    // the source, which is not a reasonable thing to ask.
    QVBoxLayout* relayKeyCard = beginCard("Relay Key (Optional)", "Network");
    auto* relayKeyHint = new QLabel(
        "Use your own server and key instead of the one built into this copy", content);
    relayKeyHint->setWordWrap(true);
    relayKeyHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    relayKeyCard->addWidget(relayKeyHint);
    auto* relayKeyBtn = new QPushButton("Set Custom Relay Key", content);
    relayKeyBtn->setStyleSheet(
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }");
    connect(relayKeyBtn, &QPushButton::clicked, dlg, [this]{ showRelayKeyDialog(); });
    relayKeyCard->addWidget(relayKeyBtn);
    relayKeyCard->addStretch();

    // ── Card: Feedback  (V5.4.3) ──────────────────────────────────────────────
    //
    // Two doors rather than one "Contact" box: a broken thing and a wanted
    // thing need different follow-ups, and asking which it is here means not
    // having to sort them out afterwards. The wording is plain on purpose —
    // "Report a Bug" reads like a chore, so the buttons say what the person
    // actually has rather than what the developer wants filed.
    // V5.4.24 — two cards, each named for what it is. One card with two
    // conversational buttons ("Something's not working — report it") meant
    // reading a sentence to work out which door was which; a heading that says
    // Report a Bug is understood before it is read.
    QVBoxLayout* feedbackCard = beginCard("Report a Bug", "Feedback");
    auto* feedbackHint = new QLabel(
        "Goes straight to the developer. Nothing is attached but the app "
        "version — no name, no email.", content);
    feedbackHint->setWordWrap(true);
    feedbackHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    feedbackCard->addWidget(feedbackHint);

    const QString kFeedbackBtnStyle =
        "QPushButton { background:#2a2a2a; color:#ccc; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#383838; }";

    auto* bugBtn = new QPushButton("Report a Bug", content);
    bugBtn->setStyleSheet(kFeedbackBtnStyle);
    connect(bugBtn, &QPushButton::clicked, dlg, [this, dlg]{
        showFeedbackDialog(dlg, /*isBug=*/true);
    });
    feedbackCard->addWidget(bugBtn);

    feedbackCard->addStretch();

    // The other half of what was one card, now its own.
    QVBoxLayout* ideaCard = beginCard("Feature Request", "Feedback");
    auto* ideaHint = new QLabel(
        "Something you'd like the app to do. Sent the same way — app version "
        "only, no name, no email.", content);
    ideaHint->setWordWrap(true);
    ideaHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    ideaCard->addWidget(ideaHint);
    auto* ideaBtn = new QPushButton("Send a Feature Request", content);
    ideaBtn->setStyleSheet(kFeedbackBtnStyle);
    connect(ideaBtn, &QPushButton::clicked, dlg, [this, dlg]{
        showFeedbackDialog(dlg, /*isBug=*/false);
    });
    ideaCard->addWidget(ideaBtn);
    ideaCard->addStretch();

    // ── Card: Media Countdowns Server  (V4.3.1 — renamed from "Relay
    //    Server" for end users; checkbox only, no user-facing config,
    //    that stays exclusive to the developer) ───────────────────────────────
    QVBoxLayout* relayCard = beginCard("Media Countdowns Server", "Network");

    // V4.5 — live connection status, right under the heading. Offline
    // means the checkbox itself is off (no network call needed to know
    // that); Online/Server Unreachable both require actually asking the
    // server, so those two only resolve once the /health request
    // finishes. A QPointer guards the callback in case the dialog closes
    // before the reply comes back.
    auto* statusLbl = new QLabel(content);
    statusLbl->setStyleSheet("font-size:11px; font-weight:bold; background:transparent; border:none;");
    relayCard->addWidget(statusLbl);

    useRelayCheck = makeCheck(relayCard, "Use Media Countdowns Server", "useRelay", true);
    relayCard->addStretch();
    refreshRelayCheckDisplay();

    auto* healthNam = new QNetworkAccessManager(dlg);
    auto refreshServerStatus = [statusLbl, healthNam, this, useRelayCheck]() {
        const char* kOfflineStyle  = "color:#e05252; font-size:11px; font-weight:bold; background:transparent; border:none;";
        const char* kCheckingStyle = "color:#888;    font-size:11px; font-weight:bold; background:transparent; border:none;";
        const char* kOnlineStyle   = "color:#6c6;    font-size:11px; font-weight:bold; background:transparent; border:none;";

        if (!useRelayCheck->isChecked()) {
            statusLbl->setText("Offline");
            statusLbl->setStyleSheet(kOfflineStyle);
            return;
        }
        // V5.4.26 — a build from the public source has no relay until one is
        // entered below. Naming that is the whole difference between "you
        // haven't set this up yet" and "the server is down", and without it
        // the check goes out as a scheme-less relative URL and fails.
        if (!RelayConfig::isConfigured()) {
            statusLbl->setText("Not set up — add a Relay Key below");
            statusLbl->setStyleSheet(kCheckingStyle);
            return;
        }
        statusLbl->setText("Checking…");
        statusLbl->setStyleSheet(kCheckingStyle);

        QNetworkRequest req(QUrl(RelayConfig::baseUrl() + "/health"));
        // V5.4.19 — 4 seconds, not Qt's default. This is a liveness check whose
        // whole job is answering quickly: a server that hasn't replied in four
        // seconds is unreachable for the purpose of a status line, and waiting
        // the default (tens of seconds) made "Checking…" look like it had hung.
        // A slow answer that arrives later is still the wrong answer to show.
        req.setTransferTimeout(4000);
        QNetworkReply* reply = healthNam->get(req);
        QPointer<QLabel> labelGuard(statusLbl);
        connect(reply, &QNetworkReply::finished, statusLbl, [reply, labelGuard, kOnlineStyle, kOfflineStyle]() {
            reply->deleteLater();
            if (!labelGuard) return;   // dialog (and this label) already closed
            if (reply->error() == QNetworkReply::NoError) {
                labelGuard->setText("Online");
                labelGuard->setStyleSheet(kOnlineStyle);
            } else {
                labelGuard->setText("Server Unreachable");
                labelGuard->setStyleSheet(kOfflineStyle);
            }
        });
    };
    refreshServerStatus();
    connect(useRelayCheck, &QCheckBox::toggled, dlg, [refreshServerStatus](bool){ refreshServerStatus(); });

    // ── Card: Delete All Tiles  (V4.6) ──────────────────────────────────────
    QVBoxLayout* deleteAllCard = beginCard("Delete All Tiles", "Backup");
    auto* deleteAllHint = new QLabel(
        "Permanently removes every tile across every tab. This cannot be undone.", content);
    deleteAllHint->setWordWrap(true);
    deleteAllHint->setStyleSheet("color:#777; font-size:11px; background:transparent; border:none;");
    deleteAllCard->addWidget(deleteAllHint);
    auto* deleteAllBtn = new QPushButton("Delete All Tiles", content);
    deleteAllBtn->setStyleSheet(
        "QPushButton { background:#3a1e1e; color:#e08080; border:1px solid #5a2a2a; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#4a2626; }");
    connect(deleteAllBtn, &QPushButton::clicked, dlg, [this, dlg]{
        QDialog confirm(dlg);
        confirm.setWindowTitle("Delete All Tiles?");
        confirm.setStyleSheet("QDialog { background:#1e1e1e; }");
        confirm.setModal(true);

        auto* vlay = new QVBoxLayout(&confirm);
        vlay->setContentsMargins(24, 24, 24, 24);
        vlay->setSpacing(16);

        auto* msgLbl = new QLabel(
            "This will permanently delete every tile across every tab.\n\nAre you sure?", &confirm);
        msgLbl->setStyleSheet("color:#eee; font-size:13px; background:transparent;");
        msgLbl->setWordWrap(true);
        vlay->addWidget(msgLbl);

        auto* btnRow = new QHBoxLayout();
        btnRow->addStretch();
        auto* noBtn = new QPushButton("No", &confirm);
        noBtn->setStyleSheet(
            "QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
            "border-radius:4px; padding:8px 20px; font-size:13px; }"
            "QPushButton:hover { background:#333; }");
        auto* yesBtn = new QPushButton("Yes, Delete All", &confirm);
        yesBtn->setStyleSheet(
            "QPushButton { background:#7a2020; color:#fff; border:none; "
            "border-radius:4px; padding:8px 20px; font-size:13px; font-weight:bold; }"
            "QPushButton:hover { background:#932828; }");
        btnRow->addWidget(noBtn);
        btnRow->addWidget(yesBtn);
        vlay->addLayout(btnRow);

        connect(noBtn, &QPushButton::clicked, &confirm, &QDialog::reject);
        connect(yesBtn, &QPushButton::clicked, &confirm, &QDialog::accept);

        if (confirm.exec() == QDialog::Accepted) {
            QStringList allIds;
            for (const TileData& td : std::as_const(m_tiles)) allIds << td.id;
            onRemoveMultipleTiles(allIds);
        }
    });
    deleteAllCard->addWidget(deleteAllBtn);
    deleteAllCard->addStretch();

    outerLay->addWidget(tabs, 1);

    // ── Buttons ───────────────────────────────────────────────────────────────
    auto* btnBar = new QWidget(dlg);
    btnBar->setStyleSheet("background:#1e1e1e;");
    auto* btnRow = new QHBoxLayout(btnBar);
    btnRow->setContentsMargins(20, 12, 20, 20);
    btnRow->setSpacing(8);

    btnRow->addStretch();

    auto* cancelBtn = new QPushButton("Cancel", btnBar);
    cancelBtn->setStyleSheet(
        "QPushButton { background:#252525; color:#aaa; border:1px solid #444; "
        "border-radius:4px; padding:8px 16px; font-size:13px; }"
        "QPushButton:hover { background:#333; }");
    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    auto* saveBtn = new QPushButton("Save", btnBar);
    saveBtn->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; "
        "border-radius:4px; padding:8px 24px; font-size:13px; font-weight:bold; }"
        "QPushButton:hover { background:#1a8de4; }");
    connect(saveBtn, &QPushButton::clicked, dlg, [=, &prefs]() {
        prefs.setValue("useRelay", useRelayCheck->isChecked());   // V4
        // V5 — switching the TV source changes where every show tile's
        // dates come from, so re-fetch them all rather than leaving the
        // grid showing whatever the previous source last said.

        prefs.setValue("pref_movies",  cbMovies->isChecked());
        prefs.setValue("pref_shows",   cbShows->isChecked());
        prefs.setValue("pref_reality", cbReality->isChecked());
        prefs.setValue("pref_docs",    cbDocs->isChecked());
        prefs.setValue("pref_talk",    cbTalk->isChecked());
        prefs.setValue("pref_foreign", cbForeign->isChecked());

        // V5.4.28 — changing the zone has to re-derive every TV tile's air
        // time, not just store the preference. Before this, the setting only
        // reached movies (which compute theirs live) and shows kept whatever
        // wall-clock time was baked in when they were last refreshed.
        const QString zoneBefore = TimeZoneUtil::currentZoneId();
        TimeZoneUtil::setZoneId(TimeZoneUtil::ids().at(tzCombo->currentIndex()));
        const bool zoneChanged = (TimeZoneUtil::currentZoneId() != zoneBefore);
        LanguageUtil::setCode(LanguageUtil::codes().at(langCombo->currentIndex()));

        QStringList enabledKeys;
        for (int k = 0; k < NUM_KINDS; ++k)
            if (kindChecks[k]->isChecked()) enabledKeys << kKindKeys[k];
        prefs.setValue("tabEnabledKinds", enabledKeys);

        prefs.setValue("tileShowTitle",         cbTitle->isChecked());
        prefs.setValue("tileShowYear",          cbYear->isChecked());
        prefs.setValue("tileShowSeason",        cbSeason->isChecked());
        prefs.setValue("tileShowEpisode",       cbEpisode->isChecked());
        prefs.setValue("tileShowTotalEpisodes", cbTotalEp->isChecked());
        prefs.setValue("tileShowWeekday",       cbWeekday->isChecked());
        prefs.setValue("tileShowDate",          cbDate->isChecked());
        prefs.setValue("tileShowTime",          cbTime->isChecked());   // V5.4

        prefs.setValue("tilesPerRow", tileSizeSlider->value());   // v3.3.38

        // V5.4.28 — apply the new zone to every show that has a published air
        // time. Local and immediate: the absolute instant is stored on the
        // tile, so this needs no network and cannot be slow, however many
        // tiles there are.
        if (zoneChanged) {
            const int retimed = retimeTilesForZone();
            if (retimed > 0) {
                saveTiles();
                sortAndRebuildAllTabs();   // a date may have moved, so the grid re-sorts
                APPLOG(QString("Time Zone changed to %1 — re-derived the air time of %2 tile(s)")
                           .arg(TimeZoneUtil::currentZoneId()).arg(retimed));
                m_statusLbl->setStyleSheet(kStatusStyleNeutral);
                m_statusLbl->setText(QString("Time zone applied to %1 show%2")
                                         .arg(retimed).arg(retimed == 1 ? "" : "s"));
            }

            // A tile saved before V5.4.28 has no stored instant, so there is
            // nothing to re-derive from and it would keep its old wall-clock
            // time until something happened to refresh it — which is exactly
            // the bug this fixes, just delayed. Refetch those once so the
            // setting takes effect now rather than eventually. Costs nothing
            // after the first zone change, because by then every tile has one.
            int missing = 0;
            for (const TileData& td : std::as_const(m_tiles))
                if (td.mediaType == "tv" && !td.airStampUtc.isValid() && !td.isWindowDate())
                    ++missing;
            if (missing > 0) {
                APPLOG(QString("Time Zone changed — %1 show(s) predate the stored air-time "
                               "instant, refreshing them to pick one up").arg(missing));
                refreshAllTiles();
            }
        }

        dlg->accept();
    });
    btnRow->addWidget(saveBtn);
    outerLay->addWidget(btnBar);

    // v3.1.5 fix #1 — size to the ACTUAL content rather than a hardcoded
    // guess. V5.4.24 — that content is now five pages, and the old code
    // measured `content`, which holds nothing since the cards moved onto their
    // pages: it returned a near-empty size hint and the dialog opened too small
    // to show the first page, cutting it in half.
    //
    // The window has to fit the BIGGEST page, or switching tabs would resize it
    // under the cursor. Every page also gets a stretch row beneath its cards so
    // spare height collects at the bottom instead of being shared out between
    // the rows — which is what pushed the two rows of the Network tab apart as
    // the window grew.
    int widestPage = 0, tallestPage = 0;
    for (const QString& name : kTabOrder) {
        QGridLayout* g = pages[name].grid;
        g->setRowStretch(pages[name].lastRow + 1, 1);
        if (QWidget* pageContent = g->parentWidget()) {
            pageContent->adjustSize();
            widestPage  = qMax(widestPage,  pageContent->sizeHint().width());
            tallestPage = qMax(tallestPage, pageContent->sizeHint().height());
        }
    }
    const int chromeHeight = btnBar->sizeHint().height() + tabs->tabBar()->sizeHint().height() + 40;
    dlg->resize(qBound(760, widestPage + 40, 1100),
                qBound(560, tallestPage + chromeHeight, 900));

    if (dlg->exec() == QDialog::Accepted) {
        loadTabLayoutSettings();
        m_tilesPerRow = qBound(2, prefs.value("tilesPerRow", 3).toInt(), 5);   // v3.3.50 — range narrowed from v3.3.38's 1-20
        applyTilesPerRowStretch();   // v3.3.39 — re-apply stretch for the new value
        sortAndRebuildAllTabs();
        for (TileWidget* tw : std::as_const(m_tileWidgets)) tw->refreshDisplayPrefs();
        // v3.3.44 — re-save so a Time Zone change immediately refreshes
        // every tile's persisted effectiveAirTime, rather than waiting
        // for the next unrelated tile add/edit/refresh to happen to do it.
        saveTiles();
    }
}

#include "mainwindow.moc"
