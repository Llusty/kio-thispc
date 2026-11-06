/*
 * Compact file-operation tracking UI for thispc-view.
 *
 * Extracted during the 0.21.0 architecture refactor. OperationManager owns
 * KJob tracking/state, while OperationPopup owns the compact popup widgets.
 * Keep behavior changes separate from structural moves.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "transferqueuecontroller.h"
#include "transferqueueui.h"
#include "transfernotificationmanager.h"

#include <KJob>
#include <KIO/CopyJob>

#include <QFrame>
#include <QFontMetrics>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QObject>
#include <QPair>
#include <QPainter>
#include <QPaintEvent>
#include <QMargins>
#include <QPointer>
#include <QPolygonF>
#include <QProgressBar>
#include <QScrollArea>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSizePolicy>
#include <QStatusBar>
#include <QString>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <utility>
#include <limits>


class OperationManager;
class OperationWindow;

// Label for operation details and context messages that wraps words but
// never forces its parent layout or card to widen (minimum width 24px).
class OperationDetailsLabel final : public QLabel
{
    Q_OBJECT

public:
    explicit OperationDetailsLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setObjectName(QStringLiteral("operationDetailsLabel"));
        setWordWrap(true);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    explicit OperationDetailsLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent)
    {
        setObjectName(QStringLiteral("operationDetailsLabel"));
        setWordWrap(true);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setToolTip(text);
    }

    void setText(const QString &text)
    {
        QLabel::setText(text);
        setToolTip(text);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(24, fontMetrics().height());
    }
};


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

        m_showDetailsButton = new QToolButton(header);
        m_showDetailsButton->setObjectName(
            QStringLiteral("operationDetailsButton"));
        m_showDetailsButton->setIcon(
            themedIcon(
                QStringLiteral("window-new"),
                QStringLiteral("view-list-details")));
        m_showDetailsButton->setToolTip(
            trLocal(
                "Otwórz szczegółowe okno operacji",
                "Open detailed operations window"));
        titleRow->addWidget(m_showDetailsButton);
        headerLayout->addLayout(titleRow);

        // Compact, single-line demo queue summary. The full queue list lives
        // only in the detailed OperationWindow (Queue tab) so the popup keeps
        // its 0.43 height behaviour.
        m_queueSummaryLabel = new QLabel(header);
        m_queueSummaryLabel->setObjectName(
            QStringLiteral("operationPopupQueueSummary"));
        m_queueSummaryLabel->setWordWrap(false);
        m_queueSummaryLabel->hide();
        headerLayout->addWidget(m_queueSummaryLabel);

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
    QLabel *m_queueSummaryLabel = nullptr;
    QProgressBar *m_overallProgress = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QFrame *m_footer = nullptr;
    QToolButton *m_cancelAllButton = nullptr;
    QToolButton *m_clearFinishedButton = nullptr;
    QToolButton *m_showDetailsButton = nullptr;
    QWidget *m_listWidget = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
};


class OperationManager final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Running,
        Paused,
        Cancelling,
        Completed,
        Cancelled,
        Failed,
    };

    struct OperationSnapshot
    {
        quint64 id = 0;
        QString title;
        QString contextText;
        QList<QUrl> sourceUrls;
        QUrl destinationUrl;
        QUrl currentSourceUrl;
        QUrl currentDestinationUrl;
        qulonglong processedBytes = 0;
        qulonglong totalBytes = 0;
        unsigned long speedBytesPerSecond = 0;
        qulonglong averageSpeedBytesPerSecond = 0;
        qint64 etaSeconds = -1;
        QList<qulonglong> speedSamples;
        unsigned long percent = 0;
        State state = State::Running;
        bool canCancel = false;
        bool canPause = false;
        bool paused = false;
    };

    OperationManager(
        QMainWindow *window,
        QToolBar *toolbar,
        QObject *parent = nullptr)
        : QObject(parent)
        , m_window(window)
        , m_autoShowTimer(new QTimer(this))
    {
        m_autoShowTimer->setSingleShot(true);
        connect(m_autoShowTimer, &QTimer::timeout, this, [this] {
            const int active = activeCount()
                + (m_queueController ? (m_queueController->activeCount() + m_queueController->queuedCount()) : 0);
            if (active > 0 && !m_userDismissedThisBatch) {
                showDetailedWindow();
            }
        });
        m_notificationManager = new TransferNotificationManager(m_window, nullptr, this);
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
            if (item && !item->finished()) {
                ++count;
            }
        }
        return count;
    }

    TransferQueueController *queueController() const { return m_queueController; }
    void setQueueController(TransferQueueController *controller);

    QList<OperationSnapshot> operations() const
    {
        QList<OperationSnapshot> result;
        result.reserve(m_items.size());
        for (const FileOperationItem *item : m_items) {
            if (!item) {
                continue;
            }
            OperationSnapshot snapshot;
            snapshot.id = item->id;
            snapshot.title = item->title;
            snapshot.contextText = item->contextText;
            snapshot.sourceUrls = item->sourceUrls;
            snapshot.destinationUrl = item->destinationUrl;
            snapshot.currentSourceUrl = item->currentSourceUrl;
            snapshot.currentDestinationUrl = item->currentDestinationUrl;
            snapshot.processedBytes = item->processedBytes;
            snapshot.totalBytes = item->totalBytes;
            snapshot.speedBytesPerSecond = item->speedBytesPerSecond;
            snapshot.averageSpeedBytesPerSecond =
                item->averageSpeedBytesPerSecond;
            snapshot.etaSeconds = item->etaSeconds;
            snapshot.speedSamples = item->speedSamples;
            snapshot.percent = item->percent;
            snapshot.state = item->state;
            snapshot.canCancel = item->job
                && item->job->capabilities().testFlag(KJob::Killable)
                && (item->state == State::Running || item->state == State::Paused);
            snapshot.canPause = item->job
                && item->job->capabilities().testFlag(KJob::Suspendable)
                && (item->state == State::Running || item->state == State::Paused);
            snapshot.paused = item->state == State::Paused;
            result.push_back(snapshot);
        }
        return result;
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
        item->id = m_nextId++;
        item->job = job;
        item->title = operationTitle.isEmpty()
            ? trLocal("Operacja plikowa", "File operation")
            : operationTitle;
        item->percent = job->percent();
        item->elapsedTimer.start();

        if (auto *copyJob = qobject_cast<KIO::CopyJob *>(job)) {
            item->sourceUrls = copyJob->srcUrls();
            item->destinationUrl = copyJob->destUrl();
            connect(
                copyJob,
                &KIO::CopyJob::copying,
                this,
                [this, item](KIO::Job *, const QUrl &source, const QUrl &destination) {
                    updateCurrentFile(item, source, destination);
                });
            connect(
                copyJob,
                &KIO::CopyJob::moving,
                this,
                [this, item](KIO::Job *, const QUrl &source, const QUrl &destination) {
                    updateCurrentFile(item, source, destination);
                });
        } else {
            item->sourceUrls = job->property("thispcSourceUrls")
                .value<QList<QUrl>>();
            item->destinationUrl = job->property("thispcDestinationUrl")
                .value<QUrl>();
            if (!item->sourceUrls.isEmpty()) {
                item->currentSourceUrl = item->sourceUrls.first();
                item->currentDestinationUrl = item->destinationUrl;
            }
        }

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

        auto *titleElided = new TransferQueueElidedLabel(
            item->title,
            textWidget,
            Qt::ElideRight);
        titleElided->setObjectName(
            QStringLiteral("operationTitleLabel"));
        item->titleLabel = titleElided;
        topLine->addWidget(item->titleLabel, 1);

        item->stateLabel = new QLabel(
            trLocal("W toku", "Running"),
            textWidget);
        item->stateLabel->setObjectName(
            QStringLiteral("operationDetailsLabel"));
        topLine->addWidget(item->stateLabel);
        textLayout->addLayout(topLine);

        item->detailsLabel = new OperationDetailsLabel(textWidget);
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
                if (item) cancelOperation(item->id);
            });

        connect(
            job,
            &KJob::suspended,
            this,
            [this, item](KJob *) {
                if (!item || item->finished()) return;
                item->state = State::Paused;
                item->stateLabel->setText(trLocal("Wstrzymano", "Paused"));
                Q_EMIT operationsChanged();
            });
        connect(
            job,
            &KJob::resumed,
            this,
            [this, item](KJob *) {
                if (!item || item->finished()) return;
                item->state = State::Running;
                item->stateLabel->setText(trLocal("W toku", "Running"));
                Q_EMIT operationsChanged();
            });

        connect(
            job,
            &KJob::percentChanged,
            this,
            [this, item](KJob *, unsigned long percent) {
                if (!item || item->finished()) {
                    return;
                }
                item->percent = qMin<unsigned long>(percent, 100UL);
                item->progressBar->setValue(
                    static_cast<int>(item->percent));
                updateSummary();
                Q_EMIT operationsChanged();
            });

        connect(
            job,
            &KJob::processedAmountChanged,
            this,
            [this, item](
                KJob *,
                KJob::Unit unit,
                qulonglong amount) {
                if (!item || item->finished() || unit != KJob::Bytes) {
                    return;
                }
                item->processedBytes = amount;
                updateTiming(item);
                updateDetails(item);
                Q_EMIT operationsChanged();
            });

        connect(
            job,
            &KJob::totalAmountChanged,
            this,
            [this, item](
                KJob *,
                KJob::Unit unit,
                qulonglong amount) {
                if (!item || item->finished() || unit != KJob::Bytes) {
                    return;
                }
                item->totalBytes = amount;
                updateTiming(item);
                updateDetails(item);
                Q_EMIT operationsChanged();
            });

        connect(
            job,
            &KJob::speed,
            this,
            [this, item](KJob *, unsigned long speed) {
                if (!item || item->finished()) {
                    return;
                }
                item->speedBytesPerSecond = speed;
                item->speedSamples.push_back(speed);
                constexpr int maximumSpeedSamples = 120;
                if (item->speedSamples.size() > maximumSpeedSamples) {
                    item->speedSamples.removeFirst();
                }
                updateDetails(item);
                Q_EMIT operationsChanged();
            });

        connect(
            job,
            &KJob::description,
            this,
            [this, item](
                KJob *job,
                const QString &,
                const QPair<QString, QString> &field1,
                const QPair<QString, QString> &field2) {
                if (!item || item->finished()) {
                    return;
                }

                const QUrl currentSource = job->property("thispcCurrentSourceUrl").value<QUrl>();
                if (currentSource.isValid()) {
                    item->currentSourceUrl = currentSource;
                    item->currentDestinationUrl = job->property("thispcCurrentDestinationUrl").value<QUrl>();
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
                Q_EMIT operationsChanged();
            });

        resetUserDismissal();
        updateDetails(item);
        updateSummary();
        Q_EMIT operationsChanged();
        onOperationTracked();
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

        disconnect(job, nullptr, this, nullptr);

        item->state = success
            ? State::Completed
            : cancelled ? State::Cancelled : State::Failed;
        item->speedBytesPerSecond = 0;
        if (!item->speedSamples.isEmpty()
            && item->speedSamples.constLast() != 0) {
            item->speedSamples.push_back(0);
            if (item->speedSamples.size() > 120) {
                item->speedSamples.removeFirst();
            }
        }
        item->etaSeconds = success ? 0 : -1;
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
        Q_EMIT operationsChanged();
        checkAutoCloseOrStopTimer();
    }

    bool cancelOperation(quint64 id)
    {
        for (FileOperationItem *item : std::as_const(m_items)) {
            if (!item || item->id != id) {
                continue;
            }
            return cancelItem(item, true);
        }
        return false;
    }

    bool togglePause(quint64 id)
    {
        for (FileOperationItem *item : std::as_const(m_items)) {
            if (!item || item->id != id || !item->job
                || !item->job->capabilities().testFlag(KJob::Suspendable)) {
                continue;
            }
            if (item->state == State::Paused) return item->job->resume();
            if (item->state == State::Running) return item->job->suspend();
            return false;
        }
        return false;
    }

    bool hasPausedOperations() const
    {
        for (const FileOperationItem *item : m_items) {
            if (item && item->state == State::Paused) return true;
        }
        return false;
    }

    void pauseAll()
    {
        for (FileOperationItem *item : m_items) {
            if (item && item->state == State::Running && item->job
                && item->job->capabilities().testFlag(KJob::Suspendable)) {
                item->job->suspend();
            }
        }
    }

    void resumeAll()
    {
        for (FileOperationItem *item : m_items) {
            if (item && item->state == State::Paused && item->job
                && item->job->capabilities().testFlag(KJob::Suspendable)) {
                item->job->resume();
            }
        }
    }

    void cancelAll()
    {
        const QList<FileOperationItem *> items = m_items;
        for (FileOperationItem *item : items) {
            cancelItem(item, false);
        }
        if (m_queueController) {
            m_queueController->cancelAll();
        }
    }

    int clearFinished()
    {
        int count = 0;
        for (int index = m_items.size() - 1; index >= 0; --index) {
            FileOperationItem *item = m_items.at(index);
            if (!item || !item->finished()) {
                continue;
            }

            m_items.removeAt(index);
            if (item->job) {
                m_byJob.remove(item->job);
            }
            if (item->row) {
                delete item->row;
            }
            delete item;
            ++count;
        }

        if (count > 0) {
            updateSummary();
            Q_EMIT operationsChanged();
        }
        return count;
    }

    bool removeFinishedOperation(quint64 id)
    {
        for (int index = 0; index < m_items.size(); ++index) {
            FileOperationItem *item = m_items.at(index);
            if (item && item->id == id && item->finished()) {
                m_items.removeAt(index);
                if (item->job) {
                    m_byJob.remove(item->job);
                }
                if (item->row) {
                    delete item->row;
                }
                delete item;
                updateSummary();
                Q_EMIT operationsChanged();
                return true;
            }
        }
        return false;
    }

    bool isCloseOnComplete() const { return m_closeOnComplete; }
    void setCloseOnComplete(bool close);
    void setUserDismissed(bool dismissed) { m_userDismissedThisBatch = dismissed; }
    bool userDismissed() const { return m_userDismissedThisBatch; }
    void resetUserDismissal() { m_userDismissedThisBatch = false; }
    int autoShowGracePeriodMs() const { return m_autoShowGracePeriodMs; }
    void setAutoShowGracePeriodMs(int ms) { m_autoShowGracePeriodMs = ms; }
    QTimer *autoShowTimer() const { return m_autoShowTimer; }
    OperationWindow *detailedWindow() const { return m_detailedWindow; }
    void setDetailedWindow(OperationWindow *window);
    TransferNotificationManager *notificationManager() const { return m_notificationManager; }
    TransferHistoryManager *historyManager() const
    {
        if (m_historyManager) {
            return m_historyManager;
        }
        return m_queueController ? m_queueController->historyManager() : nullptr;
    }
    void setHistoryManager(TransferHistoryManager *manager);
    OperationPopup *popup() const { return m_popup; }
    QToolButton *button() const { return m_button; }
    QLabel *badgeLabel() const { return m_badgeLabel; }
    void onOperationTracked();
    void checkAutoCloseOrStopTimer();

Q_SIGNALS:
    void operationsChanged();
    void closeOnCompleteChanged(bool close);

private:
    struct FileOperationItem
    {
        quint64 id = 0;
        QPointer<KJob> job;
        QFrame *row = nullptr;
        QLabel *titleLabel = nullptr;
        QLabel *detailsLabel = nullptr;
        QLabel *stateLabel = nullptr;
        QProgressBar *progressBar = nullptr;
        QToolButton *cancelButton = nullptr;
        QString title;
        QString contextText;
        QList<QUrl> sourceUrls;
        QUrl destinationUrl;
        QUrl currentSourceUrl;
        QUrl currentDestinationUrl;
        qulonglong processedBytes = 0;
        qulonglong totalBytes = 0;
        unsigned long speedBytesPerSecond = 0;
        qulonglong averageSpeedBytesPerSecond = 0;
        qint64 etaSeconds = -1;
        QList<qulonglong> speedSamples;
        unsigned long percent = 0;
        State state = State::Running;
        bool cancelRequested = false;
        QElapsedTimer elapsedTimer;

        bool finished() const
        {
            return state == State::Completed
                || state == State::Cancelled
                || state == State::Failed;
        }
    };

    bool cancelItem(FileOperationItem *item, bool showFailureMessage)
    {
        if (!item || item->finished() || !item->job
            || !item->job->capabilities().testFlag(KJob::Killable)) {
            return false;
        }

        item->cancelRequested = true;
        item->state = State::Cancelling;
        item->stateLabel->setText(
            trLocal("Anulowanie…", "Cancelling…"));
        if (item->cancelButton) {
            item->cancelButton->setEnabled(false);
        }
        Q_EMIT operationsChanged();

        if (item->job->kill(KJob::EmitResult)) {
            return true;
        }

        item->cancelRequested = false;
        item->state = State::Running;
        item->stateLabel->setText(trLocal("W toku", "Running"));
        if (item->cancelButton) {
            item->cancelButton->setEnabled(true);
        }
        Q_EMIT operationsChanged();
        if (showFailureMessage && m_window && m_window->statusBar()) {
            m_window->statusBar()->showMessage(
                trLocal(
                    "Nie udało się anulować tej operacji.",
                    "Could not cancel this operation."),
                4000);
        }
        return false;
    }

    void updateCurrentFile(
        FileOperationItem *item,
        const QUrl &source,
        const QUrl &destination)
    {
        if (!item || item->finished()) {
            return;
        }
        item->currentSourceUrl = source;
        item->currentDestinationUrl = destination;
        Q_EMIT operationsChanged();
    }

    void updateTiming(FileOperationItem *item)
    {
        if (!item) {
            return;
        }

        if (!item->elapsedTimer.isValid()) {
            return;
        }
        const qint64 elapsedMilliseconds = item->elapsedTimer.elapsed();
        if (elapsedMilliseconds > 0 && item->processedBytes > 0) {
            const long double average =
                (static_cast<long double>(item->processedBytes) * 1000.0L)
                / static_cast<long double>(elapsedMilliseconds);
            item->averageSpeedBytesPerSecond =
                average >= static_cast<long double>(
                    std::numeric_limits<qulonglong>::max())
                ? std::numeric_limits<qulonglong>::max()
                : static_cast<qulonglong>(average);
        }

        if (item->totalBytes <= item->processedBytes) {
            item->etaSeconds = item->totalBytes > 0 ? 0 : -1;
            return;
        }
        if (item->averageSpeedBytesPerSecond == 0) {
            item->etaSeconds = -1;
            return;
        }

        const qulonglong remaining =
            item->totalBytes - item->processedBytes;
        const qulonglong estimate =
            remaining / item->averageSpeedBytesPerSecond
            + (remaining % item->averageSpeedBytesPerSecond != 0 ? 1 : 0);
        item->etaSeconds = static_cast<qint64>(qMin<qulonglong>(
            estimate,
            static_cast<qulonglong>(std::numeric_limits<qint64>::max())));
    }

    void showDetailedWindow();

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

                updateSummary();
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

        connect(
            m_popup->m_showDetailsButton,
            &QToolButton::clicked,
            this,
            &OperationManager::showDetailedWindow);

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

        if (details.isEmpty() && !item->finished()) {
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

            if (item->finished()) {
                ++completed;
            } else {
                ++active;
                sumPercent += static_cast<int>(item->percent);
            }
        }

        const bool isUnified = isTransferQueueProductionDispatchConnected();
        const int queueActive = m_queueController ? m_queueController->activeCount() : 0;
        const int queueQueued = m_queueController ? m_queueController->queuedCount() : 0;
        const int totalActive = isUnified ? (active + queueActive) : active;

        int queueFinished = 0;
        int queuePercentSum = 0;
        QSet<QString> activeOrRecentHistoryUuids;
        if (m_queueController) {
            const TransferQueueSnapshot queueSnapshot = m_queueController->snapshot();
            const TransferQueueCounts qCounts = transferQueueCounts(queueSnapshot);
            queueFinished = qCounts.completed + qCounts.cancelled + qCounts.failed;
            for (const auto &task : queueSnapshot.tasks) {
                if (!task.historyUuid.isEmpty()) {
                    activeOrRecentHistoryUuids.insert(task.historyUuid);
                }
                if (task.state == TransferTaskState::Running) {
                    queuePercentSum += qBound(0, task.percent, 100);
                }
            }
        }

        int archivalHistoryCount = 0;
        int savedHistoryTotalCount = 0;
        TransferHistoryManager *histMgr = historyManager();
        if (histMgr) {
            histMgr->syncWithDiskIfNeeded();
            const QList<TransferHistoryRecord> allHistory = histMgr->records();
            savedHistoryTotalCount = allHistory.size();
            for (const auto &rec : allHistory) {
                if (!activeOrRecentHistoryUuids.contains(rec.uuid)) {
                    ++archivalHistoryCount;
                }
            }
        }

        const int sessionFinishedCount = completed + queueFinished;
        const int totalHistoryCount = sessionFinishedCount + archivalHistoryCount;

        const bool empty = m_items.isEmpty();

        m_popup->m_emptyLabel->setVisible(empty);
        m_popup->m_scrollArea->setVisible(!empty);
        updatePopupHeight();

        if (empty) {
            if (isUnified && (queueActive > 0 || queueQueued > 0)) {
                m_popup->m_emptyLabel->setText(
                    trLocal("Aktywne transfery w kolejce operacji.\nKliknij przycisk powyżej, aby otworzyć szczegóły.",
                            "Active transfers in operation queue.\nClick the button above to view details."));
                if (queueActive > 0 && queueQueued > 0) {
                    m_popup->m_summaryLabel->setText(
                        isPolish()
                            ? QStringLiteral("W toku: %1 • W kolejce: %2").arg(queueActive).arg(queueQueued)
                            : QStringLiteral("Running: %1 • Queued: %2").arg(queueActive).arg(queueQueued));
                } else if (queueActive > 0) {
                    m_popup->m_summaryLabel->setText(
                        isPolish()
                            ? (totalHistoryCount > 0
                                  ? QStringLiteral("W toku: %1 • Historia: %2").arg(queueActive).arg(totalHistoryCount)
                                  : QStringLiteral("W toku: %1").arg(queueActive))
                            : (totalHistoryCount > 0
                                  ? QStringLiteral("Running: %1 • History: %2").arg(queueActive).arg(totalHistoryCount)
                                  : QStringLiteral("Running: %1").arg(queueActive)));
                } else {
                    m_popup->m_summaryLabel->setText(
                        isPolish()
                            ? (totalHistoryCount > 0
                                  ? QStringLiteral("W kolejce: %1 • Historia: %2").arg(queueQueued).arg(totalHistoryCount)
                                  : QStringLiteral("W kolejce: %1").arg(queueQueued))
                            : (totalHistoryCount > 0
                                  ? QStringLiteral("Queued: %1 • History: %2").arg(queueQueued).arg(totalHistoryCount)
                                  : QStringLiteral("Queued: %1").arg(queueQueued)));
                }
            } else if (totalHistoryCount > 0) {
                if (savedHistoryTotalCount > 0) {
                    m_popup->m_emptyLabel->setText(
                        isPolish()
                            ? QStringLiteral("Brak aktywnych operacji plikowych.\nZapisana historia: %1 (kliknij przycisk powyżej).").arg(totalHistoryCount)
                            : QStringLiteral("No active file operations.\nSaved history: %1 (click the button above).").arg(totalHistoryCount));
                    m_popup->m_summaryLabel->setText(
                        isPolish()
                            ? QStringLiteral("Historia: %1").arg(totalHistoryCount)
                            : QStringLiteral("History: %1").arg(totalHistoryCount));
                } else {
                    m_popup->m_emptyLabel->setText(
                        isPolish()
                            ? QStringLiteral("Brak aktywnych operacji plikowych.\nZakończone w tej sesji: %1 (kliknij przycisk powyżej).").arg(sessionFinishedCount)
                            : QStringLiteral("No active file operations.\nFinished in this session: %1 (click the button above).").arg(sessionFinishedCount));
                    m_popup->m_summaryLabel->setText(
                        isPolish()
                            ? QStringLiteral("Zakończone: %1").arg(sessionFinishedCount)
                            : QStringLiteral("Completed: %1").arg(sessionFinishedCount));
                }
            } else {
                m_popup->m_emptyLabel->setText(
                    trLocal("Brak operacji plikowych i historii.", "No file operations or history."));
                m_popup->m_summaryLabel->setText(
                    trLocal("Brak historii", "No history"));
            }
        } else if (totalActive > 0) {
            m_popup->m_summaryLabel->setText(
                isPolish()
                    ? QStringLiteral("%1 w toku • %2 zakończone")
                        .arg(totalActive)
                        .arg(totalHistoryCount)
                    : QStringLiteral("%1 active • %2 completed")
                        .arg(totalActive)
                        .arg(totalHistoryCount));
        } else {
            m_popup->m_summaryLabel->setText(
                isPolish()
                    ? QStringLiteral("Zakończone: %1").arg(totalHistoryCount)
                    : QStringLiteral("Completed: %1").arg(totalHistoryCount));
        }

        m_popup->m_overallProgress->setVisible(totalActive > 0);
        const int combinedPercentSum = sumPercent + queuePercentSum;
        m_popup->m_overallProgress->setValue(
            totalActive > 0 ? (combinedPercentSum / totalActive) : 0);

        m_popup->m_cancelAllButton->setVisible(totalActive > 0);
        m_popup->m_clearFinishedButton->setVisible(completed > 0);
        m_popup->m_footer->setVisible(totalActive > 0 || completed > 0);

        if (totalActive > 0) {
            m_badgeLabel->setText(
                totalActive > 9
                    ? QStringLiteral("9+")
                    : QString::number(totalActive));
            m_badgeLabel->show();
            m_badgeLabel->raise();
        } else {
            m_badgeLabel->hide();
        }

        const bool wasActive =
            m_button->property("active").toBool();
        const bool isActive = totalActive > 0;
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

        if (empty && totalActive == 0 && queueQueued == 0) {
            if (totalHistoryCount > 0) {
                m_button->setToolTip(
                    isPolish()
                        ? QStringLiteral("Operacje plikowe — historia: %1").arg(totalHistoryCount)
                        : QStringLiteral("File operations — history: %1").arg(totalHistoryCount));
            } else {
                m_button->setToolTip(
                    trLocal(
                        "Operacje plikowe — brak historii",
                        "File operations — no history"));
            }
        } else if (totalActive > 0) {
            if (queueQueued > 0) {
                m_button->setToolTip(
                    isPolish()
                        ? QStringLiteral("Operacje plikowe — %1 w toku, %2 w kolejce")
                            .arg(totalActive).arg(queueQueued)
                        : QStringLiteral("File operations — %1 active, %2 queued")
                            .arg(totalActive).arg(queueQueued));
            } else {
                m_button->setToolTip(
                    isPolish()
                        ? QStringLiteral("Operacje plikowe — %1 w toku, %2 zakończone")
                            .arg(totalActive).arg(totalHistoryCount)
                        : QStringLiteral("File operations — %1 active, %2 completed")
                            .arg(totalActive).arg(totalHistoryCount));
            }
        } else if (queueQueued > 0) {
            m_button->setToolTip(
                isPolish()
                    ? QStringLiteral("Operacje plikowe — %1 w kolejce, historia: %2").arg(queueQueued).arg(totalHistoryCount)
                    : QStringLiteral("File operations — %1 queued, history: %2").arg(queueQueued).arg(totalHistoryCount));
        } else {
            m_button->setToolTip(
                isPolish()
                    ? QStringLiteral(
                        "Operacje plikowe — zakończone: %1")
                        .arg(totalHistoryCount)
                    : QStringLiteral(
                        "File operations — completed: %1")
                        .arg(totalHistoryCount));
        }

        // Stage 3 fix: the demo queue summary is shown on a separate line and
        // never mixed into the legacy OperationManager badge or counters.
        if (m_popup->m_queueSummaryLabel) {
            if (m_queueController) {
                const bool isProd = isTransferQueueProductionDispatchConnected();
                if (queueActive > 0 || queueQueued > 0) {
                    m_popup->m_queueSummaryLabel->setText(
                        isPolish()
                            ? (isProd
                                  ? QStringLiteral("Kolejka: %1 w toku • %2 oczekuje")
                                  : QStringLiteral("Kolejka demonstracyjna: %1 w toku • %2 oczekuje"))
                                  .arg(queueActive).arg(queueQueued)
                            : (isProd
                                  ? QStringLiteral("Queue: %1 running • %2 waiting")
                                  : QStringLiteral("Demo queue: %1 running • %2 waiting"))
                                  .arg(queueActive).arg(queueQueued));
                } else if (totalHistoryCount > 0) {
                    m_popup->m_queueSummaryLabel->setText(
                        isPolish()
                            ? (isProd
                                  ? QStringLiteral("Kolejka bezczynna • Zapisana historia: %1").arg(totalHistoryCount)
                                  : QStringLiteral("Kolejka: bezczynna • Zapisana historia: %1").arg(totalHistoryCount))
                            : (isProd
                                  ? QStringLiteral("Queue idle • Saved history: %1").arg(totalHistoryCount)
                                  : QStringLiteral("Queue: idle • Saved history: %1").arg(totalHistoryCount)));
                } else {
                    m_popup->m_queueSummaryLabel->setText(
                        isPolish()
                            ? (isProd
                                  ? QStringLiteral("Kolejka: 0 w toku • 0 oczekuje")
                                  : QStringLiteral("Kolejka demonstracyjna: 0 w toku • 0 oczekuje"))
                            : (isProd
                                  ? QStringLiteral("Queue: 0 running • 0 waiting")
                                  : QStringLiteral("Demo queue: 0 running • 0 waiting")));
                }
                m_popup->m_queueSummaryLabel->setToolTip(
                    trLocal("Szczegóły: otwórz główne okno operacji",
                            "Details: open main operations window"));
                m_popup->m_queueSummaryLabel->show();
            } else {
                m_popup->m_queueSummaryLabel->hide();
            }
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
    OperationWindow *m_detailedWindow = nullptr;
    TransferNotificationManager *m_notificationManager = nullptr;
    QPointer<TransferHistoryManager> m_historyManager;
    QPointer<TransferQueueController> m_queueController;
    QList<FileOperationItem *> m_items;
    QHash<KJob *, FileOperationItem *> m_byJob;
    quint64 m_nextId = 1;
    QTimer *m_autoShowTimer = nullptr;
    int m_autoShowGracePeriodMs = 500;
    bool m_userDismissedThisBatch = false;
    bool m_closeOnComplete = false;
};


class OperationSpeedGraph final : public QWidget
{
    Q_OBJECT

public:
    explicit OperationSpeedGraph(
        QList<qulonglong> samples = {},
        QWidget *parent = nullptr)
        : QWidget(parent)
        , m_samples(std::move(samples))
    {
        setObjectName(QStringLiteral("operationSpeedGraph"));
        setMinimumHeight(105);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(
            trLocal(
                "Wykres prędkości transferu",
                "Transfer speed graph"));
    }

    QSize minimumSizeHint() const override
    {
        return QSize(50, 60);
    }

    QSize sizeHint() const override
    {
        return QSize(200, 105);
    }

    void setSamples(QList<qulonglong> samples)
    {
        m_samples = std::move(samples);
        update();
    }

    void setTerminal(bool isTerminal, TransferTaskState terminalState = TransferTaskState::Completed)
    {
        m_isTerminal = isTerminal;
        m_terminalState = terminalState;
        update();
    }

    bool isTerminal() const { return m_isTerminal; }
    TransferTaskState terminalState() const { return m_terminalState; }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF graphRect = QRectF(rect()).adjusted(8, 8, -8, -8);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.setBrush(palette().color(QPalette::AlternateBase));
        painter.drawRoundedRect(graphRect, 5, 5);

        if (m_samples.size() < 2) {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            if (m_isTerminal) {
                QString terminalMsg;
                if (m_terminalState == TransferTaskState::Completed) {
                    terminalMsg = trLocal(
                        "Transfer ukończony natychmiastowo",
                        "Transfer completed instantly");
                } else if (m_terminalState == TransferTaskState::Cancelled) {
                    terminalMsg = trLocal(
                        "Transfer anulowany",
                        "Transfer cancelled");
                } else if (m_terminalState == TransferTaskState::Failed) {
                    terminalMsg = trLocal(
                        "Transfer zakończony niepowodzeniem",
                        "Transfer failed");
                } else {
                    terminalMsg = trLocal(
                        "Operacja zakończona",
                        "Operation completed");
                }
                painter.drawText(graphRect, Qt::AlignCenter, terminalMsg);
            } else {
                painter.drawText(
                    graphRect,
                    Qt::AlignCenter,
                    trLocal(
                        "Oczekiwanie na próbki prędkości…",
                        "Waiting for speed samples…"));
            }
            return;
        }

        qulonglong sampleMaximum = 1;
        for (qulonglong sample : m_samples) {
            sampleMaximum = qMax(sampleMaximum, sample);
        }
        const qulonglong maximum = scaleMaximum(sampleMaximum);

        const QFontMetrics metrics(font());
        QStringList scaleLabels;
        scaleLabels.reserve(5);
        int scaleLabelWidth = 0;
        for (int division = 0; division <= 4; ++division) {
            const qulonglong value = maximum * division / 4;
            const QString label = speedScaleText(value);
            scaleLabels.push_back(label);
            scaleLabelWidth = qMax(
                scaleLabelWidth,
                metrics.horizontalAdvance(label));
        }

        const qreal scaleGap = 8.0;
        const QRectF plotRect = graphRect.adjusted(
            scaleLabelWidth + scaleGap + 6.0,
            6.0,
            -6.0,
            -6.0);

        painter.setPen(QPen(palette().color(QPalette::Midlight), 1));
        for (int division = 0; division <= 4; ++division) {
            const qreal y = plotRect.bottom()
                - plotRect.height() * division / 4.0;
            painter.drawLine(
                QPointF(plotRect.left(), y),
                QPointF(plotRect.right(), y));
        }
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawLine(
            QPointF(plotRect.left(), plotRect.top()),
            QPointF(plotRect.left(), plotRect.bottom()));

        painter.setPen(palette().color(QPalette::PlaceholderText));
        for (int division = 0; division <= 4; ++division) {
            const qreal y = plotRect.bottom()
                - plotRect.height() * division / 4.0;
            const QRectF labelRect(
                graphRect.left() + 4.0,
                y - metrics.height() / 2.0,
                scaleLabelWidth,
                metrics.height());
            painter.drawText(
                labelRect,
                Qt::AlignRight | Qt::AlignVCenter,
                scaleLabels.at(division));
        }

        QPolygonF line;
        line.reserve(m_samples.size());
        for (int index = 0; index < m_samples.size(); ++index) {
            const qreal x = plotRect.left()
                + plotRect.width() * index / (m_samples.size() - 1.0);
            const qreal ratio = static_cast<qreal>(m_samples.at(index))
                / static_cast<qreal>(maximum);
            const qreal y = plotRect.bottom()
                - ratio * plotRect.height();
            line.push_back(QPointF(x, y));
        }

        QPolygonF fill = line;
        fill.push_back(QPointF(plotRect.right(), plotRect.bottom()));
        fill.push_back(QPointF(plotRect.left(), plotRect.bottom()));
        QColor fillColor = palette().color(QPalette::Highlight);
        fillColor.setAlpha(55);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fillColor);
        painter.drawPolygon(fill);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawPolyline(line);
    }

private:
    static qulonglong scaleMaximum(qulonglong sampleMaximum)
    {
        qulonglong maximum = 4;
        while (maximum < sampleMaximum
               && maximum <= std::numeric_limits<qulonglong>::max() / 2) {
            maximum *= 2;
        }
        return qMax(maximum, sampleMaximum);
    }

    static QString speedScaleText(qulonglong bytesPerSecond)
    {
        if (bytesPerSecond == 0) {
            return QStringLiteral("0");
        }
        return formatFileSize(
            static_cast<qint64>(qMin<qulonglong>(
                bytesPerSecond,
                static_cast<qulonglong>(
                    std::numeric_limits<qint64>::max()))),
            false) + QStringLiteral("/s");
    }

    QList<qulonglong> m_samples;
    bool m_isTerminal = false;
    TransferTaskState m_terminalState = TransferTaskState::Completed;
};

// Isolated demo only: feeds the legacy OperationManager with in-memory jobs
// so the Operations tab can be checked with many simultaneous operations.
// No file is read or written; it is a no-op outside the demo build/runtime.
inline void enqueueDemoLegacyOperations(OperationManager *manager, int count)
{
    if (!manager || !isTransferQueueDemoMode()) {
        return;
    }
    static const QStringList names = {
        QStringLiteral("Zdjecia_Wakacje_2026"),
        QStringLiteral("Projekt_Specjalny_Bardzo_Dluga_Nazwa_Katalogu_Do_Weryfikacji_Elidingu_W_Oknie_Operacji"),
        QStringLiteral("Kopia_Zapasowa_Dokumentow.tar.zst"),
        QStringLiteral("Film_4K_Montaz.mkv"),
        QStringLiteral("Biblioteka_Muzyczna"),
    };
    for (int i = 0; i < count; ++i) {
        const QString name = names.at(i % names.size());
        auto *job = new DemoTransferJob(
            (512ULL + 256ULL * static_cast<quint64>(i)) * 1024ULL * 1024ULL,
            45000 + 7000 * i,
            manager,
            true);
        job->setProperty(
            "thispcSourceUrls",
            QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(
                QStringLiteral("/demo/zrodlo/") + name)}));
        job->setProperty(
            "thispcDestinationUrl",
            QVariant::fromValue(QUrl::fromLocalFile(
                QStringLiteral("/demo/cel/bardzo/gleboka/struktura/katalogow/"))));
        manager->track(
            job,
            (isPolish() ? QStringLiteral("Kopiowanie (demo): %1")
                        : QStringLiteral("Copying (demo): %1")).arg(name));
        QObject::connect(job, &KJob::result, manager, [manager, job](KJob *) {
            const bool cancelled = manager->cancelRequested(job)
                || job->error() == KJob::KilledJobError;
            manager->finish(
                job,
                job->error() == 0,
                cancelled,
                job->error() ? job->errorString() : QString());
        });
        job->start();
    }
}

class OperationWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit OperationWindow(
        OperationManager *manager = nullptr,
        QWidget *parent = nullptr)
        : QWidget(parent, Qt::Window)
        , m_manager(manager)
    {
        setObjectName(QStringLiteral("operationWindow"));
        setAttribute(Qt::WA_QuitOnClose, false);
        setWindowTitle(
            trLocal("Operacje plikowe", "File operations"));
        setWindowIcon(
            themedIcon(
                QStringLiteral("folder-sync"),
                QStringLiteral("view-task")));
        if (m_manager && !m_manager->detailedWindow()) {
            m_manager->setDetailedWindow(this);
        }
        setMinimumSize(480, 260);
        QSettings settings;
        const QByteArray savedGeom = settings.value(QStringLiteral("operationWindow/geometry")).toByteArray();
        if (!savedGeom.isEmpty() && restoreGeometry(savedGeom)) {
            // Restored saved geometry
        } else {
            if (m_queueController) {
                resize(580, 380);
            } else {
                resize(580, 320);
            }
        }

        m_windowLayout = new QVBoxLayout(this);
        m_windowLayout->setContentsMargins(14, 14, 14, 14);
        m_windowLayout->setSpacing(10);

        auto *headerLayout = new QHBoxLayout;
        auto *title = new QLabel(
            trLocal("Operacje plikowe", "File operations"),
            this);
        title->setObjectName(QStringLiteral("operationPopupTitle"));
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        headerLayout->addWidget(title, 0, Qt::AlignLeft | Qt::AlignVCenter);

        m_summaryLabel = new TransferQueueElidedLabel(this, Qt::ElideRight);
        m_summaryLabel->setObjectName(
            QStringLiteral("operationSummaryLabel"));
        m_summaryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        headerLayout->addWidget(m_summaryLabel, 1, Qt::AlignRight | Qt::AlignVCenter);

        m_pauseAllButton = new QToolButton(this);
        m_pauseAllButton->setObjectName(QStringLiteral("transferQueuePauseAllButton"));
        m_pauseAllButton->setIcon(themedIcon(QStringLiteral("media-playback-pause")));
        m_pauseAllButton->setText(trLocal("Wstrzymaj wszystkie", "Pause All"));
        m_pauseAllButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_pauseAllButton->setToolTip(trLocal("Wstrzymaj wszystkie operacje", "Pause all operations"));
        connect(m_pauseAllButton, &QToolButton::clicked, this, [this] {
            if (m_queueController) m_queueController->pauseAll();
            if (m_manager) m_manager->pauseAll();
        });
        headerLayout->addWidget(m_pauseAllButton);

        m_resumeAllButton = new QToolButton(this);
        m_resumeAllButton->setObjectName(QStringLiteral("transferQueueResumeAllButton"));
        m_resumeAllButton->setIcon(themedIcon(QStringLiteral("media-playback-start")));
        m_resumeAllButton->setText(trLocal("Wznów wszystkie", "Resume All"));
        m_resumeAllButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_resumeAllButton->setToolTip(trLocal("Wznów wszystkie operacje", "Resume all operations"));
        connect(m_resumeAllButton, &QToolButton::clicked, this, [this] {
            if (m_queueController) m_queueController->resumeAll();
            if (m_manager) m_manager->resumeAll();
        });
        headerLayout->addWidget(m_resumeAllButton);

        m_cancelAllButton = new QToolButton(this);
        m_cancelAllButton->setObjectName(QStringLiteral("transferQueueCancelAllButton"));
        m_cancelAllButton->setIcon(themedIcon(QStringLiteral("process-stop"), QStringLiteral("dialog-cancel")));
        m_cancelAllButton->setText(trLocal("Anuluj wszystkie", "Cancel All"));
        m_cancelAllButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_cancelAllButton->setToolTip(trLocal("Anuluj wszystkie operacje", "Cancel all operations"));
        connect(m_cancelAllButton, &QToolButton::clicked, this, [this] {
            promptCancelAll();
        });
        headerLayout->addWidget(m_cancelAllButton);

        m_clearHistoryButton = new QToolButton(this);
        m_clearHistoryButton->setObjectName(QStringLiteral("operationClearHistoryButton"));
        m_clearHistoryButton->setIcon(themedIcon(QStringLiteral("edit-clear-history"), QStringLiteral("edit-clear")));
        m_clearHistoryButton->setText(trLocal("Wyczyść historię", "Clear History"));
        m_clearHistoryButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_clearHistoryButton->setToolTip(trLocal("Wyczyść historię zakończonych operacji", "Clear history of finished operations"));
        connect(m_clearHistoryButton, &QToolButton::clicked, this, [this] {
            clearHistory(true);
        });
        headerLayout->addWidget(m_clearHistoryButton);

        m_settingsToggleButton = new QToolButton(this);
        m_settingsToggleButton->setObjectName(QStringLiteral("operationSettingsToggleButton"));
        m_settingsToggleButton->setIcon(themedIcon(QStringLiteral("configure"), QStringLiteral("preferences-system")));
        m_settingsToggleButton->setText(trLocal("Ustawienia", "Settings"));
        m_settingsToggleButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_settingsToggleButton->setToolTip(trLocal("Ustawienia operacji i limitu transferu", "Operation and bandwidth settings"));
        m_settingsToggleButton->setCheckable(true);
        connect(m_settingsToggleButton, &QToolButton::toggled, this, [this](bool checked) {
            m_settingsContainer->setVisible(checked);
            if (checked && m_queueSettings) {
                m_settingsContainer->setMaximumHeight(m_queueSettings->sizeHint().height() + 4);
            }
        });
        headerLayout->addWidget(m_settingsToggleButton);

        m_windowLayout->addLayout(headerLayout);

        // Collapsible settings container (Unified Mode)
        m_settingsContainer = new QScrollArea(this);
        m_settingsContainer->setObjectName(QStringLiteral("operationSettingsContainer"));
        m_settingsContainer->setFrameShape(QFrame::NoFrame);
        m_settingsContainer->setWidgetResizable(true);
        m_settingsContainer->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_settingsContainer->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_settingsContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_settingsContainer->setMinimumHeight(55);
        m_settingsContainer->viewport()->setAutoFillBackground(false);
        m_settingsContainer->hide();
        m_windowLayout->addWidget(m_settingsContainer);

        // Operations page: the list of live operations.
        m_operationsPage = new QWidget(this);
        m_operationsPage->setObjectName(
            QStringLiteral("operationWindowOperationsPage"));
        auto *operationsLayout = new QVBoxLayout(m_operationsPage);
        operationsLayout->setContentsMargins(0, 0, 0, 0);
        operationsLayout->setSpacing(6);

        m_demoOperationsButton = new QToolButton(m_operationsPage);
        m_demoOperationsButton->setObjectName(
            QStringLiteral("operationDemoLegacyButton"));
        m_demoOperationsButton->setIcon(themedIcon(QStringLiteral("list-add")));
        m_demoOperationsButton->setText(
            trLocal("Dodaj 5 operacji demo", "Add 5 demo operations"));
        m_demoOperationsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_demoOperationsButton->setToolTip(
            trLocal("Symulowane operacje w pamięci — nie dotykają żadnych plików",
                    "In-memory simulated operations — no files are touched"));
        m_demoOperationsButton->hide();
        connect(
            m_demoOperationsButton,
            &QToolButton::clicked,
            this,
            [this] { enqueueDemoLegacyOperations(m_manager, 5); });
        operationsLayout->addWidget(m_demoOperationsButton, 0, Qt::AlignRight);

        m_operationsEmptyLabel = new QLabel(
            trLocal("Brak aktywnych operacji plikowych.",
                    "No active file operations."),
            m_operationsPage);
        m_operationsEmptyLabel->setObjectName(
            QStringLiteral("operationWindowEmptyLabel"));
        m_operationsEmptyLabel->setAlignment(Qt::AlignCenter);
        m_operationsEmptyLabel->setForegroundRole(QPalette::PlaceholderText);
        m_operationsEmptyLabel->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Expanding);
        m_operationsEmptyLabel->hide();
        operationsLayout->addWidget(m_operationsEmptyLabel, 1);

        m_scrollArea = new QScrollArea(m_operationsPage);
        m_scrollArea->setObjectName(
            QStringLiteral("operationWindowScrollArea"));
        m_scrollArea->setWidgetResizable(true);
        m_scrollArea->setFrameShape(QFrame::NoFrame);
        m_scrollArea->setHorizontalScrollBarPolicy(
            Qt::ScrollBarAlwaysOff);
        m_scrollArea->setVerticalScrollBarPolicy(
            Qt::ScrollBarAsNeeded);
        m_scrollArea->setSizePolicy(
            QSizePolicy::Expanding,
            QSizePolicy::Expanding);
        m_listWidget = new QWidget(m_scrollArea);
        m_listLayout = new QVBoxLayout(m_listWidget);
        m_listLayout->setContentsMargins(0, 0, 0, 0);
        m_listLayout->setSpacing(8);
        m_scrollArea->setWidget(m_listWidget);
        operationsLayout->addWidget(m_scrollArea, 1);
        m_windowLayout->addWidget(m_operationsPage, 1);

        // Queue page (for demo tab mode compatibility)
        m_tabs = new QTabWidget(this);
        m_tabs->setObjectName(QStringLiteral("operationWindowTabs"));
        m_tabs->setDocumentMode(true);
        m_tabs->hide();
        m_windowLayout->addWidget(m_tabs, 1);

        m_queuePage = new QWidget(m_tabs);
        m_queuePage->setObjectName(QStringLiteral("operationWindowQueuePage"));
        auto *queueLayout = new QVBoxLayout(m_queuePage);
        queueLayout->setContentsMargins(0, 8, 0, 0);
        queueLayout->setSpacing(8);

        m_queueNoticeLabel = new QLabel(transferQueueDemoNoticeText(), m_queuePage);
        m_queueNoticeLabel->setObjectName(QStringLiteral("transferQueueDemoNotice"));
        m_queueNoticeLabel->setWordWrap(true);
        m_queueNoticeLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_queueNoticeLabel->setStyleSheet(QStringLiteral(
            "QLabel { background-color: rgba(52, 152, 219, 0.16); "
            "border: 1px solid rgba(52, 152, 219, 0.5); border-radius: 4px; "
            "padding: 4px 8px; }"));
        queueLayout->addWidget(m_queueNoticeLabel);

        m_queueSettings = new TransferQueueSettingsWidget(m_queuePage);
        queueLayout->addWidget(m_queueSettings);

        m_queueView = new TransferQueueViewWidget(m_queuePage);
        queueLayout->addWidget(m_queueView, 1);
        m_queuePage->hide();

        connect(m_queueSettings, &TransferQueueSettingsWidget::closeWindowOnCompleteToggled,
                this, [this](bool checked) {
                    if (m_manager) m_manager->setCloseOnComplete(checked);
                });

        if (m_manager) {
            connect(
                m_manager,
                &OperationManager::operationsChanged,
                this,
                [this] {
                    if (isVisible()) scheduleRebuild();
                    updateTabTitles();
                });
        }

        if (m_manager && m_manager->queueController()) {
            setQueueController(m_manager->queueController());
        }
        if (m_manager && m_manager->historyManager()) {
            setHistoryManager(m_manager->historyManager());
        }
        rebuild();
    }

    ~OperationWindow() override
    {
        if (m_manager && m_manager->detailedWindow() == this) {
            m_manager->setDetailedWindow(nullptr);
        }
    }

    QSize sizeHint() const override
    {
        if (m_queueController) {
            return QSize(580, 380);
        }
        return QWidget::sizeHint();
    }

    QSize minimumSizeHint() const override
    {
        const bool isUnified = isTransferQueueProductionDispatchConnected();
        if (m_queueController && !isUnified) {
            return QSize(480, 380);
        }
        return QSize(480, 260);
    }

    TransferQueueSettingsWidget *queueSettingsWidget() const { return m_queueSettings; }
    TransferQueueViewWidget *queueViewWidget() const { return m_queueView; }
    QTabWidget *tabWidget() const { return m_tabs; }
    QWidget *operationsPage() const { return m_operationsPage; }
    QWidget *queuePage() const { return m_queuePage; }
    QScrollArea *operationsScrollArea() const { return m_scrollArea; }
    QLabel *queueNoticeLabel() const { return m_queueNoticeLabel; }
    QToolButton *demoOperationsButton() const { return m_demoOperationsButton; }
    QToolButton *pauseAllButton() const { return m_pauseAllButton; }
    QToolButton *resumeAllButton() const { return m_resumeAllButton; }
    QToolButton *cancelAllButton() const { return m_cancelAllButton; }
    QToolButton *clearHistoryButton() const { return m_clearHistoryButton; }
    bool hasQueueTab() const { return m_tabs && m_tabs->indexOf(m_queuePage) >= 0; }

    void clearHistory(bool promptConfirmation = false)
    {
        if (promptConfirmation && !promptClearHistoryConfirmation(this)) {
            return;
        }

        if (m_queueController) {
            for (const auto &task : m_queueController->snapshot().tasks) {
                if (task.terminal && !task.historyUuid.isEmpty()) {
                    m_dismissedHistoryUuids.insert(task.historyUuid);
                }
            }
            m_queueController->clearHistory(true);
        } else if (TransferHistoryManager *histMgr = historyManager()) {
            for (const auto &rec : histMgr->records()) {
                m_dismissedHistoryUuids.insert(rec.uuid);
            }
            histMgr->clear(true);
        }
        if (m_manager) {
            m_manager->clearFinished();
        }
        scheduleRebuild();
    }

    void showOperationsTab()
    {
        if (hasQueueTab()) m_tabs->setCurrentWidget(m_operationsPage);
    }

    void showQueueTab()
    {
        if (hasQueueTab()) m_tabs->setCurrentWidget(m_queuePage);
    }

    void setCloseOnComplete(bool close)
    {
        if (m_queueSettings) {
            m_queueSettings->closeOnCompleteCheckBox()->setChecked(close);
        }
    }

    bool isCloseOnComplete() const
    {
        return m_queueSettings ? m_queueSettings->closeOnCompleteCheckBox()->isChecked() : false;
    }

    void promptCancelAll()
    {
        const int ret = QMessageBox::question(
            this,
            trLocal("Anuluj wszystkie operacje", "Cancel All Operations"),
            trLocal("Czy na pewno chcesz anulować wszystkie operacje?",
                    "Are you sure you want to cancel all operations?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ret == QMessageBox::Yes) {
            if (m_queueController) m_queueController->cancelAll();
            if (m_manager) m_manager->cancelAll();
        }
    }

    void setQueueController(TransferQueueController *controller)
    {
        if (m_queueController) {
            disconnect(m_queueController, nullptr, this, nullptr);
        }
        m_queueController = controller;
        if (m_queueSettings) {
            m_queueSettings->setController(controller);
        }
        if (m_queueView) {
            m_queueView->setController(controller);
        }

        const bool isUnified = isTransferQueueProductionDispatchConnected();
        const bool tabMode = controller != nullptr && !isUnified;

        if (tabMode && !hasQueueTab()) {
            m_windowLayout->removeWidget(m_operationsPage);
            m_tabs->addTab(m_operationsPage, QString());
            m_tabs->addTab(m_queuePage, QString());
            m_queueSettings->setParent(m_queuePage);
            m_queuePage->layout()->removeWidget(m_queueView);
            m_queuePage->layout()->addWidget(m_queueSettings);
            m_queuePage->layout()->addWidget(m_queueView);
            m_tabs->setCurrentWidget(m_operationsPage);
            m_tabs->show();
            m_operationsPage->show();
            m_queuePage->show();
            m_scrollArea->setMinimumHeight(120);
            m_scrollArea->setMaximumHeight(QWIDGETSIZE_MAX);
            m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            setMinimumSize(480, 380);
            if (!isVisible()) {
                resize(qMax(width(), 580), qMax(height(), 380));
            }
        } else if (!tabMode) {
            if (hasQueueTab()) {
                m_tabs->removeTab(m_tabs->indexOf(m_queuePage));
                m_tabs->removeTab(m_tabs->indexOf(m_operationsPage));
            }
            m_tabs->hide();
            m_operationsPage->setParent(this);
            m_settingsContainer->setWidget(m_queueSettings);
            if (m_queueSettings) {
                m_settingsContainer->setMaximumHeight(m_queueSettings->sizeHint().height() + 4);
            }
            m_windowLayout->insertWidget(2, m_operationsPage, 1);
            m_operationsPage->show();
            m_scrollArea->setMinimumHeight(70);
            m_scrollArea->setMaximumHeight(QWIDGETSIZE_MAX);
            m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            setMinimumSize(480, 260);
            if (!isVisible()) {
                QSettings settings;
                if (!settings.contains(QStringLiteral("operationWindow/geometry"))) {
                    resize(580, 380);
                }
            }
        }

        m_queueNoticeLabel->setVisible(tabMode && !isTransferQueueProductionDispatchConnected());
        m_queueView->setDemoButtonVisible(tabMode && isTransferQueueDemoMode() && !isTransferQueueProductionDispatchConnected());
        m_demoOperationsButton->setVisible(tabMode && isTransferQueueDemoMode() && !isTransferQueueProductionDispatchConnected());

        if (controller) {
            if (controller->historyManager()) {
                disconnect(controller->historyManager(), nullptr, this, nullptr);
                connect(controller->historyManager(), &TransferHistoryManager::recordsChanged,
                        this, [this] {
                            if (isVisible()) scheduleRebuild();
                        });
                connect(controller->historyManager(), &TransferHistoryManager::historyCleared,
                        this, [this] {
                            m_dismissedHistoryUuids.clear();
                            if (isVisible()) scheduleRebuild();
                        });
            }
            connect(controller, &TransferQueueController::historyManagerChanged,
                    this, [this](TransferHistoryManager *hm) {
                        if (hm) {
                            disconnect(hm, nullptr, this, nullptr);
                            connect(hm, &TransferHistoryManager::recordsChanged,
                                    this, [this] { if (isVisible()) scheduleRebuild(); });
                            connect(hm, &TransferHistoryManager::historyCleared,
                                    this, [this] {
                                        m_dismissedHistoryUuids.clear();
                                        if (isVisible()) scheduleRebuild();
                                    });
                        }
                        if (isVisible()) scheduleRebuild();
                    });
            connect(controller, &TransferQueueController::queueChanged,
                    this, [this] {
                        if (isVisible()) scheduleRebuild();
                        updateTabTitles();
                    });
            connect(controller, &TransferQueueController::taskStateChanged,
                    this, [this](quint64, TransferTaskState, TransferTaskState) {
                        if (isVisible()) scheduleRebuild();
                        updateTabTitles();
                    });
            connect(controller, &TransferQueueController::taskFinished,
                    this, [this](quint64, TransferTaskState) {
                        if (isVisible()) scheduleRebuild();
                        updateTabTitles();
                    });
            connect(controller, &TransferQueueController::taskOrderChanged,
                    this, [this](quint64) {
                        if (isVisible()) scheduleRebuild();
                    });
            connect(controller, &TransferQueueController::taskPriorityChanged,
                    this, [this](quint64, TransferPriority) {
                        if (isVisible()) scheduleRebuild();
                    });
            connect(controller, &TransferQueueController::historySettingsChanged,
                    this, [this](bool, int) {
                        if (isVisible()) scheduleRebuild();
                    });
            connect(controller, &TransferQueueController::taskProgressChanged,
                    this, [this](quint64 id, quint64 processed, quint64 total, int percent) {
                        updateQueueTaskProgressInPlace(id, processed, total, percent);
                    });
        }
        updateTabTitles();
        rebuild();
    }

    void setHistoryManager(TransferHistoryManager *manager)
    {
        if (m_explicitHistoryManager == manager) return;
        if (m_explicitHistoryManager) {
            disconnect(m_explicitHistoryManager, nullptr, this, nullptr);
        }
        m_explicitHistoryManager = manager;
        if (m_explicitHistoryManager) {
            connect(m_explicitHistoryManager, &TransferHistoryManager::recordsChanged,
                    this, [this] { if (isVisible()) scheduleRebuild(); });
            connect(m_explicitHistoryManager, &TransferHistoryManager::historyCleared,
                    this, [this] {
                        m_dismissedHistoryUuids.clear();
                        if (isVisible()) scheduleRebuild();
                    });
        }
        if (isVisible()) scheduleRebuild();
    }

    TransferHistoryManager *historyManager() const
    {
        if (m_explicitHistoryManager) {
            return m_explicitHistoryManager;
        }
        if (m_queueController && m_queueController->historyManager()) {
            return m_queueController->historyManager();
        }
        if (m_manager && m_manager->historyManager()) {
            return m_manager->historyManager();
        }
        return nullptr;
    }

    int operationsTabCount() const
    {
        return m_manager ? m_manager->activeCount() : 0;
    }

    int queueTabCount() const
    {
        return m_queueController
            ? m_queueController->activeCount() + m_queueController->queuedCount()
            : 0;
    }

    void updateTabTitles()
    {
        if (!hasQueueTab()) {
            return;
        }
        m_tabs->setTabText(
            m_tabs->indexOf(m_operationsPage),
            (isPolish() ? QStringLiteral("Operacje (%1)") : QStringLiteral("Operations (%1)"))
                .arg(operationsTabCount()));
        m_tabs->setTabToolTip(
            m_tabs->indexOf(m_operationsPage),
            trLocal("Bieżące operacje plikowe", "Current file operations"));
        m_tabs->setTabText(
            m_tabs->indexOf(m_queuePage),
            (isPolish() ? QStringLiteral("Kolejka (%1)") : QStringLiteral("Queue (%1)"))
                .arg(queueTabCount()));
        m_tabs->setTabToolTip(
            m_tabs->indexOf(m_queuePage),
            isTransferQueueProductionDispatchConnected()
                ? trLocal("Kolejka transferów", "Transfer queue")
                : transferQueueDemoNoticeText());
    }

    void rebuild()
    {
        const int prevScroll = m_scrollArea->verticalScrollBar()->value();

        while (QLayoutItem *child = m_listLayout->takeAt(0)) {
            delete child->widget();
            delete child;
        }

        const bool legacyMode = !isTransferQueueProductionDispatchConnected();
        if (hasQueueTab()) {
            updateTabTitles();
        }

        // If in legacy mode (0.43.0 or Stage 3 demo compatibility), render legacy operations
        if (legacyMode) {
            const QList<OperationManager::OperationSnapshot> allOperations =
                m_manager ? m_manager->operations() : QList<OperationManager::OperationSnapshot>{};
            QList<OperationManager::OperationSnapshot> operations;
            int legacyFinished = 0;
            for (const auto &operation : allOperations) {
                if (operation.state == OperationManager::State::Running
                    || operation.state == OperationManager::State::Paused
                    || operation.state == OperationManager::State::Cancelling) {
                    operations.push_back(operation);
                } else {
                    ++legacyFinished;
                }
            }

            m_clearHistoryButton->setEnabled(legacyFinished > 0);
            m_clearHistoryButton->setToolTip(legacyFinished > 0
                ? trLocal("Wyczyść historię zakończonych operacji", "Clear history of finished operations")
                : trLocal("Brak zakończonych operacji do wyczyszczenia", "No finished operations to clear"));

            QSet<quint64> currentIds;
            for (const auto &operation : operations) {
                const quint64 opKey = 0x8000000000000000ULL | operation.id;
                currentIds.insert(opKey);
                if (!m_knownIds.contains(opKey)) {
                    m_expandedIds.insert(opKey);
                }
            }
            m_knownIds = currentIds;
            m_expandedIds.intersect(currentIds);

            for (const auto &operation : operations) {
                addLegacyOperationCard(operation);
            }

            if (operations.isEmpty()) {
                const int queueActive = m_queueController
                    ? (m_queueController->activeCount() + m_queueController->queuedCount())
                    : 0;
                if (queueActive == 0 && (!hasQueueTab() || m_tabs->currentIndex() == 0)) {
                    hide();
                    return;
                }
                m_scrollArea->hide();
                m_operationsEmptyLabel->show();
                m_summaryLabel->setText(
                    trLocal("Brak aktywnych operacji", "No active operations"));
                m_lastOperationCount = 0;
                return;
            }

            m_operationsEmptyLabel->hide();
            m_scrollArea->show();
            m_listLayout->addStretch(1);
            m_summaryLabel->setText(
                isPolish()
                    ? QStringLiteral("Aktywne: %1").arg(operations.size())
                    : QStringLiteral("Active: %1").arg(operations.size()));

            m_listLayout->activate();
            int contentHeight = 0;
            const auto rows = m_listWidget->findChildren<QFrame *>(
                QStringLiteral("operationRow"),
                Qt::FindDirectChildrenOnly);
            for (QFrame *row : rows) {
                if (row->layout()) {
                    row->layout()->activate();
                }
                if (contentHeight > 0) {
                    contentHeight += m_listLayout->spacing();
                }
                contentHeight += qMax(
                    row->sizeHint().height(),
                    row->minimumSizeHint().height());
            }
            m_listWidget->setMinimumHeight(contentHeight);

            if (!m_queueController) {
                const int screenLimit = screen()
                    ? qMax(260, screen()->availableGeometry().height() * 3 / 4)
                    : 620;
                const int viewportHeight = qBound(
                    150,
                    contentHeight + 4,
                    qMax(150, screenLimit - 70));
                m_scrollArea->setFixedHeight(viewportHeight);
                m_scrollArea->setVerticalScrollBarPolicy(
                    contentHeight > viewportHeight
                        ? Qt::ScrollBarAsNeeded
                        : Qt::ScrollBarAlwaysOff);
                if (layout()) {
                    layout()->activate();
                }
                resize(580, qMin(screenLimit, sizeHint().height()));
            }

            m_scrollArea->verticalScrollBar()->setValue(prevScroll);
            m_lastOperationCount = operations.size();
            return;
        }

        // UNIFIED MODE (0.44.0 Unified Operations Manager)
        TransferQueueSnapshot queueSnapshot;
        if (m_queueController) {
            queueSnapshot = m_queueController->snapshot();
        }

        const QList<OperationManager::OperationSnapshot> allLegacy =
            m_manager ? m_manager->operations() : QList<OperationManager::OperationSnapshot>{};

        QList<OperationManager::OperationSnapshot> filteredLegacy;
        int legacyActive = 0;
        int legacyFinished = 0;
        for (const auto &op : allLegacy) {
            bool matchedQueue = false;
            for (const auto &qt : queueSnapshot.tasks) {
                if (!op.title.isEmpty() && qt.title == op.title) {
                    matchedQueue = true;
                    break;
                }
            }
            if (matchedQueue) {
                continue;
            }

            filteredLegacy.push_back(op);
            if (op.state == OperationManager::State::Running
                || op.state == OperationManager::State::Paused
                || op.state == OperationManager::State::Cancelling) {
                ++legacyActive;
            } else {
                ++legacyFinished;
            }
        }

        // Keep tracked expanded IDs
        QSet<quint64> currentIds;
        for (const auto &task : queueSnapshot.tasks) {
            currentIds.insert(task.id);
            if (!m_knownIds.contains(task.id)) {
                m_expandedIds.insert(task.id);
            }
        }
        for (const auto &op : filteredLegacy) {
            const quint64 opKey = 0x8000000000000000ULL | op.id;
            currentIds.insert(opKey);
            if (!m_knownIds.contains(opKey)) {
                m_expandedIds.insert(opKey);
            }
        }
        m_knownIds = currentIds;
        m_expandedIds.intersect(currentIds);

        auto queueVisualRank = [](TransferTaskState state) -> int {
            switch (state) {
            case TransferTaskState::Running:
            case TransferTaskState::Paused:
            case TransferTaskState::Cancelling:
                return 0;
            case TransferTaskState::Queued:
                return 1;
            case TransferTaskState::Completed:
            case TransferTaskState::Cancelled:
            case TransferTaskState::Failed:
                return 2;
            }
            return 3;
        };

        QList<TransferTaskSnapshot> visualQueueTasks = queueSnapshot.tasks;
        std::stable_sort(visualQueueTasks.begin(), visualQueueTasks.end(),
                         [&queueVisualRank](const TransferTaskSnapshot &a, const TransferTaskSnapshot &b) {
                             const int rankA = queueVisualRank(a.state);
                             const int rankB = queueVisualRank(b.state);
                             if (rankA != rankB) {
                                 return rankA < rankB;
                             }
                             if (a.state == TransferTaskState::Queued && b.state == TransferTaskState::Queued) {
                                 if (a.queuePosition != b.queuePosition) {
                                     return a.queuePosition < b.queuePosition;
                                 }
                             }
                             return false;
                         });

        auto legacyVisualRank = [](OperationManager::State state) -> int {
            switch (state) {
            case OperationManager::State::Running:
            case OperationManager::State::Paused:
            case OperationManager::State::Cancelling:
                return 0;
            case OperationManager::State::Completed:
            case OperationManager::State::Cancelled:
            case OperationManager::State::Failed:
                return 2;
            }
            return 2;
        };

        std::stable_sort(filteredLegacy.begin(), filteredLegacy.end(),
                         [&legacyVisualRank](const OperationManager::OperationSnapshot &a,
                                             const OperationManager::OperationSnapshot &b) {
                             return legacyVisualRank(a.state) < legacyVisualRank(b.state);
                         });

        // 1. Active items (Running / Paused / Cancelling)
        for (const auto &task : visualQueueTasks) {
            if (queueVisualRank(task.state) == 0) {
                addQueueTaskCard(task);
            }
        }
        for (const auto &op : filteredLegacy) {
            if (legacyVisualRank(op.state) == 0) {
                addLegacyOperationCard(op);
            }
        }

        // 2. Queued items (strictly in queue order: #1, #2, #3...)
        for (const auto &task : visualQueueTasks) {
            if (queueVisualRank(task.state) == 1) {
                addQueueTaskCard(task);
            }
        }

        // 3. Finished items (Completed / Cancelled / Failed)
        for (const auto &task : visualQueueTasks) {
            if (queueVisualRank(task.state) == 2) {
                addQueueTaskCard(task);
            }
        }
        for (const auto &op : filteredLegacy) {
            if (legacyVisualRank(op.state) == 2) {
                addLegacyOperationCard(op);
            }
        }

        // 4. Archival history records (Completed / Cancelled / Failed from TransferHistoryManager)
        QSet<QString> activeOrRecentHistoryUuids;
        for (const auto &task : queueSnapshot.tasks) {
            if (!task.historyUuid.isEmpty()) {
                activeOrRecentHistoryUuids.insert(task.historyUuid);
            }
        }

        QList<TransferHistoryRecord> archivalRecords;
        TransferHistoryManager *histMgr = historyManager();
        if (histMgr) {
            histMgr->syncWithDiskIfNeeded();
            const QList<TransferHistoryRecord> allHistory = histMgr->records();
            for (const auto &rec : allHistory) {
                if (activeOrRecentHistoryUuids.contains(rec.uuid)) {
                    continue; // Skip: already shown in active/recent queue cards
                }
                if (m_dismissedHistoryUuids.contains(rec.uuid)) {
                    continue; // Skip: dismissed from view by user in this session
                }
                archivalRecords.push_back(rec);
            }
        }

        if (archivalRecords.size() == 1 && !m_knownHistoryUuids.contains(archivalRecords.first().uuid)) {
            m_expandedHistoryUuids.insert(archivalRecords.first().uuid);
        }
        for (const auto &rec : archivalRecords) {
            m_knownHistoryUuids.insert(rec.uuid);
            addArchivalHistoryCard(rec);
        }

        TransferQueueCounts qCounts = transferQueueCounts(queueSnapshot);
        const int totalActive = qCounts.active + legacyActive;
        const int totalQueued = qCounts.queued;
        const int totalFinished = (qCounts.completed + qCounts.cancelled + qCounts.failed) + legacyFinished;
        const int totalArchival = archivalRecords.size();
        const int totalTasks = queueSnapshot.tasks.size() + filteredLegacy.size() + totalArchival;

        QString summaryText;
        QString summaryTooltip;
        if (totalActive > 0 && totalQueued > 0) {
            summaryText = isPolish()
                ? QStringLiteral("W toku: %1 • W kolejce: %2").arg(totalActive).arg(totalQueued)
                : QStringLiteral("Running: %1 • Queued: %2").arg(totalActive).arg(totalQueued);
            summaryTooltip = summaryText;
        } else if (totalActive > 0) {
            summaryText = isPolish()
                ? QStringLiteral("W toku: %1").arg(totalActive)
                : QStringLiteral("Running: %1").arg(totalActive);
            summaryTooltip = summaryText;
        } else if (totalQueued > 0) {
            summaryText = isPolish()
                ? QStringLiteral("W kolejce: %1").arg(totalQueued)
                : QStringLiteral("Queued: %1").arg(totalQueued);
            summaryTooltip = summaryText;
        } else if (totalFinished > 0 || totalArchival > 0) {
            const int displayFinished = totalFinished + totalArchival;
            summaryText = isPolish()
                ? QStringLiteral("Zakończone: %1").arg(displayFinished)
                : QStringLiteral("Completed: %1").arg(displayFinished);
            summaryTooltip = trLocal("Wszystkie operacje zakończone", "All operations completed")
                             + QStringLiteral(" (%1)").arg(displayFinished);
        } else {
            summaryText = trLocal("Brak aktywnych operacji", "No active operations");
            summaryTooltip = summaryText;
        }
        m_summaryLabel->setFullText(summaryText, false);
        m_summaryLabel->setToolTip(summaryTooltip);

        const bool hasAnyPaused = (qCounts.active > 0 && m_queueController && m_queueController->isGlobalPauseActive())
            || (m_manager && m_manager->hasPausedOperations());
        m_pauseAllButton->setEnabled(totalActive > 0);
        m_resumeAllButton->setEnabled(hasAnyPaused || (m_queueController && m_queueController->dispatchPaused()));
        m_cancelAllButton->setEnabled(totalActive > 0 || totalQueued > 0);
        m_clearHistoryButton->setEnabled(totalFinished > 0);
        m_clearHistoryButton->setToolTip(totalFinished > 0
            ? trLocal("Wyczyść historię zakończonych operacji", "Clear history of finished operations")
            : trLocal("Brak zakończonych operacji do wyczyszczenia", "No finished operations to clear"));

        if (totalTasks == 0) {
            if (!m_queueController && !isTransferQueueDemoMode()) {
                hide();
                return;
            }
            m_scrollArea->hide();
            m_operationsEmptyLabel->setText(
                trLocal("Brak aktywnych operacji plikowych i historii.",
                        "No active file operations or history."));
            m_operationsEmptyLabel->show();
            m_lastOperationCount = 0;
            return;
        }

        m_operationsEmptyLabel->hide();
        m_scrollArea->show();
        m_listLayout->addStretch(1);
        m_scrollArea->verticalScrollBar()->setValue(prevScroll);
        m_lastOperationCount = totalActive;
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        QSettings settings;
        settings.setValue(QStringLiteral("operationWindow/geometry"), saveGeometry());
        if (m_manager) {
            const int active = m_manager->activeCount()
                + (m_queueController ? (m_queueController->activeCount() + m_queueController->queuedCount()) : 0);
            if (active > 0) {
                m_manager->setUserDismissed(true);
            }
        }
        QWidget::closeEvent(event);
    }

public:
    void scheduleRebuild()
    {
        if (m_rebuildScheduled) {
            return;
        }
        m_rebuildScheduled = true;
        QTimer::singleShot(120, this, [this] {
            m_rebuildScheduled = false;
            if (m_pointerInteraction
                || QGuiApplication::mouseButtons() != Qt::NoButton) {
                scheduleRebuild();
                return;
            }
            if (isVisible()) {
                rebuild();
            }
        });
    }

    void updateQueueTaskProgressInPlace(quint64 id, quint64 processed, quint64 total, int percent)
    {
        QFrame *targetRow = nullptr;
        const auto rows = m_listWidget->findChildren<QFrame *>(
            QStringLiteral("operationRow"), Qt::FindDirectChildrenOnly);
        for (QFrame *row : rows) {
            if (row->property("taskId").toULongLong() == id) {
                targetRow = row;
                break;
            }
        }

        if (!targetRow) {
            scheduleRebuild();
            return;
        }

        auto *progress = targetRow->findChild<QProgressBar *>(QStringLiteral("operationProgress"));
        if (progress) {
            progress->setValue(percent);
        }

        auto *percentLabel = targetRow->findChild<QLabel *>(QStringLiteral("operationPercentLabel"));
        if (percentLabel) {
            percentLabel->setText(QStringLiteral("%1%").arg(percent));
        }

        auto *bytesLabel = targetRow->findChild<QLabel *>(QStringLiteral("operationBytesLabel"));
        if (bytesLabel) {
            const QString bText = transferText(processed, total);
            bytesLabel->setText(!bText.isEmpty()
                                    ? bText
                                    : trLocal("Oczekiwanie na informacje o postępie…", "Waiting for progress information…"));
        }

        if (m_queueController) {
            const auto snapOpt = m_queueController->taskSnapshot(id);
            if (snapOpt) {
                const auto &snap = *snapOpt;
                auto *speedLabel = targetRow->findChild<QLabel *>(QStringLiteral("operationSpeedLabel"));
                if (speedLabel) {
                    speedLabel->setText(QStringLiteral("%1: %2").arg(trLocal("Prędkość", "Speed"), speedText(snap.speedBytesPerSecond)));
                }
                auto *avgSpeedLabel = targetRow->findChild<QLabel *>(QStringLiteral("operationAverageSpeedLabel"));
                if (avgSpeedLabel) {
                    avgSpeedLabel->setText(QStringLiteral("%1: %2").arg(trLocal("Średnia", "Average"), speedText(snap.averageSpeedBytesPerSecond)));
                }
                auto *etaLabel = targetRow->findChild<QLabel *>(QStringLiteral("operationEtaLabel"));
                if (etaLabel) {
                    etaLabel->setText(QStringLiteral("%1: %2").arg(QStringLiteral("ETA"), etaText(snap.etaSeconds)));
                }
                auto *graph = targetRow->findChild<OperationSpeedGraph *>(QStringLiteral("operationSpeedGraph"));
                if (graph) {
                    graph->setSamples(snap.speedSamples);
                    if (snap.terminal) {
                        graph->setTerminal(true, snap.state);
                    }
                }
            }
        }
    }

private:

    void protectInteraction(QToolButton *button)
    {
        connect(
            button,
            &QToolButton::pressed,
            this,
            [this] { m_pointerInteraction = true; });
        connect(
            button,
            &QToolButton::released,
            this,
            [this] {
                m_pointerInteraction = false;
                scheduleRebuild();
            });
    }

    static QString stateText(OperationManager::State state)
    {
        switch (state) {
        case OperationManager::State::Running:
            return trLocal("W toku", "Running");
        case OperationManager::State::Paused:
            return trLocal("Wstrzymano", "Paused");
        case OperationManager::State::Cancelling:
            return trLocal("Anulowanie…", "Cancelling…");
        case OperationManager::State::Completed:
            return trLocal("Zakończono", "Completed");
        case OperationManager::State::Cancelled:
            return trLocal("Anulowano", "Cancelled");
        case OperationManager::State::Failed:
            return trLocal("Błąd", "Failed");
        }
        return {};
    }

    static QString transferTaskStateText(TransferTaskState state)
    {
        switch (state) {
        case TransferTaskState::Queued: return trLocal("W kolejce", "Queued");
        case TransferTaskState::Running: return trLocal("W toku", "Running");
        case TransferTaskState::Paused: return trLocal("Wstrzymano", "Paused");
        case TransferTaskState::Cancelling: return trLocal("Anulowanie…", "Cancelling…");
        case TransferTaskState::Completed: return trLocal("Zakończono", "Completed");
        case TransferTaskState::Cancelled: return trLocal("Anulowano", "Cancelled");
        case TransferTaskState::Failed: return trLocal("Błąd", "Failed");
        }
        return {};
    }

    static QString transferText(quint64 processedBytes, quint64 totalBytes)
    {
        if (totalBytes > 0) {
            return QStringLiteral("%1: %2 / %3")
                .arg(trLocal("Przesłano", "Transferred"),
                     formatFileSize(static_cast<qint64>(processedBytes), false),
                     formatFileSize(static_cast<qint64>(totalBytes), false));
        } else if (processedBytes > 0) {
            return QStringLiteral("%1: %2 / —")
                .arg(trLocal("Przesłano", "Transferred"),
                     formatFileSize(static_cast<qint64>(processedBytes), false));
        }
        return {};
    }

    static QString speedText(qulonglong bytesPerSecond)
    {
        if (bytesPerSecond == 0) {
            return QStringLiteral("—");
        }
        return formatFileSize(
            static_cast<qint64>(qMin<qulonglong>(
                bytesPerSecond,
                static_cast<qulonglong>(std::numeric_limits<qint64>::max()))),
            false) + QStringLiteral("/s");
    }

    static QString etaText(qint64 seconds)
    {
        if (seconds < 0) {
            return QStringLiteral("—");
        }
        if (seconds < 60) {
            return isPolish()
                ? QStringLiteral("%1 s").arg(seconds)
                : QStringLiteral("%1 sec").arg(seconds);
        }
        if (seconds < 3600) {
            return isPolish()
                ? QStringLiteral("%1 min %2 s")
                    .arg(seconds / 60)
                    .arg(seconds % 60)
                : QStringLiteral("%1 min %2 sec")
                    .arg(seconds / 60)
                    .arg(seconds % 60);
        }
        return isPolish()
            ? QStringLiteral("%1 godz. %2 min")
                .arg(seconds / 3600)
                .arg((seconds % 3600) / 60)
            : QStringLiteral("%1 hr %2 min")
                .arg(seconds / 3600)
                .arg((seconds % 3600) / 60);
    }

    static QString durationText(qint64 ms)
    {
        if (ms < 0) {
            return QStringLiteral("—");
        }
        const qint64 totalSec = ms / 1000;
        if (totalSec < 1) {
            return QStringLiteral("< 1 s");
        }
        return etaText(totalSec);
    }

    void addArchivalHistoryCard(const TransferHistoryRecord &rec)
    {
        auto *row = new QFrame(m_listWidget);
        row->setObjectName(QStringLiteral("operationRow"));
        row->setProperty("historyUuid", rec.uuid);
        row->setProperty("isArchival", true);
        row->setFrameShape(QFrame::StyledPanel);
        row->setAutoFillBackground(true);

        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 10, 10, 10);
        rowLayout->setSpacing(10);

        auto *icon = new QLabel(row);
        const QString iconName = (rec.terminalState == TransferTaskState::Failed)
            ? QStringLiteral("dialog-error")
            : (rec.terminalState == TransferTaskState::Cancelled)
                ? QStringLiteral("process-stop")
                : (rec.operationType == TransferOperationType::Move)
                    ? QStringLiteral("edit-cut")
                    : QStringLiteral("emblem-default");
        icon->setPixmap(themedIcon(iconName, QStringLiteral("folder-sync")).pixmap(28, 28));
        rowLayout->addWidget(icon, 0, Qt::AlignTop);

        auto *content = new QWidget(row);
        content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        contentLayout->setSpacing(5);

        auto *topLine = new QHBoxLayout;
        QString titleText = rec.title;
        if (titleText.isEmpty()) {
            if (!rec.sourceUrls.isEmpty()) {
                const QString fileName = rec.sourceUrls.first().fileName();
                titleText = (rec.operationType == TransferOperationType::Move)
                    ? (isPolish() ? QStringLiteral("Przenoszenie: %1").arg(fileName)
                                  : QStringLiteral("Moving: %1").arg(fileName))
                    : (isPolish() ? QStringLiteral("Kopiowanie: %1").arg(fileName)
                                  : QStringLiteral("Copying: %1").arg(fileName));
            } else {
                titleText = (rec.operationType == TransferOperationType::Move)
                    ? trLocal("Przenoszenie plików", "Moving files")
                    : trLocal("Kopiowanie plików", "Copying files");
            }
        }
        auto *title = new TransferQueueElidedLabel(titleText, content, Qt::ElideRight);
        title->setObjectName(QStringLiteral("operationTitleLabel"));
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        title->setToolTip(formatSingleValueTooltip(trLocal("Zadanie", "Task"), titleText));
        topLine->addWidget(title, 1);

        const QString badgeStyle = QStringLiteral(
            "QLabel { background-color: rgba(128, 128, 128, 0.18); "
            "border-radius: 4px; padding: 1px 6px; }");

        auto *historyBadge = new QLabel(trLocal("Historia", "History"), content);
        historyBadge->setObjectName(QStringLiteral("operationHistoryBadge"));
        historyBadge->setStyleSheet(badgeStyle);
        topLine->addWidget(historyBadge, 0);

        auto *stateLabel = new QLabel(transferTaskStateText(rec.terminalState), content);
        stateLabel->setObjectName(QStringLiteral("operationDetailsLabel"));
        stateLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        topLine->addWidget(stateLabel, 0);

        auto *detailsToggle = new QToolButton(content);
        detailsToggle->setObjectName(QStringLiteral("operationDetailsToggleButton"));
        detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        detailsToggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        topLine->addWidget(detailsToggle, 0);

        auto *dismiss = new QToolButton(content);
        dismiss->setObjectName(QStringLiteral("detailedOperationDismissButton"));
        dismiss->setIcon(themedIcon(QStringLiteral("window-close"), QStringLiteral("edit-delete")));
        dismiss->setToolTip(trLocal("Ukryj z widoku", "Dismiss from view"));
        dismiss->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        protectInteraction(dismiss);
        connect(dismiss, &QToolButton::clicked, this, [this, uuid = rec.uuid] {
            m_dismissedHistoryUuids.insert(uuid);
            scheduleRebuild();
        });
        topLine->addWidget(dismiss, 0);
        contentLayout->addLayout(topLine);

        auto *detailsWidget = new QWidget(content);
        detailsWidget->setObjectName(QStringLiteral("operationExpandedDetails"));
        auto *detailsLayout = new QVBoxLayout(detailsWidget);
        detailsLayout->setContentsMargins(0, 2, 0, 2);
        detailsLayout->setSpacing(5);

        const bool expanded = m_expandedHistoryUuids.contains(rec.uuid);
        auto updateToggle = [detailsToggle](bool isExpanded) {
            detailsToggle->setIcon(themedIcon(
                isExpanded ? QStringLiteral("go-up") : QStringLiteral("go-down")));
            detailsToggle->setText(
                isExpanded ? trLocal("Mniej", "Less") : trLocal("Szczegóły", "Details"));
        };
        updateToggle(expanded);
        detailsWidget->setVisible(expanded);
        protectInteraction(detailsToggle);
        connect(detailsToggle, &QToolButton::clicked, this,
                [this, uuid = rec.uuid, detailsWidget, updateToggle] {
                    const bool expand = !m_expandedHistoryUuids.contains(uuid);
                    if (expand) m_expandedHistoryUuids.insert(uuid);
                    else m_expandedHistoryUuids.remove(uuid);
                    detailsWidget->setVisible(expand);
                    updateToggle(expand);
                });

        auto addLocation = [detailsWidget, detailsLayout](
                               const QString &objectName,
                               const QString &caption,
                               const QString &value) {
            if (value.isEmpty()) return;
            auto *label = new TransferQueueElidedLabel(detailsWidget, Qt::ElideMiddle);
            label->setObjectName(objectName);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setFullText(QStringLiteral("%1: %2").arg(caption, value));
            label->setToolTip(formatSingleValueTooltip(caption, value));
            detailsLayout->addWidget(label);
        };

        const QString fullSources = TransferQueueTaskItemWidget::formatFullSourcesText(rec.sourceUrls);
        const QString dstText = TransferQueueTaskItemWidget::displayUrl(rec.destinationUrl);
        addLocation(QStringLiteral("operationSourceLabel"),
                    trLocal("Źródło", "Source"),
                    fullSources);
        addLocation(QStringLiteral("operationDestinationLabel"),
                    trLocal("Cel", "Target"),
                    dstText);

        if (rec.endTimeUtc.isValid()) {
            addLocation(QStringLiteral("operationEndTimeLabel"),
                        trLocal("Zakończono", "Finished"),
                        rec.endTimeUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
        }
        if (rec.durationMs > 0) {
            addLocation(QStringLiteral("operationDurationLabel"),
                        trLocal("Czas trwania", "Duration"),
                        durationText(rec.durationMs));
        }

        const QString bytesStr = transferText(rec.processedBytes, rec.totalBytes);
        if (!bytesStr.isEmpty()) {
            auto *bytesLabel = new QLabel(bytesStr, content);
            bytesLabel->setObjectName(QStringLiteral("operationBytesLabel"));
            bytesLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            contentLayout->addWidget(bytesLabel);
        }

        if (rec.averageSpeedBytesPerSecond.has_value() && *rec.averageSpeedBytesPerSecond > 0) {
            addLocation(QStringLiteral("operationAverageSpeedLabel"),
                        trLocal("Średnia prędkość", "Average speed"),
                        speedText(*rec.averageSpeedBytesPerSecond));
        }

        if (rec.terminalState == TransferTaskState::Failed && !rec.errorText.isEmpty()) {
            auto *errLabel = new OperationDetailsLabel(rec.errorText, content);
            errLabel->setObjectName(QStringLiteral("operationErrorLabel"));
            detailsLayout->addWidget(errLabel);
        }

        auto *progressLayout = new QHBoxLayout;
        progressLayout->setContentsMargins(0, 0, 0, 0);
        progressLayout->setSpacing(10);
        auto *progress = new QProgressBar(content);
        progress->setObjectName(QStringLiteral("operationProgress"));
        progress->setRange(0, 100);
        const int pct = (rec.terminalState == TransferTaskState::Completed)
            ? 100
            : ((rec.totalBytes > 0)
                ? static_cast<int>(qBound<quint64>(0, (rec.processedBytes * 100) / rec.totalBytes, 100))
                : 0);
        progress->setValue(pct);
        progress->setTextVisible(false);
        progress->setMinimumHeight(14);
        progressLayout->addWidget(progress, 1);
        auto *percentLabel = new QLabel(QStringLiteral("%1%").arg(pct), content);
        percentLabel->setObjectName(QStringLiteral("operationPercentLabel"));
        QFont percentFont = percentLabel->font();
        percentFont.setBold(true);
        percentFont.setPointSize(percentFont.pointSize() + 4);
        percentLabel->setFont(percentFont);
        percentLabel->setMinimumWidth(58);
        percentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        progressLayout->addWidget(percentLabel, 0);
        contentLayout->addLayout(progressLayout);

        contentLayout->addWidget(detailsWidget);
        rowLayout->addWidget(content, 1);
        m_listLayout->addWidget(row);
    }

    static QString displayUrl(const QUrl &url)
    {
        return url.isLocalFile()
            ? QDir::toNativeSeparators(url.toLocalFile())
            : url.toDisplayString();
    }

    void addQueueTaskCard(const TransferTaskSnapshot &task)
    {
        auto *row = new QFrame(m_listWidget);
        row->setObjectName(QStringLiteral("operationRow"));
        row->setProperty("operationId", QVariant::fromValue(task.id));
        row->setProperty("taskId", QVariant::fromValue(task.id));

        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 10, 10, 10);
        rowLayout->setSpacing(10);

        auto *icon = new QLabel(row);
        const QString iconName = (task.type == TransferOperationType::Move)
            ? QStringLiteral("edit-cut")
            : (task.state == TransferTaskState::Failed)
                ? QStringLiteral("dialog-error")
                : (task.state == TransferTaskState::Cancelled)
                    ? QStringLiteral("process-stop")
                    : (task.state == TransferTaskState::Completed)
                        ? QStringLiteral("emblem-default")
                        : QStringLiteral("edit-copy");
        icon->setPixmap(themedIcon(iconName, QStringLiteral("folder-sync")).pixmap(28, 28));
        rowLayout->addWidget(icon, 0, Qt::AlignTop);

        auto *content = new QWidget(row);
        content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        contentLayout->setSpacing(5);

        auto *topLine = new QHBoxLayout;
        auto *title = new TransferQueueElidedLabel(task.title, content, Qt::ElideRight);
        title->setObjectName(QStringLiteral("operationTitleLabel"));
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        title->setToolTip(formatSingleValueTooltip(trLocal("Zadanie", "Task"), task.title));
        topLine->addWidget(title, 1);

        const QString badgeStyle = QStringLiteral(
            "QLabel { background-color: rgba(128, 128, 128, 0.18); "
            "border-radius: 4px; padding: 1px 6px; }");

        const bool isQueued = (task.state == TransferTaskState::Queued);
        if (isQueued) {
            auto *posBadge = new QLabel(content);
            posBadge->setObjectName(QStringLiteral("transferQueueTaskQueuePosBadge"));
            posBadge->setStyleSheet(badgeStyle);
            if (task.queuePosition > 0) {
                posBadge->setText(isPolish() ? QStringLiteral("#%1 w kolejce").arg(task.queuePosition)
                                             : QStringLiteral("#%1 in queue").arg(task.queuePosition));
            }
            topLine->addWidget(posBadge, 0);

            auto *moveUp = new QToolButton(content);
            moveUp->setObjectName(QStringLiteral("transferQueueTaskMoveUpButton"));
            moveUp->setIcon(themedIcon(QStringLiteral("go-up"), QStringLiteral("arrow-up")));
            moveUp->setToolTip(trLocal("Przesuń wyżej w kolejce", "Move up in queue"));
            moveUp->setAutoRaise(true);
            moveUp->setFixedSize(22, 22);
            moveUp->setEnabled(m_queueController ? m_queueController->canMoveTaskUp(task.id) : false);
            connect(moveUp, &QToolButton::clicked, this, [this, id = task.id] {
                if (m_queueController) {
                    m_queueController->moveTaskUp(id);
                    scheduleRebuild();
                }
            });
            topLine->addWidget(moveUp, 0);

            auto *moveDown = new QToolButton(content);
            moveDown->setObjectName(QStringLiteral("transferQueueTaskMoveDownButton"));
            moveDown->setIcon(themedIcon(QStringLiteral("go-down"), QStringLiteral("arrow-down")));
            moveDown->setToolTip(trLocal("Przesuń niżej w kolejce", "Move down in queue"));
            moveDown->setAutoRaise(true);
            moveDown->setFixedSize(22, 22);
            moveDown->setEnabled(m_queueController ? m_queueController->canMoveTaskDown(task.id) : false);
            connect(moveDown, &QToolButton::clicked, this, [this, id = task.id] {
                if (m_queueController) {
                    m_queueController->moveTaskDown(id);
                    scheduleRebuild();
                }
            });
            topLine->addWidget(moveDown, 0);

            auto *prioCombo = new QComboBox(content);
            prioCombo->setObjectName(QStringLiteral("transferQueueTaskPriorityCombo"));
            prioCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            prioCombo->setFixedWidth(96);
            prioCombo->addItem(TransferQueueTaskItemWidget::priorityText(TransferPriority::High), static_cast<int>(TransferPriority::High));
            prioCombo->addItem(TransferQueueTaskItemWidget::priorityText(TransferPriority::Normal), static_cast<int>(TransferPriority::Normal));
            prioCombo->addItem(TransferQueueTaskItemWidget::priorityText(TransferPriority::Low), static_cast<int>(TransferPriority::Low));
            const int comboIdx = prioCombo->findData(static_cast<int>(task.priority));
            if (comboIdx >= 0) prioCombo->setCurrentIndex(comboIdx);
            connect(prioCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, id = task.id, prioCombo](int idx) {
                if (m_queueController && idx >= 0) {
                    m_queueController->setTaskPriority(id, static_cast<TransferPriority>(prioCombo->itemData(idx).toInt()));
                    scheduleRebuild();
                }
            });
            topLine->addWidget(prioCombo, 0);
        } else if (!task.terminal) {
            auto *prioBadge = new QLabel(TransferQueueTaskItemWidget::priorityText(task.priority), content);
            prioBadge->setObjectName(QStringLiteral("transferQueueTaskPriorityBadge"));
            prioBadge->setStyleSheet(badgeStyle);
            topLine->addWidget(prioBadge, 0);

            if (task.supportsThrottling && m_queueController && m_queueController->isBandwidthLimitEnabled()) {
                auto *throtBadge = new QLabel(trLocal("Limitowany", "Throttled"), content);
                throtBadge->setObjectName(QStringLiteral("transferQueueTaskThrottlingBadge"));
                throtBadge->setStyleSheet(badgeStyle);
                topLine->addWidget(throtBadge, 0);
            }
        }

        auto *stateLabel = new QLabel(transferTaskStateText(task.state), content);
        stateLabel->setObjectName(QStringLiteral("operationDetailsLabel"));
        stateLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        topLine->addWidget(stateLabel, 0);

        auto *detailsToggle = new QToolButton(content);
        detailsToggle->setObjectName(QStringLiteral("operationDetailsToggleButton"));
        detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        detailsToggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        topLine->addWidget(detailsToggle, 0);

        auto *pause = new QToolButton(content);
        pause->setObjectName(QStringLiteral("detailedOperationPauseButton"));
        const bool isPaused = (task.state == TransferTaskState::Paused);
        pause->setIcon(themedIcon(
            isPaused ? QStringLiteral("media-playback-start")
                     : QStringLiteral("media-playback-pause")));
        pause->setToolTip(
            isPaused ? trLocal("Wznów operację", "Resume operation")
                     : trLocal("Wstrzymaj operację", "Pause operation"));
        pause->setEnabled(task.canPause || isPaused);
        pause->setVisible(task.canPause || isPaused);
        pause->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        protectInteraction(pause);
        connect(pause, &QToolButton::clicked, this, [this, id = task.id, isPaused] {
            if (m_queueController) {
                if (isPaused) m_queueController->resumeTask(id);
                else m_queueController->pauseTask(id);
            }
        });
        topLine->addWidget(pause, 0);

        auto *cancel = new QToolButton(content);
        cancel->setObjectName(QStringLiteral("detailedOperationCancelButton"));
        cancel->setIcon(themedIcon(QStringLiteral("process-stop")));
        cancel->setToolTip(trLocal("Anuluj to zadanie", "Cancel this task"));
        cancel->setEnabled(task.canCancel && !task.terminal);
        cancel->setVisible(!task.terminal);
        cancel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        protectInteraction(cancel);
        connect(cancel, &QToolButton::clicked, this, [this, id = task.id] {
            if (m_queueController) m_queueController->cancelTask(id);
        });
        topLine->addWidget(cancel, 0);

        auto *remove = new QToolButton(content);
        remove->setObjectName(QStringLiteral("detailedOperationRemoveButton"));
        remove->setIcon(themedIcon(QStringLiteral("edit-delete"), QStringLiteral("list-remove")));
        remove->setToolTip(trLocal("Usuń z historii", "Remove from history"));
        remove->setEnabled(task.terminal);
        remove->setVisible(task.terminal);
        remove->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        protectInteraction(remove);
        connect(remove, &QToolButton::clicked, this, [this, id = task.id, histUuid = task.historyUuid] {
            if (m_queueController) {
                if (!histUuid.isEmpty()) {
                    m_dismissedHistoryUuids.insert(histUuid);
                }
                m_queueController->removeFinishedTask(id);
                scheduleRebuild();
            }
        });
        topLine->addWidget(remove, 0);
        contentLayout->addLayout(topLine);

        auto *detailsWidget = new QWidget(content);
        detailsWidget->setObjectName(QStringLiteral("operationExpandedDetails"));
        auto *detailsLayout = new QVBoxLayout(detailsWidget);
        detailsLayout->setContentsMargins(0, 2, 0, 2);
        detailsLayout->setSpacing(5);

        const bool expanded = m_expandedIds.contains(task.id);
        auto updateToggle = [detailsToggle](bool isExpanded) {
            detailsToggle->setIcon(themedIcon(
                isExpanded ? QStringLiteral("go-up") : QStringLiteral("go-down")));
            detailsToggle->setText(
                isExpanded ? trLocal("Mniej", "Less") : trLocal("Szczegóły", "Details"));
        };
        updateToggle(expanded);
        detailsWidget->setVisible(expanded);
        protectInteraction(detailsToggle);
        connect(detailsToggle, &QToolButton::clicked, this,
                [this, id = task.id, detailsWidget, updateToggle] {
                    const bool expand = !m_expandedIds.contains(id);
                    if (expand) m_expandedIds.insert(id);
                    else m_expandedIds.remove(id);
                    detailsWidget->setVisible(expand);
                    updateToggle(expand);
                });

        auto addLocation = [detailsWidget, detailsLayout](
                               const QString &objectName,
                               const QString &caption,
                               const QString &value) {
            if (value.isEmpty()) return;
            auto *label = new TransferQueueElidedLabel(detailsWidget, Qt::ElideMiddle);
            label->setObjectName(objectName);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setFullText(QStringLiteral("%1: %2").arg(caption, value));
            label->setToolTip(formatSingleValueTooltip(caption, value));
            detailsLayout->addWidget(label);
        };

        if (isQueued && task.waitReason != TransferWaitReason::None) {
            auto *waitLabel = new TransferQueueElidedLabel(detailsWidget, Qt::ElideRight);
            waitLabel->setObjectName(QStringLiteral("transferQueueTaskWaitReasonLabel"));
            QFont waitFont = waitLabel->font();
            waitFont.setItalic(true);
            waitLabel->setFont(waitFont);
            waitLabel->setFullText(TransferQueueTaskItemWidget::waitReasonText(task.waitReason));
            waitLabel->setToolTip(TransferQueueTaskItemWidget::waitReasonTooltip(task.waitReason, task.waitBlockerId));
            detailsLayout->addWidget(waitLabel);
        }

        const QString fullSources = TransferQueueTaskItemWidget::formatFullSourcesText(task.sources);
        const QString dstText = TransferQueueTaskItemWidget::displayUrl(task.destination);
        addLocation(QStringLiteral("operationSourceLabel"),
                    trLocal("Źródło", "Source"),
                    fullSources);
        addLocation(QStringLiteral("operationDestinationLabel"),
                    trLocal("Cel", "Target"),
                    dstText);

        const bool showActive = (task.state == TransferTaskState::Running
                                 || task.state == TransferTaskState::Paused
                                 || task.state == TransferTaskState::Cancelling);
        const bool isCompleted = (task.state == TransferTaskState::Completed);

        if (showActive || isCompleted) {
            if (showActive) {
                auto *timingLayout = new QHBoxLayout;
                timingLayout->setContentsMargins(0, 0, 0, 0);
                timingLayout->setSpacing(14);
                auto addTiming = [content, timingLayout](const QString &objName, const QString &caption, const QString &val) {
                    auto *lbl = new QLabel(QStringLiteral("%1: %2").arg(caption, val), content);
                    lbl->setObjectName(objName);
                    timingLayout->addWidget(lbl);
                };
                addTiming(QStringLiteral("operationSpeedLabel"),
                          trLocal("Prędkość", "Speed"),
                          speedText(task.speedBytesPerSecond));
                addTiming(QStringLiteral("operationAverageSpeedLabel"),
                          trLocal("Średnia", "Average"),
                          speedText(task.averageSpeedBytesPerSecond));
                addTiming(QStringLiteral("operationEtaLabel"),
                          QStringLiteral("ETA"),
                          etaText(task.etaSeconds));
                timingLayout->addStretch(1);
                contentLayout->addLayout(timingLayout);
            }

            const quint64 finalProcessed = (isCompleted && task.totalBytes > 0)
                ? task.totalBytes
                : task.processedBytes;
            const QString bytesStr = transferText(finalProcessed, task.totalBytes);
            auto *bytesLabel = new QLabel(
                !bytesStr.isEmpty()
                    ? bytesStr
                    : (showActive ? trLocal("Oczekiwanie na informacje o postępie…", "Waiting for progress information…") : QString()),
                content);
            bytesLabel->setObjectName(QStringLiteral("operationBytesLabel"));
            bytesLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            contentLayout->addWidget(bytesLabel);

            auto *graph = new OperationSpeedGraph(task.speedSamples, detailsWidget);
            if (task.terminal) {
                graph->setTerminal(true, task.state);
            }
            detailsLayout->addWidget(graph);

            auto *progressLayout = new QHBoxLayout;
            progressLayout->setContentsMargins(0, 0, 0, 0);
            progressLayout->setSpacing(10);
            auto *progress = new QProgressBar(content);
            progress->setObjectName(QStringLiteral("operationProgress"));
            progress->setRange(0, 100);
            progress->setValue(isCompleted ? 100 : task.percent);
            progress->setTextVisible(false);
            progress->setMinimumHeight(14);
            progressLayout->addWidget(progress, 1);
            auto *percentLabel = new QLabel(QStringLiteral("%1%").arg(isCompleted ? 100 : task.percent), content);
            percentLabel->setObjectName(QStringLiteral("operationPercentLabel"));
            QFont percentFont = percentLabel->font();
            percentFont.setBold(true);
            percentFont.setPointSize(percentFont.pointSize() + 4);
            percentLabel->setFont(percentFont);
            percentLabel->setMinimumWidth(58);
            percentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            progressLayout->addWidget(percentLabel, 0);
            contentLayout->addLayout(progressLayout);
        } else if (task.state == TransferTaskState::Failed && !task.errorText.isEmpty()) {
            auto *errLabel = new OperationDetailsLabel(task.errorText, content);
            errLabel->setObjectName(QStringLiteral("operationErrorLabel"));
            detailsLayout->addWidget(errLabel);
        }

        contentLayout->addWidget(detailsWidget);
        rowLayout->addWidget(content, 1);
        m_listLayout->addWidget(row);
    }

    void addLegacyOperationCard(const OperationManager::OperationSnapshot &operation)
    {
        auto *row = new QFrame(m_listWidget);
        row->setObjectName(QStringLiteral("operationRow"));
        row->setProperty("operationId", QVariant::fromValue(operation.id));
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 10, 10, 10);
        rowLayout->setSpacing(10);

        auto *icon = new QLabel(row);
        QString iconName = QStringLiteral("folder-sync");
        if (operation.state == OperationManager::State::Completed) {
            iconName = QStringLiteral("task-complete");
        } else if (operation.state == OperationManager::State::Cancelled) {
            iconName = QStringLiteral("process-stop");
        } else if (operation.state == OperationManager::State::Failed) {
            iconName = QStringLiteral("dialog-error");
        }
        icon->setPixmap(
            themedIcon(
                iconName,
                QStringLiteral("system-run"))
                .pixmap(28, 28));
        rowLayout->addWidget(icon, 0, Qt::AlignTop);

        auto *content = new QWidget(row);
        content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        contentLayout->setSpacing(5);

        auto *topLine = new QHBoxLayout;
        auto *title = new TransferQueueElidedLabel(
            operation.title, content, Qt::ElideRight);
        title->setObjectName(QStringLiteral("operationTitleLabel"));
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        title->setToolTip(formatSingleValueTooltip(trLocal("Operacja", "Operation"), operation.title));
        topLine->addWidget(title, 1);
        auto *state = new QLabel(stateText(operation.state), content);
        state->setObjectName(QStringLiteral("operationDetailsLabel"));
        state->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        topLine->addWidget(state, 0);
        auto *detailsToggle = new QToolButton(content);
        detailsToggle->setObjectName(
            QStringLiteral("operationDetailsToggleButton"));
        detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        detailsToggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        topLine->addWidget(detailsToggle, 0);

        auto *pause = new QToolButton(content);
        pause->setObjectName(QStringLiteral("detailedOperationPauseButton"));
        pause->setIcon(themedIcon(
            operation.paused
                ? QStringLiteral("media-playback-start")
                : QStringLiteral("media-playback-pause")));
        pause->setToolTip(
            operation.paused
                ? trLocal("Wznów operację", "Resume operation")
                : trLocal("Wstrzymaj operację", "Pause operation"));
        pause->setEnabled(operation.canPause);
        pause->setVisible(operation.canPause);
        pause->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        protectInteraction(pause);
        connect(
            pause,
            &QToolButton::clicked,
            this,
            [manager = m_manager, id = operation.id] {
                if (manager) manager->togglePause(id);
            });
        topLine->addWidget(pause, 0);

        const bool legacyFinished = (operation.state == OperationManager::State::Completed
            || operation.state == OperationManager::State::Cancelled
            || operation.state == OperationManager::State::Failed);

        auto *cancel = new QToolButton(content);
        cancel->setObjectName(
            QStringLiteral("detailedOperationCancelButton"));
        cancel->setIcon(themedIcon(QStringLiteral("process-stop")));
        cancel->setToolTip(
            operation.canCancel
                ? trLocal("Anuluj operację", "Cancel operation")
                : trLocal(
                    "Ta operacja nie może być teraz anulowana",
                    "This operation cannot be cancelled now"));
        cancel->setEnabled(operation.canCancel && !legacyFinished);
        cancel->setVisible(!legacyFinished);
        cancel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        protectInteraction(cancel);
        connect(
            cancel,
            &QToolButton::clicked,
            this,
            [manager = m_manager, id = operation.id] {
                if (manager) manager->cancelOperation(id);
            });
        topLine->addWidget(cancel, 0);

        auto *remove = new QToolButton(content);
        remove->setObjectName(QStringLiteral("detailedOperationRemoveButton"));
        remove->setIcon(themedIcon(QStringLiteral("edit-delete"), QStringLiteral("list-remove")));
        remove->setToolTip(trLocal("Usuń z historii", "Remove from history"));
        remove->setEnabled(legacyFinished);
        remove->setVisible(legacyFinished);
        remove->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        protectInteraction(remove);
        connect(remove, &QToolButton::clicked, this, [this, id = operation.id] {
            if (m_manager) {
                m_manager->removeFinishedOperation(id);
                scheduleRebuild();
            }
        });
        topLine->addWidget(remove, 0);
        contentLayout->addLayout(topLine);

        auto *detailsWidget = new QWidget(content);
        detailsWidget->setObjectName(
            QStringLiteral("operationExpandedDetails"));
        auto *detailsLayout = new QVBoxLayout(detailsWidget);
        detailsLayout->setContentsMargins(0, 2, 0, 2);
        detailsLayout->setSpacing(5);

        const quint64 opKey = 0x8000000000000000ULL | operation.id;
        const bool expanded = m_expandedIds.contains(opKey);
        auto updateToggle = [detailsToggle](bool isExpanded) {
            detailsToggle->setIcon(themedIcon(
                isExpanded
                    ? QStringLiteral("go-up")
                    : QStringLiteral("go-down")));
            detailsToggle->setText(
                isExpanded
                    ? trLocal("Mniej", "Less")
                    : trLocal("Szczegóły", "Details"));
        };
        updateToggle(expanded);
        detailsWidget->setVisible(expanded);
        protectInteraction(detailsToggle);
        connect(
            detailsToggle,
            &QToolButton::clicked,
            this,
            [this,
             opKey,
             detailsWidget,
             updateToggle] {
                const bool expand = !m_expandedIds.contains(opKey);
                if (expand) {
                    m_expandedIds.insert(opKey);
                } else {
                    m_expandedIds.remove(opKey);
                }
                detailsWidget->setVisible(expand);
                updateToggle(expand);
            });

        auto addLocation = [detailsWidget, detailsLayout](
                               const QString &objectName,
                               const QString &caption,
                               const QString &value) {
            if (value.isEmpty()) {
                return;
            }
            auto *label = new TransferQueueElidedLabel(
                detailsWidget,
                Qt::ElideMiddle);
            label->setObjectName(objectName);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setFullText(QStringLiteral("%1: %2").arg(caption, value));
            label->setToolTip(formatSingleValueTooltip(caption, value));
            detailsLayout->addWidget(label);
        };

        if (operation.currentSourceUrl.isValid()) {
            QString currentName = operation.currentSourceUrl.fileName();
            if (currentName.isEmpty()) {
                currentName = displayUrl(operation.currentSourceUrl);
            }
            addLocation(
                QStringLiteral("operationCurrentFileLabel"),
                trLocal("Bieżący plik", "Current file"),
                currentName);
        }

        const bool showActive = (operation.state == OperationManager::State::Running
                                 || operation.state == OperationManager::State::Paused
                                 || operation.state == OperationManager::State::Cancelling);
        const bool isCompleted = (operation.state == OperationManager::State::Completed);

        if (showActive) {
            auto *timingLayout = new QHBoxLayout;
            timingLayout->setContentsMargins(0, 0, 0, 0);
            timingLayout->setSpacing(14);
            auto addTiming = [content, timingLayout](
                                 const QString &objectName,
                                 const QString &caption,
                                 const QString &value) {
                auto *label = new QLabel(
                    QStringLiteral("%1: %2").arg(caption, value),
                    content);
                label->setObjectName(objectName);
                timingLayout->addWidget(label);
            };
            addTiming(
                QStringLiteral("operationSpeedLabel"),
                trLocal("Prędkość", "Speed"),
                speedText(operation.speedBytesPerSecond));
            addTiming(
                QStringLiteral("operationAverageSpeedLabel"),
                trLocal("Średnia", "Average"),
                speedText(operation.averageSpeedBytesPerSecond));
            addTiming(
                QStringLiteral("operationEtaLabel"),
                QStringLiteral("ETA"),
                etaText(operation.etaSeconds));
            timingLayout->addStretch(1);
            contentLayout->addLayout(timingLayout);
        }

        const quint64 finalProcessed = (isCompleted && operation.totalBytes > 0)
            ? operation.totalBytes
            : operation.processedBytes;
        const QString transfer = transferText(finalProcessed, operation.totalBytes);
        if (!transfer.isEmpty() || showActive) {
            auto *bytesLabel = new QLabel(
                !transfer.isEmpty()
                    ? transfer
                    : trLocal("Oczekiwanie na informacje o postępie…", "Waiting for progress information…"),
                content);
            bytesLabel->setObjectName(QStringLiteral("operationBytesLabel"));
            bytesLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            contentLayout->addWidget(bytesLabel);
        }

        QString details = operation.contextText;
        if (details.isEmpty() && showActive) {
            details = trLocal(
                "Oczekiwanie na informacje o postępie…",
                "Waiting for progress information…");
        }
        if (!details.isEmpty()) {
            auto *detailsLabel = new OperationDetailsLabel(details, content);
            detailsLayout->addWidget(detailsLabel);
        }

        if (showActive || !operation.speedSamples.isEmpty()) {
            auto *graph = new OperationSpeedGraph(
                operation.speedSamples,
                detailsWidget);
            const bool isLegacyTerminal = (operation.state == OperationManager::State::Completed
                                           || operation.state == OperationManager::State::Cancelled
                                           || operation.state == OperationManager::State::Failed);
            if (isLegacyTerminal) {
                TransferTaskState tState = TransferTaskState::Completed;
                if (operation.state == OperationManager::State::Cancelled) tState = TransferTaskState::Cancelled;
                else if (operation.state == OperationManager::State::Failed) tState = TransferTaskState::Failed;
                graph->setTerminal(true, tState);
            }
            detailsLayout->addWidget(graph);
        }
        contentLayout->addWidget(detailsWidget);

        if (showActive || isCompleted) {
            auto *progressLayout = new QHBoxLayout;
            progressLayout->setContentsMargins(0, 0, 0, 0);
            progressLayout->setSpacing(10);
            auto *progress = new QProgressBar(content);
            progress->setObjectName(QStringLiteral("operationProgress"));
            progress->setRange(0, 100);
            progress->setValue(isCompleted ? 100 : static_cast<int>(operation.percent));
            progress->setTextVisible(false);
            progress->setMinimumHeight(14);
            progressLayout->addWidget(progress, 1);
            auto *percentLabel = new QLabel(
                QStringLiteral("%1%").arg(isCompleted ? 100 : operation.percent),
                content);
            percentLabel->setObjectName(
                QStringLiteral("operationPercentLabel"));
            QFont percentFont = percentLabel->font();
            percentFont.setBold(true);
            percentFont.setPointSize(percentFont.pointSize() + 4);
            percentLabel->setFont(percentFont);
            percentLabel->setMinimumWidth(58);
            percentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            progressLayout->addWidget(percentLabel);
            contentLayout->addLayout(progressLayout);
        } else if (operation.state == OperationManager::State::Failed && !operation.contextText.isEmpty()) {
            auto *errLabel = new OperationDetailsLabel(operation.contextText, content);
            errLabel->setObjectName(QStringLiteral("operationErrorLabel"));
            detailsLayout->addWidget(errLabel);
        }
        rowLayout->addWidget(content, 1);

        m_listLayout->addWidget(row);
    }

    void addOperation(const OperationManager::OperationSnapshot &operation)
    {
        addLegacyOperationCard(operation);
    }

    QPointer<OperationManager> m_manager;
    QPointer<TransferQueueController> m_queueController;
    QVBoxLayout *m_windowLayout = nullptr;
    QTabWidget *m_tabs = nullptr;
    QWidget *m_operationsPage = nullptr;
    QWidget *m_queuePage = nullptr;
    QLabel *m_operationsEmptyLabel = nullptr;
    QLabel *m_queueNoticeLabel = nullptr;
    QToolButton *m_demoOperationsButton = nullptr;
    TransferQueueSettingsWidget *m_queueSettings = nullptr;
    TransferQueueViewWidget *m_queueView = nullptr;
    int m_lastOperationCount = 0;
    TransferQueueElidedLabel *m_summaryLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_listWidget = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QToolButton *m_pauseAllButton = nullptr;
    QToolButton *m_resumeAllButton = nullptr;
    QToolButton *m_cancelAllButton = nullptr;
    QToolButton *m_clearHistoryButton = nullptr;
    QToolButton *m_settingsToggleButton = nullptr;
    QScrollArea *m_settingsContainer = nullptr;
    QSet<quint64> m_knownIds;
    QSet<quint64> m_expandedIds;
    QSet<QString> m_knownHistoryUuids;
    QSet<QString> m_expandedHistoryUuids;
    QSet<QString> m_dismissedHistoryUuids;
    QPointer<TransferHistoryManager> m_explicitHistoryManager;
    bool m_rebuildScheduled = false;
    bool m_pointerInteraction = false;
};


inline void OperationManager::onOperationTracked()
{
    if (!isTransferQueueProductionDispatchConnected()) {
        showDetailedWindow();
        return;
    }
    if (m_detailedWindow && m_detailedWindow->isVisible()) {
        m_detailedWindow->scheduleRebuild();
    } else if (!m_userDismissedThisBatch) {
        if (m_autoShowGracePeriodMs <= 0) {
            showDetailedWindow();
        } else if (!m_autoShowTimer->isActive()) {
            m_autoShowTimer->start(m_autoShowGracePeriodMs);
        }
    }
}

inline void OperationManager::showDetailedWindow()
{
    if (!m_detailedWindow) {
        m_detailedWindow = new OperationWindow(this, m_window);
        m_detailedWindow->setQueueController(m_queueController);
        if (m_notificationManager) {
            m_notificationManager->setOperationWindow(m_detailedWindow);
        }
    }
    const int legacyAct = activeCount();
    const int queueAct = m_queueController
        ? (m_queueController->activeCount() + m_queueController->queuedCount())
        : 0;
    if (legacyAct == 0 && queueAct == 0) {
        if (!m_queueController && !isTransferQueueDemoMode() && (!historyManager() || historyManager()->records().isEmpty())) {
            m_detailedWindow->hide();
            return;
        }
        if (!isTransferQueueProductionDispatchConnected() && m_detailedWindow->hasQueueTab()) {
            m_detailedWindow->showQueueTab();
        }
    } else if (!m_detailedWindow->isVisible()) {
        m_detailedWindow->showOperationsTab();
    }
    if (m_popup) {
        m_popup->hide();
    }
    m_detailedWindow->rebuild();
    if (m_detailedWindow->isMinimized()) {
        m_detailedWindow->showNormal();
    } else {
        m_detailedWindow->show();
    }
    m_detailedWindow->raise();
    m_detailedWindow->activateWindow();
}

inline void OperationManager::setDetailedWindow(OperationWindow *window)
{
    m_detailedWindow = window;
    if (m_notificationManager) {
        m_notificationManager->setOperationWindow(window);
    }
}

inline void OperationManager::setHistoryManager(TransferHistoryManager *manager)
{
    if (m_historyManager == manager) {
        return;
    }
    if (m_historyManager) {
        disconnect(m_historyManager, nullptr, this, nullptr);
    }
    m_historyManager = manager;
    if (m_historyManager) {
        connect(m_historyManager, &TransferHistoryManager::recordsChanged,
                this, [this] { updateSummary(); });
        connect(m_historyManager, &TransferHistoryManager::historyCleared,
                this, [this] {
                    clearFinished();
                    updateSummary();
                });
    }
    if (m_detailedWindow) {
        m_detailedWindow->setHistoryManager(manager);
    }
    updateSummary();
}

inline void OperationManager::setCloseOnComplete(bool close)
{
    if (m_closeOnComplete == close) return;
    m_closeOnComplete = close;
    if (m_detailedWindow) {
        m_detailedWindow->setCloseOnComplete(close);
    }
    if (m_queueController) {
        m_queueController->setCloseWindowOnComplete(close);
    }
    Q_EMIT closeOnCompleteChanged(close);
}

inline void OperationManager::checkAutoCloseOrStopTimer()
{
    const int legacyActive = activeCount();
    const int queueActive = m_queueController
        ? (m_queueController->activeCount() + m_queueController->queuedCount())
        : 0;
    const int totalActive = legacyActive + queueActive;

    if (totalActive == 0) {
        if (m_autoShowTimer && m_autoShowTimer->isActive()) {
            m_autoShowTimer->stop();
        }
        if (m_closeOnComplete && m_detailedWindow && m_detailedWindow->isVisible()) {
            bool hasErrors = false;
            for (const auto *item : m_items) {
                if (item && item->state == State::Failed) {
                    hasErrors = true;
                    break;
                }
            }
            if (!hasErrors && m_queueController) {
                const auto snap = m_queueController->snapshot();
                for (const auto &task : snap.tasks) {
                    if (task.state == TransferTaskState::Failed) {
                        hasErrors = true;
                        break;
                    }
                }
            }
            if (!hasErrors) {
                m_detailedWindow->hide();
            }
        }
    }
}

inline void OperationManager::setQueueController(TransferQueueController *controller)
{
    if (m_queueController == controller) {
        return;
    }
    if (m_queueController) {
        disconnect(m_queueController, nullptr, this, nullptr);
        if (m_queueController->historyManager()) {
            disconnect(m_queueController->historyManager(), nullptr, this, nullptr);
        }
    }
    m_queueController = controller;
    if (m_queueController) {
        setCloseOnComplete(m_queueController->closeWindowOnComplete());
    }
    if (m_detailedWindow) {
        m_detailedWindow->setQueueController(controller);
    }
    if (m_notificationManager) {
        m_notificationManager->setQueueController(controller);
    }
    if (m_queueController) {
        if (m_queueController->historyManager()) {
            disconnect(m_queueController->historyManager(), nullptr, this, nullptr);
            connect(m_queueController->historyManager(), &TransferHistoryManager::recordsChanged,
                    this, [this] { updateSummary(); });
            connect(m_queueController->historyManager(), &TransferHistoryManager::historyCleared,
                    this, [this] {
                        clearFinished();
                        updateSummary();
                    });
        }
        connect(m_queueController, &TransferQueueController::historyManagerChanged,
                this, [this](TransferHistoryManager *hm) {
                    if (hm) {
                        disconnect(hm, nullptr, this, nullptr);
                        connect(hm, &TransferHistoryManager::recordsChanged,
                                this, [this] { updateSummary(); });
                        connect(hm, &TransferHistoryManager::historyCleared,
                                this, [this] {
                                    clearFinished();
                                    updateSummary();
                                });
                    }
                    updateSummary();
                });
        connect(m_queueController, &TransferQueueController::queueChanged,
                this, [this] {
                    updateSummary();
                    checkAutoCloseOrStopTimer();
                });
        connect(m_queueController, &TransferQueueController::taskStateChanged,
                this, [this](quint64, TransferTaskState, TransferTaskState to) {
                    updateSummary();
                    if (to == TransferTaskState::Running) {
                        if (!m_userDismissedThisBatch && (!m_detailedWindow || !m_detailedWindow->isVisible())) {
                            if (!isTransferQueueProductionDispatchConnected()) {
                                showDetailedWindow();
                            } else if (m_autoShowGracePeriodMs <= 0) {
                                showDetailedWindow();
                            } else if (!m_autoShowTimer->isActive()) {
                                m_autoShowTimer->start(m_autoShowGracePeriodMs);
                            }
                        }
                    }
                    checkAutoCloseOrStopTimer();
                });
        connect(m_queueController, &TransferQueueController::taskFinished,
                this, [this](quint64, TransferTaskState) {
                    updateSummary();
                    checkAutoCloseOrStopTimer();
                });
        connect(m_queueController, &TransferQueueController::taskProgressChanged,
                this, [this](quint64, quint64, quint64, int) {
                    updateSummary();
                });
        connect(m_queueController, &TransferQueueController::closeWindowOnCompleteChanged,
                this, &OperationManager::setCloseOnComplete);
    }
    updateSummary();
}
