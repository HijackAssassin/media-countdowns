#pragma once
#include <QWidget>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QLinearGradient>
#include <QColor>
#include <QTimer>
#include <QList>

// =============================================================================
//  TileOutline — V5.4.13. The tile's colour tag, drawn ON TOP of everything.
//
//  Three attempts at this, and the first two each failed in an instructive way:
//
//   1. V4.7 put a QSS border on the image container with no selector. A Qt
//      stylesheet applies to the widget AND its children, so every label in
//      the tile silently gained a 4px box model — invisible, because
//      OutlinedLabel paints its own pixmap and never calls QLabel::paintEvent,
//      but it grew their size hints and shoved the countdown down a few
//      pixels. That was the "adding a colour moves the numbers" report.
//
//   2. V5.4.9 scoped that rule to #tileImageContainer, which fixed the shove
//      and broke the feature: the image QLabel is a child sized to the FULL
//      container rect, so it covers the frame its parent draws. The border was
//      still being painted, just underneath the artwork. The reason nobody
//      noticed in V4.7 is that the unscoped rule had been drawing a border on
//      the image label too — the bug was also the only thing making it show.
//
//  So it is a sibling widget stacked above the image, the countdown and the
//  title, which is the only arrangement where "on top" is not a matter of
//  which child happens to be created last.
//
//  Two things keep it free:
//
//   • It is MASKED to the ring it actually draws. The middle of the tile is
//     genuinely not part of this widget, so the countdown ticking underneath
//     never asks it to repaint — without that, a full-size transparent
//     overlay would be dragged into the repaint of every single second.
//   • The ring is rendered once into a pixmap and re-blitted. Nothing is
//     computed per paint, so it costs the same as drawing an image.
// =============================================================================
class TileOutline : public QWidget
{
    Q_OBJECT
public:
    // Solid edge, a dark separator, then the glow fading inward.
    //
    // The separator is the part that makes this work over ANY artwork. Without
    // it a gold tag over a sunset backdrop is gold on orange and effectively
    // invisible — the first render of this looked like a faint scratch. One
    // dark line between the colour and the picture gives every colour
    // something to sit against, which is why a neon sign is legible at all.
    static constexpr int kEdge = 5;
    static constexpr int kDark = 2;
    static constexpr int kGlow = 12;
    static constexpr int kBand = kEdge + kDark + kGlow;   // thickness the mask needs

    explicit TileOutline(QWidget* parent) : QWidget(parent)
    {
        // Never take a click — the tile underneath owns every one of them.
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        s_all.append(this);
        hide();
    }

    ~TileOutline() override { s_all.removeOne(this); }

    void setColor(const QColor& c)
    {
        if (m_color == c) return;
        m_color = c;
        m_cache = QPixmap();          // colour changed — the ring must be re-rendered
        setVisible(c.isValid());
        if (c.isValid()) { applyMask(); ensureAnimation(); update(); }
    }

    QColor color() const { return m_color; }

protected:
    void resizeEvent(QResizeEvent*) override
    {
        m_cache = QPixmap();
        applyMask();
    }

    void paintEvent(QPaintEvent*) override
    {
        if (!m_color.isValid() || size().isEmpty()) return;
        if (m_cache.isNull() || m_dirty) rebuild();
        if (m_cache.isNull()) return;
        QPainter p(this);
        p.drawPixmap(0, 0, m_cache);
    }

private:
    // Only the ring belongs to this widget. Everything inside it stays the
    // tile's own business, including its repaints.
    void applyMask()
    {
        if (size().isEmpty()) return;
        QRegion outer(rect());
        QRegion inner(rect().adjusted(kBand, kBand, -kBand, -kBand));
        setMask(outer.subtracted(inner));
    }

    void rebuild()
    {
        m_dirty = false;
        const qreal dpr = devicePixelRatioF();
        // Reuse the pixmap across frames — allocating a new one 15 times a
        // second per tile is the only part of this that would actually cost
        // anything.
        if (m_cache.isNull() || m_cache.size() != size() * dpr) {
            m_cache = QPixmap(size() * dpr);
            m_cache.setDevicePixelRatio(dpr);
        }
        m_cache.fill(Qt::transparent);

        QPainter p(&m_cache);
        p.setRenderHint(QPainter::Antialiasing, false);   // straight edges, no seams

        const int w = width(), h = height();

        // The glow: the chosen colour fading inward over kGlow pixels, painted
        // as a stack of single-pixel rectangles rather than a real blur, so it
        // costs nothing and renders identically on every machine. Each ring is
        // stroked with the same diagonal gradient as the edge, so the light
        // falls consistently across the whole frame instead of the glow being
        // flat behind a shaded border.
        // d is the distance inward from the separator, so the glow is at its
        // strongest where it meets the edge and fades to nothing further in.
        // Indexing this the other way round is what made the first attempt
        // look like a grey band hovering inside the border rather than light
        // coming off it.
        for (int d = 0; d < kGlow; ++d) {
            const qreal t = qreal(d) / kGlow;
            // Quadratic falloff — linear reads as a flat band with an edge of
            // its own, which is the opposite of a glow.
            const qreal alpha = 0.75 * (1.0 - t) * (1.0 - t);
            const int inset = kEdge + kDark + d;
            p.setPen(QPen(QBrush(edgeGradient(w, h, alpha)), 1));
            p.drawRect(QRect(inset, inset, w - 1 - inset * 2, h - 1 - inset * 2));
        }

        // The separator, between the colour and the artwork.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 190));
        drawFrame(p, w, h, kEdge, kDark);

        // The edge itself, lit from the top-left so the colour has depth
        // rather than reading as a flat sticker. A tag is meant to be visible
        // from across the grid.
        p.setBrush(edgeGradient(w, h, 1.0));
        drawFrame(p, w, h, 0, kEdge);
    }

    // One diagonal gradient of the tag colour, at a given opacity — with a
    // band of light travelling along it.
    //
    // V5.4.15, the "lava lamp": the gradient repeats and its start point slides
    // down the diagonal as m_phase advances, so a bright band drifts around the
    // frame instead of the whole thing pulsing. A pulse was tried in V4.9 and
    // rejected for looking bad, and it deserved to be — brightening everything
    // at once reads as a fault indicator. Movement along the edge reads as
    // something flowing, which is the thing that was actually wanted.
    //
    // One full lap takes kCycleMs, deliberately slow: this sits behind a
    // countdown somebody is reading.
    QLinearGradient edgeGradient(int w, int h, qreal alpha) const
    {
        QColor lit = m_color.lighter(165), mid = m_color, dim = m_color.darker(140);
        lit.setAlphaF(alpha); mid.setAlphaF(alpha); dim.setAlphaF(alpha);

        // A diagonal vector, offset along itself by the current phase. Repeat
        // spread makes the band wrap round rather than running out.
        const qreal len = qMax(1, (w + h) / 2);
        const qreal ux = qreal(w) / (w + h), uy = qreal(h) / (w + h);
        const qreal off = m_phase * len;
        QLinearGradient g(off * ux, off * uy, off * ux + len * ux, off * uy + len * uy);
        g.setSpread(QGradient::RepeatSpread);
        g.setColorAt(0.00, dim);
        g.setColorAt(0.35, mid);
        g.setColorAt(0.50, lit);
        g.setColorAt(0.65, mid);
        g.setColorAt(1.00, dim);
        return g;
    }

    // ── The animation ──────────────────────────────────────────────────────
    //
    // ONE timer drives every outline, the same arrangement TileWidget already
    // uses for the countdown: a timer per tile would be dozens of timers all
    // waking the process at slightly different moments.
    //
    // Only outlines that are actually on screen are redrawn. isVisible() is
    // false for a tile sitting on a tab you are not looking at, so a library
    // spread across ten tabs animates one tab's worth, not ten.
    static void ensureAnimation()
    {
        if (s_timer) return;
        s_timer = new QTimer;
        s_timer->setInterval(kFrameMs);
        QObject::connect(s_timer, &QTimer::timeout, [] {
            const qreal step = qreal(kFrameMs) / kCycleMs;
            bool anyVisible = false;
            for (TileOutline* o : std::as_const(s_all)) {
                if (!o->m_color.isValid() || !o->isVisible()) continue;
                anyVisible = true;
                o->m_phase += step;
                if (o->m_phase >= 1.0) o->m_phase -= 1.0;
                o->m_dirty = true;
                o->update();
            }
            // Nothing tagged is on screen — stop waking up for it.
            if (!anyVisible) s_timer->stop();
        });
        s_timer->start();
    }

    // A hollow rectangle `thickness` deep, starting `inset` in from the edge.
    static void drawFrame(QPainter& p, int w, int h, int inset, int thickness)
    {
        p.drawRect(QRect(inset, inset, w - inset * 2, thickness));                       // top
        p.drawRect(QRect(inset, h - inset - thickness, w - inset * 2, thickness));       // bottom
        p.drawRect(QRect(inset, inset, thickness, h - inset * 2));                       // left
        p.drawRect(QRect(w - inset - thickness, inset, thickness, h - inset * 2));       // right
    }

    QColor  m_color;
    QPixmap m_cache;
    qreal   m_phase = 0.0;     // 0..1, where the travelling highlight is
    bool    m_dirty = false;   // phase moved, so the ring needs re-rendering

    static constexpr int kFrameMs = 66;      // ~15fps — plenty for something this slow
    static constexpr int kCycleMs = 6000;    // one full lap of the frame

    static inline QTimer* s_timer = nullptr;
    static inline QList<TileOutline*> s_all;
};
