/* Small presentation/controller layer for on-demand checksums in Properties. */
#pragma once

#include "browsercommon.h"
#include "checksumjob.h"

#include <QApplication>
#include <QClipboard>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

class ChecksumWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit ChecksumWidget(const QUrl &url, QWidget *parent = nullptr,
                            ChecksumJobOptions options = {})
        : QWidget(parent)
        , m_options(options)
    {
        setObjectName(QStringLiteral("checksumWidget"));
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_result = new QLineEdit(this);
        m_result->setObjectName(QStringLiteral("checksumResult"));
        m_result->setReadOnly(true);
        m_result->setPlaceholderText(QStringLiteral("—"));
        form->addRow(QStringLiteral("SHA-256:"), m_result);
        layout->addLayout(form);

        m_status = new QLabel(this);
        m_status->setObjectName(QStringLiteral("checksumStatus"));
        m_status->setWordWrap(true);
        layout->addWidget(m_status);

        m_progress = new QProgressBar(this);
        m_progress->setObjectName(QStringLiteral("checksumProgress"));
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        m_progress->hide();
        layout->addWidget(m_progress);

        auto *buttons = new QHBoxLayout;
        m_calculate = new QPushButton(trLocal("Oblicz", "Calculate"), this);
        m_calculate->setObjectName(QStringLiteral("checksumCalculate"));
        m_cancel = new QPushButton(trLocal("Anuluj", "Cancel"), this);
        m_cancel->setObjectName(QStringLiteral("checksumCancel"));
        m_copy = new QPushButton(trLocal("Kopiuj", "Copy"), this);
        m_copy->setObjectName(QStringLiteral("checksumCopy"));
        buttons->addWidget(m_calculate);
        buttons->addWidget(m_cancel);
        buttons->addWidget(m_copy);
        buttons->addStretch(1);
        layout->addLayout(buttons);
        layout->addStretch(1);

        connect(m_calculate, &QPushButton::clicked, this, [this] {
            if (m_job) m_job->start();
        });
        connect(m_cancel, &QPushButton::clicked, this, [this] {
            if (m_job) m_job->cancel();
        });
        connect(m_copy, &QPushButton::clicked, this, [this] {
            if (m_job && m_job->data().hasValidResult())
                QApplication::clipboard()->setText(m_job->data().sha256);
        });
        setUrl(url);
    }

    void setUrl(const QUrl &url)
    {
        if (m_job) {
            m_job->cancel();
            delete m_job;
        }
        m_result->clear();
        m_progress->setValue(0);
        m_progress->hide();
        m_job = new ChecksumJob(url, this, m_options);
        connect(m_job, &ChecksumJob::progress, this, [this](quint64 done, quint64 total) {
            const int percent = total == 0 ? 0
                : static_cast<int>((static_cast<long double>(done) * 100.0L) / total);
            m_progress->setValue(qBound(0, percent, 100));
        });
        connect(m_job, &ChecksumJob::stateChanged, this, &ChecksumWidget::updateState);
        updateState(m_job->data());
    }

    ChecksumJob *job() const { return m_job; }

private:
    void updateState(const ChecksumData &data)
    {
        const bool running = data.state == ChecksumState::Running;
        const bool supported = data.capability == ChecksumCapability::SupportedLocalFile;
        m_calculate->setEnabled(supported && !running);
        m_cancel->setEnabled(running);
        m_progress->setVisible(running);
        m_copy->setEnabled(data.hasValidResult());
        if (!data.hasValidResult()) m_result->clear();

        if (data.state == ChecksumState::Running) {
            m_status->setText(trLocal("Obliczanie sumy kontrolnej…", "Calculating checksum…"));
            m_progress->setValue(0);
        } else if (data.state == ChecksumState::Completed) {
            m_result->setText(data.sha256);
            m_progress->setValue(100);
            m_progress->hide();
            m_status->setText(trLocal("Gotowe.", "Done."));
        } else if (data.state == ChecksumState::Cancelled) {
            m_status->setText(trLocal("Obliczanie anulowano.", "Calculation cancelled."));
        } else if (data.state == ChecksumState::ChangedDuringHash) {
            m_status->setText(trLocal(
                "Plik zmienił się podczas obliczania. Wynik został odrzucony.",
                "The file changed during calculation. The result was discarded."));
        } else if (data.state == ChecksumState::Failed) {
            m_status->setText(trLocal("Nie udało się obliczyć sumy: ", "Could not calculate checksum: ")
                              + data.errorMessage);
        } else {
            switch (data.capability) {
            case ChecksumCapability::SupportedLocalFile:
                m_status->setText(trLocal(
                    "Suma jest obliczana wyłącznie na żądanie.",
                    "The checksum is calculated only on request."));
                break;
            case ChecksumCapability::DirectoryNotApplicable:
                m_status->setText(trLocal(
                    "Sumy katalogów nie mają zastosowania.",
                    "Directory checksums are not applicable."));
                break;
            case ChecksumCapability::SymlinkUnavailable:
                m_status->setText(trLocal(
                    "Suma dowiązania symbolicznego jest niedostępna; cel nie zostanie odczytany.",
                    "A symbolic-link checksum is unavailable; its target will not be read."));
                break;
            case ChecksumCapability::RemoteUnavailable:
                m_status->setText(trLocal(
                    "Sumy plików zdalnych są niedostępne.",
                    "Remote-file checksums are unavailable."));
                break;
            case ChecksumCapability::Unreadable:
                m_status->setText(trLocal(
                    "Pliku nie można odczytać.",
                    "The file cannot be read."));
                break;
            }
        }
    }

    ChecksumJobOptions m_options;
    ChecksumJob *m_job = nullptr;
    QLineEdit *m_result = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_calculate = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_copy = nullptr;
};
