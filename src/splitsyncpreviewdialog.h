/*
 * Split View synchronization plan preview dialog.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "splitsyncmodel.h"
#include "splitsyncexecutiondialog.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

class SyncPlanPreviewDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SyncPlanPreviewDialog(
        const QUrl &leftUrl,
        const QUrl &rightUrl,
        const QList<SyncPlanEntry> &plan,
        SyncDirection direction,
        quint64 generation,
        QWidget *parent = nullptr)
        : QDialog(parent)
        , m_leftUrl(leftUrl)
        , m_rightUrl(rightUrl)
        , m_plan(plan)
        , m_direction(direction)
        , m_generation(generation)
    {
        setWindowTitle(trLocal("Podgląd planu synchronizacji", "Sync Plan Preview"));
        setMinimumSize(880, 580);
        resize(960, 620);

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(14, 14, 14, 14);
        mainLayout->setSpacing(10);

        // Header paths & direction
        auto *headerGroup = new QGroupBox(trLocal("Szczegóły synchronizacji", "Synchronization Details"), this);
        auto *headerLayout = new QVBoxLayout(headerGroup);
        headerLayout->setContentsMargins(10, 8, 10, 8);
        headerLayout->setSpacing(4);

        const QUrl sourceUrl = (direction == SyncDirection::LeftToRight) ? leftUrl : rightUrl;
        const QUrl destUrl = (direction == SyncDirection::LeftToRight) ? rightUrl : leftUrl;
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

        auto *directionLabel = new QLabel(this);
        directionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        directionLabel->setText(QStringLiteral("<b>%1:</b> %2  <i>(%3)</i>")
                                    .arg(trLocal("Kierunek", "Direction"),
                                         syncDirectionLabel(direction),
                                         trLocal("Snapshot porównania paneli",
                                                 "Pane comparison snapshot")));

        headerLayout->addWidget(sourcePathLabel);
        headerLayout->addWidget(destPathLabel);
        headerLayout->addWidget(directionLabel);
        mainLayout->addWidget(headerGroup);

        // Summary counts
        const SyncPlanSummary summary = summarizeSyncPlan(m_plan);
        auto *summaryLabel = new QLabel(this);
        summaryLabel->setText(
            isPolish()
                ? QStringLiteral("Wszystkich: %1 | Bez zmian: %2 | Do skopiowania: %3 | Do aktualizacji: %4 | Konflikty: %5 | Nieobsługiwane: %6")
                      .arg(summary.total)
                      .arg(summary.noAction)
                      .arg(summary.copyFile)
                      .arg(summary.updateFile)
                      .arg(summary.conflict)
                      .arg(summary.unsupported)
                : QStringLiteral("Total: %1 | No action: %2 | Copy: %3 | Update: %4 | Conflicts: %5 | Unsupported: %6")
                      .arg(summary.total)
                      .arg(summary.noAction)
                      .arg(summary.copyFile)
                      .arg(summary.updateFile)
                      .arg(summary.conflict)
                      .arg(summary.unsupported));
        mainLayout->addWidget(summaryLabel);

        // Plan Table Widget
        auto *table = new QTableWidget(this);
        table->setColumnCount(6);
        table->setHorizontalHeaderLabels({
            trLocal("Nazwa", "Name"),
            trLocal("Planowana akcja", "Planned action"),
            trLocal("Źródło", "Source"),
            trLocal("Cel", "Destination"),
            trLocal("Uzasadnienie", "Reason"),
            trLocal("Typ", "Type")
        });
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
        table->setColumnWidth(0, 200);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->setVisible(false);

        table->setRowCount(m_plan.size());
        QMimeDatabase mimeDb;

        for (int row = 0; row < m_plan.size(); ++row) {
            const SyncPlanEntry &entry = m_plan.at(row);

            // Column 0: Icon + Name
            QIcon icon;
            if (entry.isDirectory) {
                icon = themedIcon(QStringLiteral("folder"));
            } else {
                const FileInfo &info = entry.hasSource ? entry.sourceInfo : entry.destinationInfo;
                const QMimeType mime = resolvedMimeType(info, mimeDb);
                QString iconName = !info.iconName.isEmpty() ? info.iconName : mime.iconName();
                if (iconName.isEmpty()) iconName = mime.genericIconName();
                icon = themedIcon(!iconName.isEmpty() ? iconName : QStringLiteral("text-x-generic"));
            }
            auto *nameItem = new QTableWidgetItem(icon, entry.name);

            // Column 1: Planned Action
            auto *actionItem = new QTableWidgetItem(syncPlanActionLabel(entry.action));

            // Column 2: Source Metadata
            QString sourceText = entry.hasSource ? formatFileItemMetadata(entry.sourceInfo) : QStringLiteral("—");
            auto *sourceItem = new QTableWidgetItem(sourceText);

            // Column 3: Destination Metadata
            QString destText = entry.hasDestination ? formatFileItemMetadata(entry.destinationInfo) : QStringLiteral("—");
            auto *destItem = new QTableWidgetItem(destText);

            // Column 4: Reason
            QString reasonText = entry.reason;
            if (entry.action == SyncPlanAction::UpdateFile && !entry.differenceDetails.isEmpty()
                && entry.differenceDetails != trLocal("Metadane zgodne", "Metadata matches")) {
                reasonText += QStringLiteral(" [%1]").arg(entry.differenceDetails);
            }
            auto *reasonItem = new QTableWidgetItem(reasonText);

            // Column 5: Type
            QString typeText;
            if (entry.action == SyncPlanAction::Conflict) {
                typeText = trLocal("Niezgodny", "Mismatch");
            } else if (entry.isDirectory) {
                typeText = trLocal("Folder", "Folder");
            } else {
                const FileInfo &info = entry.hasSource ? entry.sourceInfo : entry.destinationInfo;
                typeText = fileTypeLabel(info, mimeDb);
            }
            auto *typeItem = new QTableWidgetItem(typeText);

            table->setItem(row, 0, nameItem);
            table->setItem(row, 1, actionItem);
            table->setItem(row, 2, sourceItem);
            table->setItem(row, 3, destItem);
            table->setItem(row, 4, reasonItem);
            table->setItem(row, 5, typeItem);
        }

        mainLayout->addWidget(table, 1);

        // Buttons
        auto *buttonBox = new QDialogButtonBox(this);
        m_syncButton = buttonBox->addButton(trLocal("Synchronizuj…", "Synchronize…"), QDialogButtonBox::ActionRole);
        m_syncButton->setIcon(themedIcon(QStringLiteral("view-refresh")));
        m_syncButton->setEnabled(summary.executable > 0);
        connect(m_syncButton, &QPushButton::clicked, this, &SyncPlanPreviewDialog::executeSync);

        auto *closeButton = buttonBox->addButton(QDialogButtonBox::Close);
        connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
        mainLayout->addWidget(buttonBox);
    }

    QList<SyncPlanEntry> plan() const
    {
        return m_plan;
    }

    SyncDirection direction() const
    {
        return m_direction;
    }

    quint64 generation() const
    {
        return m_generation;
    }

    bool wasExecuted() const
    {
        return m_executed;
    }

    QPushButton *syncButton() const
    {
        return m_syncButton;
    }

private:
    void executeSync()
    {
        const SyncPlanSummary summary = summarizeSyncPlan(m_plan);
        if (summary.executable == 0) return;

        const QUrl sourceUrl = (m_direction == SyncDirection::LeftToRight) ? m_leftUrl : m_rightUrl;
        const QUrl destUrl = (m_direction == SyncDirection::LeftToRight) ? m_rightUrl : m_leftUrl;

        SyncConfirmationDialog confirmDlg(sourceUrl, destUrl, m_direction, summary, this);
        if (confirmDlg.exec() != QDialog::Accepted) {
            return;
        }

        SyncExecutionDialog execDlg(sourceUrl, destUrl, m_plan, m_direction, this);
        execDlg.exec();

        m_executed = true;
        accept();
    }

    QUrl m_leftUrl;
    QUrl m_rightUrl;
    QList<SyncPlanEntry> m_plan;
    SyncDirection m_direction = SyncDirection::LeftToRight;
    quint64 m_generation = 0;
    bool m_executed = false;
    QPushButton *m_syncButton = nullptr;
};
