#pragma once
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QPixmap>
#include <QFontMetrics>

// =============================================================================
//  OutlinedLabel  —  drop-in replacement that caches the rendered text to a
//  QPixmap and only re-renders when text, font, or size actually changes.
//
//  OLD approach: 9x drawText() calls with Antialiasing ON every paint event.
//  With 8 labels per tile × N tiles firing every second = massive CPU waste.
//
//  NEW approach:
//    • Text is rendered once into a QPixmap via QPainterPath (smooth outline,
//      single stroke pass). The pixmap is cached.
//    • paintEvent just blits the cached pixmap — nearly zero CPU.
//    • Cache is invalidated only when text/font/size changes, not every frame.
// =============================================================================
class OutlinedLabel : public QLabel
{
    Q_OBJECT
public:
    explicit OutlinedLabel(QWidget* parent = nullptr) : QLabel(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
    }

    explicit OutlinedLabel(const QString& text, QWidget* parent = nullptr)
        : QLabel(text, parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        rebuildCacheIfNeeded();
        if (m_cache.isNull()) return;
        QPainter p(this);
        // Blit cached pixmap — no text rendering at all
        p.drawPixmap(0, 0, m_cache);
    }

    void resizeEvent(QResizeEvent* e) override
    {
        QLabel::resizeEvent(e);
        invalidateCache();
    }

    // Intercept text/font/colour changes to invalidate cache
    void changeEvent(QEvent* e) override
    {
        QLabel::changeEvent(e);
        // V5.4.15 — PaletteChange matters as much as FontChange now that the
        // text colour is a per-tile setting. The cache key is built from the
        // text, the font size and the widget size, so a recolour alone leaves
        // it valid and the label keeps blitting the OLD colour: the setting
        // appeared to do nothing until something else happened to change the
        // text. The fill colour is read from the palette in rebuildCacheIfNeeded.
        if (e->type() == QEvent::FontChange || e->type() == QEvent::PaletteChange)
            invalidateCache();
    }

private:
    void invalidateCache() { m_cacheKey = -1; }

    void rebuildCacheIfNeeded()
    {
        const QString txt = text();
        // Use qHash of the text so any character change busts the cache.
        // txt.length() alone caused frozen digits — "05" and "06" have the
        // same length, same font, same size → identical key → no rebuild.
        int key = static_cast<int>(qHash(txt)) ^ (font().pointSize() << 20)
                ^ (width() << 8) ^ height();
        if (key == m_cacheKey && !m_cache.isNull()) return;
        m_cacheKey = key;

        QSize sz = size();
        if (sz.isEmpty()) { m_cache = QPixmap(); return; }

        const qreal dpr = devicePixelRatioF();
        m_cache = QPixmap(sz * dpr);
        m_cache.setDevicePixelRatio(dpr);
        m_cache.fill(Qt::transparent);

        if (txt.isEmpty()) return;

        QPainter p(&m_cache);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        QFont f = font();
        p.setFont(f);

        Qt::Alignment align = alignment();
        QRect r(0, 0, sz.width(), sz.height());

        // Build QPainterPath for the text — one path, one stroke, one fill
        // This is dramatically cheaper than 9 separate drawText() calls
        QPainterPath path;
        // Find text position based on alignment
        QFontMetrics fm(f);
        QRect br = fm.boundingRect(r, static_cast<int>(align) | Qt::TextWordWrap, txt);

        path.addText(
            alignedTextOrigin(f, txt, r, align),
            f, txt);

        // Outline: stroke the path with black, width 2.5
        QPen outlinePen(Qt::black, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(outlinePen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        // Fill: paint the text in white on top
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::WindowText));
        p.drawPath(path);
    }

    // Calculate the baseline origin for QPainterPath::addText() so the text
    // lands in the same position as Qt::AlignCenter / AlignLeft would.
    QPointF alignedTextOrigin(const QFont& f, const QString& txt,
                               const QRect& r, Qt::Alignment align) const
    {
        QFontMetrics fm(f);
        int textW = fm.horizontalAdvance(txt);
        int textH = fm.height();

        qreal x, y;

        if (align & Qt::AlignHCenter)
            x = r.left() + (r.width() - textW) / 2.0;
        else if (align & Qt::AlignRight)
            x = r.right() - textW;
        else
            x = r.left();

        if (align & Qt::AlignVCenter)
            y = r.top() + (r.height() + textH) / 2.0 - fm.descent();
        else if (align & Qt::AlignBottom)
            y = r.bottom() - fm.descent();
        else
            y = r.top() + fm.ascent();

        return QPointF(x, y);
    }

    QPixmap m_cache;
    int     m_cacheKey = -1;
};
