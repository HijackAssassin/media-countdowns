#pragma once
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include "tiledata.h"

class CustomTileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CustomTileDialog(QWidget* parent = nullptr);
    TileData result() const { return m_result; }

private slots:
    void onSelectImage();
    void onClearImage();
    void onDateToggled(bool enabled);
    void onMonthChanged(int index);
    void onTypeChanged(int index);
    void onSpecialDayChanged(int index);
    void onLoopToggled(bool checked);
    void onLoopIntervalChanged(int index);
    void onSave();

private:
    void    updatePreview(const QString& path);
    QString copyImageToAppFolder(const QString& srcPath);
    QDate   selectedDate() const;
    void    setSelectedDate(const QDate& d);
    int     daysInSelectedMonth() const;
    void    refreshDayCombo();
    void    applyLoopFieldVisibility();
    void    updateTimeLabel();
    void    setPickerTime(const QTime& t);
    QTime   computeDefaultTime() const;

    TileData      m_result;
    QString       m_imagePath;
    bool          m_timeDirty         = false;   // v3.0.2 — has the user touched the time picker?

    QComboBox*    m_typeCombo         = nullptr;
    QLabel*       m_specialDayLabel   = nullptr;
    QComboBox*    m_specialDayCombo   = nullptr;
    QString       m_lastAppliedType   = "Custom";
    QLineEdit*    m_titleEdit         = nullptr;
    // Date
    QLabel*       m_dateLabel         = nullptr;
    QCheckBox*    m_dateCheck         = nullptr;
    QWidget*      m_dateRowWidget     = nullptr;
    QComboBox*    m_monthCombo        = nullptr;
    QComboBox*    m_dayCombo          = nullptr;
    QSpinBox*     m_yearSpin          = nullptr;
    // Time
    QLabel*       m_timeLabel         = nullptr;
    QWidget*      m_timeRowWidget     = nullptr;
    QComboBox*    m_hourCombo         = nullptr;
    QComboBox*    m_minuteCombo       = nullptr;
    QComboBox*    m_ampmCombo         = nullptr;
    // Weekly-only
    QLabel*       m_weekdayLabel      = nullptr;
    QWidget*      m_weekdayRowWidget  = nullptr;
    QComboBox*    m_weekdayCombo      = nullptr;
    // Monthly-only
    QLabel*       m_domLabel          = nullptr;
    QWidget*      m_domRowWidget      = nullptr;
    QSpinBox*     m_domSpin           = nullptr;
    // Loop controls
    QCheckBox*    m_loopCheck         = nullptr;
    QComboBox*    m_loopIntervalCombo = nullptr;
    // Image
    QLabel*       m_previewLabel      = nullptr;
    QLabel*       m_pathLabel         = nullptr;
};
