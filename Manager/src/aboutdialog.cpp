#include "aboutdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QPixmap>
#include <QEnterEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QTimer>

// =============================================================================
//  AboutPageWidget
// =============================================================================
AboutPageWidget::AboutPageWidget(QWidget* parent, int topCompensation) : QWidget(parent)
{
    setMouseTracking(true);

    auto* hlay = new QHBoxLayout(this);
    hlay->setContentsMargins(0, 0, 0, 0);
    hlay->setSpacing(0);

    auto makeArrow = [this](const QString& text) {
        auto* b = new QPushButton(text, this);
        b->setFixedSize(46, 72);
        b->setCursor(Qt::PointingHandCursor);
        b->setVisible(false);
        b->setStyleSheet(
            "QPushButton { background:rgba(20,20,20,175); color:#fff; "
            "border:1px solid rgba(255,255,255,70); border-radius:6px; "
            "font-size:30px; font-weight:bold; padding:0px; }"
            "QPushButton:hover { background:rgba(0,120,212,205); border-color:rgba(255,255,255,130); }");
        return b;
    };

    // Dedicated empty side slots for the arrows — same screen position on
    // every page since the dialog itself never changes size.
    auto* leftSlot = new QWidget(this);
    leftSlot->setFixedWidth(64);
    auto* leftSlotLay = new QVBoxLayout(leftSlot);
    leftSlotLay->setContentsMargins(0, 0, 0, 0);
    m_leftBtn = makeArrow("<");
    leftSlotLay->addStretch();
    leftSlotLay->addWidget(m_leftBtn, 0, Qt::AlignHCenter);
    leftSlotLay->addStretch();
    hlay->addWidget(leftSlot);

    auto* centerWidget = new QWidget(this);
    auto* vlay = new QVBoxLayout(centerWidget);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(10);
    vlay->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_centerLayout = vlay;

    // v3.1.91 fix #2 — a fixed offset (always present, not stretchy) that
    // corrects for the dialog's asymmetric outer chrome (page indicator
    // above vs. separator+Close button below), so that when the flexible
    // spacers below center a page's content, it's actually centered in the
    // whole dialog — not just within this widget's own bounds, which sit
    // lower than the dialog's true middle once that chrome is accounted for.
    if (topCompensation > 0) vlay->addSpacing(topCompensation);   // index 0 (only present if > 0)

    // v3.1.9 — top/bottom spacers whose stretch gets toggled per page: 0
    // (collapsed) for page 1, whose text scroll expands/scrolls to fill
    // the space instead; >0 for every other page, centering their much
    // shorter image+text block instead of leaving empty space below it.
    vlay->addStretch(0);   // top flexible spacer

    m_imageLabel = new QLabel(centerWidget);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setStyleSheet("background:transparent; border:none;");
    vlay->addWidget(m_imageLabel, 0, Qt::AlignHCenter);

    m_textScroll = new QScrollArea(centerWidget);
    m_textScroll->setWidgetResizable(true);
    m_textScroll->setFrameShape(QFrame::NoFrame);
    m_textScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_textScroll->setStyleSheet(
        "QScrollArea { background:transparent; border:none; }"
        "QScrollBar:vertical { background:#1e1e1e; width:8px; }"
        "QScrollBar::handle:vertical { background:#444; border-radius:4px; }");

    m_textLabel = new QLabel(m_textScroll);
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextFormat(Qt::RichText);
    m_textLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_textLabel->setStyleSheet("color:#ccc; font-size:13px; background:transparent;");
    m_textScroll->setWidget(m_textLabel);
    vlay->addWidget(m_textScroll);

    vlay->addStretch(0);   // bottom flexible spacer, mirrors the top one

    // v3.1.91 — the stretch-toggle indices in setPage() depend on whether
    // the fixed compensation spacer above was added (shifting everything
    // after it by one), so record them here instead of hardcoding.
    m_topStretchIndex  = topCompensation > 0 ? 1 : 0;
    m_textIndex        = m_topStretchIndex + 2;
    m_bottomStretchIndex = m_topStretchIndex + 3;

    hlay->addWidget(centerWidget, 1);

    auto* rightSlot = new QWidget(this);
    rightSlot->setFixedWidth(64);
    auto* rightSlotLay = new QVBoxLayout(rightSlot);
    rightSlotLay->setContentsMargins(0, 0, 0, 0);
    m_rightBtn = makeArrow(">");
    rightSlotLay->addStretch();
    rightSlotLay->addWidget(m_rightBtn, 0, Qt::AlignHCenter);
    rightSlotLay->addStretch();
    hlay->addWidget(rightSlot);

    connect(m_leftBtn,  &QPushButton::clicked, this, &AboutPageWidget::previousRequested);
    connect(m_rightBtn, &QPushButton::clicked, this, &AboutPageWidget::nextRequested);
}

void AboutPageWidget::setPage(const QString& imagePath, const QString& bodyHtml,
                                QSize maxImageSize, bool centerText, bool centerVertically,
                                int forceTextWidth, int forceImageWidth)
{
    QPixmap original = imagePath.isEmpty() ? QPixmap() : QPixmap(imagePath);
    QSize imgSize(0, 0);
    if (!original.isNull()) {
        QSize natural = original.size();
        if (forceImageWidth > 0) {
            // v3.1.94 fix #5 — scale UP (not just down) so the image
            // reaches the target width, capped by maxImageSize's height so
            // a narrow/portrait image can't become absurdly tall.
            imgSize = natural.scaled(QSize(forceImageWidth, maxImageSize.height()), Qt::KeepAspectRatio);
        } else {
            imgSize = (natural.width() > maxImageSize.width() || natural.height() > maxImageSize.height())
                      ? natural.scaled(maxImageSize, Qt::KeepAspectRatio)
                      : natural;
        }
        m_imageLabel->setFixedSize(imgSize);
        m_imageLabel->setPixmap(original.scaled(imgSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_imageLabel->setVisible(true);
    } else {
        m_imageLabel->setVisible(false);
    }

    // v3.1.92 fix #4 — forceTextWidth (used for page 1) makes the text use
    // the full available width instead of being constrained to whatever
    // its own (much smaller) image implies.
    int contentWidth = forceTextWidth > 0 ? forceTextWidth : qMax(imgSize.width(), 550);
    m_textLabel->setAlignment(centerText ? (Qt::AlignTop | Qt::AlignHCenter) : (Qt::AlignTop | Qt::AlignLeft));
    m_textLabel->setText(
        centerText ? QString("<div style='text-align:center;'>%1</div>").arg(bodyHtml) : bodyHtml);
    m_textLabel->setFixedWidth(contentWidth);
    m_textScroll->setFixedWidth(contentWidth);

    // V5 — pages that centre their content (everything except page 1) use
    // the scroll area's natural size, and QScrollArea's own height hint is
    // a small fixed default that ignores what's inside it. Taller content
    // therefore got a scrollbar even with most of the dialog sitting empty
    // below it — which is what put a scrollbar on the attribution page.
    // Give it the height the text actually needs instead. Page 1 is left
    // alone: its text genuinely is longer than the dialog and is meant to
    // scroll.
    if (centerVertically) {
        int naturalHeight = m_textLabel->heightForWidth(contentWidth);
        if (naturalHeight > 0) {
            // Never taller than the space the page can offer, so an
            // unexpectedly long page still scrolls rather than overflowing
            // the fixed dialog frame.
            int ceiling = qMax(120, height() - imgSize.height() - 40);
            m_textScroll->setFixedHeight(qMin(naturalHeight + 4, ceiling));
        }
    } else {
        m_textScroll->setMinimumHeight(0);
        m_textScroll->setMaximumHeight(QWIDGETSIZE_MAX);
    }

    // v3.1.9 — no more resizing the dialog per page. Instead, toggle which
    // element absorbs the fixed frame's spare vertical space: page 1's text
    // scroll stretches to fill/scroll it (spacers collapsed to 0), while
    // every other (much shorter) page's spacers stretch to center its
    // image+text block and the text scroll just uses its own natural size.
    int spacerStretch = centerVertically ? 1 : 0;
    int textStretch    = centerVertically ? 0 : 1;
    m_centerLayout->setStretch(m_topStretchIndex, spacerStretch);
    m_centerLayout->setStretch(m_textIndex, textStretch);
    m_centerLayout->setStretch(m_bottomStretchIndex, spacerStretch);
}

void AboutPageWidget::setNavAllowed(bool showLeft, bool showRight)
{
    m_navLeftAllowed  = showLeft;
    m_navRightAllowed = showRight;
    updateOverlayVisibility(m_hovering);
}

void AboutPageWidget::updateOverlayVisibility(bool hovering)
{
    m_leftBtn->setVisible(hovering && m_navLeftAllowed);
    m_rightBtn->setVisible(hovering && m_navRightAllowed);
}

void AboutPageWidget::enterEvent(QEnterEvent* event)
{
    QWidget::enterEvent(event);
    m_hovering = true;
    updateOverlayVisibility(true);
}

void AboutPageWidget::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    m_hovering = false;
    updateOverlayVisibility(false);
}

// =============================================================================
//  AboutDialog
// =============================================================================
namespace {
const char* kPage1Image = ":/Longhorn.png";
const char* kPage1Html  =
    "<p style='font-size:15px; font-weight:bold; color:#fff;'>Patrick Price — July 16, 2026</p>"
    "<p>I created this app as a second attempt at recreating a free app called \"Event Countdowns\" "
    "that I downloaded off the Microsoft Store in 2023.</p>"
    "<p>I thought the app was so amazing that it was one of the only apps I ever downloaded from the "
    "MS Store.</p>"
    "<p>It helped me keep track of Movies &amp; TV Shows that were coming out at the time, "
    "specifically Marvel.</p>"
    "<p>The past couple years I have had to reinstall Windows a couple dozen times.</p>"
    "<p>One of those times I found out that the app I was using was removed from the MS Store.</p>"
    "<p>I felt like it was a major loss at the time since it was so satisfying and helpful seeing "
    "the countdowns all laid out.</p>"
    "<p>It must have been early 2025 because in late June of 2025 I decided to take up the task of "
    "learning how to make an app.</p>"
    "<p>I spent about a month working on recreating the app as best as I could and I even added some "
    "improvements.</p>"
    "<p>I ended up calling my first app \"EventCountdowns\", yes I named it the same as the one I was "
    "recreating.</p>"
    "<p>It was almost perfect except for one very important issue, the notifications were broken, "
    "sometimes they would work, but sometimes it would send notifications for things that were years "
    "in the future. Unfortunately, I was unable to figure out what was causing it.</p>"
    "<p>So I decided to start fresh, and now we have the amazing \"MediaCountdowns\", I started "
    "working on this in late April of 2026.</p>"
    "<p>I feel like I have created an app that is truly unique and helpful, I put a lot of time into "
    "this to make it as good as possible, I am very proud of this accomplishment.</p>"
    "<p>This app wouldn't be possible without me discovering how I could use an API, for this app I "
    "used TMDB's API, it makes it so that the app sends metadata requests, which basically means "
    "that you can search for media (Movies or Shows) and it will create a tile for you, that "
    "contains the date and time for when it releases, for TV shows it will update weekly to the "
    "next episode. And the tiles automatically fetch images. Whenever the app is reopened it "
    "rechecks all tiles to see if a date is changed so its always accurate. There are Types of "
    "tiles (Movie, Show, Game, Special &amp; Custom), the special type allows you to pick holidays "
    "or birthdays so they will loop every year.</p>"
    "<p>While the app cannot search for video games, you can create a game type tile and put the "
    "name and date with an image from Google.</p>"
    "<p>You can also use tab customization in the settings if you want to show specific types of "
    "tiles in their own tabs.</p>"
    "<p>Anyway its been a fun journey and I will continue to improve the app until I run out of "
    "ideas.</p>"
    "<p>If you have any ideas you think I should add, email me at "
    "Patrickjp292004@gmail.com</p>";

const char* kPage2Image = ":/AboutPage2.PNG";
const char* kPage2Html  =
    "<p style='font-size:18px; font-weight:bold; color:#fff;'>Original \"Event Countdowns\" App "
    "from Microsoft Store</p>"
    "<p style='color:#888; font-size:13px;'>Source: This is one of the images I found on Google, "
    "they all seem to have dead links.</p>";

const char* kPage3Image = ":/AboutPage3.PNG";
const char* kPage3Html  =
    "<p style='font-size:18px; font-weight:bold; color:#fff;'>\"EventCountdowns\" — My First "
    "Attempt at a Recreation of the Original App</p>"
    "<p style='color:#888; font-size:13px;'>Source: This is one of the images I found on Google, "
    "they all seem to have dead links.</p>";

const char* kPage4Image = ":/AboutPage4.PNG";
const char* kPage4Html  =
    "<p style='font-size:18px; font-weight:bold; color:#fff;'>\"Media Countdowns\" — My Second "
    "Attempt at a Recreation of the Original App</p>"
    "<p style='color:#888; font-size:13px;'>Source: This is one of the images I found on Google, "
    "they all seem to have dead links.</p>";

// V5 — data source attribution.
//
// TMDB's API terms of use require BOTH their logo and this exact sentence,
// and require the logo to be less prominent than the app's own branding —
// hence a dedicated About page rather than anything on the main window.
// The wording below is quoted verbatim from those terms and should not be
// paraphrased.
//
// Both logos are drawn inline through the rich-text body (Qt resolves
// ":/name" inside <img> tags) because this page shows two of them, while
// the Page struct only carries a single image path.
//
// IGDB does not mandate a logo, but crediting it alongside TMDB is only
// fair given games data comes from there.
const char* kPage5Image = "";   // both logos live in the HTML below
const char* kPage5Html  =
    "<div style='text-align:center;'>"
    "<p style='font-size:18px; font-weight:bold; color:#fff;'>Where the Data Comes From</p>"

    "<p style='margin-top:18px;'><img src=':/TMDB.png' width='340'></p>"
    "<p style='color:#bbb; font-size:13px; margin-top:2px;'>"
    "Movie and TV show information, and all backdrop artwork.</p>"
    "<p style='color:#888; font-size:12px; margin-top:6px;'>"
    "This application uses TMDB and the TMDB APIs but is not endorsed, certified, "
    "or otherwise approved by TMDB.</p>"

    // IGDB_dark.png rather than IGDB.png: the supplied asset is 8-bit
    // grayscale with no alpha (black on white), which renders as a white
    // box on this dark page. The variant is white-on-transparent so it
    // sits on the background properly. The original file is untouched.
    "<p style='margin-top:26px;'><img src=':/IGDB_dark.png' width='150'></p>"
    "<p style='color:#bbb; font-size:13px; margin-top:2px;'>"
    "Video game information and cover art.</p>"

    "<p style='margin-top:26px;'><img src=':/TVmaze_dark.png' width='170'></p>"
    "<p style='color:#bbb; font-size:13px; margin-top:2px;'>"
    "Episode air dates, used under CC BY-SA.</p>"

    "<p style='color:#999; font-size:12px; margin-top:22px;'>"
    "Media Countdowns is free and always will be. It makes no money, shows no ads, "
    "and charges for nothing &mdash; which is what keeps these services free to use here.</p>"
    "</div>";
}

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("About");
    setModal(true);
    setStyleSheet("QDialog { background:#1e1e1e; }");

    // v3.1.92 fix #1+#3 — page 2's image stays 3x (1500x1200); pages 3 and
    // 4 are now 2x their previous cap (500x400 -> 1000x800). The frame is
    // sized to whichever page ends up needing the most room, not just
    // page 2 specifically.
    m_pages = {
        { kPage1Image, kPage1Html, QSize(350, 280),   false, false },
        { kPage2Image, kPage2Html, QSize(1500, 1200), false, true  },
        { kPage3Image, kPage3Html, QSize(1000, 800),  false, true  },
        { kPage4Image, kPage4Html, QSize(1000, 800),  false, true  },
        // V5 — attribution page. Has no Page-level image (both logos are
        // inline in its HTML), so it centres its text like page 1 rather
        // than centring an image block.
        { kPage5Image, kPage5Html, QSize(0, 0),       true,  true  },
    };

    auto* vlay = new QVBoxLayout(this);
    vlay->setContentsMargins(20, 16, 20, 16);
    vlay->setSpacing(10);

    m_pageIndicator = new QLabel(this);
    m_pageIndicator->setStyleSheet("color:#aaa; font-size:12px; font-weight:bold; background:transparent;");
    vlay->addWidget(m_pageIndicator);

    // v3.1.91 fix #2 — build the separator and Close button now (so their
    // sizeHint can be measured) but don't add them to the layout until
    // after AboutPageWidget is created with the compensation it needs.
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:#2a2a2a;");

    auto* closeBtn = new QPushButton("Close", this);
    closeBtn->setStyleSheet(
        "QPushButton { background:#0078d4; color:#fff; border:none; "
        "border-radius:4px; padding:8px 24px; font-size:13px; font-weight:bold; }"
        "QPushButton:hover { background:#1a8de4; }");

    // The page indicator sits above AboutPageWidget; the separator + Close
    // button sit below it. Those two chrome zones aren't the same height,
    // so centering a page's content within AboutPageWidget alone doesn't
    // land it in the dialog's true center — compensate by the difference.
    int topChrome = m_pageIndicator->sizeHint().height() + vlay->spacing();
    int bottomChrome = vlay->spacing() + sep->sizeHint().height()
                      + vlay->spacing() + closeBtn->sizeHint().height();
    int compensation = qMax(0, bottomChrome - topChrome);

    m_pageWidget = new AboutPageWidget(this, compensation);
    connect(m_pageWidget, &AboutPageWidget::previousRequested, this, [this]{
        if (m_currentIndex > 0) { --m_currentIndex; refreshPage(); }
    });
    connect(m_pageWidget, &AboutPageWidget::nextRequested, this, [this]{
        if (m_currentIndex < m_pages.size() - 1) { ++m_currentIndex; refreshPage(); }
    });
    vlay->addWidget(m_pageWidget);

    vlay->addWidget(sep);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(closeBtn);
    vlay->addLayout(btnRow);

    // v3.1.92 fix #3 — size the dialog to whichever of pages 2-4 needs the
    // most room (not just page 2). Page 1 is deliberately excluded here —
    // its long text has no natural cap and just adapts to whatever frame
    // the other pages settle on (see refreshPage()).
    // v3.1.93 fix #2 — measuring this by setting each page's content into
    // m_pageWidget and querying layout()->totalSizeHint() turned out to be
    // unreliable: Qt's nested layout caching (dialog -> AboutPageWidget ->
    // centerWidget, three levels deep) kept reporting the FIRST measured
    // page's dimensions even after later pages were set into the same
    // widget, clipping any page that actually needed more room. Computing
    // the needed width directly — the same image-scaling math setPage()
    // uses, but without touching any real widgets — sidesteps that
    // caching bug entirely.
    QSize maxNeeded(0, 0);
    for (int i = 1; i < m_pages.size(); ++i) {
        const Page& p = m_pages[i];
        QPixmap original(p.imagePath);
        QSize imgSize(0, 0);
        if (!original.isNull()) {
            QSize natural = original.size();
            imgSize = (natural.width() > p.maxImageSize.width() || natural.height() > p.maxImageSize.height())
                      ? natural.scaled(p.maxImageSize, Qt::KeepAspectRatio)
                      : natural;
        }
        int contentWidth  = qMax(imgSize.width(), 550);
        int neededWidth   = contentWidth + 128 /*arrow slots*/ + 40 /*dialog margins*/;
        int neededHeight  = imgSize.height() + 10 /*spacing*/ + 150 /*generous text allowance*/
                           + 250 /*indicator+separator+button chrome, generous*/;
        maxNeeded.setWidth(qMax(maxNeeded.width(), neededWidth));
        maxNeeded.setHeight(qMax(maxNeeded.height(), neededHeight));
    }
    resize(maxNeeded);

    // v3.1.92 fix #2 — extend the height down to the taskbar (the dialog
    // already opens pinned to the top of the screen), giving page 1's text
    // a lot more room than pages 2-4 alone would need.
    // v3.1.93 fix #1 — leave a small buffer below availableGeometry():
    // resize() sets this widget's own size, not the full window frame (title
    // bar/borders the OS adds on top), so filling the available height
    // exactly could still push the actual window edge slightly behind the
    // taskbar. A modest margin accounts for that safely.
    QScreen* scr = (parentWidget() && parentWidget()->screen()) ? parentWidget()->screen()
                                                                  : QGuiApplication::primaryScreen();
    if (scr) {
        int targetHeight = scr->availableGeometry().height() - 40;
        if (targetHeight > height()) resize(width(), targetHeight);
    }

    setFixedSize(size());

    // v3.1.92 fix #4 — now that the frame is locked, page 1's text can use
    // the full available width (up to where the arrow slots start) instead
    // of being constrained to whatever its own (much smaller) image implies.
    int arrowSlotsWidth = 64 * 2;
    int dialogMargins   = vlay->contentsMargins().left() + vlay->contentsMargins().right();
    m_pages[0].forceTextWidth = width() - arrowSlotsWidth - dialogMargins;

    // v3.1.94 fix #5 — page 2's image should reach the arrows on both
    // sides too, not just use its own natural size if that happens to be
    // narrower than the frame.
    m_pages[1].forceImageWidth = width() - arrowSlotsWidth - dialogMargins;

    // V5 — the attribution page has no Page-level image, so without this it
    // would fall back to the 550px minimum and sit as a narrow column in a
    // frame sized for page 2. Same treatment as page 1.
    m_pages[4].forceTextWidth = width() - arrowSlotsWidth - dialogMargins;

    m_currentIndex = 0;
    refreshPage();
}

void AboutDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (!m_positioned) {
        m_positioned = true;
        // Deferred to the next event loop pass so Qt's own internal
        // first-show geometry handling has already finished by the time
        // this runs.
        QTimer::singleShot(0, this, &AboutDialog::positionAtTop);
    }
}

// v3.1.9 fix — opens pinned to the top of the screen (horizontally
// centered), rather than vertically centered, which is simpler and avoids
// ever landing partly below the taskbar regardless of screen size.
void AboutDialog::positionAtTop()
{
    QScreen* scr = (parentWidget() && parentWidget()->screen()) ? parentWidget()->screen()
                                                                  : QGuiApplication::primaryScreen();
    if (!scr) return;
    QRect avail = scr->availableGeometry();
    QRect anchor = parentWidget() ? parentWidget()->geometry() : avail;
    int x = anchor.x() + (anchor.width() - width()) / 2;
    x = qBound(avail.left(), x, qMax(avail.left(), avail.right() - width()));
    move(x, avail.top());
}

void AboutDialog::refreshPage()
{
    const Page& p = m_pages[m_currentIndex];
    m_pageWidget->setPage(p.imagePath, p.bodyHtml, p.maxImageSize, p.centerText, p.centerVertically,
                          p.forceTextWidth, p.forceImageWidth);
    m_pageWidget->setNavAllowed(m_currentIndex > 0, m_currentIndex < m_pages.size() - 1);
    m_pageIndicator->setText(QString("About (%1 of %2)").arg(m_currentIndex + 1).arg(m_pages.size()));
    // v3.1.91 — no resizing here. The dialog was sized once, in the
    // constructor, from page 2's content, and stays that size for every page.
}
