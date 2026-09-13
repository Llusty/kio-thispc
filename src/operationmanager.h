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
#include <KIO/CopyJob>

#include <QFrame>
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
#include <QSizePolicy>
#include <QStatusBar>
#include <QString>
#include <QStyle>
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
    };

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
            if (item && !item->finished()) {
                ++count;
            }
        }
        return count;
    }

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
                && item->state == State::Running;
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

        item->titleLabel = new QLabel(
            item->title,
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
                if (item) cancelOperation(item->id);
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
                KJob *,
                const QString &,
                const QPair<QString, QString> &field1,
                const QPair<QString, QString> &field2) {
                if (!item || item->finished()) {
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
                Q_EMIT operationsChanged();
            });

        updateDetails(item);
        updateSummary();
        Q_EMIT operationsChanged();
        showDetailedWindow();
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

    void cancelAll()
    {
        const QList<FileOperationItem *> items = m_items;
        for (FileOperationItem *item : items) {
            cancelItem(item, false);
        }
    }

    void clearFinished()
    {
        for (int index = m_items.size() - 1; index >= 0; --index) {
            FileOperationItem *item = m_items.at(index);
            if (!item || !item->finished()) {
                continue;
            }

            m_items.removeAt(index);
            if (item->row) {
                delete item->row;
            }
            delete item;
        }

        updateSummary();
        Q_EMIT operationsChanged();
    }

Q_SIGNALS:
    void operationsChanged();

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
    OperationWindow *m_detailedWindow = nullptr;
    QList<FileOperationItem *> m_items;
    QHash<KJob *, FileOperationItem *> m_byJob;
    quint64 m_nextId = 1;
};


class OperationSpeedGraph final : public QWidget
{
    Q_OBJECT

public:
    explicit OperationSpeedGraph(
        QList<qulonglong> samples,
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

        painter.setPen(QPen(palette().color(QPalette::Midlight), 1));
        for (int division = 1; division < 4; ++division) {
            const qreal y = graphRect.top()
                + graphRect.height() * division / 4.0;
            painter.drawLine(
                QPointF(graphRect.left(), y),
                QPointF(graphRect.right(), y));
        }

        if (m_samples.size() < 2) {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(
                graphRect,
                Qt::AlignCenter,
                trLocal(
                    "Oczekiwanie na próbki prędkości…",
                    "Waiting for speed samples…"));
            return;
        }

        qulonglong maximum = 1;
        for (qulonglong sample : m_samples) {
            maximum = qMax(maximum, sample);
        }

        QPolygonF line;
        line.reserve(m_samples.size());
        for (int index = 0; index < m_samples.size(); ++index) {
            const qreal x = graphRect.left()
                + graphRect.width() * index / (m_samples.size() - 1.0);
            const qreal ratio = static_cast<qreal>(m_samples.at(index))
                / static_cast<qreal>(maximum);
            const qreal y = graphRect.bottom()
                - ratio * (graphRect.height() - 4);
            line.push_back(QPointF(x, y));
        }

        QPolygonF fill = line;
        fill.push_back(QPointF(graphRect.right(), graphRect.bottom()));
        fill.push_back(QPointF(graphRect.left(), graphRect.bottom()));
        QColor fillColor = palette().color(QPalette::Highlight);
        fillColor.setAlpha(55);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fillColor);
        painter.drawPolygon(fill);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawPolyline(line);

        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(
            graphRect.adjusted(6, 4, -6, -4),
            Qt::AlignTop | Qt::AlignRight,
            formatFileSize(
                static_cast<qint64>(qMin<qulonglong>(
                    maximum,
                    static_cast<qulonglong>(
                        std::numeric_limits<qint64>::max()))),
                false) + QStringLiteral("/s"));
    }

private:
    QList<qulonglong> m_samples;
};


class OperationWindow final : public QWidget
{
public:
    explicit OperationWindow(
        OperationManager *manager,
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
        setMinimumSize(460, 210);
        resize(580, 320);

        auto *windowLayout = new QVBoxLayout(this);
        windowLayout->setContentsMargins(14, 14, 14, 14);
        windowLayout->setSpacing(10);

        auto *headerLayout = new QHBoxLayout;
        auto *title = new QLabel(
            trLocal("Operacje plikowe", "File operations"),
            this);
        title->setObjectName(QStringLiteral("operationPopupTitle"));
        headerLayout->addWidget(title);
        headerLayout->addStretch(1);
        m_summaryLabel = new QLabel(this);
        m_summaryLabel->setObjectName(
            QStringLiteral("operationSummaryLabel"));
        headerLayout->addWidget(m_summaryLabel);
        windowLayout->addLayout(headerLayout);

        m_scrollArea = new QScrollArea(this);
        m_scrollArea->setWidgetResizable(true);
        m_scrollArea->setFrameShape(QFrame::NoFrame);
        m_scrollArea->setHorizontalScrollBarPolicy(
            Qt::ScrollBarAlwaysOff);
        m_listWidget = new QWidget(m_scrollArea);
        m_listLayout = new QVBoxLayout(m_listWidget);
        m_listLayout->setContentsMargins(0, 0, 0, 0);
        m_listLayout->setSpacing(8);
        m_scrollArea->setWidget(m_listWidget);
        windowLayout->addWidget(m_scrollArea, 1);

        if (m_manager) {
            QObject::connect(
                m_manager,
                &OperationManager::operationsChanged,
                this,
                [this] {
                    if (isVisible()) scheduleRebuild();
                });
        }
        rebuild();
    }

    void rebuild()
    {
        while (QLayoutItem *child = m_listLayout->takeAt(0)) {
            delete child->widget();
            delete child;
        }

        const QList<OperationManager::OperationSnapshot> allOperations =
            m_manager
                ? m_manager->operations()
                : QList<OperationManager::OperationSnapshot>{};
        QList<OperationManager::OperationSnapshot> operations;
        for (const auto &operation : allOperations) {
            if (operation.state == OperationManager::State::Running
                || operation.state == OperationManager::State::Cancelling) {
                operations.push_back(operation);
            }
        }

        QSet<quint64> currentIds;
        for (const auto &operation : operations) {
            currentIds.insert(operation.id);
            if (!m_knownIds.contains(operation.id)) {
                m_expandedIds.insert(operation.id);
            }
        }
        m_knownIds = currentIds;
        m_expandedIds.intersect(currentIds);

        for (const auto &operation : operations) {
            addOperation(operation);
        }

        if (operations.isEmpty()) {
            hide();
            return;
        } else {
            m_listLayout->addStretch(1);
        }

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
        const int screenLimit = screen()
            ? qMax(260, screen()->availableGeometry().height() * 3 / 4)
            : 620;
        const int viewportHeight = qBound(
            150,
            contentHeight + 4,
            qMax(150, screenLimit - 70));
        m_listWidget->setMinimumHeight(contentHeight);
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

private:
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

    void protectInteraction(QToolButton *button)
    {
        QObject::connect(
            button,
            &QToolButton::pressed,
            this,
            [this] { m_pointerInteraction = true; });
        QObject::connect(
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

    static QString transferText(
        const OperationManager::OperationSnapshot &operation)
    {
        QString transfer;
        if (operation.totalBytes > 0) {
            transfer = QStringLiteral("%1 / %2")
                .arg(
                    formatFileSize(
                        static_cast<qint64>(operation.processedBytes),
                        false),
                    formatFileSize(
                        static_cast<qint64>(operation.totalBytes),
                        false));
        } else if (operation.processedBytes > 0) {
            transfer = formatFileSize(
                static_cast<qint64>(operation.processedBytes),
                false);
        }
        return transfer;
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
            return trLocal("Obliczanie…", "Calculating…");
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

    static QString displayUrl(const QUrl &url)
    {
        return url.isLocalFile()
            ? QDir::toNativeSeparators(url.toLocalFile())
            : url.toDisplayString();
    }

    static QString sourceText(
        const QList<QUrl> &sources)
    {
        if (sources.isEmpty()) {
            return {};
        }
        const QString first = displayUrl(sources.first());
        if (sources.size() == 1) {
            return first;
        }
        return isPolish()
            ? QStringLiteral("%1  (+%2 kolejnych)")
                .arg(first)
                .arg(sources.size() - 1)
            : QStringLiteral("%1  (+%2 more)")
                .arg(first)
                .arg(sources.size() - 1);
    }

    void addOperation(
        const OperationManager::OperationSnapshot &operation)
    {
        auto *row = new QFrame(m_listWidget);
        row->setObjectName(QStringLiteral("operationRow"));
        row->setProperty("operationId", QVariant::fromValue(operation.id));
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 10, 10, 10);
        rowLayout->setSpacing(10);

        auto *icon = new QLabel(row);
        icon->setPixmap(
            themedIcon(
                QStringLiteral("folder-sync"),
                QStringLiteral("system-run"))
                .pixmap(28, 28));
        rowLayout->addWidget(icon, 0, Qt::AlignTop);

        auto *content = new QWidget(row);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        contentLayout->setSpacing(5);

        auto *topLine = new QHBoxLayout;
        auto *title = new QLabel(operation.title, content);
        title->setObjectName(QStringLiteral("operationTitleLabel"));
        topLine->addWidget(title, 1);
        auto *state = new QLabel(stateText(operation.state), content);
        state->setObjectName(QStringLiteral("operationDetailsLabel"));
        topLine->addWidget(state);
        auto *detailsToggle = new QToolButton(content);
        detailsToggle->setObjectName(
            QStringLiteral("operationDetailsToggleButton"));
        detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        topLine->addWidget(detailsToggle);

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
        cancel->setEnabled(operation.canCancel);
        protectInteraction(cancel);
        QObject::connect(
            cancel,
            &QToolButton::clicked,
            this,
            [manager = m_manager, id = operation.id] {
                if (manager) manager->cancelOperation(id);
            });
        topLine->addWidget(cancel);
        contentLayout->addLayout(topLine);

        auto *detailsWidget = new QWidget(content);
        detailsWidget->setObjectName(
            QStringLiteral("operationExpandedDetails"));
        auto *detailsLayout = new QVBoxLayout(detailsWidget);
        detailsLayout->setContentsMargins(0, 2, 0, 2);
        detailsLayout->setSpacing(5);

        const bool expanded = m_expandedIds.contains(operation.id);
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
        QObject::connect(
            detailsToggle,
            &QToolButton::clicked,
            this,
            [this,
             id = operation.id,
             detailsWidget,
             updateToggle] {
                const bool expand = !m_expandedIds.contains(id);
                if (expand) {
                    m_expandedIds.insert(id);
                } else {
                    m_expandedIds.remove(id);
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
            auto *label = new QLabel(
                QStringLiteral("%1: %2").arg(caption, value),
                detailsWidget);
            label->setObjectName(objectName);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setToolTip(value);
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

        QString details = operation.contextText;
        const QString transfer = transferText(operation);
        if (!transfer.isEmpty()) {
            details = details.isEmpty()
                ? transfer
                : details + QStringLiteral("  •  ") + transfer;
        }
        if (details.isEmpty()
            && (operation.state == OperationManager::State::Running
                || operation.state == OperationManager::State::Cancelling)) {
            details = trLocal(
                "Oczekiwanie na informacje o postępie…",
                "Waiting for progress information…");
        }
        auto *detailsLabel = new QLabel(details, content);
        detailsLabel->setObjectName(
            QStringLiteral("operationDetailsLabel"));
        detailsLabel->setWordWrap(true);
        detailsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailsLayout->addWidget(detailsLabel);

        auto *graph = new OperationSpeedGraph(
            operation.speedSamples,
            detailsWidget);
        detailsLayout->addWidget(graph);
        contentLayout->addWidget(detailsWidget);

        auto *progressLayout = new QHBoxLayout;
        progressLayout->setContentsMargins(0, 0, 0, 0);
        progressLayout->setSpacing(10);
        auto *progress = new QProgressBar(content);
        progress->setObjectName(QStringLiteral("operationProgress"));
        progress->setRange(0, 100);
        progress->setValue(static_cast<int>(operation.percent));
        progress->setTextVisible(false);
        progress->setMinimumHeight(14);
        progressLayout->addWidget(progress, 1);
        auto *percentLabel = new QLabel(
            QStringLiteral("%1%").arg(operation.percent),
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
        rowLayout->addWidget(content, 1);

        m_listLayout->addWidget(row);
    }

    OperationManager *m_manager = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_listWidget = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QSet<quint64> m_knownIds;
    QSet<quint64> m_expandedIds;
    bool m_rebuildScheduled = false;
    bool m_pointerInteraction = false;
};


inline void OperationManager::showDetailedWindow()
{
    if (!m_detailedWindow) {
        m_detailedWindow = new OperationWindow(this, m_window);
    }
    if (activeCount() == 0) {
        m_detailedWindow->hide();
        return;
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
