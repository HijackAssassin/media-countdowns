#pragma once
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QImage>
#include <QtMath>
#include <cmath>

// =============================================================================
//  ColorWheel — a circular hue/saturation picker for the tab Color Picker
//  feature. Hue varies by angle around the circle (0° = right, increasing
//  counter-clockwise, matching QColor::hue()'s convention), saturation by
//  distance from center (0 at center, 255 at the rim). Brightness/value is
//  handled by a separate slider next to this widget, since a flat 2D wheel
//  can only ever represent 2 of HSV's 3 dimensions at once.
//
//  The wheel image is cached and only rebuilt when the widget is resized or
//  the brightness value changes (since that affects every pixel's color) —
//  redrawing it pixel-by-pixel on every mouse-drag repaint would otherwise
//  be wasteful.
// =============================================================================
class ColorWheel : public QWidget
{
    Q_OBJECT
public:
    explicit ColorWheel(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(200, 200);
    }

    // hue: 0-359, sat: 0-255, val: 0-255 — matches QColor::hue()/
    // saturation()/value() exactly, so callers never need to rescale.
    void setHsv(int hue, int sat, int val)
    {
        bool valueChanged = (val != m_val);
        m_hue = hue < 0 ? 0 : hue;
        m_sat = sat;
        m_val = val;
        if (valueChanged) m_wheelCache = QImage();  // brightness affects every pixel; must rebuild
        update();
    }
    int hue() const { return m_hue; }
    int sat() const { return m_sat; }

signals:
    // Emitted only from direct user interaction (click/drag) — never from
    // setHsv(), so callers can safely call setHsv() to sync this widget
    // from some other source (e.g. the hex field) without triggering a
    // feedback loop back through this signal.
    void hueSatChanged(int hue, int sat);

protected:
    void paintEvent(QPaintEvent*) override
    {
        ensureWheelCached();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.drawImage(0, 0, m_wheelCache);

        int size = qMin(width(), height());
        qreal cx = size / 2.0, cy = size / 2.0, R = size / 2.0 - 2;
        qreal radius = R * (m_sat / 255.0);
        qreal angle = qDegreesToRadians(qreal(m_hue));
        QPointF pos(cx + std::cos(angle) * radius, cy - std::sin(angle) * radius);

        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(pos, 7, 7);
        p.setPen(QPen(Qt::white, 2));
        p.drawEllipse(pos, 6, 6);
    }

    void mousePressEvent(QMouseEvent* e) override { handleMouse(e->pos()); }
    void mouseMoveEvent(QMouseEvent* e) override { if (e->buttons() & Qt::LeftButton) handleMouse(e->pos()); }

private:
    void ensureWheelCached()
    {
        int size = qMin(width(), height());
        if (!m_wheelCache.isNull() && m_wheelCache.width() == size) return;
        m_wheelCache = QImage(size, size, QImage::Format_ARGB32);
        m_wheelCache.fill(Qt::transparent);
        qreal cx = size / 2.0, cy = size / 2.0, R = size / 2.0 - 2;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                qreal dx = x - cx, dy = cy - y;   // flip y: angle increases counter-clockwise, matching hue convention
                qreal dist = std::sqrt(dx * dx + dy * dy);
                if (dist > R) continue;   // leave transparent outside the circle
                qreal angle = std::atan2(dy, dx);
                if (angle < 0) angle += 2 * M_PI;
                int hue = int(qRadiansToDegrees(angle)) % 360;
                int sat = qBound(0, int(dist / R * 255.0), 255);
                m_wheelCache.setPixelColor(x, y, QColor::fromHsv(hue, sat, m_val));
            }
        }
    }
    void handleMouse(QPoint pos)
    {
        int size = qMin(width(), height());
        qreal cx = size / 2.0, cy = size / 2.0, R = size / 2.0 - 2;
        qreal dx = pos.x() - cx, dy = cy - pos.y();
        qreal dist = std::sqrt(dx * dx + dy * dy);
        qreal angle = std::atan2(dy, dx);
        if (angle < 0) angle += 2 * M_PI;
        m_hue = int(qRadiansToDegrees(angle)) % 360;
        m_sat = qBound(0, int(dist / R * 255.0), 255);
        update();
        emit hueSatChanged(m_hue, m_sat);
    }

    int m_hue = 0, m_sat = 0, m_val = 255;
    QImage m_wheelCache;
};
