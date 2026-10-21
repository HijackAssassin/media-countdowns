#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include "tiledata.h"
#include "tmdbscraper.h"

class AddTileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddTileDialog(QWidget* parent = nullptr);
    TileData result() const { return m_result; }

private slots:
    void onFetch();
    void onDataReady(const TileData& data);
    void onScraperError(const QString& msg);

private:
    void setBusy(bool busy);

    QLineEdit*   m_nameEdit  = nullptr;
    QPushButton* m_fetchBtn  = nullptr;
    QPushButton* m_okBtn     = nullptr;
    QLabel*      m_statusLbl = nullptr;

    TmdbScraper* m_scraper   = nullptr;
    TileData     m_result;
};
