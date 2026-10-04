/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "duplicateactioncontroller.h"
#include "duplicateactiondata.h"
#include "duplicatefinderdata.h"
#include "duplicatefinderjob.h"
#include "duplicategroupmodel.h"
#include "storagetreemapwidget.h"

class FileActions;
#include "storageanalysisaccumulator.h"
#include "storageanalysisdata.h"
#include "storageanalysismodel.h"
#include "storagescandata.h"
#include "storagescanjob.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QTreeView>
#include <QVBoxLayout>

class StorageScanDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit StorageScanDialog(const QUrl &url, QWidget *parent = nullptr)
        : QDialog(parent)
        , m_url(url)
    {
        setObjectName(QStringLiteral("storageScanDialog"));
        setAttribute(Qt::WA_DeleteOnClose);
        setModal(false);
        resize(680, 540);
        setMinimumSize(520, 420);

        const QString rootPath = url.isLocalFile() ? url.toLocalFile() : url.toString();
        const QString rootName = url.isLocalFile() ? rootPath.section(QLatin1Char('/'), -1) : url.toString();
        setWindowTitle(trLocal("Analiza pamięci — ", "Storage Analysis — ") + (rootName.isEmpty() ? rootPath : rootName));

        setupUi(rootPath);

        StorageScanOptions options;
        options.rootUrl = url;
        options.rootPath = url.isLocalFile() ? url.toLocalFile() : QString();
        options.stayOnFilesystem = true;
        options.followSymlinks = false;
        options.includeHidden = true;

        m_job = new StorageScanJob(options, this);

        connect(m_job, &StorageScanJob::progress, this, &StorageScanDialog::onProgress);
        connect(m_job, &StorageScanJob::stateChanged, this, &StorageScanDialog::onStateChanged);
        connect(m_job, &StorageScanJob::finished, this, &StorageScanDialog::onFinished);

        m_job->start();
    }

    ~StorageScanDialog() override
    {
        if (m_dupJob) {
            m_dupJob->cancel();
        }
        if (m_job) {
            m_job->cancel();
        }
    }

    void done(int r) override
    {
        if (m_dupJob) {
            m_dupJob->cancel();
        }
        if (m_job) {
            m_job->cancel();
        }
        QDialog::done(r);
        close();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (m_dupJob) {
            m_dupJob->cancel();
        }
        if (m_job) {
            m_job->cancel();
        }
        QDialog::closeEvent(event);
    }

public:
    StorageScanJob *job() const { return m_job; }
    QTabWidget *tabWidget() const { return m_tabWidget; }
    StorageFilesTableModel *filesModel() const { return m_filesModel; }
    StorageDirsTableModel *dirsModel() const { return m_dirsModel; }
    QTableView *filesTableView() const { return m_filesTableView; }
    QTableView *dirsTableView() const { return m_dirsTableView; }
    QLabel *filesBanner() const { return m_filesBanner; }
    QLabel *dirsBanner() const { return m_dirsBanner; }
    QComboBox *filesLimitCombo() const { return m_filesLimitCombo; }
    QComboBox *dirsLimitCombo() const { return m_dirsLimitCombo; }
    QComboBox *filesSortCombo() const { return m_filesSortCombo; }
    QComboBox *dirsSortCombo() const { return m_dirsSortCombo; }

    QTreeView *duplicatesTreeView() const { return m_dupTreeView; }
    DuplicateGroupModel *duplicatesModel() const { return m_dupModel; }
    QPushButton *findDuplicatesButton() const { return m_findDuplicatesButton; }
    QPushButton *rerunDuplicatesButton() const { return m_dupRerunButton; }
    QPushButton *cancelDuplicatesButton() const { return m_dupCancelButton; }
    QLabel *duplicatesBanner() const { return m_dupBanner; }
    DuplicateFinderJob *duplicateJob() const { return m_dupJob; }
    const DuplicateCandidatePlan &duplicatePlan() const { return m_dupPlan; }
    StorageTreemapWidget *treemapWidget() const { return m_treemapWidget; }

    void setFileActions(FileActions *actions)
    {
        m_fileActions = actions;
        if (m_dupActionController) {
            m_dupActionController->setFileActions(actions);
        }
    }
    FileActions *fileActions() const { return m_fileActions; }
    DuplicateActionController *duplicateActionController() const { return m_dupActionController; }
    QLabel *dupSelectedLabel() const { return m_dupSelectedLabel; }
    QPushButton *dupClearSelButton() const { return m_dupClearSelButton; }
    QPushButton *dupTrashButton() const { return m_dupTrashButton; }
    QPushButton *dupMoveButton() const { return m_dupMoveButton; }

Q_SIGNALS:
    void navigateRequested(const QUrl &url);
    void drillDownRequested(const QUrl &url);

private Q_SLOTS:
    void onProgress(const StorageScanStats &stats, const QString &currentPath)
    {
        m_filesVal->setText(QString::number(stats.files));
        m_dirsVal->setText(QString::number(stats.directories));
        m_symlinksVal->setText(QString::number(stats.symlinks));
        m_totalVal->setText(QString::number(stats.scannedEntries));

        m_logicalVal->setText(QStringLiteral("%1 (%2 B)")
            .arg(StorageScanStats::formatBytes(stats.logicalBytes))
            .arg(QString::number(stats.logicalBytes)));

        m_allocatedVal->setText(QStringLiteral("%1 (%2 B)")
            .arg(StorageScanStats::formatBytes(stats.allocatedBytes))
            .arg(QString::number(stats.allocatedBytes)));

        m_uniqueVal->setText(QString::number(stats.uniquePhysicalFiles));
        m_aliasesVal->setText(QString::number(stats.hardlinkAliases));

        m_skippedVal->setText(QString::number(stats.skippedMounts));
        m_inaccessibleVal->setText(QString::number(stats.inaccessible));
        m_disappearedVal->setText(QString::number(stats.disappeared));
        m_errorsVal->setText(QString::number(stats.errors));

        if (!currentPath.isEmpty()) {
            m_currentPathLabel->setText(elidePath(currentPath));
        }
    }

    void onStateChanged(StorageScanState state)
    {
        switch (state) {
        case StorageScanState::Idle:
            m_statusLabel->setText(trLocal("Oczekiwanie…", "Idle…"));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            break;
        case StorageScanState::Running:
            m_statusLabel->setText(trLocal("Skanowanie w toku…", "Scanning in progress…"));
            m_progressBar->setRange(0, 0); // Indeterminate
            m_cancelButton->setEnabled(true);
            break;
        case StorageScanState::Completed:
            m_statusLabel->setText(trLocal("Skanowanie ukończone.", "Scan completed."));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(100);
            m_currentPathLabel->setText(QString());
            m_cancelButton->setEnabled(false);
            break;
        case StorageScanState::Cancelled:
            m_statusLabel->setText(trLocal("Skanowanie anulowane.", "Scan cancelled."));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            m_cancelButton->setEnabled(false);
            break;
        case StorageScanState::Failed:
            m_statusLabel->setText(trLocal("Błąd skanowania folderu.", "Failed to scan folder."));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            m_cancelButton->setEnabled(false);
            break;
        case StorageScanState::RemoteUnsupported:
            m_statusLabel->setText(trLocal("Lokalizacje zdalne nie są obsługiwane w analizie pamięci.",
                                           "Remote locations are not supported for storage analysis."));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            m_cancelButton->setEnabled(false);
            break;
        }
    }

    void onFinished(StorageScanState state, const StorageScanStats &stats)
    {
        onProgress(stats, QString());
        onStateChanged(state);

        if (state == StorageScanState::Completed || state == StorageScanState::Cancelled) {
            const QString rootPath = m_url.isLocalFile() ? m_url.toLocalFile() : QString();
            const auto result = StorageAnalysisAccumulator::analyze(rootPath, m_job->entries(), stats, state);
            m_filesModel->setResult(result);
            m_dirsModel->setResult(result);
            if (m_treemapWidget) {
                m_treemapWidget->setData(rootPath, m_job->entries(), stats, state);
            }

            if (result.isPartial) {
                m_filesBanner->setText(QStringLiteral("⚠️ ") + result.partialReason);
                m_filesBanner->setVisible(true);
                m_dirsBanner->setText(QStringLiteral("⚠️ ") + result.partialReason);
                m_dirsBanner->setVisible(true);
            } else {
                m_filesBanner->setVisible(false);
                m_dirsBanner->setVisible(false);
            }

            updateDuplicatesPlan(result.isPartial ? result.partialReason : QString());
        }
    }

private:
    QString elidePath(const QString &path) const
    {
        QFontMetrics fm(font());
        return fm.elidedText(path, Qt::ElideMiddle, 480);
    }

    void setupUi(const QString &rootPath)
    {
        auto *mainLayout = new QVBoxLayout(this);

        // Header section
        auto *headerBox = new QGroupBox(trLocal("Katalog bazowy", "Scan root"), this);
        auto *headerLayout = new QVBoxLayout(headerBox);
        m_rootLabel = new QLabel(rootPath, headerBox);
        m_rootLabel->setWordWrap(true);
        m_rootLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        headerLayout->addWidget(m_rootLabel);

        m_statusLabel = new QLabel(trLocal("Rozpoczynanie…", "Starting…"), headerBox);
        QFont statusFont = m_statusLabel->font();
        statusFont.setBold(true);
        m_statusLabel->setFont(statusFont);
        headerLayout->addWidget(m_statusLabel);

        m_progressBar = new QProgressBar(headerBox);
        m_progressBar->setObjectName(QStringLiteral("storageScanProgressBar"));
        m_progressBar->setTextVisible(false);
        m_progressBar->setFixedHeight(14);
        headerLayout->addWidget(m_progressBar);

        m_currentPathLabel = new QLabel(headerBox);
        m_currentPathLabel->setObjectName(QStringLiteral("storageScanCurrentPath"));
        m_currentPathLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
        headerLayout->addWidget(m_currentPathLabel);

        mainLayout->addWidget(headerBox);

        // Tab widget
        m_tabWidget = new QTabWidget(this);
        m_tabWidget->setObjectName(QStringLiteral("storageScanTabWidget"));

        // Tab 1: Summary (Existing Stage 1 layout)
        auto *summaryTab = new QWidget(m_tabWidget);
        auto *summaryLayout = new QVBoxLayout(summaryTab);
        summaryLayout->setContentsMargins(4, 4, 4, 4);

        // Metrics grid
        auto *metricsBox = new QGroupBox(trLocal("Statystyki przestrzeni dyskowej", "Storage Statistics"), summaryTab);
        auto *grid = new QGridLayout(metricsBox);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);

        int row = 0;
        grid->addWidget(new QLabel(trLocal("Pliki:", "Files:"), metricsBox), row, 0);
        m_filesVal = new QLabel(QStringLiteral("0"), metricsBox);
        grid->addWidget(m_filesVal, row, 1);
        grid->addWidget(new QLabel(trLocal("Rozmiar logiczny:", "Logical size:"), metricsBox), row, 2);
        m_logicalVal = new QLabel(QStringLiteral("0 B"), metricsBox);
        grid->addWidget(m_logicalVal, row, 3);

        ++row;
        grid->addWidget(new QLabel(trLocal("Katalogi:", "Directories:"), metricsBox), row, 0);
        m_dirsVal = new QLabel(QStringLiteral("0"), metricsBox);
        grid->addWidget(m_dirsVal, row, 1);
        grid->addWidget(new QLabel(trLocal("Rozmiar na dysku:", "Allocated size:"), metricsBox), row, 2);
        m_allocatedVal = new QLabel(QStringLiteral("0 B"), metricsBox);
        grid->addWidget(m_allocatedVal, row, 3);

        ++row;
        grid->addWidget(new QLabel(trLocal("Dowiązania sym.:", "Symlinks:"), metricsBox), row, 0);
        m_symlinksVal = new QLabel(QStringLiteral("0"), metricsBox);
        grid->addWidget(m_symlinksVal, row, 1);
        grid->addWidget(new QLabel(trLocal("Unikalne pliki fiz.:", "Unique physical files:"), metricsBox), row, 2);
        m_uniqueVal = new QLabel(QStringLiteral("0"), metricsBox);
        grid->addWidget(m_uniqueVal, row, 3);

        ++row;
        grid->addWidget(new QLabel(trLocal("Łącznie elementów:", "Total entries:"), metricsBox), row, 0);
        m_totalVal = new QLabel(QStringLiteral("0"), metricsBox);
        grid->addWidget(m_totalVal, row, 1);
        grid->addWidget(new QLabel(trLocal("Aliasy hardlinków:", "Hardlink aliases:"), metricsBox), row, 2);
        m_aliasesVal = new QLabel(QStringLiteral("0"), metricsBox);
        grid->addWidget(m_aliasesVal, row, 3);

        summaryLayout->addWidget(metricsBox);

        // Diagnostics
        auto *diagBox = new QGroupBox(trLocal("Diagnostyka skanera", "Scan Diagnostics"), summaryTab);
        auto *diagGrid = new QGridLayout(diagBox);
        diagGrid->setColumnStretch(1, 1);
        diagGrid->setColumnStretch(3, 1);

        int drow = 0;
        diagGrid->addWidget(new QLabel(trLocal("Inne systemy plików:", "Skipped mounts:"), diagBox), drow, 0);
        m_skippedVal = new QLabel(QStringLiteral("0"), diagBox);
        diagGrid->addWidget(m_skippedVal, drow, 1);
        diagGrid->addWidget(new QLabel(trLocal("Niedostępne:", "Inaccessible:"), diagBox), drow, 2);
        m_inaccessibleVal = new QLabel(QStringLiteral("0"), diagBox);
        diagGrid->addWidget(m_inaccessibleVal, drow, 3);

        ++drow;
        diagGrid->addWidget(new QLabel(trLocal("Zniknięte:", "Disappeared:"), diagBox), drow, 0);
        m_disappearedVal = new QLabel(QStringLiteral("0"), diagBox);
        diagGrid->addWidget(m_disappearedVal, drow, 1);
        diagGrid->addWidget(new QLabel(trLocal("Błędy odczytu:", "Errors:"), diagBox), drow, 2);
        m_errorsVal = new QLabel(QStringLiteral("0"), diagBox);
        diagGrid->addWidget(m_errorsVal, drow, 3);

        summaryLayout->addWidget(diagBox);
        summaryLayout->addStretch(1);
        m_tabWidget->addTab(summaryTab, trLocal("Podsumowanie", "Summary"));

        // Tab 2: Largest Files
        auto *filesTab = new QWidget(m_tabWidget);
        auto *filesLayout = new QVBoxLayout(filesTab);
        filesLayout->setContentsMargins(4, 4, 4, 4);

        auto *filesControlsLayout = new QHBoxLayout();
        filesControlsLayout->addWidget(new QLabel(trLocal("Pokaż:", "Show:"), filesTab));
        m_filesLimitCombo = new QComboBox(filesTab);
        m_filesLimitCombo->setObjectName(QStringLiteral("filesLimitCombo"));
        m_filesLimitCombo->addItem(QStringLiteral("10"), 10);
        m_filesLimitCombo->addItem(QStringLiteral("25"), 25);
        m_filesLimitCombo->addItem(QStringLiteral("50"), 50);
        m_filesLimitCombo->addItem(QStringLiteral("100"), 100);
        m_filesLimitCombo->setCurrentIndex(2); // default 50
        filesControlsLayout->addWidget(m_filesLimitCombo);

        filesControlsLayout->addSpacing(12);
        filesControlsLayout->addWidget(new QLabel(trLocal("Sortuj według:", "Sort by:"), filesTab));
        m_filesSortCombo = new QComboBox(filesTab);
        m_filesSortCombo->setObjectName(QStringLiteral("filesSortCombo"));
        m_filesSortCombo->addItem(trLocal("Rozmiar logiczny (malejąco)", "Logical size (descending)"),
                                  static_cast<int>(StorageAnalysisRankingMode::LogicalDescending));
        m_filesSortCombo->addItem(trLocal("Rozmiar na dysku (malejąco)", "Allocated size (descending)"),
                                  static_cast<int>(StorageAnalysisRankingMode::AllocatedDescending));
        filesControlsLayout->addWidget(m_filesSortCombo);
        filesControlsLayout->addStretch(1);
        filesLayout->addLayout(filesControlsLayout);

        m_filesBanner = new QLabel(filesTab);
        m_filesBanner->setObjectName(QStringLiteral("filesBanner"));
        m_filesBanner->setStyleSheet(QStringLiteral("background: #ffeaa7; color: #2d3436; padding: 4px; border-radius: 4px; font-weight: bold;"));
        m_filesBanner->setVisible(false);
        filesLayout->addWidget(m_filesBanner);

        m_filesModel = new StorageFilesTableModel(this);
        m_filesTableView = new QTableView(filesTab);
        m_filesTableView->setObjectName(QStringLiteral("filesTableView"));
        m_filesTableView->setModel(m_filesModel);
        m_filesTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_filesTableView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_filesTableView->setAlternatingRowColors(true);
        m_filesTableView->horizontalHeader()->setStretchLastSection(true);
        m_filesTableView->horizontalHeader()->setHighlightSections(false);
        m_filesTableView->setContextMenuPolicy(Qt::CustomContextMenu);
        filesLayout->addWidget(m_filesTableView);

        connect(m_filesLimitCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            m_filesModel->setLimit(m_filesLimitCombo->currentData().toInt());
        });
        connect(m_filesSortCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            m_filesModel->setRankingMode(static_cast<StorageAnalysisRankingMode>(m_filesSortCombo->currentData().toInt()));
        });
        connect(m_filesTableView, &QAbstractItemView::activated, this, [this](const QModelIndex &index) {
            const auto *entry = m_filesModel->entryAt(index.row());
            if (entry) {
                const QString parentDir = entry->path.section(QLatin1Char('/'), 0, -2);
                Q_EMIT navigateRequested(QUrl::fromLocalFile(parentDir.isEmpty() ? QStringLiteral("/") : parentDir));
            }
        });
        connect(m_filesTableView, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
            const QModelIndex index = m_filesTableView->indexAt(pos);
            const auto *entry = m_filesModel->entryAt(index.row());
            if (!entry) {
                return;
            }
            QMenu menu(this);
            auto *showAction = menu.addAction(trLocal("Pokaż w folderze", "Show in folder"));
            connect(showAction, &QAction::triggered, this, [this, entry] {
                const QString parentDir = entry->path.section(QLatin1Char('/'), 0, -2);
                Q_EMIT navigateRequested(QUrl::fromLocalFile(parentDir.isEmpty() ? QStringLiteral("/") : parentDir));
            });
            menu.exec(m_filesTableView->viewport()->mapToGlobal(pos));
        });

        m_tabWidget->addTab(filesTab, trLocal("Największe pliki", "Largest Files"));

        // Tab 3: Largest Directories
        auto *dirsTab = new QWidget(m_tabWidget);
        auto *dirsLayout = new QVBoxLayout(dirsTab);
        dirsLayout->setContentsMargins(4, 4, 4, 4);

        auto *dirsControlsLayout = new QHBoxLayout();
        dirsControlsLayout->addWidget(new QLabel(trLocal("Pokaż:", "Show:"), dirsTab));
        m_dirsLimitCombo = new QComboBox(dirsTab);
        m_dirsLimitCombo->setObjectName(QStringLiteral("dirsLimitCombo"));
        m_dirsLimitCombo->addItem(QStringLiteral("10"), 10);
        m_dirsLimitCombo->addItem(QStringLiteral("25"), 25);
        m_dirsLimitCombo->addItem(QStringLiteral("50"), 50);
        m_dirsLimitCombo->addItem(QStringLiteral("100"), 100);
        m_dirsLimitCombo->setCurrentIndex(2); // default 50
        dirsControlsLayout->addWidget(m_dirsLimitCombo);

        dirsControlsLayout->addSpacing(12);
        dirsControlsLayout->addWidget(new QLabel(trLocal("Sortuj według:", "Sort by:"), dirsTab));
        m_dirsSortCombo = new QComboBox(dirsTab);
        m_dirsSortCombo->setObjectName(QStringLiteral("dirsSortCombo"));
        m_dirsSortCombo->addItem(trLocal("Rozmiar logiczny (malejąco)", "Logical size (descending)"),
                                 static_cast<int>(StorageAnalysisRankingMode::LogicalDescending));
        m_dirsSortCombo->addItem(trLocal("Rozmiar na dysku (malejąco)", "Allocated size (descending)"),
                                 static_cast<int>(StorageAnalysisRankingMode::AllocatedDescending));
        dirsControlsLayout->addWidget(m_dirsSortCombo);
        dirsControlsLayout->addStretch(1);
        dirsLayout->addLayout(dirsControlsLayout);

        m_dirsBanner = new QLabel(dirsTab);
        m_dirsBanner->setObjectName(QStringLiteral("dirsBanner"));
        m_dirsBanner->setStyleSheet(QStringLiteral("background: #ffeaa7; color: #2d3436; padding: 4px; border-radius: 4px; font-weight: bold;"));
        m_dirsBanner->setVisible(false);
        dirsLayout->addWidget(m_dirsBanner);

        m_dirsModel = new StorageDirsTableModel(this);
        m_dirsTableView = new QTableView(dirsTab);
        m_dirsTableView->setObjectName(QStringLiteral("dirsTableView"));
        m_dirsTableView->setModel(m_dirsModel);
        m_dirsTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_dirsTableView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_dirsTableView->setAlternatingRowColors(true);
        m_dirsTableView->horizontalHeader()->setStretchLastSection(true);
        m_dirsTableView->horizontalHeader()->setHighlightSections(false);
        m_dirsTableView->setContextMenuPolicy(Qt::CustomContextMenu);
        dirsLayout->addWidget(m_dirsTableView);

        connect(m_dirsLimitCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            m_dirsModel->setLimit(m_dirsLimitCombo->currentData().toInt());
        });
        connect(m_dirsSortCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            m_dirsModel->setRankingMode(static_cast<StorageAnalysisRankingMode>(m_dirsSortCombo->currentData().toInt()));
        });
        connect(m_dirsTableView, &QAbstractItemView::activated, this, [this](const QModelIndex &index) {
            const auto *entry = m_dirsModel->entryAt(index.row());
            if (entry && !entry->isMountBoundary) {
                Q_EMIT drillDownRequested(QUrl::fromLocalFile(entry->path));
            }
        });
        connect(m_dirsTableView, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
            const QModelIndex index = m_dirsTableView->indexAt(pos);
            const auto *entry = m_dirsModel->entryAt(index.row());
            if (!entry) {
                return;
            }
            QMenu menu(this);
            auto *showAction = menu.addAction(trLocal("Pokaż w folderze", "Show in folder"));
            connect(showAction, &QAction::triggered, this, [this, entry] {
                Q_EMIT navigateRequested(QUrl::fromLocalFile(entry->path));
            });
            if (!entry->isMountBoundary) {
                auto *drillAction = menu.addAction(trLocal("Analizuj ten folder", "Analyze this folder"));
                connect(drillAction, &QAction::triggered, this, [this, entry] {
                    Q_EMIT drillDownRequested(QUrl::fromLocalFile(entry->path));
                });
            }
            menu.exec(m_dirsTableView->viewport()->mapToGlobal(pos));
        });

        m_tabWidget->addTab(dirsTab, trLocal("Największe katalogi", "Largest Directories"));

        // Tab 4: Duplicates
        auto *dupTab = new QWidget(m_tabWidget);
        auto *dupLayout = new QVBoxLayout(dupTab);
        dupLayout->setContentsMargins(4, 4, 4, 4);

        // Pre-hash plan box
        m_dupPlanBox = new QGroupBox(trLocal("Kandydaci na duplikaty (analiza wstępna)", "Duplicate Candidates (pre-scan)"), dupTab);
        auto *planGrid = new QGridLayout(m_dupPlanBox);
        planGrid->setColumnStretch(1, 1);
        planGrid->setColumnStretch(3, 1);

        planGrid->addWidget(new QLabel(trLocal("Pliki fizyczne:", "Physical files:"), m_dupPlanBox), 0, 0);
        m_dupPhysicalFilesVal = new QLabel(QStringLiteral("0"), m_dupPlanBox);
        planGrid->addWidget(m_dupPhysicalFilesVal, 0, 1);

        planGrid->addWidget(new QLabel(trLocal("Grupy o tym samym rozmiarze:", "Size candidate groups:"), m_dupPlanBox), 0, 2);
        m_dupGroupsVal = new QLabel(QStringLiteral("0"), m_dupPlanBox);
        planGrid->addWidget(m_dupGroupsVal, 0, 3);

        planGrid->addWidget(new QLabel(trLocal("Pliki do sprawdzenia:", "Candidate files:"), m_dupPlanBox), 1, 0);
        m_dupCandidateFilesVal = new QLabel(QStringLiteral("0"), m_dupPlanBox);
        planGrid->addWidget(m_dupCandidateFilesVal, 1, 1);

        planGrid->addWidget(new QLabel(trLocal("Dane do hashowania:", "Candidate bytes:"), m_dupPlanBox), 1, 2);
        m_dupCandidateBytesVal = new QLabel(QStringLiteral("0 B"), m_dupPlanBox);
        planGrid->addWidget(m_dupCandidateBytesVal, 1, 3);

        auto *findBtnLayout = new QHBoxLayout();
        m_findDuplicatesButton = new QPushButton(trLocal("Znajdź duplikaty (SHA-256)", "Find Duplicates (SHA-256)"), m_dupPlanBox);
        m_findDuplicatesButton->setObjectName(QStringLiteral("findDuplicatesButton"));
        m_findDuplicatesButton->setEnabled(false);
        findBtnLayout->addWidget(m_findDuplicatesButton);

        m_dupRerunButton = new QPushButton(trLocal("Uruchom ponownie", "Run again"), m_dupPlanBox);
        m_dupRerunButton->setObjectName(QStringLiteral("dupRerunButton"));
        m_dupRerunButton->setVisible(false);
        findBtnLayout->addWidget(m_dupRerunButton);

        findBtnLayout->addStretch(1);
        planGrid->addLayout(findBtnLayout, 2, 0, 1, 4);

        dupLayout->addWidget(m_dupPlanBox);

        // Progress box
        m_dupProgressBox = new QGroupBox(trLocal("Wyszukiwanie duplikatów w toku…", "Finding duplicates in progress…"), dupTab);
        auto *progressLayout = new QVBoxLayout(m_dupProgressBox);

        m_dupStatusLabel = new QLabel(m_dupProgressBox);
        progressLayout->addWidget(m_dupStatusLabel);

        m_dupProgressBar = new QProgressBar(m_dupProgressBox);
        m_dupProgressBar->setObjectName(QStringLiteral("dupProgressBar"));
        m_dupProgressBar->setRange(0, 100);
        m_dupProgressBar->setValue(0);
        progressLayout->addWidget(m_dupProgressBar);

        m_dupCurrentFileLabel = new QLabel(m_dupProgressBox);
        m_dupCurrentFileLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
        progressLayout->addWidget(m_dupCurrentFileLabel);

        auto *cancelLayout = new QHBoxLayout();
        m_dupCancelButton = new QPushButton(trLocal("Anuluj wyszukiwanie", "Cancel duplicate search"), m_dupProgressBox);
        m_dupCancelButton->setObjectName(QStringLiteral("dupCancelButton"));
        cancelLayout->addWidget(m_dupCancelButton);
        cancelLayout->addStretch(1);
        progressLayout->addLayout(cancelLayout);

        m_dupProgressBox->setVisible(false);
        dupLayout->addWidget(m_dupProgressBox);

        // Banner
        m_dupBanner = new QLabel(dupTab);
        m_dupBanner->setObjectName(QStringLiteral("dupBanner"));
        m_dupBanner->setStyleSheet(QStringLiteral("background: #ffeaa7; color: #2d3436; padding: 4px; border-radius: 4px; font-weight: bold;"));
        m_dupBanner->setVisible(false);
        dupLayout->addWidget(m_dupBanner);

        // Tree view
        m_dupModel = new DuplicateGroupModel(this);
        m_dupTreeView = new QTreeView(dupTab);
        m_dupTreeView->setObjectName(QStringLiteral("dupTreeView"));
        m_dupTreeView->setModel(m_dupModel);
        m_dupTreeView->setAlternatingRowColors(true);
        m_dupTreeView->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_dupTreeView->setContextMenuPolicy(Qt::CustomContextMenu);
        m_dupTreeView->header()->setStretchLastSection(true);
        m_dupTreeView->header()->setHighlightSections(false);
        dupLayout->addWidget(m_dupTreeView);

        // Duplicate actions bar
        auto *actionToolbar = new QHBoxLayout();
        m_dupSelectedLabel = new QLabel(trLocal("Zaznaczono: 0 plików (0 B)", "Selected: 0 files (0 B)"), dupTab);
        m_dupSelectedLabel->setObjectName(QStringLiteral("dupSelectedLabel"));
        actionToolbar->addWidget(m_dupSelectedLabel);

        actionToolbar->addStretch(1);

        m_dupClearSelButton = new QPushButton(trLocal("Wyczyść zaznaczenie", "Clear selection"), dupTab);
        m_dupClearSelButton->setObjectName(QStringLiteral("dupClearSelButton"));
        m_dupClearSelButton->setEnabled(false);
        actionToolbar->addWidget(m_dupClearSelButton);

        m_dupTrashButton = new QPushButton(trLocal("Przenieś do Kosza", "Move to Trash"), dupTab);
        m_dupTrashButton->setObjectName(QStringLiteral("dupTrashButton"));
        m_dupTrashButton->setEnabled(false);
        actionToolbar->addWidget(m_dupTrashButton);

        m_dupMoveButton = new QPushButton(trLocal("Przenieś do…", "Move to…"), dupTab);
        m_dupMoveButton->setObjectName(QStringLiteral("dupMoveButton"));
        m_dupMoveButton->setEnabled(false);
        actionToolbar->addWidget(m_dupMoveButton);

        dupLayout->addLayout(actionToolbar);

        m_dupActionController = new DuplicateActionController(this, m_fileActions);

        connect(m_dupModel, &DuplicateGroupModel::selectionChanged, this, &StorageScanDialog::updateDuplicateActionButtons);

        connect(m_dupClearSelButton, &QPushButton::clicked, this, [this] {
            m_dupModel->clearSelection();
        });

        connect(m_dupTrashButton, &QPushButton::clicked, this, [this] {
            const QString rootPath = m_url.isLocalFile() ? m_url.toLocalFile() : QString();
            m_dupActionController->executeTrash(m_dupModel, rootPath);
        });

        connect(m_dupMoveButton, &QPushButton::clicked, this, [this] {
            const QString rootPath = m_url.isLocalFile() ? m_url.toLocalFile() : QString();
            m_dupActionController->executeMove(m_dupModel, rootPath);
        });

        connect(m_findDuplicatesButton, &QPushButton::clicked, this, &StorageScanDialog::startDuplicateFinder);
        connect(m_dupRerunButton, &QPushButton::clicked, this, &StorageScanDialog::startDuplicateFinder);
        connect(m_dupCancelButton, &QPushButton::clicked, this, [this] {
            if (m_dupJob) {
                m_dupJob->cancel();
            }
        });
        connect(m_dupTreeView, &QAbstractItemView::activated, this, [this](const QModelIndex &index) {
            const auto *file = m_dupModel->fileForIndex(index);
            if (file) {
                const QString parentDir = file->canonicalPath.section(QLatin1Char('/'), 0, -2);
                Q_EMIT navigateRequested(QUrl::fromLocalFile(parentDir.isEmpty() ? QStringLiteral("/") : parentDir));
            }
        });
        connect(m_dupTreeView, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
            const QModelIndex index = m_dupTreeView->indexAt(pos);
            const auto *file = m_dupModel->fileForIndex(index);
            if (!file) {
                return;
            }
            QMenu menu(this);
            auto *showAction = menu.addAction(trLocal("Pokaż w folderze", "Show in folder"));
            connect(showAction, &QAction::triggered, this, [this, file] {
                const QString parentDir = file->canonicalPath.section(QLatin1Char('/'), 0, -2);
                Q_EMIT navigateRequested(QUrl::fromLocalFile(parentDir.isEmpty() ? QStringLiteral("/") : parentDir));
            });
            menu.exec(m_dupTreeView->viewport()->mapToGlobal(pos));
        });

        m_tabWidget->addTab(dupTab, trLocal("Duplikaty", "Duplicates"));

        // Tab 5: Space Map
        m_treemapWidget = new StorageTreemapWidget(m_tabWidget);
        connect(m_treemapWidget, &StorageTreemapWidget::navigateRequested, this, &StorageScanDialog::navigateRequested);
        m_tabWidget->addTab(m_treemapWidget, trLocal("Mapa zajętości", "Space Map"));

        mainLayout->addWidget(m_tabWidget);

        // Buttons
        auto *buttonBox = new QDialogButtonBox(this);
        m_cancelButton = new QPushButton(trLocal("Anuluj", "Cancel"), buttonBox);
        m_cancelButton->setObjectName(QStringLiteral("storageScanCancelButton"));
        buttonBox->addButton(m_cancelButton, QDialogButtonBox::RejectRole);

        m_closeButton = new QPushButton(trLocal("Zamknij", "Close"), buttonBox);
        m_closeButton->setObjectName(QStringLiteral("storageScanCloseButton"));
        buttonBox->addButton(m_closeButton, QDialogButtonBox::AcceptRole);

        connect(m_cancelButton, &QPushButton::clicked, this, [this] {
            if (m_job) {
                m_job->cancel();
            }
            if (m_dupJob) {
                m_dupJob->cancel();
            }
        });
        connect(m_closeButton, &QPushButton::clicked, this, &QWidget::close);

        mainLayout->addWidget(buttonBox);
    }

    void updateDuplicatesPlan(const QString &sourcePartialReason)
    {
        const QString rootPath = m_url.isLocalFile() ? m_url.toLocalFile() : QString();
        m_dupPlan = DuplicatePlanBuilder::buildPlan(rootPath, m_job->entries());
        m_dupPhysicalFilesVal->setText(QString::number(m_dupPlan.totalPhysicalFiles));
        m_dupGroupsVal->setText(QString::number(m_dupPlan.candidateGroups));
        m_dupCandidateFilesVal->setText(QString::number(m_dupPlan.candidateFiles));
        m_dupCandidateBytesVal->setText(QStringLiteral("%1 (%2 B)")
            .arg(StorageScanStats::formatBytes(m_dupPlan.candidateBytes))
            .arg(QString::number(m_dupPlan.candidateBytes)));
        m_findDuplicatesButton->setEnabled(m_dupPlan.candidateFiles > 0);

        if (!sourcePartialReason.isEmpty()) {
            m_dupBanner->setText(QStringLiteral("⚠️ ") + sourcePartialReason);
            m_dupBanner->setVisible(true);
        }
    }

    void updateDuplicateActionButtons()
    {
        if (!m_dupSelectedLabel) return;
        const int count = m_dupModel ? m_dupModel->selectedCount() : 0;
        const quint64 bytes = m_dupModel ? m_dupModel->selectedLogicalBytes() : 0;
        m_dupSelectedLabel->setText(trLocal("Zaznaczono: %1 plików (%2)", "Selected: %1 files (%2)")
            .arg(count)
            .arg(StorageScanStats::formatBytes(bytes)));
        const bool finderRunning = (m_dupJob && m_dupJob->state() == DuplicateFinderState::Running);
        const bool canAct = (count > 0) && !finderRunning;
        if (m_dupClearSelButton) m_dupClearSelButton->setEnabled(count > 0 && !finderRunning);
        if (m_dupTrashButton) m_dupTrashButton->setEnabled(canAct);
        if (m_dupMoveButton) m_dupMoveButton->setEnabled(canAct);
    }

    void startDuplicateFinder()
    {
        if (m_dupJob) {
            m_dupJob->cancel();
            delete m_dupJob;
            m_dupJob = nullptr;
        }

        const QString rootPath = m_url.isLocalFile() ? m_url.toLocalFile() : QString();
        m_dupJob = new DuplicateFinderJob(rootPath, m_job->entries(), m_job->stats(), m_job->state(), this);
        connect(m_dupJob, &DuplicateFinderJob::progress, this, &StorageScanDialog::onDupProgress);
        connect(m_dupJob, &DuplicateFinderJob::stateChanged, this, &StorageScanDialog::onDupStateChanged);
        connect(m_dupJob, &DuplicateFinderJob::finished, this, &StorageScanDialog::onDupFinished);

        m_dupModel->clear();
        updateDuplicateActionButtons();
        m_dupProgressBox->setVisible(true);
        m_findDuplicatesButton->setEnabled(false);
        m_dupRerunButton->setVisible(false);
        m_dupProgressBar->setValue(0);
        m_dupCurrentFileLabel->clear();
        m_dupStatusLabel->setText(trLocal("Rozpoczynanie obliczania sum kontrolnych…", "Starting checksum calculations…"));

        m_dupJob->start();
    }

    void onDupProgress(quint64 resolvedFiles, quint64 totalFiles,
                       quint64 resolvedBytes, quint64 totalBytes,
                       const QString &currentFilePath)
    {
        int percent = 0;
        if (totalBytes > 0) {
            percent = static_cast<int>((static_cast<long double>(resolvedBytes) * 100.0L) / static_cast<long double>(totalBytes));
        } else if (totalFiles > 0) {
            percent = static_cast<int>((resolvedFiles * 100) / totalFiles);
        }
        m_dupProgressBar->setValue(qBound(0, percent, 100));
        m_dupStatusLabel->setText(trLocal("Obliczanie sum kontrolnych: %1 / %2 plików (%3)",
                                          "Calculating checksums: %1 / %2 files (%3)")
            .arg(resolvedFiles)
            .arg(totalFiles)
            .arg(StorageScanStats::formatBytes(resolvedBytes)));
        if (!currentFilePath.isEmpty()) {
            m_dupCurrentFileLabel->setText(elidePath(currentFilePath));
        }
    }

    void onDupStateChanged(DuplicateFinderState state)
    {
        if (state == DuplicateFinderState::Cancelled) {
            m_dupStatusLabel->setText(trLocal("Wyszukiwanie duplikatów anulowane.", "Duplicate search cancelled."));
            m_dupProgressBox->setVisible(false);
            m_findDuplicatesButton->setEnabled(true);
            m_dupRerunButton->setVisible(true);
            updateDuplicateActionButtons();
        }
    }

    void onDupFinished(const DuplicateFinderResult &result)
    {
        m_dupModel->setResult(result);
        m_dupTreeView->expandAll();
        m_dupProgressBox->setVisible(false);
        m_findDuplicatesButton->setEnabled(true);
        m_dupRerunButton->setVisible(true);
        updateDuplicateActionButtons();

        QStringList warnings;
        if (result.isSourcePartial && !result.sourcePartialReason.isEmpty()) {
            warnings << result.sourcePartialReason;
        }
        if (result.isFinderPartial && !result.finderPartialReason.isEmpty()) {
            warnings << result.finderPartialReason;
        }
        if (!warnings.isEmpty()) {
            m_dupBanner->setText(QStringLiteral("⚠️ ") + warnings.join(QStringLiteral(" | ")));
            m_dupBanner->setVisible(true);
        } else {
            m_dupBanner->setVisible(false);
        }
    }

    QUrl m_url;
    StorageScanJob *m_job = nullptr;

    QLabel *m_rootLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_currentPathLabel = nullptr;

    QTabWidget *m_tabWidget = nullptr;

    QLabel *m_filesVal = nullptr;
    QLabel *m_dirsVal = nullptr;
    QLabel *m_symlinksVal = nullptr;
    QLabel *m_totalVal = nullptr;
    QLabel *m_logicalVal = nullptr;
    QLabel *m_allocatedVal = nullptr;
    QLabel *m_uniqueVal = nullptr;
    QLabel *m_aliasesVal = nullptr;

    QLabel *m_skippedVal = nullptr;
    QLabel *m_inaccessibleVal = nullptr;
    QLabel *m_disappearedVal = nullptr;
    QLabel *m_errorsVal = nullptr;

    QComboBox *m_filesLimitCombo = nullptr;
    QComboBox *m_filesSortCombo = nullptr;
    QLabel *m_filesBanner = nullptr;
    StorageFilesTableModel *m_filesModel = nullptr;
    QTableView *m_filesTableView = nullptr;

    QComboBox *m_dirsLimitCombo = nullptr;
    QComboBox *m_dirsSortCombo = nullptr;
    QLabel *m_dirsBanner = nullptr;
    StorageDirsTableModel *m_dirsModel = nullptr;
    QTableView *m_dirsTableView = nullptr;

    // Duplicates tab members
    DuplicateCandidatePlan m_dupPlan;
    DuplicateFinderJob *m_dupJob = nullptr;
    QGroupBox *m_dupPlanBox = nullptr;
    QLabel *m_dupPhysicalFilesVal = nullptr;
    QLabel *m_dupGroupsVal = nullptr;
    QLabel *m_dupCandidateFilesVal = nullptr;
    QLabel *m_dupCandidateBytesVal = nullptr;
    QPushButton *m_findDuplicatesButton = nullptr;
    QPushButton *m_dupRerunButton = nullptr;

    QGroupBox *m_dupProgressBox = nullptr;
    QLabel *m_dupStatusLabel = nullptr;
    QProgressBar *m_dupProgressBar = nullptr;
    QLabel *m_dupCurrentFileLabel = nullptr;
    QPushButton *m_dupCancelButton = nullptr;

    QLabel *m_dupBanner = nullptr;
    DuplicateGroupModel *m_dupModel = nullptr;
    QTreeView *m_dupTreeView = nullptr;

    FileActions *m_fileActions = nullptr;
    DuplicateActionController *m_dupActionController = nullptr;
    QLabel *m_dupSelectedLabel = nullptr;
    QPushButton *m_dupClearSelButton = nullptr;
    QPushButton *m_dupTrashButton = nullptr;
    QPushButton *m_dupMoveButton = nullptr;

    StorageTreemapWidget *m_treemapWidget = nullptr;

    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};
