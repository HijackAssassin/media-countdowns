#pragma once
#include <QWidget>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include "outlinedlabel.h"

class CountdownWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CountdownWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet("background: transparent;");

        // Outer: vertically centers the content block
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        // Inner grid: each column is one counter.
        // Row 0 = number, Row 1 = label.
        // This guarantees each label is directly below its number
        // regardless of how wide the day number grows.
        auto* block = new QWidget(this);
        block->setAttribute(Qt::WA_TranslucentBackground);
        m_grid = new QGridLayout(block);
        m_grid->setContentsMargins(0, 0, 0, 0);
        m_grid->setVerticalSpacing(3);
        m_grid->setHorizontalSpacing(24);

        m_nums[0] = makeNum("0",   true);
        m_nums[1] = makeNum("00",  false);
        m_nums[2] = makeNum("00",  false);
        m_nums[3] = makeNum("00",  false);

        // Force all number labels to the same height as the days label so they
        // all sit on the same baseline — hms are smaller font but same cell height
        int numRowH = m_nums[0]->sizeHint().height();
        for (int i = 1; i < 4; ++i) m_nums[i]->setFixedHeight(numRowH);

        const char* units[] = { "days", "hours", "minutes", "seconds" };
        for (int i = 0; i < 4; ++i) {
            m_units[i] = makeUnit(units[i]);
            m_grid->addWidget(m_nums[i],  0, i, Qt::AlignHCenter | Qt::AlignBottom);
            m_grid->addWidget(m_units[i], 1, i, Qt::AlignHCenter | Qt::AlignTop);
        }

        outer->addStretch(1);
        outer->addWidget(block,  0, Qt::AlignHCenter);
        outer->addStretch(1);
    }

    void setSeconds(qint64 secs)
    {
        if (secs <= 0) { setExpired(); return; }
        showCountdown();

        qint64 d = secs / 86400;
        qint64 h = (secs % 86400) / 3600;
        qint64 m = (secs % 3600) / 60;
        qint64 s = secs % 60;

        setNum(m_nums[0], QString::number(d),                    m_last[0]);
        setNum(m_nums[1], QString("%1").arg(h,2,10,QChar('0')),  m_last[1]);
        setNum(m_nums[2], QString("%1").arg(m,2,10,QChar('0')),  m_last[2]);
        setNum(m_nums[3], QString("%1").arg(s,2,10,QChar('0')),  m_last[3]);
    }

    // V5.4.15 — the tile's text colour. An invalid colour means "the default",
    // which is what every tile had before this existed. The unit labels take a
    // dimmed version of the same colour rather than a second setting: they are
    // captions for the numbers, and letting them drift to an unrelated colour
    // makes the block stop reading as one thing.
    void setTextColor(const QColor& c)
    {
        const QColor numbers = c.isValid() ? c : QColor(Qt::white);
        const QColor units   = c.isValid() ? c.darker(118) : QColor(0xdd, 0xdd, 0xdd);
        for (int i = 0; i < 4; ++i) {
            QPalette pn = m_nums[i]->palette();
            pn.setColor(QPalette::WindowText, numbers);
            m_nums[i]->setPalette(pn);
            QPalette pu = m_units[i]->palette();
            pu.setColor(QPalette::WindowText, units);
            m_units[i]->setPalette(pu);
        }
        // OutlinedLabel drops its cached pixmap on a PaletteChange (see
        // outlinedlabel.h), so setting the palette above is enough to make the
        // new colour render on the next paint.
    }

    void setNoDate()  { hideAll(); }
    void setExpired() { hideAll(); }

    void scaleFonts(int tileWidth)
    {
        if (tileWidth == m_lastScaleWidth) return;
        m_lastScaleWidth = tileWidth;
        int dayPt  = qMax(9,  tileWidth / 10);
        int hmsPt  = qMax(6,  tileWidth / 15);
        int unitPt = qMax(3,  tileWidth / 42);
        for (int i = 0; i < 4; ++i) {
            int pt = (i == 0) ? dayPt : hmsPt;
            QFont fn = m_nums[i]->font();
            fn.setPointSize(pt);
            fn.setWeight(QFont::Medium);   // 500 — between Light and DemiBold
            fn.setLetterSpacing(QFont::AbsoluteSpacing, qMax(1, tileWidth / 80));
            m_nums[i]->setFont(fn);

            QFont fu = m_units[i]->font();
            fu.setPointSize(unitPt);
            fu.setBold(false);
            m_units[i]->setFont(fu);

            // Unit label needs room for descenders; match column width to number
            QFontMetrics fmu(fu);
            m_units[i]->setMinimumWidth(m_nums[i]->sizeHint().width() + 16);
            m_units[i]->setFixedHeight(fmu.height() + fmu.descent() + 6);
        }
        // Re-sync all hms labels to same height as days after font change
        int h = m_nums[0]->sizeHint().height();
        for (int i = 1; i < 4; ++i) m_nums[i]->setFixedHeight(h);
    }

private:
    QGridLayout*   m_grid = nullptr;
    OutlinedLabel* m_nums[4]  = {};
    OutlinedLabel* m_units[4] = {};
    qint64         m_last[4]  = {-1,-1,-1,-1};
    int            m_lastScaleWidth = -1;

    static OutlinedLabel* makeNum(const QString& text, bool isBig = true) {
        auto* l = new OutlinedLabel(text);
        l->setAlignment(Qt::AlignCenter);
        l->setAttribute(Qt::WA_TranslucentBackground);
        QFont f;
        f.setPointSize(isBig ? 42 : 28);
        f.setWeight(QFont::Medium);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 4);
        l->setFont(f);
        QPalette pal = l->palette();
        pal.setColor(QPalette::WindowText, Qt::white);
        l->setPalette(pal);
        l->setContentsMargins(0, 0, 0, 0);
        return l;
    }
    static OutlinedLabel* makeUnit(const QString& text) {
        auto* l = new OutlinedLabel(text);
        l->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        l->setAttribute(Qt::WA_TranslucentBackground);
        QFont f;
        f.setPointSize(7);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        l->setFont(f);
        QFontMetrics fm(f);
        l->setMinimumHeight(fm.height() + fm.descent() + 6);
        QPalette pal = l->palette();
        pal.setColor(QPalette::WindowText, QColor(0xdd, 0xdd, 0xdd));
        l->setPalette(pal);
        l->setContentsMargins(0, 0, 0, 0);
        return l;
    }

    inline void setNum(OutlinedLabel* lbl, const QString& val, qint64& last) {
        qint64 v = val.toLongLong();
        if (v == last) return;
        last = v;
        lbl->setText(val);
    }

    // V5.4.21 — there used to be a message label shown here in place of the
    // numbers. Nothing ever set its text, so it was an empty label being shown
    // and hidden for years; what a Released or dateless tile actually reads is
    // its title line, built by TileWidget::formatTitleLine(). Removed rather
    // than given wording, because the title line already says "No Release Date"
    // and the date, and a second message in the middle would repeat it.
    void hideAll() {
        for (int i = 0; i < 4; ++i) { m_nums[i]->hide(); m_units[i]->hide(); }
    }
    void showCountdown() {
        for (int i = 0; i < 4; ++i) { m_nums[i]->show(); m_units[i]->show(); }
    }
};
