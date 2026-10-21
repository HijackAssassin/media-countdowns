#pragma once
#include <QDialog>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVector>
#include <QSize>

class QVBoxLayout;
class QShowEvent;

// =============================================================================
//  AboutPageWidget — v3.1.9. Reserves dedicated empty side slots for the
//  hover-reveal nav arrows. setPage() sizes the image to its own natural
//  size (capped per page) and sizes the text label to a fixed width — but
//  no longer computes or returns any size for the dialog to resize to. The
//  containing dialog is fixed at one size for its whole lifetime; every
//  page's content just has to fit within that same frame, with the text
//  scroll area's own scrollbar handling any overflow.
// =============================================================================
class AboutPageWidget : public QWidget
{
    Q_OBJECT
public:
    // v3.1.91 fix #2 — topCompensation is a fixed pixel amount added above
    // the flexible top spacer. Centering a page's content via matching
    // top/bottom stretch only centers it within THIS widget's own bounds —
    // but this widget doesn't occupy the whole dialog (there's a page
    // indicator above it and a separator+Close button below, and those two
    // chrome zones aren't the same height). topCompensation corrects for
    // that difference so centered pages end up visually centered in the
    // whole dialog, not just within this widget.
    explicit AboutPageWidget(QWidget* parent = nullptr, int topCompensation = 0);

    // v3.1.9 — centerVertically: true keeps this page's image+text block
    // vertically centered within the fixed frame (used for every page
    // except page 1, whose long text should instead expand/scroll to fill
    // the available space rather than sit centered with padding around it).
    // v3.1.92 fix #4 — forceTextWidth (0 = use the default per-image
    // computation) overrides the text area's width, for pages whose own
    // (smaller) image shouldn't constrain how wide their text gets to be.
    // v3.1.94 fix #5 — forceImageWidth (0 = use the image's own natural/
    // capped size) scales the image UP if needed so it reaches this exact
    // width, instead of only ever scaling it down when it exceeds the cap.
    void setPage(const QString& imagePath, const QString& bodyHtml,
                 QSize maxImageSize, bool centerText, bool centerVertically,
                 int forceTextWidth = 0, int forceImageWidth = 0);
    void setNavAllowed(bool showLeft, bool showRight);

signals:
    void previousRequested();
    void nextRequested();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void updateOverlayVisibility(bool hovering);

    QVBoxLayout* m_centerLayout = nullptr;
    int m_topStretchIndex = 0;
    int m_textIndex = 0;
    int m_bottomStretchIndex = 0;
    QLabel*      m_imageLabel  = nullptr;
    QScrollArea* m_textScroll  = nullptr;
    QLabel*      m_textLabel   = nullptr;
    QPushButton* m_leftBtn     = nullptr;
    QPushButton* m_rightBtn    = nullptr;
    bool m_navLeftAllowed  = false;
    bool m_navRightAllowed = false;
    bool m_hovering        = false;
};

// =============================================================================
//  AboutDialog — v3.1.91. Paginated "About" content: app backstory, the two
//  predecessor apps, this one, then a 5th page with just the TMDB
//  attribution. The dialog is sized ONCE, from page 2's content (its image
//  is intentionally much larger than the others), before it's ever shown,
//  and is never resized again for the rest of its lifetime — every other
//  page's content just has to fit within that same fixed frame. This
//  guarantees the hover arrows always land in the exact same screen
//  position on every page. Opens pinned to the top of the screen
//  (horizontally centered).
// =============================================================================
class AboutDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct Page { QString imagePath; QString bodyHtml; QSize maxImageSize; bool centerText; bool centerVertically;
                  int forceTextWidth = 0; int forceImageWidth = 0; };

    void refreshPage();
    void positionAtTop();

    QVector<Page> m_pages;
    int m_currentIndex = 0;
    AboutPageWidget* m_pageWidget    = nullptr;
    QLabel*          m_pageIndicator = nullptr;
    bool m_positioned = false;   // position at the top only once, on the first real show
};
