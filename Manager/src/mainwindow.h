#pragma once
#include <QMainWindow>
#include <QTabWidget>
#include <QScrollArea>
#include <QWidget>
#include <QGridLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <QList>
#include <QSet>
#include <QVariantAnimation>
#include <QDialog>
#include <QComboBox>
#include <QToolButton>
#include <QVector>
#include <QIcon>
#include <QColor>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QSystemTrayIcon>
#include "tiledata.h"
#include "tmdbscraper.h"
#include "igdbscraper.h"
#include "tvmazescraper.h"
#include "numerals.h"   // V5 — roman/arabic sequel-number handling in search
#include "showoverrides.h"   // V5 — relay-published air-time corrections

class TileWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // v3.1.4 — Tab Layout: 13 possible tab "kinds" (was 9 as of v3.0.0 — added
    // Special/Custom Countdowns and their Released counterparts). Which ones
    // are enabled and in what order is user-configurable via Settings → Tab
    // Layout. Public so file-scope helper tables/functions in mainwindow.cpp
    // can reference it.
    enum TabKind {
        K_COUNTDOWNS = 0, K_RELEASED, K_OTHER, K_FAVORITE,
        K_MOVIE_COUNTDOWNS, K_SHOW_COUNTDOWNS, K_GAME_COUNTDOWNS,
        K_SPECIAL_COUNTDOWNS, K_CUSTOM_COUNTDOWNS,
        K_RELEASED_MOVIE, K_RELEASED_SHOW, K_RELEASED_GAME,
        K_RELEASED_SPECIAL, K_RELEASED_CUSTOM,
        NUM_KINDS
    };

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void checkForUpdates();
    // V5.4.8 — asks the relay for an installation id the first time only.
    void ensureRegisteredWithRelay();
    // V5.4.3 — one dialog for both feedback kinds; isBug picks the wording.
    void showFeedbackDialog(QWidget* parent, bool isBug);

    // V5 — startup recap of anything that released while the app was shut.
    //
    // Two steps on purpose: captureMissedReleases() must run BEFORE the
    // startup refresh, because a refresh advances a show past the very
    // episode that was missed. showMissedReleases() then displays that
    // snapshot, returning true if a dialog was shown (in which case it
    // chains into checkForUpdates() on dismissal). The struct and its list
    // live with the other private members below — moc rejects type and
    // field declarations inside a slots section.
    void captureMissedReleases();
    bool showMissedReleases();
    void checkMotd();   // V4.5 — both gated on the Media Countdowns Server connection being enabled
    void showMotdDialog(const QString& message);
    // V5.4.13 — the startup order is: update check, tiles, then the message of
    // the day. Both of the first two have to be finished before the message is
    // asked for, so each one reports in here and the last to arrive fires it.
    void maybeCheckMotd();
    void onSearchClicked();
    void onCustomTileClicked();
    void onSearchResultsReady(const QList<SearchResult>& results);
    void onCreditsReady(int tmdbId, const QString& director, const QString& castLine);
    void onPickerItemActivated(QListWidgetItem* item);
    void onScraperDataReady(const TileData& data);
    void onTileRefreshed(const TileData& updated);
    void onPosterReady(const QString& tileId, const QString& localPath, bool makeActive);
    void onScraperError(const QString& msg);
    void onImageChanged(const QString& tileId, const QString& path);
    void onTileDataChanged(const QString& tileId);
    void onRemoveTile(const QString& tileId);
    void onDuplicateTile(const QString& tileId);
    void onRefetchRequested(const QString& tileId);
    void onTvmazeNeedsConfirmation(const QString& tileId,
                                   const QList<SearchResult>& candidates);   // V5
    void onForceImageRefetchRequested(const QString& tileId);   // v3.1.0
    void onTestNotification(const QString& tileId);
    void onGlobalTick();
    void onExportClicked();
    void onImportClicked();

private slots:
    void onTabMoved(int from, int to);
    void onJumpToTabItemClicked(QListWidgetItem* item);   // V4.14
    void onJumpToTabReordered();   // V4.14 — fires after a drag reorder completes

private:
    void loadTiles();
    void saveTiles();
    void refreshAllTiles();
    // V5.4.11 — the animated dots on the bottom-left status line. Driven
    // entirely by m_refreshPending: call updateRefreshStatus() after anything
    // that changes that counter and the indicator follows on its own.
    void updateRefreshStatus();
    QString refreshingStatusText() const;
    void sortAndRebuildAllTabs();
    void refreshTabBar();
    // V4.4 — per-tab custom color feature. Colors are keyed by TabKind (the
    // logical tab identity, e.g. "Movie (Countdowns)") rather than by grid
    // slot, since the same kind can move between slots/groups without ever
    // losing its own chosen color.
    void loadTabColors();
    void refreshTabColorIcons();
    // V4.7 — extracted from what was originally showTabColorPickerDialog()'s
    // own inline UI code, so the wheel/swatches/RGB/hex dialog can be
    // reused for per-tile color tags too, rather than duplicating it.
    enum class ColorPickerResult { Cancelled, Cleared, Saved };
    struct ColorPickerOutcome { ColorPickerResult result; QColor color; };
    // V5.4.15 — the picker's controls, without a window around them, so two of
    // them can sit on the Customize dialog's tabs and each report changes live.
    struct ColorPane {
        QWidget* widget;
        std::function<QColor()> currentColor;
        // Drives the whole pane — wheel, brightness, RGB and hex — from
        // outside, so Clear can put every control back to the tab's default
        // instead of only changing the tile.
        std::function<void(const QColor&)> setColor;
        // Puts the controls at the default AND empties the hex box, so a
        // cleared tab reads as "nothing set" instead of showing #FFFFFF as
        // though white had been chosen.
        std::function<void()> showCleared;
    };
    ColorPane buildColorPane(QWidget* parent, const QColor& initialColor,
                             std::function<void(const QColor&)> onChanged);
    ColorPickerOutcome runColorPickerDialog(const QString& title, const QColor& initialColor);

    void showTabColorPickerDialog(int kind);
    void showTileColorPickerDialog(const QString& tileId);   // V4.7
    // V4.6 — multi-select delete
    void toggleSelectMode();
    void clearTileSelection();   // V4.8 — deselects everything, stays in select mode
    void onTileSelectionChanged(const QString& tileId, bool selected);
    void onDeleteAllSelectedRequested();
    void onRemoveMultipleTiles(const QStringList& ids);
    // V4.5 — the shared "Color Picker / Clear Color / Hide Tab" menu,
    // used by both the top bar's right-click and the Manage Tabs grid's
    // right-click. Returns true if anything about the tab layout itself
    // changed (i.e. "Hide Tab" was used), so callers that show their own
    // separate view of the tab grid (Manage Tabs) know to refresh it.
    bool showTabContextMenu(int kind, const QPoint& globalPos);
    void populateActiveKindGrid();
    bool tileMatchesKind(const TileData& td, int kind) const;
    bool kindHasAnyTile(int kind) const;
    void appendTileWidget(const TileData& data);
    void createTileWidgetNoRebuild(const TileData& data);   // v3.3.36 — see mainwindow.cpp for why
    void setInputBusy(bool busy);
    void showPicker(const QList<SearchResult>& results);
    void hidePicker();
    void repositionPicker();
    void notifyTrayApp();
    void cleanupOrphanedFiles();
    void dedupeSharedImages();   // V5.4.26 — one tile, one copy of its own picture
    int  retimeTilesForZone();   // V5.4.28 — re-derive TV air times after a Time Zone change
    void installSmoothScroll(QScrollArea* area);
    void smoothScrollBy(QScrollArea* area, int delta);
    void setupDebugWindow();
    void showDebugWindow();
    void showRecapDialog();      // V5.4.22 — what has actually come out
    void showApiDialog();
    void showRelayKeyDialog();   // V5.4.18 — custom relay URL + key
    void showIgdbCredentialsDialog();
    void showPreferencesDialog();
    void showAboutDialog();   // v3.1.4
    void loadTabLayoutSettings();
    void applyTilesPerRowStretch();   // v3.3.39 — see mainwindow.cpp for why this needs to be its own step
    QList<int> loadTabOrder() const;
    void fireDirectNotification(const TileData& td);

    // v3.3.38 — MAX_COLS is the ceiling used only to pre-allocate column
    // stretch factors upfront (for all 20 possible columns, regardless of
    // the current setting, so nothing needs re-doing when it changes).
    // Actual grid placement uses m_tilesPerRow, the runtime, user-
    // configurable "Tile Size" setting (default 3, loaded in the
    // constructor and reloaded whenever Settings is saved).
    static constexpr int MAX_COLS = 20;
    int m_tilesPerRow = 3;

    QTabWidget*   m_tabs                    = nullptr;
    class TabColorOverlay* m_tabColorOverlay = nullptr;   // V4.5
    // V4.4 — one QColor per TabKind; an invalid/default-constructed QColor
    // means "no custom color set for this kind", the normal starting state.
    QColor        m_tabColors[NUM_KINDS];
    // V4.6 — multi-select delete
    bool          m_selectMode          = false;
    QPushButton*  m_selectModeBtn       = nullptr;
    QPushButton*  m_clearSelectionBtn   = nullptr;   // V4.8 — appears under the ⋮ button once anything is selected
    QWidget*      m_selectModeOverlay   = nullptr;   // V4.8 — covers the bottom search/custom-tile bar while active
    int           m_selectedTileCount   = 0;   // tracked incrementally via onTileSelectionChanged, avoids re-scanning all tiles just to update the button label
    QScrollArea*  m_scrollAreas[NUM_KINDS]   = {};
    QWidget*      m_tabContainers[NUM_KINDS] = {};
    QGridLayout*  m_grids[NUM_KINDS]         = {};

    // v3.1.94 fix #2 — custom "page-based" overflow instead of Qt's default
    // scroll-by-a-bit behavior: when not all tabs fit, only a whole page's
    // worth show at once, and prev/next buttons jump between pages rather
    // than nudging the scroll position.
    QPushButton*  m_tabPrevBtn  = nullptr;
    QPushButton*  m_tabNextBtn  = nullptr;
    QComboBox*    m_tabGroupDropdown = nullptr;
    QWidget*      m_tabsCornerWidget = nullptr;   // V4.12 — for computing available tab-bar width
    QPushButton*  m_manageTabsBtn = nullptr;
    QToolButton*  m_jumpToTabBtn = nullptr;   // v3.3.41 — left-corner "jump to any tab" dropdown
    QListWidget*  m_jumpToTabList = nullptr;   // V4.14 — the actual drag-reorderable list inside that dropdown
    // v3.3.42 — replaces the previous fixed "6 groups x 8 slots" constants
    // (and briefly, a width-measured dynamic system) with values chosen
    // once at startup based on detected monitor resolution — see
    // detectTabResolutionTier(). m_tabManagerRows/Cols describe how a
    // group's m_tabsPerGroup slots are visually arranged in Manage Tabs
    // (e.g. 2 rows of 6, rather than 1 row of 12, at higher resolutions).
    int m_tabGroupCount  = 6;
    int m_tabsPerGroup   = 8;
    int m_tabManagerRows = 1;
    int m_tabManagerCols = 8;
    void detectTabResolutionTier();
    QVector<QVector<int>> m_tabSlots;   // [group][slot] = kind index, or -1 for an empty slot
    int m_currentTabGroup = 0;                        // which group is currently shown
    void refreshJumpToTabMenu();   // v3.3.41 — rebuilds the left-corner "jump to any tab" menu
    void loadTabSlotAssignment();
    void saveTabSlotAssignment();
    void reconcileTabSlots();       // sync slots with enabled/disabled kinds
    QVector<int> flattenTabSlots() const;  // all assigned kinds, group-major slot order
    bool tabGroupHasAnyTab(int group) const;
    int  nextNonEmptyTabGroup(int from) const;
    int  prevNonEmptyTabGroup(int from) const;
    void applyCurrentTabGroup();
    void updateTabBarMaxWidth();   // V4.12 — shrinks tabs to fit before the group arrows get pushed out
    void showManageTabsDialog();

    // Which kinds are checked on in Settings, and the persisted drag order
    // (both the visible ones and any currently-hidden ones, so hidden kinds
    // reappear in a sensible spot once re-enabled or populated).
    QSet<int>  m_enabledKinds;
    // Kind shown at each current QTabWidget index — rebuilt by refreshTabBar().
    QList<int> m_visibleKindOrder;
    // v3.0.1 — only the active tab's grid is ever populated (tiles can match
    // more than one kind, so we filter fresh into whichever tab is current
    // rather than pre-sorting each tile into one exclusive bucket).
    int m_lastPopulatedKind = -1;

    QWidget*     m_bottomBar       = nullptr;
    QLineEdit*   m_searchEdit      = nullptr;
    QPushButton* m_searchBtn       = nullptr;
    QPushButton* m_searchModeBtn   = nullptr;   // v3.3.0 — toggles Movie/TV vs Game search
    QLabel*      m_statusLbl       = nullptr;

    QWidget*     m_pickerFrame     = nullptr;
    QListWidget* m_pickerList      = nullptr;

    QVariantAnimation* m_scrollAnim[NUM_KINDS]   = {};
    int                m_scrollTarget[NUM_KINDS] = {};

    QList<SearchResult> m_currentResults;
    QList<TileData>     m_tiles;
    QList<TileWidget*>  m_tileWidgets;
    // V4.12 fix — for TV shows needing an extra season-scan fetch, the
    // backdrop image download and the season-scan are two independent
    // async operations racing each other. If the image finishes first,
    // onPosterReady() would otherwise find no tile/widget yet (since
    // dataReady() — which actually creates it — hasn't fired) and silently
    // lose the downloaded image path forever. This buffers that update by
    // tile id so createTileWidgetNoRebuild() can apply it the instant the
    // tile actually gets created, regardless of which async op wins.
    QHash<QString, QPair<QString, bool>> m_pendingPosterUpdates;
    TmdbScraper*        m_scraper        = nullptr;
    IgdbScraper*        m_igdbScraper    = nullptr;
    TvmazeScraper*      m_tvmaze         = nullptr;   // V5 — keyless TV source

    // V5 — a non-game search now asks TVmaze (shows) and TMDB (movies) at
    // the same time, so results have to be pooled until both have answered
    // rather than the first reply clobbering the picker.
    QList<SearchResult> m_searchMerged;
    int                 m_searchPending  = 0;
    QString             m_lastSearchQuery;   // V5 — for numeral-based filtering of results

    // V5 — snapshot of what released while the app was closed, taken before
    // the startup refresh can advance a show past it. See the two functions
    // declared in the slots section above.
    // label/date are what was actually SHOWN, so dismissal stamps the
    // release the user saw rather than whatever the tile has drifted to.
    struct MissedRelease { QString tileId; QString line; QDate date; QTime time;
                           QString label; };
    QList<MissedRelease> m_missedAtStartup;
    // V5 — the startup refresh waits behind the recap dialog, so this
    // guards against it running twice (or never, if the dialog is left
    // sitting open).
    bool m_startupRefreshDone = false;
    void runStartupRefresh();
    int                 m_searchGeneration = 0;   // V5 — invalidates the stale-search safety timer

    // V5 — TVmaze is the default TV source. Off only if the user has
    // deliberately turned it off in Settings to use their own TMDB key.
    static bool useTvmazeForTv();
    bool                m_searchModeGame = false;   // v3.3.0 — false = Movie/TV (TMDB), true = Game (IGDB)
    QTimer*             m_globalTick     = nullptr;
    QTimer*             m_dataRefreshTimer = nullptr;  // v3.3.14 — periodic TMDB/IGDB refresh while running
    int                 m_refreshPending = 0;
    // V5.4.12 — the refresh watchdog. m_refreshBatchId is gone with the fixed
    // deadline it existed to invalidate: a watchdog that measures progress
    // can't go stale, because a new batch simply resets the clock.
    QTimer*             m_refreshWatchdog = nullptr;
    QElapsedTimer       m_lastRefreshProgress;   // restarted every time a reply lands
    static constexpr int kRefreshStallMs = 30000;   // give up only after this long with nothing arriving
    QSet<QString>       m_pendingExpiryRefresh;  // v3.3.17 — tile ids awaiting an instant refetch attempt right at episode expiry, before falling back to the local bump
    QTimer*             m_refreshDotsTimer = nullptr;   // V5.4.11 — cycles the status line's trailing dots once a second
    int                 m_refreshDots      = 0;         // 0-3, how many dots are showing right now
    bool                m_motdShown        = false;     // V5.4.12 — the message of the day is a once-per-launch thing
    bool                m_updateCheckDone  = false;     // V5.4.13 — update prompt finished (or never appeared)
    bool                m_tilesLoaded      = false;     // V5.4.13 — the first refresh has completed

    // ── Debug window ──────────────────────────────────────────────────────────
    QDialog*        m_debugWindow    = nullptr;
    QPlainTextEdit* m_debugLog       = nullptr;

    // ── Direct test notification tray icon ────────────────────────────────────
    // A hidden system tray icon owned by the main app, used only to fire
    // test notifications directly — no IPC to TrayApp required.
    QSystemTrayIcon* m_testTray      = nullptr;
};
