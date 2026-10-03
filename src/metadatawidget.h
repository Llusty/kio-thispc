/* Lazy read-only metadata tab. */
#pragma once

#include "browsercommon.h"
#include "metadataprovider.h"

#include <QFormLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

class MetadataWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit MetadataWidget(const QUrl &url, QWidget *parent = nullptr,
                            MetadataExtractor extractor = {})
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("metadataWidget"));
        auto *layout = new QVBoxLayout(this);
        m_status = new QLabel(this);
        m_status->setObjectName(QStringLiteral("metadataStatus"));
        m_status->setWordWrap(true);
        layout->addWidget(m_status);
        m_rows = new QWidget(this);
        m_form = new QFormLayout(m_rows);
        m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(m_rows);
        layout->addWidget(scroll, 1);
        m_provider = new MetadataProvider(url, this, std::move(extractor));
        connect(m_provider, &MetadataProvider::dataChanged, this, &MetadataWidget::render);
        render(m_provider->data());
    }

    MetadataProvider *provider() const { return m_provider; }
    void activate() { m_provider->start(); }

    void setUrl(const QUrl &url)
    {
        delete m_provider;
        m_provider = new MetadataProvider(url, this);
        connect(m_provider, &MetadataProvider::dataChanged, this, &MetadataWidget::render);
        render(m_provider->data());
    }

private:
    void render(const MetadataData &data)
    {
        while (m_form->rowCount()) m_form->removeRow(0);
        m_rows->setVisible(data.state == MetadataState::Available);
        if (data.state == MetadataState::Available) {
            m_status->hide();
            for (const auto &row : data.rows) {
                auto *value = new QLabel(row.value, m_rows);
                value->setWordWrap(true);
                value->setTextInteractionFlags(Qt::TextSelectableByMouse);
                value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                m_form->addRow(row.label + QStringLiteral(":"), value);
            }
            return;
        }
        m_status->show();
        if (data.state == MetadataState::Loading)
            m_status->setText(trLocal("Odczytywanie metadanych…", "Loading metadata…"));
        else if (data.state == MetadataState::NoMetadata)
            m_status->setText(trLocal("Brak metadanych.", "No metadata."));
        else if (data.state == MetadataState::Disappeared)
            m_status->setText(trLocal("Plik zniknął podczas odczytu metadanych.", "The file disappeared while metadata was being read."));
        else if (data.state == MetadataState::Failed)
            m_status->setText(trLocal("Nie udało się odczytać metadanych: ", "Could not read metadata: ") + data.errorMessage);
        else {
            switch (data.capability) {
            case MetadataCapability::SupportedLocalFile: m_status->setText(trLocal("Metadane zostaną odczytane po otwarciu tej karty.", "Metadata will be loaded when this tab is opened.")); break;
            case MetadataCapability::DirectoryNotApplicable: m_status->setText(trLocal("Metadane katalogów nie mają zastosowania.", "Directory metadata is not applicable.")); break;
            case MetadataCapability::SymlinkUnavailable: m_status->setText(trLocal("Metadane dowiązania są niedostępne; cel nie zostanie odczytany.", "Symbolic-link metadata is unavailable; its target will not be read.")); break;
            case MetadataCapability::RemoteUnavailable: m_status->setText(trLocal("Metadane plików zdalnych są niedostępne bez pobierania pliku.", "Remote-file metadata is unavailable without downloading the file.")); break;
            case MetadataCapability::Unreadable: m_status->setText(trLocal("Pliku nie można odczytać.", "The file cannot be read.")); break;
            }
        }
    }

    MetadataProvider *m_provider = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_rows = nullptr;
    QFormLayout *m_form = nullptr;
};
