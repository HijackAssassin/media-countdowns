#include "tilewidget.h"
#include "edittiledialog.h"
#include "outlinedlabel.h"
#include "countdownwidget.h"
#include "applogger.h"
#include "tiledisplayprefs.h"
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QResizeEvent>
#include <QMenu>
#include <QPixmap>
#include <QDateTime>
#include <QFontMetrics>
#include <QFile>
#include <QApplication>

// ── Static member definitions ─────────────────────────────────────────────────
QTimer*            TileWidget::s_sharedTimer = nullptr;
QList<TileWidget*> TileWidget::s_allTiles;

// =============================================================================
TileWidget::TileWidget(const TileData& data, QWidget* parent)
    : QWidget(parent), m_data(data)
{
    s_allTiles.append(this);
    recomputeTargetEpoch();   // pre-compute once on construction
    buildUi();
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth(160);
}

TileWidget::~TileWidget()
{
    s_allTiles.removeOne(this);
}

void TileWidget::buildUi()
{
    auto* vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(0,0,0,0);
    vlay->setSpacing(0);

    m_imageContainer = new QWidget(this);
    // The object name is what lets refreshBorderStyle() address this widget
    // ALONE — see the reasoning there.
    m_imageContainer->setObjectName("tileImageContainer");
    m_imageContainer->setStyleSheet(containerStyle());

    m_imageLabel = new QLabel(m_imageContainer);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setStyleSheet("background:#000;");
    applyImage(m_data.imagePath);

    m_countdownWidget = new CountdownWidget(m_imageContainer);
    m_titleOverlay    = new OutlinedLabel(m_imageContainer);
    m_titleOverlay->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_titleOverlay->setWordWrap(false);
    m_titleOverlay->setAttribute(Qt::WA_TranslucentBackground);
    m_titleOverlay->setAutoFillBackground(false);

    // V4.11 — favorite star, top-right corner. Gold fill via the palette
    // (OutlinedLabel always paints WindowText with a black outline behind
    // it), same visibility-over-any-background reasoning as the title text.
    m_favoriteStar = new OutlinedLabel(QString::fromUtf8("\xe2\x98\x85"), m_imageContainer);   // ★
    m_favoriteStar->setAlignment(Qt::AlignCenter);
    {
        QPalette pal = m_favoriteStar->palette();
        pal.setColor(QPalette::WindowText, QColor("#FFD700"));
        m_favoriteStar->setPalette(pal);
    }
    m_favoriteStar->setVisible(m_data.isFavorite);

    // V5.4.13 — created last and raised in layoutOverlays(), so the colour tag
    // is above the artwork rather than behind it.
    m_outline = new TileOutline(m_imageContainer);

    vlay->addWidget(m_imageContainer);
    refreshOverlays();
    refreshBorderStyle();   // V4.7 — in case this tile was loaded with an existing tagColor already set
    applyTextColor(m_data.textColor);   // V5.4.15 — same, for a saved text colour
}

// =============================================================================
//  recomputeTargetEpoch — called once when tile data is set or changed.
//  Stores the target as seconds-since-epoch so tick() can simply decrement
//  m_remainingSecs by 1 each second instead of doing a full datetime diff.
// =============================================================================
void TileWidget::recomputeTargetEpoch()
{
    if (!m_data.hasDate()) {
        m_targetEpoch   = 0;
        m_remainingSecs = 0;
        return;
    }
    QTime t = m_data.effectiveTime().isValid() ? m_data.effectiveTime() : QTime(0, 0, 0);
    QDateTime target(m_data.effectiveDate(), t, Qt::LocalTime);
    m_targetEpoch   = target.toSecsSinceEpoch();
    // Compute remaining once accurately using the actual current time
    m_remainingSecs = qMax(qint64(0),
        m_targetEpoch - QDateTime::currentDateTimeUtc().toSecsSinceEpoch()
            + QDateTime::currentDateTime().offsetFromUtc());
    // Simpler: just use secsTo on the local datetime
    m_remainingSecs = qMax(qint64(0), QDateTime::currentDateTime().secsTo(target));
}

// =============================================================================
//  tick() — called by MainWindow::onGlobalTick() once per second.
//  Recomputes remaining seconds from the real wall clock every tick.
//  The Qt 1000ms timer drifts — decrementing causes displayed seconds to
//  fall behind real time. One secsTo() call per tile per second is negligible.
// =============================================================================
void TileWidget::tick(bool tabVisible)
{
    if (!m_data.hasDate() || m_data.isExpired()) return;
    if (!tabVisible) return;

    m_remainingSecs = qMax(qint64(0),
        QDateTime::currentDateTime().secsTo(
            QDateTime(m_data.effectiveDate(),
                      m_data.effectiveTime().isValid()
                          ? m_data.effectiveTime() : QTime(0, 0, 0))));

    m_countdownWidget->setSeconds(m_remainingSecs);
}

void TileWidget::refreshCountdown()
{
    if (!m_data.hasDate()) {
        m_countdownWidget->setNoDate(); return;
    }
    if (m_data.isExpired()) {
        m_countdownWidget->setExpired(); return;
    }
    // Re-sync from real time (called on data change / full refresh, not every tick)
    recomputeTargetEpoch();
    m_countdownWidget->setSeconds(m_remainingSecs);
}

void TileWidget::refreshOverlays()
{
    // Full refresh: update countdown + rebuild title + recalc font + layout
    // Only call this when data actually changes, not on every tick.
    refreshCountdown();
    refreshTextStyle();   // V5.4.18 — the palette may have been re-resolved away
    QString titleLine = formatTitleLine();
    m_titleOverlay->setText(titleLine);
    m_cachedTitleFontPt = -1;   // force recalc since title content changed
    fitOverlayFont(m_titleOverlay, titleLine, TITLE_PT);
    layoutOverlays();
}

// =============================================================================
void TileWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!m_imageContainer) return;
    int w = event->size().width();
    int imgH = w * 9 / 16;
    m_imageContainer->setFixedHeight(imgH);
    m_imageLabel->setGeometry(0, 0, w, imgH);
    if (!m_cachedPixmap.isNull()) {
        m_imageLabel->setPixmap(
            m_cachedPixmap.scaled(w, imgH, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    }
    // Width changed — invalidate cached font size so it recalculates
    m_cachedTitleFontPt = -1;
    fitOverlayFont(m_titleOverlay, m_titleOverlay->text(), TITLE_PT);
    layoutOverlays();
}

void TileWidget::layoutOverlays()
{
    if (!m_imageContainer) return;
    int w    = m_imageContainer->width();
    int imgH = m_imageContainer->height();
    if (w <= 0 || imgH <= 0) return;
    int tH = qMax(m_titleOverlay->sizeHint().height(), imgH / 6);
    m_countdownWidget->setGeometry(0, 0, w, imgH);
    m_countdownWidget->scaleFonts(w);

    // V5.4.13 — the title starts after the colour tag rather than under it.
    // At x=0 its first letter sat beneath the edge and its glow, so a tagged
    // tile read as "y Adventures with Superman". The inset is applied whether
    // or not a tag is set, so switching a colour on and off never re-wraps or
    // re-fits the title — it stays in exactly the same place.
    const int inset = TileOutline::kEdge + 2;
    m_titleOverlay->setGeometry(inset, imgH - tH, w - inset * 2, tH);
    m_titleOverlay->raise();

    if (m_outline) {
        m_outline->setGeometry(0, 0, w, imgH);
        m_outline->raise();   // last raise wins — the tag sits above the title too
    }

    // V4.11 — favorite star, top-right corner, sized relative to tile width
    // so it looks reasonable across the app's full tile-size range.
    int starSize = qBound(16, w / 10, 32);
    int margin = qMax(4, w / 40);
    m_favoriteStar->setGeometry(w - starSize - margin, margin, starSize, starSize);
    QFont starFont = m_favoriteStar->font();
    starFont.setPointSize(qMax(8, starSize * 3 / 4));
    m_favoriteStar->setFont(starFont);
    m_favoriteStar->raise();
}

void TileWidget::fitOverlayFont(OutlinedLabel* label, const QString& text, int startPt)
{
    if (!label || !m_imageContainer) return;

    // Use cached size if available (avoid expensive recalc every second)
    int sz = m_cachedTitleFontPt;

    if (sz < 0) {
        // First calculation — bold, so measure with bold
        int availW = m_imageContainer->width() - 20;
        sz = startPt;
        if (availW > 0) {
            QFont f = label->font(); f.setBold(true); f.setPointSize(sz);
            while (sz > 7 && QFontMetrics(f).horizontalAdvance(text) > availW) {
                --sz; f.setPointSize(sz);
            }
        }
        m_cachedTitleFontPt = sz;
    }

    // Only call setFont when the size actually changed
    QFont f = label->font();
    if (f.pointSize() != sz || !f.bold()) {
        f.setBold(true);
        f.setPointSize(sz);
        label->setFont(f);
        QPalette pal = label->palette();
        // V5.4.15 — the tile's own text colour, not a hardcoded white. This
        // runs on every resize and every data change, so hardcoding it here
        // silently undid the setting a moment after it was applied.
        pal.setColor(QPalette::WindowText,
                     m_data.textColor.isValid() ? m_data.textColor : QColor(Qt::white));
        label->setPalette(pal);
    }
}

void TileWidget::invalidateFontCache()
{
    m_cachedTitleFontPt = -1;
}

// =============================================================================
QString TileWidget::extractShowName() const
{
    QString t = m_data.displayTitle();
    int bullet = t.indexOf(" \xe2\x80\xa2 ");
    return (bullet >= 0) ? t.left(bullet) : t;
}

// =============================================================================
//  formatEpisodeTag — returns the status label in compact S##E## notation,
//  shaped by the Settings → Tile Display preferences (Season/Episode/Total
//  Episodes can each be toggled independently).
//
//  The scraper stores statusLabel as "S02E01" for a single episode, or
//  "S02E01+E02+E03" when multiple episodes share the same air date — we
//  collapse that run into a range for display.
//
//  Examples:
//    "S2E1"        + Season+Episode  → "S02E01"
//    "S2E1"        + Episode only    → "E01"
//    "S2E1"        + Season only     → "S02"
//    "S2E1" +Total (season has 8 eps)→ "S02E01/E08"
//    "S5E1+E2+E3+E4" + Season+Episode→ "S05E01-E04"
//    "Last Episode"                  → ""  (non-episode labels are filtered upstream)
// =============================================================================
QString TileWidget::formatEpisodeTag() const
{
    QString sl = m_data.statusLabel;
    if (sl.isEmpty() || !sl.startsWith('S') || !sl.contains('E')) return {};

    bool showSeason  = TileDisplayPrefs::showSeason();
    bool showEpisode = TileDisplayPrefs::showEpisode();
    bool showTotal   = TileDisplayPrefs::showTotalEpisodes(); // already gated on showEpisode()
    if (!showSeason && !showEpisode) return {};

    int eIdx   = sl.indexOf('E');
    int season = sl.mid(1, eIdx - 1).toInt();
    QString epPart = sl.mid(eIdx + 1);  // everything after the first 'E'
    QString seasonStr = QString("S%1").arg(season, 2, 10, QChar('0'));

    QString epStr;
    if (epPart.contains('+')) {
        // Multi-episode e.g. "01+E02+E03+E04" — collapse to a range: only
        // the first and last episode numbers matter for display, not every
        // one in between.
        QStringList parts = epPart.split('+');
        QString firstStr = parts.first();
        QString lastStr  = parts.last();
        if (firstStr.startsWith('E')) firstStr = firstStr.mid(1);
        if (lastStr.startsWith('E'))  lastStr  = lastStr.mid(1);
        int firstNum = firstStr.toInt();
        int lastNum  = lastStr.toInt();
        epStr = QString("E%1").arg(firstNum, 2, 10, QChar('0'));
        if (lastNum > firstNum)
            epStr += QString("-E%1").arg(lastNum, 2, 10, QChar('0'));
    } else {
        epStr = QString("E%1").arg(epPart.toInt(), 2, 10, QChar('0'));
    }
    // "Total Episodes" tacks the season's total onto the end of the range
    if (showTotal && m_data.seasonEpisodeCount > 0)
        epStr += QString("/E%1").arg(m_data.seasonEpisodeCount, 2, 10, QChar('0'));

    if (showSeason && showEpisode) return seasonStr + epStr;
    if (showEpisode)               return epStr;
    return seasonStr;   // Season only — episode number omitted
}

// =============================================================================
//  formatTitleLine — builds the tile's overlay text from the Settings →
//  Tile Display preferences: Title, Year, Season/Episode (via
//  formatEpisodeTag), Weekday, and Date.
// =============================================================================
QString TileWidget::formatTitleLine() const
{
    QString name = extractShowName();

    QString titlePart;
    if (TileDisplayPrefs::showTitle()) titlePart = name;
    if (TileDisplayPrefs::showYear() && m_data.releaseYear > 0)
        titlePart += QString("(%1)").arg(m_data.releaseYear);

    if (!m_data.hasDate()) {
        QString noDateMsg = TileDisplayPrefs::showDate() ? "No Release Date" : QString();
        if (titlePart.isEmpty()) return noDateMsg;
        if (noDateMsg.isEmpty()) return titlePart;
        return titlePart + "  \xe2\x80\xa2  " + noDateMsg;
    }

    QString epTag = (m_data.mediaType == "tv") ? formatEpisodeTag() : QString();

    QString dateSeg;
    // V5 — a month-only date has no known day, so it has no known weekday
    // either. targetDate holds the last of the month purely as a countdown
    // bound; printing "Wednesday" off that would state something the source
    // never said. V5.4 — a year-only window ("2026") is the same, one step
    // coarser, so both are covered by isWindowDate().
    if (TileDisplayPrefs::showWeekday() && !m_data.isWindowDate()) {
        QDate d = m_data.effectiveDate();
        if (d.isValid()) dateSeg = d.toString("dddd");
    }
    if (TileDisplayPrefs::showDate()) {
        QString dateText = m_data.displayDate();
        dateSeg = dateSeg.isEmpty() ? dateText : (dateSeg + ", " + dateText);
        // v3.1.0 Feature 3 — flag guessed/lapsed episode dates right after
        // the date itself, per spec.
        //
        // V5.4.2 — a month or year window is flagged the same way. "2026" is
        // a real announcement, but the DAY it resolves to is not: targetDate
        // holds December 31 only as a countdown bound. Marking it keeps the
        // tile honest about the difference between "we know when" and "we
        // know roughly when", which is the same thing (Estimated) already
        // meant for a guessed episode date.
        if (!dateSeg.isEmpty()) {
            if (m_data.isEstimatedDate || m_data.isWindowDate())
                dateSeg += " (Estimated)";
            else if (m_data.inMidSeasonBreak)
                dateSeg += " (Mid-Season Break)";
        }
    }

    // V5.4 — the release time, as its own segment rather than glued onto the
    // date, so it still shows when Date is switched off.
    //
    // Never for a window date: "2026" has no time of day, and effectiveTime()
    // would happily supply the Time Zone default, printing a clock reading
    // against December 31 that no source ever stated.
    QString timeSeg;
    if (TileDisplayPrefs::showTime() && !m_data.isWindowDate()) {
        QTime t = m_data.effectiveTime();
        if (t.isValid()) timeSeg = t.toString("h:mm AP");
    }

    const QString dot = "  \xe2\x80\xa2  ";
    QString line = titlePart;
    if (!epTag.isEmpty())   line += (line.isEmpty() ? "" : " ")   + epTag;
    if (!dateSeg.isEmpty()) line += (line.isEmpty() ? "" : dot)   + dateSeg;
    if (!timeSeg.isEmpty()) line += (line.isEmpty() ? "" : dot)   + timeSeg;
    return line;
}

// =============================================================================
void TileWidget::setSelectMode(bool on)
{
    m_selectMode = on;
    if (!on) setSelected(false);   // exiting select mode always clears any selection
    setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void TileWidget::setSelected(bool selected)
{
    if (m_selected == selected) return;
    m_selected = selected;
    refreshBorderStyle();
}

void TileWidget::setTagColor(const QColor& color)
{
    if (m_data.tagColor == color) return;
    m_data.tagColor = color;
    refreshBorderStyle();
    emit tileDataChanged(m_data.id);   // keeps MainWindow's m_tiles in sync, same as any other data edit
}

void TileWidget::setTextColor(const QColor& color)
{
    if (m_data.textColor == color) return;
    m_data.textColor = color;
    applyTextColor(color);
    emit tileDataChanged(m_data.id);   // keeps MainWindow's m_tiles in sync
}

// Preview only — deliberately does NOT touch m_data or emit anything, so the
// Customize dialog can show a colour on the real tile without committing it.
void TileWidget::previewTagColor(const QColor& color)
{
    if (!m_outline) return;
    if (m_selected) return;              // selection still wins while it is on
    m_outline->setColor(color);
}

void TileWidget::previewTextColor(const QColor& color)
{
    applyTextColor(color);
}

// The one place the text colour reaches the widgets, so preview and commit
// can never render differently from one another.
// =============================================================================
//  refreshTextStyle — V5.4.18. Puts the tile's SAVED text colour back on the
//  widgets, and the counterpart to refreshBorderStyle().
//
//  The outline and the text were not equally robust, and that is the whole bug
//  behind "the text colour reverts when I press Save":
//
//   • The outline colour lives in TileOutline's own member and is painted from
//     it. Nothing can take it away.
//   • The text colour lived ONLY in Qt palettes. Saving emits tileDataChanged,
//     which rebuilds the grid, and populateActiveKindGrid() re-parents every
//     tile (setParent + show). Re-parenting re-polishes a widget and re-resolves
//     its palette against the new parent, which dropped the colour — so the
//     text went back to white the instant it was saved. Cancel emits nothing,
//     no rebuild happens, and the colour survives: that is exactly why
//     Clear-then-Cancel appeared to "fix" it.
//
//  So the palette is now treated as a display detail that can be lost at any
//  time, and m_data.textColor as the truth it is restored from — on every data
//  change, every layout pass, and every show (which is what a re-parent ends
//  with).
// =============================================================================
void TileWidget::refreshTextStyle()
{
    applyTextColor(m_data.textColor);
}

void TileWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // A re-parent ends in show(), and that is where the palette was being lost.
    refreshTextStyle();
}

void TileWidget::applyTextColor(const QColor& color)
{
    if (m_countdownWidget) m_countdownWidget->setTextColor(color);
    if (m_titleOverlay) {
        QPalette pal = m_titleOverlay->palette();
        pal.setColor(QPalette::WindowText, color.isValid() ? color : QColor(Qt::white));
        m_titleOverlay->setPalette(pal);
    }
}

void TileWidget::setFavorite(bool fav)
{
    if (m_data.isFavorite == fav) return;
    m_data.isFavorite = fav;
    m_favoriteStar->setVisible(fav);
    emit tileDataChanged(m_data.id);
}

// =============================================================================
//  containerStyle — the image container's stylesheet, always addressed by id.
//
//  V5.4.13 — this no longer carries any border at all; the colour tag is now
//  TileOutline, a widget stacked above the artwork (see tileoutline.h for why
//  a stylesheet border could not work here). The id selector stays: the rule
//  that remains is a background, and an unscoped background cascades onto
//  every child just as readily as a border did.
//
//  V5.4.9 — every rule here is written as "#tileImageContainer { … }" rather
//  than bare declarations, because a Qt stylesheet applies to the widget AND
//  everything inside it. A bare "border: 4px solid" therefore handed a 4px box
//  model to every single OutlinedLabel in the tile: the countdown digits, the
//  unit labels under them and the title line. Nothing LOOKED bordered —
//  OutlinedLabel paints its own cached pixmap and never calls
//  QLabel::paintEvent, so the border was never drawn — but the labels' size
//  hints grew by 8px all the same, and the countdown grid re-laid itself out
//  around them. Measured on a 400px tile: the unit labels dropped 4px and the
//  day number rose 4px the moment a colour was picked, and went back when it
//  was cleared. That is the "adding a colour nudges the countdown" report.
//
//  An id selector matches this widget only, so the border is drawn in exactly
//  the same place and nothing inside the tile moves at all.
// =============================================================================
QString TileWidget::containerStyle()
{
    return "#tileImageContainer { background:#000; }";
}

void TileWidget::refreshBorderStyle()
{
    // Selection (temporary, only in select mode) always wins over a persistent
    // tag colour, so it's never ambiguous which tiles are currently selected
    // for a batch delete.
    //
    // V4.10 — reverted the V4.9 hue-cycle/pulse animation entirely per direct
    // feedback that it looked bad in practice. V5.4.13 keeps that: the tag has
    // a gradient edge and an inward glow so it stands out from across the
    // grid, but it does not move. Everything here renders once into a cached
    // pixmap, so a tagged tile costs the same per second as an untagged one.
    if (!m_outline) return;
    if (m_selected)                     m_outline->setColor(QColor("#0078d4"));
    else if (m_data.tagColor.isValid()) m_outline->setColor(m_data.tagColor);
    else                                m_outline->setColor(QColor());   // invalid = no tag
}

void TileWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_selectMode) {
            setSelected(!m_selected);
            emit selectionChanged(m_data.id, m_selected);
        } else {
            openEditDialog();
        }
    }
    QWidget::mousePressEvent(event);
}

void TileWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (m_selectMode) {
        if (!m_anySelected) return;   // V4.12 — nothing to act on, don't show a menu at all
        QMenu menu(this);
        menu.setStyleSheet(
            "QMenu { background:#242424; color:#eee; border:1px solid #444; }"
            "QMenu::item:selected { background:#0078d4; }");
        QAction* deleteAllAct = menu.addAction("Delete All");
        QAction* chosen = menu.exec(event->globalPos());
        if (chosen == deleteAllAct) {
            // V4.11 fix — the right-clicked tile is very likely one of the
            // tiles about to be deleted (especially with just 1 selected,
            // since right-clicking that same tile is the natural thing to
            // do). A synchronous emit here would let onRemoveMultipleTiles()
            // delete this widget while its own contextMenuEvent() is still
            // on the call stack — exactly the same class of crash the
            // Remove Tile action below already guards against. Queueing
            // defers it until after this event fully unwinds.
            QMetaObject::invokeMethod(this, [this]{ emit deleteAllSelectedRequested(); },
                                      Qt::QueuedConnection);
        }
        return;
    }

    QMenu menu(this);
    auto* editAct   = menu.addAction("Edit Tile");
    auto* favAct    = menu.addAction(m_data.isFavorite ? "Remove from Favorites" : "Add to Favorites");   // V4.11
    auto* dupAct    = menu.addAction("Duplicate Tile");
    // V5.4.15 — one entry, not three. "Color Picker" and "Clear Color" both
    // meant the outline only; now that a tile has an outline colour AND a text
    // colour, listing every combination in the context menu would be four
    // entries for what is really one idea. Customize Colors opens both on tabs,
    // each with its own Clear.
    auto* colorAct  = menu.addAction("Customize Colors");
    menu.addSeparator();
    auto* removeAct = menu.addAction("Remove Tile");
    menu.addSeparator();
    auto* testAct   = menu.addAction("Test Notification");
    QAction* chosen = menu.exec(event->globalPos());

    if (chosen == editAct) {
        openEditDialog();
    } else if (chosen == favAct) {
        setFavorite(!m_data.isFavorite);
    } else if (chosen == colorAct) {
        QString id = m_data.id;
        QMetaObject::invokeMethod(this, [this, id]{ emit colorPickerRequested(id); },
                                  Qt::QueuedConnection);
    } else if (chosen == dupAct) {
        QString id = m_data.id;
        QMetaObject::invokeMethod(this, [this, id]{ emit duplicateTile(id); },
                                  Qt::QueuedConnection);
    } else if (chosen == removeAct) {
        QString id = m_data.id;
        QMetaObject::invokeMethod(this, [this, id]{ emit removeTile(id); },
                                  Qt::QueuedConnection);
    } else if (chosen == testAct) {
        emit testNotification(m_data.id);
    }
}

void TileWidget::openEditDialog()
{
    EditTileDialog dlg(m_data, this);

    // Wire the "Test Notification" button inside the dialog to our signal.
    // Since dlg.exec() runs its own event loop, signals from the dialog ARE
    // processed — this connection fires while the dialog is still open.
    connect(&dlg, &EditTileDialog::testNotificationRequested,
            this, [this]{ emit testNotification(m_data.id); });

    const int editResult = dlg.exec();

    // V5.4.19 — "Customize Colors" closes this dialog and opens the colour menu
    // for the same tile. Queued, because the colour dialog runs its own event
    // loop and this one is still unwinding: opening it inline would nest a
    // second modal loop inside a dialog that is mid-close.
    //
    // V5.4.20 — it can now arrive here ACCEPTED, when the user answered "Save"
    // to the unsaved-changes prompt. So the colour menu is queued at the end
    // rather than returned to immediately, and an accepted dialog still falls
    // through to the code below that applies the edits. Rejected means either
    // nothing was changed or the user chose not to keep it, and the apply step
    // is skipped exactly as it would be for Cancel.
    const bool openColorsAfter = dlg.wantsCustomizeColors();
    auto queueColorMenu = [this, openColorsAfter]() {
        if (!openColorsAfter) return;
        QString id = m_data.id;
        QMetaObject::invokeMethod(this, [this, id]{ emit colorPickerRequested(id); },
                                  Qt::QueuedConnection);
    };

    if (editResult != QDialog::Accepted) { queueColorMenu(); return; }

    if (dlg.wantsRemove()) {
        // IMPORTANT: Never emit removeTile synchronously here.
        // We're inside mousePressEvent → openEditDialog. A direct emit would
        // call onRemoveTile → delete this, then execution returns to this
        // method and then to mousePressEvent on a destroyed object → crash.
        // Queueing defers the deletion until after the current event fully
        // unwinds, which is safe.
        // No colour menu here even if it was asked for: the tile is going away.
        QString id = m_data.id;
        QMetaObject::invokeMethod(this, [this, id]{ emit removeTile(id); },
                                  Qt::QueuedConnection);
        return;
    }

    bool changed = false;
    QString newTitle      = dlg.customTitle();
    QString effectiveCustom = (newTitle == m_data.title) ? QString() : newTitle;
    if (effectiveCustom != m_data.customTitle) { m_data.customTitle = effectiveCustom; changed = true; }

    QTime newTime = dlg.customAirTime();
    if (newTime != m_data.customAirTime) { m_data.customAirTime = newTime; changed = true; }

    // V5 — Season/Episode override. Only applied when the numbers were
    // actually moved, so simply opening the dialog never rewrites a label
    // that came from the data source. The next fetched date overwrites this
    // like any other scraped field, which is the intended behaviour: it's a
    // stopgap for a season the source hasn't published yet, not a permanent
    // edit. Clearing seasonEpisodeCount alongside it stops the tile showing
    // a stale "/08" total belonging to the previous season.
    if (dlg.seasonEpisodeWasReset()) {
        // Boxes match what the source reported — drop the override and put
        // the official numbers back. No refetch needed: they're stored on
        // the tile precisely so this is instant.
        if (m_data.episodeOverride) {
            if (!m_data.officialStatusLabel.isEmpty()) {
                m_data.statusLabel        = m_data.officialStatusLabel;
                m_data.seasonEpisodeCount = m_data.officialSeasonEpisodeCount;
            }
            m_data.episodeOverride = false;
            changed = true;
        }
    } else if (dlg.seasonEpisodeChanged()) {
        QString overridden = dlg.overriddenStatusLabel();
        int     total      = dlg.overriddenEpisodeTotal();
        if (m_data.statusLabel != overridden || m_data.seasonEpisodeCount != total) {
            m_data.statusLabel        = overridden;
            m_data.seasonEpisodeCount = total;
            // Marks these as hand-typed so a refresh — including the one
            // triggered by moving between Countdowns and Released — leaves
            // them alone until the source genuinely has a new date.
            m_data.episodeOverride    = true;
            changed = true;
        }
    }

    QDate newDate = dlg.customDate();
    bool wantsDate = dlg.dateChecked();  // reliable — tracks actual user intent
    bool hadDate   = m_data.hasDate() || m_data.noDateOverride;

    bool dateChanged = (!wantsDate && hadDate)            // user removed date
                    || (wantsDate && m_data.noDateOverride)  // user re-enabled date
                    || (wantsDate && newDate != m_data.customDate);  // date value changed

    if (dateChanged) {
        if (!wantsDate) {
            m_data.noDateOverride = true;
            m_data.customDate     = QDate();
            m_data.customDateStr  = "";
        } else {
            m_data.noDateOverride = false;
            m_data.customDate     = newDate;
            m_data.customDateStr  = dlg.customDateStr();
        }
        changed = true;
    }

    // Loop fields
    if (m_data.isLooped      != dlg.isLooped())       { m_data.isLooped      = dlg.isLooped();       changed = true; }
    if (m_data.loopInterval  != dlg.loopInterval())   { m_data.loopInterval  = dlg.loopInterval();   changed = true; }
    if (m_data.loopWeekday   != dlg.loopWeekday())    { m_data.loopWeekday   = dlg.loopWeekday();    changed = true; }
    if (m_data.loopDayOfMonth!= dlg.loopDayOfMonth()) { m_data.loopDayOfMonth= dlg.loopDayOfMonth(); changed = true; }
    if (m_data.presetType    != dlg.presetType())     { m_data.presetType    = dlg.presetType();     changed = true; }
    if (m_data.mediaType     != dlg.mediaType())      { m_data.mediaType     = dlg.mediaType();       changed = true; }

    // v3.1.0 — multi-image support: compare the FULL image state (fetched
    // slot + custom history + which one is active), not just a single path.
    QString newActivePath  = dlg.selectedImagePath();
    QString newFetchedPath = dlg.fetchedImagePath();
    QStringList newCustomPaths = dlg.customImagePaths();

    bool imagesChanged = (newActivePath  != m_data.imagePath)
                       || (newFetchedPath != m_data.fetchedImagePath)
                       || (newCustomPaths != m_data.customImagePaths);

    if (imagesChanged) {
        QStringList oldAll = m_data.allImagePaths();
        m_data.fetchedImagePath = newFetchedPath;
        m_data.customImagePaths = newCustomPaths;
        m_data.imagePath        = newActivePath;
        QStringList newAll = m_data.allImagePaths();

        // Anything that was known before but isn't anymore was explicitly
        // deleted (the red X) during this session — safe to remove from
        // disk now that Save has actually been confirmed.
        for (const QString& oldPath : oldAll)
            if (!newAll.contains(oldPath)) deleteBackdropIfOwned(oldPath);

        applyImage(newActivePath);
        changed = true;
    }

    if (changed) {
        refreshOverlays();
        emit tileDataChanged(m_data.id);
    }
    if (dlg.anyResetPressed())       emit refetchRequested(m_data.id);
    if (dlg.wantsForcedRefetch())    emit forceImageRefetchRequested(m_data.id);

    queueColorMenu();   // V5.4.20 — edits are saved; now show the colour menu
}

// =============================================================================
void TileWidget::applyImage(const QString& path)
{
    if (!m_imageLabel) return;
    if (path.isEmpty()) {
        m_cachedPixmap = QPixmap();
        m_imageLabel->clear(); m_imageLabel->setStyleSheet("background:#000;"); return;
    }
    QPixmap px(path);
    if (px.isNull()) { m_cachedPixmap = QPixmap(); m_imageLabel->clear(); return; }
    m_cachedPixmap = px;   // cache the full-res pixmap — resize uses this, no more disk hits
    int w = m_imageLabel->width()  > 0 ? m_imageLabel->width()  : 400;
    int h = m_imageLabel->height() > 0 ? m_imageLabel->height() : 225;
    m_imageLabel->setPixmap(
        m_cachedPixmap.scaled(w, h, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    m_imageLabel->setStyleSheet("background:#000;");
}

void TileWidget::deleteBackdropIfOwned(const QString& path)
{
    if (path.isEmpty()) return;
    if (!(path.contains("/fetched_images/") || path.contains("\\fetched_images\\") ||
          path.contains("/custom_images/")  || path.contains("\\custom_images\\")))
        return;   // not ours to delete

    // V5.4.25 — never delete a file another tile is still showing.
    //
    // Duplicating a tile copies its images now (V5.4.23), so nothing NEW
    // shares a file. But tiles duplicated before that still do, and this is
    // the function that removes the file: without this check, deleting one of
    // those old twins still blanks the other, which is exactly the bug that
    // was reported. Cheap, and it makes the rule explicit rather than relying
    // on nothing ever sharing a path again.
    for (const TileWidget* other : std::as_const(s_allTiles)) {
        if (other == this) continue;
        if (other->m_data.allImagePaths().contains(path)) return;
    }

    QFile::remove(path);
}

void TileWidget::refreshImage() { applyImage(m_data.imagePath); }

void TileWidget::updateData(const TileData& data)
{
    m_data = data;
    refreshBorderStyle();   // V5.4.18 — colours follow the data, both of them
    refreshTextStyle();
    recomputeTargetEpoch();   // date/time may have changed — re-compute once
    applyImage(m_data.imagePath);
    refreshOverlays();
}
