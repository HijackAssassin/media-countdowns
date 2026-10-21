#pragma once
#include <QWidget>
#include <QLabel>
#include <QTimer>
#include "outlinedlabel.h"
#include "countdownwidget.h"
#include "tileoutline.h"
#include "tiledata.h"

class TileWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TileWidget(const TileData& data, QWidget* parent = nullptr);
    ~TileWidget() override;

    const TileData& tileData() const { return m_data; }
    void updateData(const TileData& data);
    void refreshImage();
    void refreshDisplayPrefs() { refreshOverlays(); }   // v3.0.2 — Settings → Tile Display changed

    // Called by the shared static timer — only repaints if on visible tab
    void tick(bool tabVisible);

    // V4.6 — multi-select delete. Select mode changes what a left-click and
    // right-click on this tile do (toggle selection / show a "Delete All"
    // menu) instead of their normal behavior (open edit dialog / full menu).
    void setSelectMode(bool on);
    bool isSelectMode() const { return m_selectMode; }
    void setSelected(bool selected);
    bool isSelected() const { return m_selected; }
    // V4.12 — tells this tile whether ANYTHING is currently selected across
    // the whole grid, kept in sync by MainWindow. Used purely so the
    // select-mode context menu can hide "Delete All" when nothing is
    // selected, rather than showing an action with nothing to act on.
    void setAnySelected(bool any) { m_anySelected = any; }
    // V4.7 — per-tile color tag, purely visual (thin border). Selection
    // (above) always takes priority over the tag color whenever both would
    // apply, same pattern as the tab bar's own selected-vs-colored rule.
    void setTagColor(const QColor& color);
    QColor tagColor() const { return m_data.tagColor; }
    // V5.4.15 — the tile's text colour, alongside the outline colour above.
    void setTextColor(const QColor& color);
    QColor textColor() const { return m_data.textColor; }
    // Live preview for the Customize dialog. These paint the tile WITHOUT
    // touching m_data or emitting tileDataChanged, so nothing is saved and
    // Cancel has something to restore to — the dialog simply previews the
    // original colours back. Committing goes through the setters above.
    void previewTagColor(const QColor& color);
    void previewTextColor(const QColor& color);
    // V4.11 — favorite: a gold star in the top-right corner, purely visual.
    void setFavorite(bool fav);
    bool isFavorite() const { return m_data.isFavorite; }

signals:
    void imageChanged(const QString& tileId, const QString& newImagePath);
    void tileDataChanged(const QString& tileId);
    void removeTile(const QString& tileId);
    void duplicateTile(const QString& tileId);
    void refetchRequested(const QString& tileId);
    void forceImageRefetchRequested(const QString& tileId);   // v3.1.0 — "Refetch Image" button
    void testNotification(const QString& tileId);
    void selectionChanged(const QString& tileId, bool selected);   // V4.6
    void deleteAllSelectedRequested();                              // V4.6
    void colorPickerRequested(const QString& tileId);               // V4.7

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;   // V5.4.18 — see refreshTextStyle

private:
    void buildUi();
    void applyImage(const QString& path);
    void recomputeTargetEpoch();   // pre-compute once; tick() just decrements
    void refreshCountdown();
    void refreshOverlays();
    void layoutOverlays();
    void openEditDialog();
    void deleteBackdropIfOwned(const QString& path);
    void fitOverlayFont(OutlinedLabel* label, const QString& text, int startPt);
    void invalidateFontCache();
    void refreshBorderStyle();   // V4.7 — shared by setSelected/setTagColor; selection always wins
    void applyTextColor(const QColor& color);   // V5.4.15 — countdown + title in one place
    // V5.4.18 — re-applies the SAVED text colour, mirroring refreshBorderStyle.
    // See the comment on its definition for why the palette can't be trusted
    // to survive on its own.
    void refreshTextStyle();
    // V5.4.9 — the container's stylesheet, always id-scoped so nothing in it
    // cascades onto the labels inside the tile and shifts them.
    static QString containerStyle();

    QString extractShowName()  const;
    QString formatEpisodeTag() const;
    QString formatTitleLine()  const;

    TileData         m_data;
    QWidget*         m_imageContainer  = nullptr;
    bool             m_selectMode      = false;   // V4.6
    bool             m_selected        = false;   // V4.6
    bool             m_anySelected     = false;   // V4.12 — is ANYTHING selected across the whole grid
    QLabel*          m_imageLabel      = nullptr;
    CountdownWidget* m_countdownWidget = nullptr;
    OutlinedLabel*   m_titleOverlay    = nullptr;
    TileOutline*     m_outline         = nullptr;   // V5.4.13 — the colour tag, above everything
    OutlinedLabel*   m_favoriteStar    = nullptr;   // V4.11
    int              m_cachedTitleFontPt = -1;
    QPixmap          m_cachedPixmap;

    // ── Pre-computed countdown ────────────────────────────────────────────────
    // Calculated once when data is set or edited; decremented by 1 each tick.
    // Avoids a full QDateTime diff every second for every tile.
    qint64  m_targetEpoch    = 0;   // target as secs-since-epoch
    qint64  m_remainingSecs  = 0;   // counts down; recomputed when data changes

    static constexpr int TITLE_PT = 18;

    // ── Shared static timer — ONE timer drives ALL tile instances ─────────────
    static QTimer*           s_sharedTimer;
    static QList<TileWidget*> s_allTiles;
    static void ensureSharedTimer();
};
