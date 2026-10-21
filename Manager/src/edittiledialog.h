#pragma once
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QStringList>
#include "tiledata.h"

// =============================================================================
//  ImagePreviewWidget — v3.1.0. A fixed-size image preview that reveals
//  hover-only overlay controls: left/right arrows to cycle through a tile's
//  known images (fetched backdrop + custom history), and a delete ("×")
//  button to drop whichever image is currently shown. Purely a display/input
//  widget — it doesn't know about TileData at all; the dialog tells it what
//  to show and which controls to allow, and reacts to its signals.
// =============================================================================
class ImagePreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ImagePreviewWidget(QWidget* parent = nullptr);

    void setImage(const QString& path);              // shows it, or blank if empty
    void setNavAllowed(bool showLeft, bool showRight); // whether >1 image exists on each side
    void setDeleteAllowed(bool allowed);               // whether there's anything to delete

signals:
    void previousRequested();
    void nextRequested();
    void deleteRequested();
    // V5.4.21 — the picture itself is the obvious thing to click when you want
    // to change the picture, so clicking it does what "Select Image…" does.
    void selectRequested();

protected:
    void enterEvent(QEnterEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void layoutOverlays();
    void updateOverlayVisibility(bool hovering);

    QLabel*      m_imageLabel  = nullptr;
    QPushButton* m_leftBtn     = nullptr;
    QPushButton* m_rightBtn    = nullptr;
    QPushButton* m_deleteBtn   = nullptr;
    bool m_navLeftAllowed  = false;
    bool m_navRightAllowed = false;
    bool m_deleteAllowed   = false;
    bool m_hovering        = false;   // v3.1.1 — so setNavAllowed/setDeleteAllowed can refresh immediately
};

class EditTileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit EditTileDialog(const TileData& data, QWidget* parent = nullptr);

    // v3.1.0 — the currently-previewed image is what gets saved as active.
    QString selectedImagePath()  const;
    QString fetchedImagePath()   const { return m_fetchedPath; }
    QStringList customImagePaths() const { return m_customPaths; }
    bool    wantsForcedRefetch() const { return m_wantsForcedRefetch; }
    bool    titleWasReset()     const { return m_titleReset; }
    bool    dateWasReset()      const { return m_dateReset; }
    bool    timeWasReset()      const { return m_timeReset; }
    bool    anyResetPressed()   const { return m_titleReset || m_dateReset || m_timeReset; }
    QString customTitle()       const { return m_titleEdit->text().trimmed(); }
    QDate   customDate()        const;
    QString customDateStr()     const;
    QTime   customAirTime()     const;
    bool    dateChecked()       const { return m_dateCheckState; }
    bool    wantsRemove()       const { return m_wantsRemove; }
    // V5.4.19 — true when the dialog was closed via "Customize Colors", so the
    // caller opens that instead of treating the close as a plain cancel.
    bool    wantsCustomizeColors() const { return m_wantsCustomizeColors; }
    // V5.4.20 — has anything in this dialog been touched since it opened?
    // Built by walking the dialog's own widgets rather than comparing field by
    // field against the tile, so a field added later is covered without anyone
    // remembering to add it here — the same trap that has cost this codebase
    // five separate bugs already.
    bool    hasUnsavedChanges() const { return stateSignature() != m_openingSignature; }
    bool    isLooped()          const { return m_loopCheck->isChecked(); }
    QString loopInterval()      const { return m_loopIntervalCombo->currentText(); }
    int     loopWeekday()       const { return m_weekdayCombo->currentIndex() + 1; }
    int     loopDayOfMonth()    const { return m_domSpin->value(); }
    QString presetType()        const;
    QString mediaType()         const;   // v3.0.0 — from the Type dropdown

    // V5 — Season/Episode override. Empty when the numbers weren't touched
    // or don't apply, so the caller can tell "leave the label alone" from
    // "the user set S03E01". Returns a normal "S03E01" status label.
    bool    seasonEpisodeChanged() const;
    QString overriddenStatusLabel() const;
    int     overriddenEpisodeTotal() const;
    // True when the row's own Reset was pressed, meaning the tile should
    // drop its manual override and let the data source drive again.
    bool    seasonEpisodeWasReset() const { return m_seasonEpisodeReset; }

signals:
    void testNotificationRequested();

private:
    QString stateSignature() const;   // V5.4.20

private slots:
    void onSelectImage();
    void onResetTitle();
    void onResetSeasonEpisode();          // V5 — restores only the S/E/Total boxes
    void refreshSeasonEpisodeResetState();// V5 — greys that button when nothing differs
    void onResetDate();
    void onResetTime();
    void onRefetchImage();       // v3.1.0 — renamed/reworked from onResetImage
    void onPreviousImage();      // v3.1.0
    void onNextImage();          // v3.1.0
    void onDeleteImage();        // v3.1.0
    void onDateToggled(bool enabled);
    void onMonthChanged(int index);
    void onTypeChanged(int index);
    void onSpecialDayChanged(int index);
    void onLoopToggled(bool checked);
    void onLoopIntervalChanged(int index);
    void onSave();
    void onRemove();

private:
    QStringList allImages() const;   // fetched (if any) + customs, oldest → newest
    void  refreshImagePreview();     // syncs the preview widget to m_previewIndex
    void  updateRefetchButtonState(); // v3.1.3 — only clickable when there's no fetched image
    QDate selectedDate() const;
    void  setSelectedDate(const QDate& d);
    int   daysInSelectedMonth() const;
    void  refreshDayCombo();
    void  applyLoopFieldVisibility();
    void  updateResetButtons();
    void  updateTimeLabel();
    void  setPickerTime(const QTime& t);
    QTime computeDefaultTime() const;

    TileData     m_data;
    // v3.1.0 — working copies of the tile's image state for this session.
    // Nothing touches disk until Save; Cancel just discards these.
    QString      m_fetchedPath;
    QStringList  m_customPaths;
    int          m_previewIndex = -1;       // position in allImages() being shown
    bool         m_wantsForcedRefetch = false;
    bool         m_titleReset  = false;
    bool         m_dateReset   = false;
    bool         m_timeReset   = false;
    bool         m_timeExplicit = false;  // v3.0.2 — true once there's a real override to save
    bool         m_wantsRemove = false;
    bool         m_wantsCustomizeColors = false;   // V5.4.19
    QString      m_openingSignature;               // V5.4.20 — state when the dialog opened
    bool         m_dateCheckState = true;  // tracks checkbox regardless of visibility

    // Initial values for dirty-checking reset buttons
    QString      m_initTitle;
    QDate        m_initDate;
    QTime        m_initTime;

    ImagePreviewWidget* m_previewWidget = nullptr;
    QLabel*      m_backdropHeadingLabel = nullptr;  // v3.1.1 — "Backdrop Image (1 of 2)"
    QLabel*      m_pathLabel         = nullptr;
    QComboBox*   m_typeCombo         = nullptr;
    QLabel*      m_specialDayLabel   = nullptr;
    QComboBox*   m_specialDayCombo   = nullptr;
    QString      m_lastAppliedType   = "Custom";
    QLineEdit*   m_titleEdit         = nullptr;
    // V5 — Season/Episode override (TV only). See the constructor for why
    // these are an override that the next fetched date is allowed to replace.
    QLabel*      m_seasonEpisodeLabel = nullptr;
    QSpinBox*    m_seasonSpin         = nullptr;
    QSpinBox*    m_episodeSpin        = nullptr;
    QSpinBox*    m_totalSpin          = nullptr;
    QPushButton* m_rSeasonEpisodeBtn  = nullptr;
    QList<QLabel*> m_seCaptions;   // Season / Episode / Total captions
    int          m_initSeason         = 0;
    int          m_initEpisode        = 0;
    int          m_initTotal          = 0;
    // What Reset restores to — the last values a data source reported.
    int          m_sourceSeason       = 0;
    int          m_sourceEpisode      = 0;
    int          m_sourceTotal        = 0;
    bool         m_seasonEpisodeReset = false;
    bool         m_startedOverridden  = false;   // tile already had a manual override
    QPushButton* m_rTitleBtn         = nullptr;
    QLabel*      m_dateLabel         = nullptr;
    QCheckBox*   m_dateCheck         = nullptr;
    QWidget*     m_dateRowWidget     = nullptr;
    QComboBox*   m_monthCombo        = nullptr;
    QComboBox*   m_dayCombo          = nullptr;
    QSpinBox*    m_yearSpin          = nullptr;
    QPushButton* m_rDateBtn          = nullptr;
    QLabel*      m_timeLabel         = nullptr;
    QWidget*     m_timeRowWidget     = nullptr;
    QComboBox*   m_hourCombo         = nullptr;
    QComboBox*   m_minuteCombo       = nullptr;
    QComboBox*   m_ampmCombo         = nullptr;
    QPushButton* m_rTimeBtn          = nullptr;
    QLabel*      m_weekdayLabel      = nullptr;
    QWidget*     m_weekdayRowWidget  = nullptr;
    QComboBox*   m_weekdayCombo      = nullptr;
    QLabel*      m_domLabel          = nullptr;
    QWidget*     m_domRowWidget      = nullptr;
    QSpinBox*    m_domSpin           = nullptr;
    QCheckBox*   m_loopCheck         = nullptr;
    QComboBox*   m_loopIntervalCombo = nullptr;
    QPushButton* m_refetchImageBtn   = nullptr;  // v3.1.0 — renamed from m_rImageBtn
};

