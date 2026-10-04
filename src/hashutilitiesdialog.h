/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "checksumdata.h"
#include "checksumjob.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

class HashUtilitiesDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit HashUtilitiesDialog(const QUrl &url, QWidget *parent = nullptr,
                                ChecksumJobOptions options = {})
        : QDialog(parent)
        , m_url(url)
        , m_options(options)
    {
        setObjectName(QStringLiteral("hashUtilitiesDialog"));
        setAttribute(Qt::WA_DeleteOnClose);
        setModal(false);
        resize(540, 360);
        setMinimumSize(440, 300);

        const QString localPath = url.isLocalFile() ? url.toLocalFile() : url.toString();
        const QString fileName = url.isLocalFile() ? QFileInfo(localPath).fileName() : url.toString();
        setWindowTitle(trLocal("Suma kontrolna — ", "Checksum — ") + (fileName.isEmpty() ? localPath : fileName));

        setupUi(localPath);
        initJob(m_options.algorithm);
    }

    ~HashUtilitiesDialog() override
    {
        if (m_job) {
            m_job->cancel();
        }
    }

    void done(int r) override
    {
        if (m_job) {
            m_job->cancel();
        }
        QDialog::done(r);
        close();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (m_job) {
            m_job->cancel();
        }
        QDialog::closeEvent(event);
    }

public:
    ChecksumJob *job() const { return m_job; }
    QComboBox *algorithmCombo() const { return m_algorithmCombo; }
    QPushButton *calculateButton() const { return m_calculateButton; }
    QPushButton *cancelButton() const { return m_cancelButton; }
    QPushButton *copyButton() const { return m_copyButton; }
    QPushButton *closeButton() const { return m_closeButton; }
    QLineEdit *resultEdit() const { return m_resultEdit; }
    QLabel *noticeLabel() const { return m_noticeLabel; }
    QLabel *statusLabel() const { return m_statusLabel; }
    QProgressBar *progressBar() const { return m_progressBar; }

private Q_SLOTS:
    void onAlgorithmChanged(int index)
    {
        Q_UNUSED(index);
        const ChecksumAlgorithm alg = currentSelectedAlgorithm();

        // Update security wording
        const QString notice = algorithmNotice(alg);
        if (!notice.isEmpty()) {
            m_noticeLabel->setText(notice);
            m_noticeLabel->setVisible(true);
        } else {
            m_noticeLabel->clear();
            m_noticeLabel->setVisible(false);
        }

        // Switching algorithm invalidates any previous calculation result
        m_resultEdit->clear();
        m_copyButton->setEnabled(false);
        m_progressBar->setValue(0);
        m_progressBar->hide();

        initJob(alg);
    }

    void onCalculateClicked()
    {
        if (m_job && !m_job->isRunning()) {
            m_job->start();
        }
    }

    void onCancelClicked()
    {
        if (m_job) {
            m_job->cancel();
        }
    }

    void onCopyClicked()
    {
        if (m_job && m_job->data().hasValidResult()) {
            QApplication::clipboard()->setText(m_job->data().digest);
        }
    }

    void onProgress(quint64 done, quint64 total)
    {
        int percent = 0;
        if (total > 0) {
            percent = static_cast<int>((static_cast<long double>(done) * 100.0L) / static_cast<long double>(total));
        } else if (done > 0) {
            percent = 100;
        }
        m_progressBar->setValue(qBound(0, percent, 100));
    }

    void onStateChanged(const ChecksumData &data)
    {
        const bool running = data.state == ChecksumState::Running;
        const bool supported = data.capability == ChecksumCapability::SupportedLocalFile;

        m_algorithmCombo->setEnabled(!running);
        m_calculateButton->setEnabled(supported && !running);
        m_cancelButton->setEnabled(running);
        m_progressBar->setVisible(running);
        m_copyButton->setEnabled(data.hasValidResult());

        if (!data.hasValidResult()) {
            m_resultEdit->clear();
        }

        switch (data.state) {
        case ChecksumState::Idle:
            switch (data.capability) {
            case ChecksumCapability::SupportedLocalFile:
                m_statusLabel->setText(trLocal(
                    "Gotowy do obliczenia sumy kontrolnej.",
                    "Ready to calculate checksum."));
                break;
            case ChecksumCapability::DirectoryNotApplicable:
                m_statusLabel->setText(trLocal(
                    "Sumy katalogów nie mają zastosowania.",
                    "Directory checksums are not applicable."));
                break;
            case ChecksumCapability::SymlinkUnavailable:
                m_statusLabel->setText(trLocal(
                    "Suma dowiązania symbolicznego jest niedostępna; cel nie zostanie odczytany.",
                    "A symbolic-link checksum is unavailable; its target will not be read."));
                break;
            case ChecksumCapability::RemoteUnavailable:
                m_statusLabel->setText(trLocal(
                    "Lokalizacje zdalne nie są obsługiwane w analizie sum kontrolnych.",
                    "Remote locations are not supported for checksum analysis."));
                break;
            case ChecksumCapability::Unreadable:
                m_statusLabel->setText(trLocal(
                    "Pliku nie można odczytać.",
                    "The file cannot be read."));
                break;
            }
            break;

        case ChecksumState::Running:
            m_statusLabel->setText(trLocal("Obliczanie sumy kontrolnej (%1)…", "Calculating checksum (%1)…")
                .arg(algorithmDisplayName(data.algorithm)));
            m_progressBar->setValue(0);
            break;

        case ChecksumState::Completed:
            m_resultEdit->setText(data.digest);
            m_progressBar->setValue(100);
            m_progressBar->hide();
            m_statusLabel->setText(trLocal("Ukończono pomyślnie (%1).", "Completed successfully (%1).")
                .arg(algorithmDisplayName(data.algorithm)));
            break;

        case ChecksumState::Cancelled:
            m_statusLabel->setText(trLocal("Obliczanie anulowano.", "Calculation cancelled."));
            m_progressBar->hide();
            break;

        case ChecksumState::ChangedDuringHash:
            m_statusLabel->setText(trLocal(
                "Plik zmienił się podczas obliczania. Wynik został odrzucony.",
                "The file changed during calculation. The result was discarded."));
            m_progressBar->hide();
            break;

        case ChecksumState::Failed:
            m_statusLabel->setText(trLocal("Nie udało się obliczyć sumy: ", "Could not calculate checksum: ")
                + data.errorMessage);
            m_progressBar->hide();
            break;
        }
    }

private:
    ChecksumAlgorithm currentSelectedAlgorithm() const
    {
        return static_cast<ChecksumAlgorithm>(m_algorithmCombo->currentData().toInt());
    }

    void initJob(ChecksumAlgorithm alg)
    {
        if (m_job) {
            m_job->cancel();
            delete m_job;
            m_job = nullptr;
        }

        ChecksumJobOptions opt = m_options;
        opt.algorithm = alg;

        m_job = new ChecksumJob(m_url, this, opt);
        connect(m_job, &ChecksumJob::progress, this, &HashUtilitiesDialog::onProgress);
        connect(m_job, &ChecksumJob::stateChanged, this, &HashUtilitiesDialog::onStateChanged);

        onStateChanged(m_job->data());
    }

    void setupUi(const QString &localPath)
    {
        auto *mainLayout = new QVBoxLayout(this);

        auto *form = new QFormLayout;

        // Path
        auto *pathEdit = new QLineEdit(localPath, this);
        pathEdit->setObjectName(QStringLiteral("hashPathEdit"));
        pathEdit->setReadOnly(true);
        form->addRow(trLocal("Ścieżka pliku:", "File path:"), pathEdit);

        // Algorithm
        m_algorithmCombo = new QComboBox(this);
        m_algorithmCombo->setObjectName(QStringLiteral("hashAlgorithmCombo"));
        m_algorithmCombo->addItem(algorithmDisplayName(ChecksumAlgorithm::Sha256), static_cast<int>(ChecksumAlgorithm::Sha256));
        m_algorithmCombo->addItem(algorithmDisplayName(ChecksumAlgorithm::Sha1), static_cast<int>(ChecksumAlgorithm::Sha1));
        m_algorithmCombo->addItem(algorithmDisplayName(ChecksumAlgorithm::Md5), static_cast<int>(ChecksumAlgorithm::Md5));
        m_algorithmCombo->setCurrentIndex(0); // Default SHA-256
        form->addRow(trLocal("Algorytm:", "Algorithm:"), m_algorithmCombo);

        mainLayout->addLayout(form);

        // Security / legacy advisory notice
        m_noticeLabel = new QLabel(this);
        m_noticeLabel->setObjectName(QStringLiteral("hashNoticeLabel"));
        m_noticeLabel->setWordWrap(true);
        m_noticeLabel->setStyleSheet(QStringLiteral("color: #7f8c8d; font-size: 11px; padding: 2px 0;"));
        m_noticeLabel->setVisible(false);
        mainLayout->addWidget(m_noticeLabel);

        // Result field
        auto *resultLayout = new QVBoxLayout;
        resultLayout->addWidget(new QLabel(trLocal("Wynik sumy kontrolnej:", "Checksum result:"), this));

        m_resultEdit = new QLineEdit(this);
        m_resultEdit->setObjectName(QStringLiteral("hashResultEdit"));
        m_resultEdit->setReadOnly(true);
        m_resultEdit->setPlaceholderText(QStringLiteral("—"));
        QFont monoFont = m_resultEdit->font();
        monoFont.setFamily(QStringLiteral("monospace"));
        m_resultEdit->setFont(monoFont);
        resultLayout->addWidget(m_resultEdit);
        mainLayout->addLayout(resultLayout);

        // Status & Progress
        m_statusLabel = new QLabel(this);
        m_statusLabel->setObjectName(QStringLiteral("hashStatusLabel"));
        m_statusLabel->setWordWrap(true);
        mainLayout->addWidget(m_statusLabel);

        m_progressBar = new QProgressBar(this);
        m_progressBar->setObjectName(QStringLiteral("hashProgressBar"));
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        m_progressBar->hide();
        mainLayout->addWidget(m_progressBar);

        // Buttons
        auto *buttonBox = new QHBoxLayout;
        m_calculateButton = new QPushButton(trLocal("Oblicz", "Calculate"), this);
        m_calculateButton->setObjectName(QStringLiteral("hashCalculateButton"));
        m_calculateButton->setDefault(true);

        m_cancelButton = new QPushButton(trLocal("Anuluj", "Cancel"), this);
        m_cancelButton->setObjectName(QStringLiteral("hashCancelButton"));
        m_cancelButton->setEnabled(false);

        m_copyButton = new QPushButton(trLocal("Kopiuj", "Copy"), this);
        m_copyButton->setObjectName(QStringLiteral("hashCopyButton"));
        m_copyButton->setEnabled(false);

        m_closeButton = new QPushButton(trLocal("Zamknij", "Close"), this);
        m_closeButton->setObjectName(QStringLiteral("hashCloseButton"));

        buttonBox->addWidget(m_calculateButton);
        buttonBox->addWidget(m_cancelButton);
        buttonBox->addWidget(m_copyButton);
        buttonBox->addStretch(1);
        buttonBox->addWidget(m_closeButton);

        mainLayout->addLayout(buttonBox);

        // Connections
        connect(m_algorithmCombo, &QComboBox::currentIndexChanged, this, &HashUtilitiesDialog::onAlgorithmChanged);
        connect(m_calculateButton, &QPushButton::clicked, this, &HashUtilitiesDialog::onCalculateClicked);
        connect(m_cancelButton, &QPushButton::clicked, this, &HashUtilitiesDialog::onCancelClicked);
        connect(m_copyButton, &QPushButton::clicked, this, &HashUtilitiesDialog::onCopyClicked);
        connect(m_closeButton, &QPushButton::clicked, this, &QWidget::close);
    }

    QUrl m_url;
    ChecksumJobOptions m_options;
    ChecksumJob *m_job = nullptr;

    QComboBox *m_algorithmCombo = nullptr;
    QLabel *m_noticeLabel = nullptr;
    QLineEdit *m_resultEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_calculateButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};
