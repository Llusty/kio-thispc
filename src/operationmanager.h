/*
 * Compact file-operation tracking UI for thispc-view.
 *
 * Extracted during the 0.21.0 architecture refactor. OperationManager owns
 * KJob tracking/state, while OperationPopup owns the compact popup widgets.
 * Keep behavior changes separate from structural moves.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <KJob>

#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QObject>
#include <QPair>
#include <QMargins>
#include <QPointer>
#include <QProgressBar>
#include <QScrollArea>
#include <QScreen>
#include <QSizePolicy>
#include <QStatusBar>
#include <QString>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <utility>


class OperationManager;


class OperationPopup final : public QFrame
{
public:
    explicit OperationPopup(QMainWindow *window)
        : QFrame(window, Qt::Popup | Qt::FramelessWindowHint)
        , m_window(window)
    {
        setObjectName(QStringLiteral("operationPopup"));
        setFixedWidth(410);
        setMaximumHeight(720);
        hide();

        auto *popupLayout = new QVBoxLayout(this);
        popupLayout->setContentsMargins(0, 0, 0, 0);
        popupLayout->setSpacing(0);

        auto *header = new QFrame(this);
        header->setObjectName(QStringLiteral("operationPopupHeader"));
        auto *headerLayout = new QVBoxLayout(header);
        headerLayout->setContentsMargins(12, 10, 12, 8);
        headerLayout->setSpacing(5);

        auto *titleRow = new QHBoxLayout;
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(8);

        auto *title = new QLabel(
            trLocal("Operacje", "Operations"),
            header);
        title->setObjectName(QStringLiteral("operationPopupTitle"));
        titleRow->addWidget(title);
        titleRow->addStretch(1);

        m_summaryLabel = new QLabel(header);
        m_summaryLabel->setObjectName(
            QStringLiteral("operationSummaryLabel"));
        titleRow->addWidget(m_summaryLabel);
        headerLayout->addLayout(titleRow);

        m_overallProgress = new QProgressBar(header);
        m_overallProgress->setObjectName(
            QStringLiteral("operationOverallProgress"));
        m_overallProgress->setRange(0, 100);
        m_overallProgress->setValue(0);
        m_overallProgress->setTextVisible(false);
        m_overallProgress->hide();
        headerLayout->addWidget(m_overallProgress);
        popupLayout->addWidget(header);

        auto *body = new QWidget(this);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(8, 8, 8, 8);
        bodyLayout->setSpacing(0);

        m_emptyLabel = new QLabel(
            trLocal(
                "Brak operacji plikowych.",
                "No file operations."),
            body);
        m_emptyLabel->setObjectName(
            QStringLiteral("operationEmptyLabel"));
        m_emptyLabel->setAlignment(Qt::AlignCenter);
        m_emptyLabel->setMinimumHeight(90);
        bodyLayout->addWidget(m_emptyLabel);

        m_scrollArea = new QScrollArea(body);
        m_scrollArea->setWidgetResizable(true);
        m_scrollArea->setFrameShape(QFrame::NoFrame);
        m_scrollArea->setHorizontalScrollBarPolicy(
            Qt::ScrollBarAlwaysOff);
        m_scrollArea->setMinimumHeight(100);
        m_scrollArea->setMaximumHeight(560);
        m_scrollArea->setVerticalScrollBarPolicy(
            Qt::ScrollBarAsNeeded);

        m_listWidget = new QWidget(m_scrollArea);
        m_listLayout = new QVBoxLayout(m_listWidget);
        m_listLayout->setContentsMargins(0, 0, 0, 0);
        m_listLayout->setSpacing(6);
        m_listLayout->addStretch(1);
        m_scrollArea->setWidget(m_listWidget);
        bodyLayout->addWidget(m_scrollArea);
        popupLayout->addWidget(body);

        m_footer = new QFrame(this);
        m_footer->setObjectName(
            QStringLiteral("operationPopupFooter"));
        auto *footerLayout = new QHBoxLayout(m_footer);
        footerLayout->setContentsMargins(8, 6, 8, 6);
        footerLayout->setSpacing(6);

        m_cancelAllButton = new QToolButton(m_footer);
        m_cancelAllButton->setObjectName(
            QStringLiteral("operationFooterButton"));
        m_cancelAllButton->setIcon(
            themedIcon(QStringLiteral("process-stop")));
        m_cancelAllButton->setText(
            trLocal("Anuluj wszystkie", "Cancel all"));
        m_cancelAllButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_cancelAllButton->hide();
        footerLayout->addWidget(m_cancelAllButton);

        footerLayout->addStretch(1);

        m_clearFinishedButton = new QToolButton(m_footer);
        m_clearFinishedButton->setObjectName(
            QStringLiteral("operationFooterButton"));
        m_clearFinishedButton->setIcon(
            themedIcon(
                QStringLiteral("edit-clear-list"),
                QStringLiteral("edit-clear")));
        m_clearFinishedButton->setText(
            trLocal(
                "Wyczyść zakończone",
                "Clear completed"));
        m_clearFinishedButton->setToolButtonStyle(
            Qt::ToolButtonTextBesideIcon);
        m_clearFinishedButton->hide();
        footerLayout->addWidget(m_clearFinishedButton);

        popupLayout->addWidget(m_footer);
    }

private:
    friend class OperationManager;

    void positionNear(QToolButton *button)
    {
        if (!button || !m_window) {
            return;
        }

        const QPoint buttonTopLeft =
            button->mapToGlobal(QPoint(0, 0));

        // Keep only a small part of the popup over the browser window.
        // When there is free desktop space on the right, most of the
        // operation history floats outside the main window, Brave-style.
        constexpr int windowOverlap = 96;
        const int windowRight =
            m_window->mapToGlobal(
                QPoint(m_window->width(), 0)).x();

        QPoint popupPos(
            windowRight - windowOverlap,
            buttonTopLeft.y() + button->height() + 4);

        QScreen *screen = QGuiApplication::screenAt(buttonTopLeft);
        if (!screen) {
            screen = button->screen();
        }

        if (screen) {
            const QRect available = screen->availableGeometry();

            if (popupPos.x() < available.left() + 4) {
                popupPos.setX(available.left() + 4);
            }
            if (popupPos.x() + width() > available.right() - 4) {
                popupPos.setX(
                    available.right() - width() - 4);
            }

            if (popupPos.y() + height() > available.bottom() - 4) {
                const int aboveY =
                    buttonTopLeft.y() - height() - 4;
                popupPos.setY(
                    qMax(available.top() + 4, aboveY));
            }
        }

        move(popupPos);
    }

    void updateHeight(
        const QList<QFrame *> &rows,
        QToolButton *button)
    {
        constexpr int minimumListHeight = 100;
        constexpr int maximumListHeight = 560;
        constexpr int minimumRowHeight = 72;

        if (rows.isEmpty()) {
            m_listWidget->setMinimumHeight(0);
            m_listWidget->setMaximumHeight(QWIDGETSIZE_MAX);
            m_scrollArea->setFixedHeight(minimumListHeight);
            m_scrollArea->setVerticalScrollBarPolicy(
                Qt::ScrollBarAlwaysOff);
        } else {
            m_listLayout->activate();

            const QMargins margins =
                m_listLayout->contentsMargins();
            int contentHeight = margins.top() + margins.bottom();
            int visibleRows = 0;

            for (QFrame *row : rows) {
                if (!row || row->isHidden()) {
                    continue;
                }

                if (row->layout()) {
                    row->layout()->activate();
                }
                row->ensurePolished();

                const int rowHeight = qMax(
                    minimumRowHeight,
                    qMax(
                        row->sizeHint().height(),
                        row->minimumSizeHint().height()));

                if (visibleRows > 0) {
                    contentHeight += m_listLayout->spacing();
                }
                contentHeight += rowHeight;
                ++visibleRows;
            }

            contentHeight += 4;
            const int desiredHeight = qBound(
                minimumListHeight,
                contentHeight,
                maximumListHeight);

            // Keep the contents at their natural full height. The viewport
            // follows it until 560 px and then becomes scrollable.
            m_listWidget->setMinimumHeight(contentHeight);
            m_listWidget->setMaximumHeight(contentHeight);
            m_scrollArea->setFixedHeight(desiredHeight);
            m_scrollArea->setVerticalScrollBarPolicy(
                contentHeight > maximumListHeight
                    ? Qt::ScrollBarAsNeeded
                    : Qt::ScrollBarAlwaysOff);
        }

        if (layout()) {
            layout()->activate();
        }

        const int preferredHeight = qMin(720, sizeHint().height());
        resize(410, preferredHeight);
        updateGeometry();

        if (isVisible()) {
            positionNear(button);
        }
    }

    QMainWindow *m_window = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QProgressBar *m_overallProgress = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QFrame *m_footer = nullptr;
    QToolButton *m_cancelAllButton = nullptr;
    QToolButton *m_clearFinishedButton = nullptr;
    QWidget *m_listWidget = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
};


class OperationManager final : public QObject
{
public:
    OperationManager(
        QMainWindow *window,
        QToolBar *toolbar,
        QObject *parent = nullptr)
        : QObject(parent)
        , m_window(window)
    {
        build(toolbar);
    }

    ~OperationManager() override
    {
        for (FileOperationItem *item : std::as_const(m_items)) {
            delete item;
        }
    }

    int activeCount() const
    {
        int count = 0;
        for (const FileOperationItem *item : m_items) {
            if (item && !item->finished) {
                ++count;
            }
        }
        return count;
    }

    bool cancelRequested(KJob *job) const
    {
        const FileOperationItem *item =
            m_byJob.value(job, nullptr);
        return item && item->cancelRequested;
    }

    void track(KJob *job, const QString &operationTitle)
    {
        if (!job || !m_popup || !m_popup->m_listLayout) {
            return;
        }

        auto *item = new FileOperationItem;
        item->job = job;
        item->percent = job->percent();

        item->row = new QFrame(m_popup->m_listWidget);
        item->row->setObjectName(QStringLiteral("operationRow"));
        auto *rowLayout = new QHBoxLayout(item->row);
        rowLayout->setContentsMargins(9, 7, 7, 7);
        rowLayout->setSpacing(9);

        auto *icon = new QLabel(item->row);
        icon->setPixmap(
            themedIcon(
                QStringLiteral("folder-sync"),
                QStringLiteral("system-run"))
                .pixmap(24, 24));
        icon->setFixedSize(28, 28);
        icon->setAlignment(Qt::AlignCenter);
        rowLayout->addWidget(icon, 0, Qt::AlignTop);

        auto *textWidget = new QWidget(item->row);
        auto *textLayout = new QVBoxLayout(textWidget);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(3);

        auto *topLine = new QHBoxLayout;
        topLine->setContentsMargins(0, 0, 0, 0);
        topLine->setSpacing(7);

        item->titleLabel = new QLabel(
            operationTitle.isEmpty()
                ? trLocal("Operacja plikowa", "File operation")
                : operationTitle,
            textWidget);
        item->titleLabel->setObjectName(
            QStringLiteral("operationTitleLabel"));
        topLine->addWidget(item->titleLabel, 1);

        item->stateLabel = new QLabel(
            trLocal("W toku", "Running"),
            textWidget);
        item->stateLabel->setObjectName(
            QStringLiteral("operationDetailsLabel"));
        topLine->addWidget(item->stateLabel);
        textLayout->addLayout(topLine);

        item->detailsLabel = new QLabel(textWidget);
        item->detailsLabel->setObjectName(
            QStringLiteral("operationDetailsLabel"));
        item->detailsLabel->setTextInteractionFlags(
            Qt::TextSelectableByMouse);
        item->detailsLabel->setWordWrap(true);
        textLayout->addWidget(item->detailsLabel);

        item->progressBar = new QProgressBar(textWidget);
        item->progressBar->setObjectName(
            QStringLiteral("operationProgress"));
        item->progressBar->setRange(0, 100);
        item->progressBar->setValue(
            static_cast<int>(item->percent));
        item->progressBar->setTextVisible(false);
        textLayout->addWidget(item->progressBar);

        rowLayout->addWidget(textWidget, 1);

        item->cancelButton = new QToolButton(item->row);
        item->cancelButton->setIcon(
            themedIcon(QStringLiteral("process-stop")));
        item->cancelButton->setToolTip(
            trLocal("Anuluj operację", "Cancel operation"));
        const bool killable =
            job->capabilities().testFlag(KJob::Killable);
        item->cancelButton->setEnabled(killable);
        if (!killable) {
            item->cancelButton->setToolTip(
                trLocal(
                    "Ta operacja nie obsługuje anulowania",
                    "This operation cannot be cancelled"));
        }
        rowLayout->addWidget(item->cancelButton, 0, Qt::AlignTop);

        m_popup->m_listLayout->insertWidget(
            qMax(0, m_popup->m_listLayout->count() - 1),
            item->row);

        m_items.push_back(item);
        m_byJob.insert(job, item);

        connect(
            item->cancelButton,
            &QToolButton::clicked,
            this,
            [this, item] {
                if (!item || item->finished || !item->job) {
                    return;
                }

                item->cancelRequested = true;
                item->stateLabel->setText(
                    trLocal("Anulowanie…", "Cancelling…"));
                item->cancelButton->setEnabled(false);

                if (!item->job->kill(KJob::EmitResult)) {
                    item->cancelRequested = false;
                    item->stateLabel->setText(
                        trLocal("W toku", "Running"));
                    item->cancelButton->setEnabled(true);
                    if (m_window && m_window->statusBar()) {
                        m_window->statusBar()->showMessage(
                            trLocal(
                                "Nie udało się anulować tej operacji.",
                                "Could not cancel this operation."),
                            4000);
                    }
                }
            });

        connect(
            job,
            &KJob::percentChanged,
            this,
            [this, item](KJob *, unsigned long percent) {
                if (!item || item->finished) {
                    return;
                }
                item->percent = qMin<unsigned long>(percent, 100UL);
                item->progressBar->setValue(
                    static_cast<int>(item->percent));
                updateSummary();
            });

        connect(
            job,
            &KJob::processedAmountChanged,
            this,
            [this, item](
                KJob *,
                KJob::Unit unit,
                qulonglong amount) {
                if (!item || item->finished || unit != KJob::Bytes) {
                    return;
                }
                item->processedBytes = amount;
                updateDetails(item);
            });

        connect(
            job,
            &KJob::totalAmountChanged,
            this,
            [this, item](
                KJob *,
                KJob::Unit unit,
                qulonglong amount) {
                if (!item || item->finished || unit != KJob::Bytes) {
                    return;
                }
                item->totalBytes = amount;
                updateDetails(item);
            });

        connect(
            job,
            &KJob::speed,
            this,
            [this, item](KJob *, unsigned long speed) {
                if (!item || item->finished) {
                    return;
                }
                item->speedBytesPerSecond = speed;
                updateDetails(item);
            });

        connect(
            job,
            &KJob::description,
            this,
            [this, item](
                KJob *,
                const QString &,
                const QPair<QString, QString> &field1,
                const QPair<QString, QString> &field2) {
                if (!item || item->finished) {
                    return;
                }

                QString context;
                if (!field1.second.isEmpty()) {
                    context = field1.first.isEmpty()
                        ? field1.second
                        : QStringLiteral("%1: %2")
                            .arg(field1.first, field1.second);
                }
                if (!field2.second.isEmpty()) {
                    const QString second = field2.first.isEmpty()
                        ? field2.second
                        : QStringLiteral("%1: %2")
                            .arg(field2.first, field2.second);
                    context = context.isEmpty()
                        ? second
                        : context + QStringLiteral("  •  ") + second;
                }

                item->contextText = context;
                updateDetails(item);
            });

        updateDetails(item);
        updateSummary();
    }

    void finish(
        KJob *job,
        bool success,
        bool cancelled,
        const QString &errorText)
    {
        FileOperationItem *item = m_byJob.value(job, nullptr);
        if (!item) {
            return;
        }

        item->finished = true;
        item->failed = !success && !cancelled;
        item->speedBytesPerSecond = 0;
        item->job.clear();
        m_byJob.remove(job);

        if (item->cancelButton) {
            item->cancelButton->hide();
        }

        if (success) {
            item->percent = 100;
            if (item->progressBar) {
                item->progressBar->setValue(100);
            }
            item->stateLabel->setText(
                trLocal("Zakończono", "Completed"));
        } else if (cancelled) {
            item->stateLabel->setText(
                trLocal("Anulowano", "Cancelled"));
        } else {
            item->stateLabel->setText(
                trLocal("Błąd", "Failed"));
            if (!errorText.isEmpty()) {
                item->contextText = errorText;
            }
        }

        updateDetails(item);
        updateSummary();
    }

    void cancelAll()
    {
        const QList<FileOperationItem *> items = m_items;
        for (FileOperationItem *item : items) {
            if (!item || item->finished || !item->job) {
                continue;
            }
            if (!item->job->capabilities().testFlag(KJob::Killable)) {
                continue;
            }

            item->cancelRequested = true;
            item->stateLabel->setText(
                trLocal("Anulowanie…", "Cancelling…"));
            if (item->cancelButton) {
                item->cancelButton->setEnabled(false);
            }
            if (!item->job->kill(KJob::EmitResult)) {
                item->cancelRequested = false;
                item->stateLabel->setText(
                    trLocal("W toku", "Running"));
                if (item->cancelButton) {
                    item->cancelButton->setEnabled(true);
                }
            }
        }
    }

    void clearFinished()
    {
        for (int index = m_items.size() - 1; index >= 0; --index) {
            FileOperationItem *item = m_items.at(index);
            if (!item || !item->finished) {
                continue;
            }

            m_items.removeAt(index);
            if (item->row) {
                delete item->row;
            }
            delete item;
        }

        updateSummary();
    }

private:
    struct FileOperationItem
    {
        QPointer<KJob> job;
        QFrame *row = nullptr;
        QLabel *titleLabel = nullptr;
        QLabel *detailsLabel = nullptr;
        QLabel *stateLabel = nullptr;
        QProgressBar *progressBar = nullptr;
        QToolButton *cancelButton = nullptr;
        QString contextText;
        qulonglong processedBytes = 0;
        qulonglong totalBytes = 0;
        unsigned long speedBytesPerSecond = 0;
        unsigned long percent = 0;
        bool finished = false;
        bool cancelRequested = false;
        bool failed = false;
    };

    void build(QToolBar *toolbar)
    {
        if (!toolbar || !m_window) {
            return;
        }

        auto *spacer = new QWidget(toolbar);
        spacer->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Preferred);
        toolbar->addWidget(spacer);

        m_button = new QToolButton(toolbar);
        m_button->setObjectName(QStringLiteral("operationButton"));
        m_button->setIcon(
            themedIcon(
                QStringLiteral("view-task"),
                QStringLiteral("folder-sync")));
        m_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_button->setFixedSize(34, 32);
        m_button->setToolTip(
            trLocal("Operacje plikowe", "File operations"));
        toolbar->addWidget(m_button);

        m_badgeLabel = new QLabel(m_button);
        m_badgeLabel->setObjectName(QStringLiteral("operationBadge"));
        m_badgeLabel->setAlignment(Qt::AlignCenter);
        m_badgeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_badgeLabel->setFixedSize(18, 16);
        m_badgeLabel->move(16, -1);
        m_badgeLabel->hide();

        // A real Qt::Popup is intentionally retained. QMenu cached action
        // geometry and clipped operation rows in the earlier implementation.
        m_popup = new OperationPopup(m_window);

        connect(
            m_button,
            &QToolButton::clicked,
            this,
            [this] {
                if (!m_popup) {
                    return;
                }

                if (m_popup->isVisible()) {
                    m_popup->hide();
                    return;
                }

                updatePopupHeight();
                m_popup->positionNear(m_button);
                m_popup->show();
                m_popup->raise();
            });

        connect(
            m_popup->m_cancelAllButton,
            &QToolButton::clicked,
            this,
            &OperationManager::cancelAll);

        connect(
            m_popup->m_clearFinishedButton,
            &QToolButton::clicked,
            this,
            &OperationManager::clearFinished);

        updateSummary();
    }

    QList<QFrame *> rows() const
    {
        QList<QFrame *> result;
        result.reserve(m_items.size());
        for (const FileOperationItem *item : m_items) {
            if (item && item->row) {
                result.push_back(item->row);
            }
        }
        return result;
    }

    void updatePopupHeight()
    {
        if (m_popup) {
            m_popup->updateHeight(rows(), m_button);
        }
    }

    QString transferText(const FileOperationItem *item) const
    {
        if (!item) {
            return {};
        }

        QString transfer;

        if (item->totalBytes > 0) {
            transfer = QStringLiteral("%1 / %2")
                .arg(
                    formatFileSize(
                        static_cast<qint64>(item->processedBytes),
                        false),
                    formatFileSize(
                        static_cast<qint64>(item->totalBytes),
                        false));
        } else if (item->processedBytes > 0) {
            transfer = formatFileSize(
                static_cast<qint64>(item->processedBytes),
                false);
        }

        if (item->speedBytesPerSecond > 0) {
            const QString speed =
                formatFileSize(
                    static_cast<qint64>(
                        item->speedBytesPerSecond),
                    false)
                + QStringLiteral("/s");
            transfer = transfer.isEmpty()
                ? speed
                : transfer + QStringLiteral("  •  ") + speed;
        }

        return transfer;
    }

    void updateDetails(FileOperationItem *item)
    {
        if (!item || !item->detailsLabel) {
            return;
        }

        QString details = item->contextText;
        const QString transfer = transferText(item);

        if (!transfer.isEmpty()) {
            details = details.isEmpty()
                ? transfer
                : details + QStringLiteral("  •  ") + transfer;
        }

        if (details.isEmpty() && !item->finished) {
            details = trLocal(
                "Oczekiwanie na informacje o postępie…",
                "Waiting for progress information…");
        }

        item->detailsLabel->setText(details);
        updatePopupHeight();
    }

    void updateSummary()
    {
        if (!m_button || !m_popup) {
            return;
        }

        int active = 0;
        int completed = 0;
        int sumPercent = 0;

        for (const FileOperationItem *item : m_items) {
            if (!item) {
                continue;
            }

            if (item->finished) {
                ++completed;
            } else {
                ++active;
                sumPercent += static_cast<int>(item->percent);
            }
        }

        const bool empty = m_items.isEmpty();

        m_popup->m_emptyLabel->setVisible(empty);
        m_popup->m_scrollArea->setVisible(!empty);
        updatePopupHeight();

        if (empty) {
            m_popup->m_summaryLabel->setText(
                trLocal("Brak historii", "No history"));
        } else if (active > 0) {
            m_popup->m_summaryLabel->setText(
                isPolish()
                    ? QStringLiteral("%1 aktywne • %2 zakończone")
                        .arg(active)
                        .arg(completed)
                    : QStringLiteral("%1 active • %2 completed")
                        .arg(active)
                        .arg(completed));
        } else {
            m_popup->m_summaryLabel->setText(
                isPolish()
                    ? QStringLiteral("Zakończone: %1").arg(completed)
                    : QStringLiteral("Completed: %1").arg(completed));
        }

        m_popup->m_overallProgress->setVisible(active > 0);
        m_popup->m_overallProgress->setValue(
            active > 0 ? sumPercent / active : 0);

        m_popup->m_cancelAllButton->setVisible(active > 0);
        m_popup->m_clearFinishedButton->setVisible(completed > 0);
        m_popup->m_footer->setVisible(active > 0 || completed > 0);

        if (active > 0) {
            m_badgeLabel->setText(
                active > 9
                    ? QStringLiteral("9+")
                    : QString::number(active));
            m_badgeLabel->show();
            m_badgeLabel->raise();
        } else {
            m_badgeLabel->hide();
        }

        const bool wasActive =
            m_button->property("active").toBool();
        const bool isActive = active > 0;
        if (wasActive != isActive) {
            m_button->setProperty("active", isActive);
            m_button->style()->unpolish(m_button);
            m_button->style()->polish(m_button);
            m_button->update();
        }

        m_button->setIcon(
            themedIcon(
                isActive
                    ? QStringLiteral("folder-sync")
                    : QStringLiteral("view-task"),
                QStringLiteral("system-run")));

        if (empty) {
            m_button->setToolTip(
                trLocal(
                    "Operacje plikowe — brak historii",
                    "File operations — no history"));
        } else if (active > 0) {
            m_button->setToolTip(
                isPolish()
                    ? QStringLiteral(
                        "Operacje plikowe — %1 aktywne, %2 zakończone")
                        .arg(active)
                        .arg(completed)
                    : QStringLiteral(
                        "File operations — %1 active, %2 completed")
                        .arg(active)
                        .arg(completed));
        } else {
            m_button->setToolTip(
                isPolish()
                    ? QStringLiteral(
                        "Operacje plikowe — zakończone: %1")
                        .arg(completed)
                    : QStringLiteral(
                        "File operations — completed: %1")
                        .arg(completed));
        }

        updatePopupHeight();

        if (m_popup->isVisible()) {
            QTimer::singleShot(
                0,
                this,
                [this] {
                    if (m_popup && m_popup->isVisible()) {
                        updatePopupHeight();
                    }
                });
        }
    }

    QMainWindow *m_window = nullptr;
    QToolButton *m_button = nullptr;
    QLabel *m_badgeLabel = nullptr;
    OperationPopup *m_popup = nullptr;
    QList<FileOperationItem *> m_items;
    QHash<KJob *, FileOperationItem *> m_byJob;
};
