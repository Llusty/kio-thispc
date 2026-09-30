/*
 * Split View pane comparison dialog.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "splitcomparemodel.h"
#include "splitsyncpreviewdialog.h"
#include "searchcontroller.h"

#include <KIO/ListJob>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

class SplitCompareDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SplitCompareDialog(
        const QUrl &leftUrl,
        const QUrl &rightUrl,
        bool showHidden,
        QWidget *parent = nullptr)
        : QDialog(parent)
        , m_leftUrl(leftUrl)
        , m_rightUrl(rightUrl)
        , m_showHidden(showHidden)
    {
        setWindowTitle(trLocal("Porównanie paneli", "Compare Panels"));
        setMinimumSize(850, 560);
        resize(920, 600);

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(14, 14, 14, 14);
        mainLayout->setSpacing(10);

        // Header Paths with HTML escaping
        auto *headerGroup = new QGroupBox(trLocal("Porównywane lokalizacje", "Compared Locations"), this);
        auto *headerLayout = new QVBoxLayout(headerGroup);
        headerLayout->setContentsMargins(10, 8, 10, 8);
        headerLayout->setSpacing(4);

        m_leftPathLabel = new QLabel(this);
        m_leftPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_leftPathLabel->setText(QStringLiteral("<b>%1:</b> %2")
                                     .arg(trLocal("Lewy panel", "Left pane"),
                                          urlForDisplay(m_leftUrl).toHtmlEscaped()));

        m_rightPathLabel = new QLabel(this);
        m_rightPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_rightPathLabel->setText(QStringLiteral("<b>%1:</b> %2")
                                      .arg(trLocal("Prawy panel", "Right pane"),
                                           urlForDisplay(m_rightUrl).toHtmlEscaped()));

        headerLayout->addWidget(m_leftPathLabel);
        headerLayout->addWidget(m_rightPathLabel);
        mainLayout->addWidget(headerGroup);

        // Status & Summary Row
        auto *summaryLayout = new QHBoxLayout;
        m_summaryLabel = new QLabel(this);
        m_summaryLabel->setText(trLocal("Wczytywanie elementów…", "Loading items…"));
        summaryLayout->addWidget(m_summaryLabel, 1);

        m_refreshButton = new QPushButton(themedIcon(QStringLiteral("view-refresh")),
                                          trLocal("Odśwież", "Refresh"), this);
        connect(m_refreshButton, &QPushButton::clicked, this, &SplitCompareDialog::startListing);
        summaryLayout->addWidget(m_refreshButton);

        mainLayout->addLayout(summaryLayout);

        // Progress bar during async listing
        m_progressBar = new QProgressBar(this);
        m_progressBar->setRange(0, 0); // Indeterminate
        m_progressBar->setTextVisible(false);
        m_progressBar->setMaximumHeight(4);
        mainLayout->addWidget(m_progressBar);

        // Table Widget
        m_table = new QTableWidget(this);
        m_table->setColumnCount(6);
        m_table->setHorizontalHeaderLabels({
            trLocal("Nazwa", "Name"),
            trLocal("Stan", "Status"),
            trLocal("Szczegóły", "Details"),
            trLocal("Lewy (Rozmiar / Data)", "Left (Size / Date)"),
            trLocal("Prawy (Rozmiar / Data)", "Right (Size / Date)"),
            trLocal("Typ", "Type")
        });
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        m_table->setColumnWidth(0, 220);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setAlternatingRowColors(true);
        m_table->verticalHeader()->setVisible(false);
        mainLayout->addWidget(m_table, 1);

        // Bottom Action Bar: Sync Direction + Preview Button + Close Button Box
        auto *bottomLayout = new QHBoxLayout;

        auto *dirLabel = new QLabel(trLocal("Kierunek:", "Direction:"), this);
        m_directionCombo = new QComboBox(this);
        m_directionCombo->addItem(trLocal("Lewy → Prawy", "Left → Right"), static_cast<int>(SyncDirection::LeftToRight));
        m_directionCombo->addItem(trLocal("Prawy → Lewy", "Right → Left"), static_cast<int>(SyncDirection::RightToLeft));

        m_previewPlanButton = new QPushButton(themedIcon(QStringLiteral("view-preview")),
                                              trLocal("Podgląd planu…", "Preview sync plan…"), this);
        m_previewPlanButton->setEnabled(false);
        connect(m_previewPlanButton, &QPushButton::clicked, this, &SplitCompareDialog::openSyncPlanPreview);

        bottomLayout->addWidget(dirLabel);
        bottomLayout->addWidget(m_directionCombo);
        bottomLayout->addWidget(m_previewPlanButton);

        m_syncButton = new QPushButton(themedIcon(QStringLiteral("view-refresh")),
                                       trLocal("Synchronizuj…", "Synchronize…"), this);
        m_syncButton->setEnabled(false);
        connect(m_syncButton, &QPushButton::clicked, this, &SplitCompareDialog::startSyncExecution);
        connect(m_directionCombo, &QComboBox::currentIndexChanged, this, &SplitCompareDialog::updateSyncButtonState);
        bottomLayout->addWidget(m_syncButton);

        bottomLayout->addStretch(1);

        m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        bottomLayout->addWidget(m_buttonBox);

        mainLayout->addLayout(bottomLayout);

        startListing();
    }

    ~SplitCompareDialog() override
    {
        cancelJobs();
    }

    QList<CompareEntry> comparisonResults() const
    {
        return m_entries;
    }

    quint64 generation() const
    {
        return m_generation;
    }

    void startListing()
    {
        cancelJobs();
        ++m_generation;
        m_leftPending.clear();
        m_rightPending.clear();
        m_entries.clear();
        m_leftFinished = false;
        m_rightFinished = false;
        m_leftError.clear();
        m_rightError.clear();

        m_progressBar->show();
        m_refreshButton->setEnabled(false);
        if (m_previewPlanButton) m_previewPlanButton->setEnabled(false);
        if (m_syncButton) m_syncButton->setEnabled(false);
        m_summaryLabel->setText(trLocal("Pobieranie zawartości paneli…", "Fetching pane contents…"));
        m_table->setRowCount(0);

        const quint64 gen = m_generation;

        // Left ListJob
        KIO::ListJob *leftJob = KIO::listDir(m_leftUrl, KIO::HideProgressInfo);
        leftJob->setUiDelegate(nullptr);
        m_leftJob = leftJob;

        connect(leftJob, &KIO::ListJob::entries, this,
                [this, gen, leftJob](KIO::Job *, const KIO::UDSEntryList &entries) {
            if (gen != m_generation || m_leftJob != leftJob) return;
            for (const KIO::UDSEntry &entry : entries) {
                const QString rawName = entry.stringValue(KIO::UDSEntry::UDS_NAME);
                if (rawName.isEmpty() || rawName == QStringLiteral(".") || rawName == QStringLiteral("..")
                    || (!m_showHidden && rawName.startsWith(QLatin1Char('.')))) {
                    continue;
                }
                m_leftPending.push_back(fileInfoForEntry(m_leftUrl, entry));
            }
        });

        connect(leftJob, &KJob::result, this, [this, gen, leftJob](KJob *job) {
            if (gen != m_generation || m_leftJob != leftJob) return;
            m_leftJob = nullptr;
            m_leftFinished = true;
            if (job->error()) {
                m_leftError = job->errorString();
            }
            checkBothFinished();
        });

        // Right ListJob
        KIO::ListJob *rightJob = KIO::listDir(m_rightUrl, KIO::HideProgressInfo);
        rightJob->setUiDelegate(nullptr);
        m_rightJob = rightJob;

        connect(rightJob, &KIO::ListJob::entries, this,
                [this, gen, rightJob](KIO::Job *, const KIO::UDSEntryList &entries) {
            if (gen != m_generation || m_rightJob != rightJob) return;
            for (const KIO::UDSEntry &entry : entries) {
                const QString rawName = entry.stringValue(KIO::UDSEntry::UDS_NAME);
                if (rawName.isEmpty() || rawName == QStringLiteral(".") || rawName == QStringLiteral("..")
                    || (!m_showHidden && rawName.startsWith(QLatin1Char('.')))) {
                    continue;
                }
                m_rightPending.push_back(fileInfoForEntry(m_rightUrl, entry));
            }
        });

        connect(rightJob, &KJob::result, this, [this, gen, rightJob](KJob *job) {
            if (gen != m_generation || m_rightJob != rightJob) return;
            m_rightJob = nullptr;
            m_rightFinished = true;
            if (job->error()) {
                m_rightError = job->errorString();
            }
            checkBothFinished();
        });
    }

private:
    void cancelJobs()
    {
        if (m_leftJob) {
            m_leftJob->kill();
            m_leftJob = nullptr;
        }
        if (m_rightJob) {
            m_rightJob->kill();
            m_rightJob = nullptr;
        }
    }

    void checkBothFinished()
    {
        if (!m_leftFinished || !m_rightFinished) {
            return;
        }

        m_progressBar->hide();
        m_refreshButton->setEnabled(true);

        if (!m_leftError.isEmpty() || !m_rightError.isEmpty()) {
            if (m_previewPlanButton) m_previewPlanButton->setEnabled(false);
            QStringList errors;
            if (!m_leftError.isEmpty()) {
                errors.append(trLocal("Błąd lewego panelu: ", "Left pane error: ") + m_leftError);
            }
            if (!m_rightError.isEmpty()) {
                errors.append(trLocal("Błąd prawego panelu: ", "Right pane error: ") + m_rightError);
            }
            m_summaryLabel->setText(errors.join(QStringLiteral(" | ")));
            return;
        }

        if (m_previewPlanButton) m_previewPlanButton->setEnabled(true);
        populateResults();
    }

    void openSyncPlanPreview()
    {
        if (!m_directionCombo) return;
        const SyncDirection direction = static_cast<SyncDirection>(
            m_directionCombo->currentData().toInt());
        const auto plan = computeSyncPlan(m_entries, direction);
        SyncPlanPreviewDialog dialog(m_leftUrl, m_rightUrl, plan, direction, m_generation, this);
        if (dialog.exec() == QDialog::Accepted || dialog.wasExecuted()) {
            startListing();
        }
    }

    void startSyncExecution()
    {
        if (!m_directionCombo) return;
        const SyncDirection direction = static_cast<SyncDirection>(
            m_directionCombo->currentData().toInt());
        const auto plan = computeSyncPlan(m_entries, direction);
        const auto summary = summarizeSyncPlan(plan);
        if (summary.executable == 0) return;

        const QUrl sourceUrl = (direction == SyncDirection::LeftToRight) ? m_leftUrl : m_rightUrl;
        const QUrl destUrl = (direction == SyncDirection::LeftToRight) ? m_rightUrl : m_leftUrl;

        SyncConfirmationDialog confirmDlg(sourceUrl, destUrl, direction, summary, this);
        if (confirmDlg.exec() != QDialog::Accepted) {
            return;
        }

        SyncExecutionDialog execDlg(sourceUrl, destUrl, plan, direction, this);
        execDlg.exec();

        startListing();
    }

    void updateSyncButtonState()
    {
        if (!m_syncButton || !m_directionCombo || m_entries.isEmpty()) {
            if (m_syncButton) m_syncButton->setEnabled(false);
            return;
        }
        const SyncDirection direction = static_cast<SyncDirection>(
            m_directionCombo->currentData().toInt());
        const auto plan = computeSyncPlan(m_entries, direction);
        const auto summary = summarizeSyncPlan(plan);
        m_syncButton->setEnabled(summary.executable > 0);
    }

    void populateResults()
    {
        m_entries = computePaneComparison(m_leftPending, m_rightPending);

        int countSame = 0;
        int countChanged = 0;
        int countOnlyLeft = 0;
        int countOnlyRight = 0;

        for (const CompareEntry &entry : std::as_const(m_entries)) {
            switch (entry.status) {
            case CompareStatus::Same: ++countSame; break;
            case CompareStatus::Changed: ++countChanged; break;
            case CompareStatus::OnlyLeft: ++countOnlyLeft; break;
            case CompareStatus::OnlyRight: ++countOnlyRight; break;
            }
        }

        m_summaryLabel->setText(
            isPolish()
                ? QStringLiteral("Wszystkich: %1 | Takie same: %2 | Zmienione: %3 | Tylko lewy: %4 | Tylko prawy: %5")
                      .arg(m_entries.size())
                      .arg(countSame)
                      .arg(countChanged)
                      .arg(countOnlyLeft)
                      .arg(countOnlyRight)
                : QStringLiteral("Total: %1 | Same: %2 | Changed: %3 | Only left: %4 | Only right: %5")
                      .arg(m_entries.size())
                      .arg(countSame)
                      .arg(countChanged)
                      .arg(countOnlyLeft)
                      .arg(countOnlyRight));

        m_table->setRowCount(m_entries.size());

        QMimeDatabase mimeDb;

        for (int row = 0; row < m_entries.size(); ++row) {
            const CompareEntry &entry = m_entries.at(row);

            // Column 0: Name + Icon
            QIcon icon;
            if (entry.isDirectory) {
                icon = themedIcon(QStringLiteral("folder"));
            } else {
                const FileInfo &info = entry.hasLeft ? entry.leftInfo : entry.rightInfo;
                const QMimeType mime = resolvedMimeType(info, mimeDb);
                QString iconName = !info.iconName.isEmpty() ? info.iconName : mime.iconName();
                if (iconName.isEmpty()) iconName = mime.genericIconName();
                icon = themedIcon(!iconName.isEmpty() ? iconName : QStringLiteral("text-x-generic"));
            }
            auto *nameItem = new QTableWidgetItem(icon, entry.name);

            // Column 1: Status
            auto *statusItem = new QTableWidgetItem(compareStatusLabel(entry.status));

            // Column 2: Reason / Details
            auto *reasonItem = new QTableWidgetItem(entry.differenceReason);

            // Column 3: Left Info
            QString leftText = entry.hasLeft ? formatFileItemMetadata(entry.leftInfo) : QStringLiteral("—");
            auto *leftItem = new QTableWidgetItem(leftText);

            // Column 4: Right Info
            QString rightText = entry.hasRight ? formatFileItemMetadata(entry.rightInfo) : QStringLiteral("—");
            auto *rightItem = new QTableWidgetItem(rightText);

            // Column 5: Type
            QString typeText;
            if (entry.isDirectory) {
                typeText = trLocal("Folder", "Folder");
            } else if (entry.hasLeft && entry.hasRight && entry.leftInfo.isDir != entry.rightInfo.isDir) {
                typeText = trLocal("Niezgodny", "Mismatch");
            } else {
                const FileInfo &info = entry.hasLeft ? entry.leftInfo : entry.rightInfo;
                typeText = fileTypeLabel(info, mimeDb);
            }
            auto *typeItem = new QTableWidgetItem(typeText);

            m_table->setItem(row, 0, nameItem);
            m_table->setItem(row, 1, statusItem);
            m_table->setItem(row, 2, reasonItem);
            m_table->setItem(row, 3, leftItem);
            m_table->setItem(row, 4, rightItem);
            m_table->setItem(row, 5, typeItem);
        }

        updateSyncButtonState();
    }

    QUrl m_leftUrl;
    QUrl m_rightUrl;
    bool m_showHidden = false;

    quint64 m_generation = 0;
    QPointer<KIO::ListJob> m_leftJob;
    QPointer<KIO::ListJob> m_rightJob;
    QList<FileInfo> m_leftPending;
    QList<FileInfo> m_rightPending;
    bool m_leftFinished = false;
    bool m_rightFinished = false;
    QString m_leftError;
    QString m_rightError;
    QList<CompareEntry> m_entries;

    QLabel *m_leftPathLabel = nullptr;
    QLabel *m_rightPathLabel = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QTableWidget *m_table = nullptr;
    QComboBox *m_directionCombo = nullptr;
    QPushButton *m_previewPlanButton = nullptr;
    QPushButton *m_syncButton = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};
