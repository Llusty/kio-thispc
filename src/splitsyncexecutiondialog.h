/*
 * Split View synchronization confirmation and execution dialogs.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "splitsyncexecutor.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

class SyncConfirmationDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SyncConfirmationDialog(
        const QUrl &sourceUrl,
        const QUrl &destUrl,
        SyncDirection direction,
        const SyncPlanSummary &summary,
        QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(trLocal("Potwierdzenie synchronizacji", "Confirm Synchronization"));
        setMinimumSize(540, 360);
        resize(580, 400);

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(14, 14, 14, 14);
        mainLayout->setSpacing(12);

        // Header Paths & Direction
        auto *headerGroup = new QGroupBox(trLocal("Kierunek i lokalizacje", "Direction and Locations"), this);
        auto *headerLayout = new QVBoxLayout(headerGroup);
        headerLayout->setContentsMargins(10, 8, 10, 8);
        headerLayout->setSpacing(4);

        const QString sourceLabel = (direction == SyncDirection::LeftToRight)
            ? trLocal("Lewy panel", "Left pane")
            : trLocal("Prawy panel", "Right pane");
        const QString destLabel = (direction == SyncDirection::LeftToRight)
            ? trLocal("Prawy panel", "Right pane")
            : trLocal("Lewy panel", "Left pane");

        auto *sourcePathLabel = new QLabel(this);
        sourcePathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sourcePathLabel->setText(QStringLiteral("<b>%1 (%2):</b> %3")
                                     .arg(trLocal("Źródło", "Source"),
                                          sourceLabel,
                                          urlForDisplay(sourceUrl).toHtmlEscaped()));

        auto *destPathLabel = new QLabel(this);
        destPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        destPathLabel->setText(QStringLiteral("<b>%1 (%2):</b> %3")
                                   .arg(trLocal("Cel", "Destination"),
                                        destLabel,
                                        urlForDisplay(destUrl).toHtmlEscaped()));

        auto *dirLabel = new QLabel(this);
        dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        dirLabel->setText(QStringLiteral("<b>%1:</b> %2")
                              .arg(trLocal("Kierunek", "Direction"),
                                   syncDirectionLabel(direction)));

        headerLayout->addWidget(sourcePathLabel);
        headerLayout->addWidget(destPathLabel);
        headerLayout->addWidget(dirLabel);
        mainLayout->addWidget(headerGroup);

        // Operations Box
        auto *opsGroup = new QGroupBox(trLocal("Zaplanowane operacje", "Planned Operations"), this);
        auto *opsLayout = new QVBoxLayout(opsGroup);
        opsLayout->setContentsMargins(10, 8, 10, 8);
        opsLayout->setSpacing(4);

        auto *opsLabel = new QLabel(this);
        opsLabel->setText(isPolish()
            ? QStringLiteral("• Pliki do skopiowania: <b>%1</b>\n• Pliki do zaktualizowania: <b>%2</b>")
                  .arg(summary.copyFile).arg(summary.updateFile)
            : QStringLiteral("• Files to copy: <b>%1</b>\n• Files to update: <b>%2</b>")
                  .arg(summary.copyFile).arg(summary.updateFile));
        opsLayout->addWidget(opsLabel);
        mainLayout->addWidget(opsGroup);

        // Safety rules Box
        auto *rulesGroup = new QGroupBox(trLocal("Zasady bezpieczeństwa (wyłączone z wykonania)", "Safety Rules (Excluded from execution)"), this);
        auto *rulesLayout = new QVBoxLayout(rulesGroup);
        rulesLayout->setContentsMargins(10, 8, 10, 8);
        rulesLayout->setSpacing(3);

        auto *rulesLabel = new QLabel(this);
        rulesLabel->setStyleSheet(QStringLiteral("color: #666;"));
        rulesLabel->setText(isPolish()
            ? QStringLiteral("• Konflikty typu (plik / folder) NIE będą modyfikowane\n"
                             "• Nieobsługiwane elementy (np. foldery źródłowe) NIE będą modyfikowane\n"
                             "• Foldery NIE są synchronizowane rekurencyjnie\n"
                             "• Żadne pliki po stronie docelowej NIE będą usuwane")
            : QStringLiteral("• Type conflicts (file / folder) will NOT be modified\n"
                             "• Unsupported items (e.g. source folders) will NOT be modified\n"
                             "• Folders are NOT synchronized recursively\n"
                             "• No destination files will be deleted"));
        rulesLayout->addWidget(rulesLabel);
        mainLayout->addWidget(rulesGroup);

        mainLayout->addStretch(1);

        // Buttons
        auto *buttonBox = new QDialogButtonBox(this);
        auto *syncBtn = buttonBox->addButton(trLocal("Synchronizuj", "Synchronize"), QDialogButtonBox::AcceptRole);
        syncBtn->setIcon(themedIcon(QStringLiteral("view-refresh")));
        auto *cancelBtn = buttonBox->addButton(trLocal("Anuluj", "Cancel"), QDialogButtonBox::RejectRole);

        connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

        // Default to Cancel for safety
        cancelBtn->setDefault(true);
        cancelBtn->setFocus();

        mainLayout->addWidget(buttonBox);
    }
};

class SyncExecutionDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SyncExecutionDialog(
        const QUrl &sourceUrl,
        const QUrl &destUrl,
        const QList<SyncPlanEntry> &plan,
        SyncDirection direction,
        QWidget *parent = nullptr)
        : QDialog(parent)
        , m_executor(new SplitSyncExecutor(sourceUrl, destUrl, plan, direction, this))
    {
        setWindowTitle(trLocal("Wykonywanie synchronizacji", "Executing Synchronization"));
        setMinimumSize(620, 420);
        resize(660, 460);

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(14, 14, 14, 14);
        mainLayout->setSpacing(10);

        // Header Status
        m_statusLabel = new QLabel(trLocal("Przygotowywanie synchronizacji…", "Preparing synchronization…"), this);
        m_statusLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
        mainLayout->addWidget(m_statusLabel);

        // Current item
        m_itemLabel = new QLabel(this);
        m_itemLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        mainLayout->addWidget(m_itemLabel);

        // Progress bar
        const SyncPlanSummary sum = summarizeSyncPlan(plan);
        m_progressBar = new QProgressBar(this);
        m_progressBar->setRange(0, qMax(1, sum.executable));
        m_progressBar->setValue(0);
        mainLayout->addWidget(m_progressBar);

        // Summary details box (hidden during active execution, shown when finished)
        m_summaryBox = new QGroupBox(trLocal("Raport końcowy", "Final Report"), this);
        auto *summaryLayout = new QVBoxLayout(m_summaryBox);
        summaryLayout->setContentsMargins(10, 8, 10, 8);
        summaryLayout->setSpacing(6);

        m_summaryTextLabel = new QLabel(this);
        m_summaryTextLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        summaryLayout->addWidget(m_summaryTextLabel);

        m_errorTable = new QTableWidget(this);
        m_errorTable->setColumnCount(3);
        m_errorTable->setHorizontalHeaderLabels({
            trLocal("Nazwa", "Name"),
            trLocal("Operacja", "Operation"),
            trLocal("Błąd", "Error")
        });
        m_errorTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_errorTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_errorTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        m_errorTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_errorTable->setAlternatingRowColors(true);
        m_errorTable->verticalHeader()->setVisible(false);
        m_errorTable->hide();
        summaryLayout->addWidget(m_errorTable);

        m_summaryBox->hide();
        mainLayout->addWidget(m_summaryBox, 1);

        // Action Buttons: Cancel during execution, Close when finished
        auto *bottomLayout = new QHBoxLayout;
        bottomLayout->addStretch(1);

        m_cancelButton = new QPushButton(trLocal("Anuluj", "Cancel"), this);
        connect(m_cancelButton, &QPushButton::clicked, this, &SyncExecutionDialog::onCancelClicked);
        bottomLayout->addWidget(m_cancelButton);

        m_closeButton = new QPushButton(trLocal("Zamknij", "Close"), this);
        m_closeButton->hide();
        connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
        bottomLayout->addWidget(m_closeButton);

        mainLayout->addLayout(bottomLayout);

        // Connect executor signals
        connect(m_executor, &SplitSyncExecutor::progress, this,
                [this](int current, int total, const SyncPlanEntry &, const QString &description) {
            m_progressBar->setMaximum(total);
            m_progressBar->setValue(current);
            m_statusLabel->setText(
                isPolish()
                    ? QStringLiteral("Synchronizowanie %1 z %2").arg(current).arg(total)
                    : QStringLiteral("Synchronizing %1 of %2").arg(current).arg(total));
            m_itemLabel->setText(description);
        });

        connect(m_executor, &SplitSyncExecutor::finished, this, &SyncExecutionDialog::onExecutionFinished);

        // Start execution asynchronously when event loop starts
        QTimer::singleShot(0, m_executor, &SplitSyncExecutor::start);
    }

    SyncExecutionReport report() const
    {
        return m_report;
    }

private:
    void onCancelClicked()
    {
        m_cancelButton->setEnabled(false);
        m_statusLabel->setText(trLocal("Anulowanie…", "Cancelling…"));
        m_executor->cancel();
    }

    void onExecutionFinished(const SyncExecutionReport &report)
    {
        m_report = report;
        m_progressBar->hide();
        m_itemLabel->hide();
        m_cancelButton->hide();
        m_closeButton->show();
        m_closeButton->setFocus();

        if (report.preflightFailed) {
            m_statusLabel->setText(trLocal("Weryfikacja wstępna nie powiodła się — brak zmian na dysku",
                                           "Preflight verification failed — no changes made on disk"));
            m_summaryTextLabel->setText(
                trLocal("Wykryto niezgodność stanu katalogów ze snapshotem porównania:\n\n",
                        "Folder state discrepancy detected compared to comparison snapshot:\n\n")
                + report.preflightError);
        } else if (report.wasCancelled) {
            m_statusLabel->setText(trLocal("Synchronizacja anulowana przez użytkownika",
                                           "Synchronization cancelled by user"));
            m_summaryTextLabel->setText(
                isPolish()
                    ? QStringLiteral("Skopiowano: %1 | Zaktualizowano: %2 | Anulowano: %3 | Błędy: %4")
                          .arg(report.copiedCount)
                          .arg(report.updatedCount)
                          .arg(report.cancelledCount)
                          .arg(report.errorCount)
                    : QStringLiteral("Copied: %1 | Updated: %2 | Cancelled: %3 | Errors: %4")
                          .arg(report.copiedCount)
                          .arg(report.updatedCount)
                          .arg(report.cancelledCount)
                          .arg(report.errorCount));
        } else {
            m_statusLabel->setText(report.errorCount == 0
                ? trLocal("Synchronizacja zakończona pomyślnie", "Synchronization completed successfully")
                : trLocal("Synchronizacja zakończona z błędami", "Synchronization completed with errors"));

            m_summaryTextLabel->setText(
                isPolish()
                    ? QStringLiteral("Skopiowano: %1 | Zaktualizowano: %2 | Bez zmian: %3 | Konflikty: %4 | Nieobsługiwane: %5 | Błędy: %6")
                          .arg(report.copiedCount)
                          .arg(report.updatedCount)
                          .arg(report.noActionCount)
                          .arg(report.conflictCount)
                          .arg(report.unsupportedCount)
                          .arg(report.errorCount)
                    : QStringLiteral("Copied: %1 | Updated: %2 | No action: %3 | Conflicts: %4 | Unsupported: %5 | Errors: %6")
                          .arg(report.copiedCount)
                          .arg(report.updatedCount)
                          .arg(report.noActionCount)
                          .arg(report.conflictCount)
                          .arg(report.unsupportedCount)
                          .arg(report.errorCount));
        }

        if (!report.errors.isEmpty()) {
            m_errorTable->setRowCount(report.errors.size());
            for (int i = 0; i < report.errors.size(); ++i) {
                const auto &err = report.errors.at(i);
                m_errorTable->setItem(i, 0, new QTableWidgetItem(err.name));
                m_errorTable->setItem(i, 1, new QTableWidgetItem(syncPlanActionLabel(err.action)));
                m_errorTable->setItem(i, 2, new QTableWidgetItem(err.errorMessage));
            }
            m_errorTable->show();
        }

        m_summaryBox->show();
    }

    SplitSyncExecutor *m_executor = nullptr;
    SyncExecutionReport m_report;

    QLabel *m_statusLabel = nullptr;
    QLabel *m_itemLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QGroupBox *m_summaryBox = nullptr;
    QLabel *m_summaryTextLabel = nullptr;
    QTableWidget *m_errorTable = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};
