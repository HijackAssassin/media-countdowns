#include "addtiledialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>

AddTileDialog::AddTileDialog(QWidget* parent)
    : QDialog(parent)
    , m_scraper(new TmdbScraper(this))
{
    setWindowTitle("Add New Tile");
    setFixedWidth(520);
    setModal(true);

    connect(m_scraper, &TmdbScraper::dataReady,    this, &AddTileDialog::onDataReady);
    connect(m_scraper, &TmdbScraper::scraperError, this, &AddTileDialog::onScraperError);

    auto* vlay = new QVBoxLayout(this);
    vlay->setSpacing(12);
    vlay->setContentsMargins(20, 20, 20, 20);

    auto* instr = new QLabel("Enter a movie or TV show name:", this);
    instr->setStyleSheet("color:#aaaaaa; font-size:12px;");
    vlay->addWidget(instr);

    auto* row = new QHBoxLayout;
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText("e.g.  Invincible,  Supergirl 2026,  The Batman");
    m_nameEdit->setMinimumHeight(36);
    connect(m_nameEdit, &QLineEdit::returnPressed, this, &AddTileDialog::onFetch);
    row->addWidget(m_nameEdit);

    m_fetchBtn = new QPushButton("Search", this);
    m_fetchBtn->setFixedSize(80, 36);
    connect(m_fetchBtn, &QPushButton::clicked, this, &AddTileDialog::onFetch);
    row->addWidget(m_fetchBtn);
    vlay->addLayout(row);

    m_statusLbl = new QLabel(this);
    m_statusLbl->setWordWrap(true);
    m_statusLbl->setStyleSheet("color:#666688; font-size:12px;");
    m_statusLbl->hide();
    vlay->addWidget(m_statusLbl);

    vlay->addStretch();

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();

    auto* cancelBtn = new QPushButton("Cancel", this);
    cancelBtn->setFixedWidth(80);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    m_okBtn = new QPushButton("Add Tile", this);
    m_okBtn->setFixedWidth(90);
    m_okBtn->setEnabled(false);
    m_okBtn->setStyleSheet(
        "QPushButton:enabled  { background-color:#0078d4; color:#fff; border:none; border-radius:4px; }"
        "QPushButton:disabled { background-color:#333;    color:#666; border:none; border-radius:4px; }");
    connect(m_okBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(m_okBtn);
    vlay->addLayout(btnRow);
}

void AddTileDialog::onFetch()
{
    QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) return;
    setBusy(true);
    m_okBtn->setEnabled(false);
    m_statusLbl->setStyleSheet("color:#6666aa; font-size:12px;");
    m_statusLbl->setText("Searching TMDB…");
    m_statusLbl->show();
    m_scraper->searchMedia(name);
}

void AddTileDialog::onDataReady(const TileData& data)
{
    m_result = data;
    setBusy(false);
    m_okBtn->setEnabled(true);

    QString preview = QString("<b>%1</b><br>").arg(data.title.toHtmlEscaped());
    if (!data.statusLabel.isEmpty()) preview += data.statusLabel + " – ";
    preview += data.dateDisplay;

    m_statusLbl->setStyleSheet("color:#55bb55; font-size:12px;");
    m_statusLbl->setText("✓  " + preview);
}

void AddTileDialog::onScraperError(const QString& msg)
{
    setBusy(false);
    m_okBtn->setEnabled(false);
    m_statusLbl->setStyleSheet("color:#cc5555; font-size:12px;");
    m_statusLbl->setText("✗  " + msg);
}

void AddTileDialog::setBusy(bool busy)
{
    m_nameEdit->setEnabled(!busy);
    m_fetchBtn->setEnabled(!busy);
    m_fetchBtn->setText(busy ? "…" : "Search");
}
