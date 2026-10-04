/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "duplicateactiondata.h"
#include "duplicategroupmodel.h"

class FileActions;

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

class DuplicateConfirmationDialog final : public QDialog
{
    Q_OBJECT

public:
    enum class ActionType {
        Trash,
        Move
    };

    DuplicateConfirmationDialog(QWidget *parent, ActionType action,
                                const DuplicateRevalidationReport &report,
                                const QString &moveDestination = QString())
        : QDialog(parent)
    {
        setWindowTitle(action == ActionType::Trash
            ? trLocal("Potwierdzenie przeniesienia do Kosza", "Confirm Move to Trash")
            : trLocal("Potwierdzenie przeniesienia", "Confirm Move"));
        resize(560, 420);
        setMinimumSize(480, 360);

        auto *layout = new QVBoxLayout(this);
        layout->setSpacing(10);

        auto *headerLabel = new QLabel(this);
        headerLabel->setObjectName(QStringLiteral("headerLabel"));
        QFont f = headerLabel->font();
        f.setBold(true);
        f.setPointSize(f.pointSize() + 1);
        headerLabel->setFont(f);
        headerLabel->setText(action == ActionType::Trash
            ? trLocal("Przenieść wybrane duplikaty do Kosza?", "Move selected duplicates to Trash?")
            : trLocal("Przenieść wybrane duplikaty do wskazanego folderu?", "Move selected duplicates to folder?"));
        layout->addWidget(headerLabel);

        if (action == ActionType::Move) {
            auto *destLabel = new QLabel(trLocal("Folder docelowy: ", "Destination folder: ") + moveDestination, this);
            destLabel->setObjectName(QStringLiteral("destLabel"));
            destLabel->setWordWrap(true);
            layout->addWidget(destLabel);
        }

        // Summary box
        auto *summaryBox = new QFrame(this);
        summaryBox->setObjectName(QStringLiteral("summaryBox"));
        summaryBox->setFrameShape(QFrame::StyledPanel);
        auto *summaryLayout = new QVBoxLayout(summaryBox);
        summaryLayout->setSpacing(4);

        summaryLayout->addWidget(new QLabel(
            trLocal("Liczba elementów gotowych do operacji: %1 (z %2 wybranych)",
                    "Items ready for operation: %1 (of %2 selected)")
                .arg(report.validItems.size())
                .arg(report.totalRequested()), summaryBox));

        summaryLayout->addWidget(new QLabel(
            trLocal("Objęte grupy duplikatów: %1", "Affected duplicate groups: %1")
                .arg(report.affectedGroupsCount), summaryBox));

        summaryLayout->addWidget(new QLabel(
            trLocal("Rozmiar logiczny danych: %1 (%2 B)", "Logical data size: %1 (%2 B)")
                .arg(StorageScanStats::formatBytes(report.validLogicalBytes))
                .arg(report.validLogicalBytes), summaryBox));

        summaryLayout->addWidget(new QLabel(
            trLocal("Orientacyjny rozmiar na dysku: %1 (%2 B)", "Estimated allocated size: %1 (%2 B)")
                .arg(StorageScanStats::formatBytes(report.validAllocatedBytes))
                .arg(report.validAllocatedBytes), summaryBox));

        layout->addWidget(summaryBox);

        // Warnings
        if (report.hasAllCopiesWarning) {
            auto *allCopiesWarn = new QLabel(this);
            allCopiesWarn->setObjectName(QStringLiteral("allCopiesWarn"));
            allCopiesWarn->setStyleSheet(QStringLiteral("background: #ffeaa7; color: #d63031; padding: 6px; border-radius: 4px; font-weight: bold;"));
            allCopiesWarn->setWordWrap(true);
            allCopiesWarn->setText(trLocal(
                "⚠️ UWAGA: W co najmniej jednej grupie wybrano WSZYSTKIE kopie fizyczne!\n"
                "Po wykonaniu operacji w tych grupach nie pozostanie żaden plik.",
                "⚠️ WARNING: ALL physical copies are selected in at least one group!\n"
                "After the operation, no copy will remain in those groups."));
            layout->addWidget(allCopiesWarn);
        }

        if (!report.allValid()) {
            auto *staleWarn = new QLabel(this);
            staleWarn->setObjectName(QStringLiteral("staleWarn"));
            staleWarn->setStyleSheet(QStringLiteral("background: #fab1a0; color: #2d3436; padding: 6px; border-radius: 4px; font-weight: bold;"));
            staleWarn->setWordWrap(true);
            staleWarn->setText(trLocal(
                "⚠️ Część plików uległa zmianie lub zniknęła po skanowaniu i została wyłączona z operacji:\n"
                "• Zmienione/nieaktualne: %1\n"
                "• Brakujące: %2\n"
                "Operacja zostanie wykonana WYŁĄCZNIE dla %3 poprawnych plików.",
                "⚠️ Some files changed or disappeared after scan and were excluded:\n"
                "• Changed/stale: %1\n"
                "• Missing: %2\n"
                "The operation will be executed ONLY for %3 valid files.")
                .arg(report.staleItems.size())
                .arg(report.disappearedItems.size())
                .arg(report.validItems.size()));
            layout->addWidget(staleWarn);
        }

        // Details list (scrollable)
        layout->addWidget(new QLabel(trLocal("Pliki objęte operacją:", "Files included in operation:"), this));
        auto *listWidget = new QListWidget(this);
        listWidget->setObjectName(QStringLiteral("itemsListWidget"));
        for (const auto &validItem : report.validItems) {
            auto *item = new QListWidgetItem(validItem.item.targetPath, listWidget);
            item->setToolTip(validItem.item.targetPath);
        }
        for (const auto &staleItem : report.staleItems) {
            auto *item = new QListWidgetItem(QStringLiteral("[POMINIĘTO — ZMIENIONY] ") + staleItem.item.targetPath, listWidget);
            item->setForeground(Qt::darkRed);
            item->setToolTip(staleItem.reason);
        }
        for (const auto &disappearedItem : report.disappearedItems) {
            auto *item = new QListWidgetItem(QStringLiteral("[POMINIĘTO — BRAK PLIKU] ") + disappearedItem.item.targetPath, listWidget);
            item->setForeground(Qt::gray);
            item->setToolTip(disappearedItem.reason);
        }
        layout->addWidget(listWidget, 1);

        // Buttons
        auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttonBox->setObjectName(QStringLiteral("buttonBox"));
        buttonBox->button(QDialogButtonBox::Ok)->setText(action == ActionType::Trash
            ? trLocal("Przenieś do Kosza", "Move to Trash")
            : trLocal("Przenieś", "Move"));
        buttonBox->button(QDialogButtonBox::Cancel)->setText(trLocal("Anuluj", "Cancel"));
        connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttonBox);
    }

    QLabel *allCopiesWarningLabel() const { return findChild<QLabel *>(QStringLiteral("allCopiesWarn")); }
    QLabel *staleWarningLabel() const { return findChild<QLabel *>(QStringLiteral("staleWarn")); }
    QListWidget *itemsListWidget() const { return findChild<QListWidget *>(QStringLiteral("itemsListWidget")); }
};

class DuplicateActionController : public QObject
{
    Q_OBJECT

public:
    struct OperationResult {
        QSet<QString> successfulPaths;
        QHash<QString, QString> movedFinalPaths; // source -> final destination
        QList<QString> failedPaths;
        bool cancelled = false;
        QString errorMessage;
    };

    using TrashExecutor = std::function<void(const QList<QUrl> &, std::function<void(const OperationResult &)>)>;
    using MoveExecutor = std::function<void(const QList<QUrl> &, const QUrl &, std::function<void(const OperationResult &)>)>;

    using ConfirmationHandler = std::function<bool(DuplicateConfirmationDialog::ActionType action, const DuplicateRevalidationReport &report, const QString &dest)>;
    using DestinationChooser = std::function<QString(const QString &defaultDir)>;

    explicit DuplicateActionController(QWidget *parentWidget, FileActions *fileActions = nullptr)
        : QObject(parentWidget)
        , m_parentWidget(parentWidget)
        , m_fileActions(fileActions)
    {
    }

    void setFileActions(FileActions *actions)
    {
        m_fileActions = actions;
    }

    FileActions *fileActions() const
    {
        return m_fileActions;
    }

    void setTrashExecutor(TrashExecutor executor)
    {
        m_trashExecutor = std::move(executor);
    }

    void setMoveExecutor(MoveExecutor executor)
    {
        m_moveExecutor = std::move(executor);
    }

    void setConfirmationHandler(ConfirmationHandler handler)
    {
        m_confirmationHandler = std::move(handler);
    }

    void setDestinationChooser(DestinationChooser chooser)
    {
        m_destinationChooser = std::move(chooser);
    }

    void showMessage(QWidget *parent, const QString &title, const QString &text, bool isWarning = false)
    {
        if (qEnvironmentVariableIsSet("THISPC_TEST_HARNESS") || QGuiApplication::platformName() == QStringLiteral("offscreen")) {
            return;
        }
        if (isWarning) {
            QMessageBox::warning(parent, title, text);
        } else {
            QMessageBox::information(parent, title, text);
        }
    }

    void executeTrash(DuplicateGroupModel *model, const QString &scanRoot = QString(),
                      std::function<void(bool success, const QString &msg)> onFinished = {})
    {
        Q_UNUSED(scanRoot);
        if (!model) return;
        const auto selected = model->selectedActionItems();
        if (selected.isEmpty()) {
            showMessage(m_parentWidget, trLocal("Brak zaznaczenia", "No selection"),
                trLocal("Wybierz co najmniej jeden plik do przeniesienia do Kosza.",
                        "Select at least one file to move to Trash."));
            return;
        }

        // 1. Strict preflight revalidation
        const auto report = DuplicateReviewValidator::revalidate(selected, model->groups());
        if (report.validItems.isEmpty()) {
            showMessage(m_parentWidget, trLocal("Nie można wykonać operacji", "Cannot execute operation"),
                trLocal("Żaden z wybranych plików nie przeszedł pomyślnie walidacji (pliki zniknęły lub zostały zmodyfikowane).",
                        "None of the selected files passed revalidation (files disappeared or changed)."),
                true);
            return;
        }

        // 2. Explicit confirmation dialog
        bool confirmed = false;
        if (m_confirmationHandler) {
            confirmed = m_confirmationHandler(DuplicateConfirmationDialog::ActionType::Trash, report, QString());
        } else {
            DuplicateConfirmationDialog dlg(m_parentWidget, DuplicateConfirmationDialog::ActionType::Trash, report);
            confirmed = (dlg.exec() == QDialog::Accepted);
        }

        if (!confirmed) {
            if (onFinished) onFinished(false, trLocal("Operacja anulowana przez użytkownika.", "Operation cancelled by user."));
            return;
        }

        // 3. Collect validated URLs
        QList<QUrl> validUrls;
        QSet<QString> validPaths;
        for (const auto &item : report.validItems) {
            validUrls.append(QUrl::fromLocalFile(item.item.targetPath));
            validPaths.insert(item.item.targetPath);
        }

        auto handleResult = [this, model, validPaths, onFinished](const OperationResult &res) {
            if (res.cancelled) {
                if (onFinished) onFinished(false, trLocal("Operacja została anulowana.", "Operation was cancelled."));
                return;
            }
            // Update model for successful paths
            model->applySuccessfulTrash(res.successfulPaths);

            if (!res.failedPaths.isEmpty()) {
                showMessage(m_parentWidget, trLocal("Częściowy błąd", "Partial error"),
                    trLocal("Część plików nie mogła zostać przeniesiona do Kosza (%1 z %2).",
                            "Some files could not be moved to Trash (%1 of %2).")
                        .arg(res.failedPaths.size())
                        .arg(validPaths.size()),
                    true);
            }

            if (onFinished) {
                onFinished(res.failedPaths.isEmpty(), res.errorMessage);
            }
        };

        // 4. Dispatch via executor
        if (m_trashExecutor) {
            m_trashExecutor(validUrls, handleResult);
        } else {
            if (onFinished) onFinished(false, trLocal("Brak mechanizmu wykonawczego operacji.", "No operation executor available."));
        }
    }

    void executeMove(DuplicateGroupModel *model, const QString &scanRoot,
                     std::function<void(bool success, const QString &msg)> onFinished = {})
    {
        if (!model) return;
        const auto selected = model->selectedActionItems();
        if (selected.isEmpty()) {
            showMessage(m_parentWidget, trLocal("Brak zaznaczenia", "No selection"),
                trLocal("Wybierz co najmniej jeden plik do przeniesienia.",
                        "Select at least one file to move."));
            return;
        }

        // 1. Destination directory selection
        QString chosenDir;
        if (m_destinationChooser) {
            chosenDir = m_destinationChooser(scanRoot.isEmpty() ? QDir::homePath() : scanRoot);
        } else {
            chosenDir = QFileDialog::getExistingDirectory(
                m_parentWidget, trLocal("Wybierz folder docelowy", "Select destination directory"),
                scanRoot.isEmpty() ? QDir::homePath() : scanRoot);
        }
        if (chosenDir.isEmpty()) {
            if (onFinished) onFinished(false, trLocal("Nie wybrano folderu docelowego.", "No destination directory selected."));
            return;
        }

        const QString cleanDest = QDir::cleanPath(chosenDir);
        const QUrl destUrl = QUrl::fromLocalFile(cleanDest);

        // 2. Strict preflight revalidation
        const auto report = DuplicateReviewValidator::revalidate(selected, model->groups());
        if (report.validItems.isEmpty()) {
            showMessage(m_parentWidget, trLocal("Nie można wykonać operacji", "Cannot execute operation"),
                trLocal("Żaden z wybranych plików nie przeszedł pomyślnie walidacji (pliki zniknęły lub zostały zmodyfikowane).",
                        "None of the selected files passed revalidation (files disappeared or changed)."),
                true);
            return;
        }

        // Check for self-destination
        QList<DuplicateRevalidatedItem> executableItems;
        QList<QUrl> moveUrls;
        for (const auto &item : report.validItems) {
            const QString sourceParent = QFileInfo(item.item.targetPath).absolutePath();
            if (QDir::cleanPath(sourceParent) == cleanDest) {
                // Same directory destination -> moving to self is rejected/no-op
                continue;
            }
            executableItems.append(item);
            moveUrls.append(QUrl::fromLocalFile(item.item.targetPath));
        }

        if (executableItems.isEmpty()) {
            showMessage(m_parentWidget, trLocal("Folder docelowy", "Destination folder"),
                trLocal("Wszystkie wybrane pliki znajdują się już w wybranym folderze. Nic nie przeniesiono.",
                        "All selected files are already in the chosen destination. Nothing moved."));
            return;
        }

        DuplicateRevalidationReport execReport = report;
        execReport.validItems = executableItems;

        // 3. Explicit confirmation dialog
        bool confirmed = false;
        if (m_confirmationHandler) {
            confirmed = m_confirmationHandler(DuplicateConfirmationDialog::ActionType::Move,
                                              execReport, cleanDest);
        } else {
            DuplicateConfirmationDialog dlg(m_parentWidget, DuplicateConfirmationDialog::ActionType::Move,
                                            execReport, cleanDest);
            confirmed = (dlg.exec() == QDialog::Accepted);
        }
        if (!confirmed) {
            if (onFinished) onFinished(false, trLocal("Operacja anulowana przez użytkownika.", "Operation cancelled by user."));
            return;
        }

        auto handleResult = [this, model, scanRoot, cleanDest, executableItems, onFinished](const OperationResult &res) {
            if (res.cancelled) {
                if (onFinished) onFinished(false, trLocal("Operacja została anulowana.", "Operation was cancelled."));
                return;
            }

            QHash<QString, QString> movedMap = res.movedFinalPaths;
            if (movedMap.isEmpty() && !res.successfulPaths.isEmpty()) {
                for (const auto &src : res.successfulPaths) {
                    const QString destPath = cleanDest + QLatin1Char('/') + QFileInfo(src).fileName();
                    movedMap.insert(src, destPath);
                }
            }

            model->applySuccessfulMove(movedMap, scanRoot);

            if (!res.failedPaths.isEmpty()) {
                showMessage(m_parentWidget, trLocal("Częściowy błąd", "Partial error"),
                    trLocal("Część plików nie mogła zostać przeniesiona (%1 z %2).",
                            "Some files could not be moved (%1 of %2).")
                        .arg(res.failedPaths.size())
                        .arg(executableItems.size()),
                    true);
            }

            if (onFinished) {
                onFinished(res.failedPaths.isEmpty(), res.errorMessage);
            }
        };

        // 4. Dispatch via executor
        if (m_moveExecutor) {
            m_moveExecutor(moveUrls, destUrl, handleResult);
        } else {
            if (onFinished) onFinished(false, trLocal("Brak mechanizmu wykonawczego operacji.", "No operation executor available."));
        }
    }

private:
    QWidget *m_parentWidget = nullptr;
    FileActions *m_fileActions = nullptr;
    TrashExecutor m_trashExecutor;
    MoveExecutor m_moveExecutor;
    ConfirmationHandler m_confirmationHandler;
    DestinationChooser m_destinationChooser;
};
