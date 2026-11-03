/*
 * Native KDE Plasma desktop notification manager for ThisPC transfer operations.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "transfertaskdescriptor.h"
#include "transferqueuecontroller.h"

#include <KNotification>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMainWindow>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <functional>
#include <optional>
#include <utility>

enum class NotificationKind {
    Success,
    Error
};

struct NotificationPayload {
    NotificationKind kind = NotificationKind::Success;
    QString title;
    QString message;
    QString iconName;
    quint64 taskId = 0;
    int taskCount = 1;
};

class TransferNotificationManager final : public QObject
{
    Q_OBJECT

public:
    using DispatchBackend = std::function<bool(const NotificationPayload &)>;

    explicit TransferNotificationManager(
        QMainWindow *mainWindow = nullptr,
        QWidget *operationWindow = nullptr,
        QObject *parent = nullptr)
        : QObject(parent)
        , m_mainWindow(mainWindow)
        , m_operationWindow(operationWindow)
        , m_coalesceTimer(new QTimer(this))
    {
        m_coalesceTimer->setSingleShot(true);
        connect(m_coalesceTimer, &QTimer::timeout, this, &TransferNotificationManager::flushPendingCompletions);
    }

    ~TransferNotificationManager() override
    {
        if (m_coalesceTimer) {
            m_coalesceTimer->stop();
        }
        m_pendingCompletions.clear();
    }

    void setMainWindow(QMainWindow *window) { m_mainWindow = window; }
    void setOperationWindow(QWidget *window) { m_operationWindow = window; }

    TransferQueueController *queueController() const { return m_queueController; }
    void setQueueController(TransferQueueController *controller)
    {
        if (m_queueController == controller) {
            return;
        }
        if (m_queueController) {
            disconnect(m_queueController, nullptr, this, nullptr);
        }
        m_queueController = controller;
        if (m_queueController) {
            connect(m_queueController, &TransferQueueController::taskFinished,
                    this, &TransferNotificationManager::onTaskFinished);
        }
    }

    bool isEnabled() const
    {
        if (m_queueController) {
            return m_queueController->desktopNotificationsEnabled();
        }
        return m_enabledFallback;
    }

    void setEnabled(bool enabled)
    {
        if (m_queueController) {
            m_queueController->setDesktopNotificationsEnabled(enabled);
        } else {
            m_enabledFallback = enabled;
        }
    }

    // Custom backend delegate for testing / mock verification.
    void setDispatchBackend(DispatchBackend backend)
    {
        m_dispatchBackend = std::move(backend);
    }

    // Deterministic override for user observation state (testing).
    void setUserObservingOverride(std::optional<bool> overrideState)
    {
        m_userObservingOverride = overrideState;
    }

    // Number of notifications dispatched since creation (or reset).
    int dispatchedCount() const { return m_dispatchedCount; }
    int dispatchedSuccessCount() const { return m_dispatchedSuccessCount; }
    int dispatchedErrorCount() const { return m_dispatchedErrorCount; }
    const QList<NotificationPayload> &history() const { return m_history; }
    void clearHistory()
    {
        m_history.clear();
        m_dispatchedCount = 0;
        m_dispatchedSuccessCount = 0;
        m_dispatchedErrorCount = 0;
    }

    // Idempotency: clear notified tasks set if needed
    void clearNotifiedTaskIds() { m_notifiedTaskIds.clear(); }
    bool hasNotified(quint64 taskId) const { return m_notifiedTaskIds.contains(taskId); }

    // Directly flush pending batch completions immediately (e.g. for testing)
    void flushNow()
    {
        if (m_coalesceTimer && m_coalesceTimer->isActive()) {
            m_coalesceTimer->stop();
        }
        flushPendingCompletions();
    }

    // Static factory to build a KNotification payload conforming to KDE Plasma standards
    static KNotification *createKNotification(const NotificationPayload &payload, QObject *parent = nullptr)
    {
        const QString eventId = (payload.kind == NotificationKind::Error)
            ? QStringLiteral("fatalerror")
            : QStringLiteral("notification");
        auto *notification = new KNotification(
            eventId,
            KNotification::CloseOnTimeout | KNotification::DefaultEvent,
            parent);
        notification->setTitle(payload.title);
        notification->setText(payload.message);
        notification->setIconName(payload.iconName);
        notification->setHint(QStringLiteral("desktop-entry"), QStringLiteral("org.kde.thispcview"));
        if (payload.kind == NotificationKind::Error) {
            notification->setUrgency(KNotification::HighUrgency);
        } else {
            notification->setUrgency(KNotification::NormalUrgency);
        }
        return notification;
    }

    bool isUserObserving() const
    {
        if (m_userObservingOverride.has_value()) {
            return *m_userObservingOverride;
        }

        const bool appActive = (QGuiApplication::applicationState() == Qt::ApplicationActive);
        if (!appActive) {
            THISPC_TRACE("TransferNotificationManager: isUserObserving -> false (application inactive)");
            return false;
        }

        QWidget *activeWin = QApplication::activeWindow();
        if (activeWin && !activeWin->isMinimized()) {
            THISPC_TRACE("TransferNotificationManager: isUserObserving -> true (active window present)");
            return true;
        }

        if (m_mainWindow && m_mainWindow->isActiveWindow() && !m_mainWindow->isMinimized()) {
            THISPC_TRACE("TransferNotificationManager: isUserObserving -> true (mainWindow active and not minimized)");
            return true;
        }
        if (m_operationWindow && m_operationWindow->isVisible()
            && m_operationWindow->isActiveWindow() && !m_operationWindow->isMinimized()) {
            THISPC_TRACE("TransferNotificationManager: isUserObserving -> true (operationWindow active and not minimized)");
            return true;
        }
        THISPC_TRACE("TransferNotificationManager: isUserObserving -> false (windows not active or minimized)");
        return false;
    }

public Q_SLOTS:
    void onTaskFinished(quint64 id, TransferTaskState terminalState)
    {
        THISPC_TRACE("TransferNotificationManager: onTaskFinished id=%llu, terminalState=%d, enabled=%d",
                     static_cast<unsigned long long>(id), static_cast<int>(terminalState), isEnabled() ? 1 : 0);
#ifdef THISPC_TEST_HARNESS
        if (!m_dispatchBackend) {
            return;
        }
#endif

        if (!isEnabled()) {
            return;
        }

        // Idempotency check: terminal notification emitted at most once per task ID.
        if (m_notifiedTaskIds.contains(id)) {
            THISPC_TRACE("TransferNotificationManager: ignoring task id=%llu (already notified)",
                         static_cast<unsigned long long>(id));
            return;
        }
        m_notifiedTaskIds.insert(id);

        // Cancelled tasks do not emit desktop notifications (neither success nor error).
        if (terminalState == TransferTaskState::Cancelled) {
            THISPC_TRACE("TransferNotificationManager: task id=%llu cancelled (no notification)",
                         static_cast<unsigned long long>(id));
            return;
        }

        if (terminalState == TransferTaskState::Failed) {
            handleTaskFailed(id);
            return;
        }

        if (terminalState == TransferTaskState::Completed) {
            handleTaskCompleted(id);
            return;
        }
    }

private:
    struct CompletionRecord {
        quint64 taskId = 0;
        TransferOperationType type = TransferOperationType::Copy;
        QString summaryItemName;
        int itemCount = 1;
    };

    static QString sanitizeItemName(const QUrl &url)
    {
        if (!url.isValid()) {
            return trLocal("element", "item");
        }
        if (url.isLocalFile()) {
            const QString local = url.toLocalFile();
            const QFileInfo fi(local);
            const QString name = fi.fileName();
            if (!name.isEmpty()) {
                return name;
            }
            const QDir dir(local);
            const QString dirName = dir.dirName();
            if (!dirName.isEmpty()) {
                return dirName;
            }
        }
        const QString fileName = url.fileName();
        if (!fileName.isEmpty()) {
            return fileName;
        }
        return url.path().section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty);
    }

    void handleTaskFailed(quint64 id)
    {
        // Errors notify even when the user is observing the window.
        QString opName;
        QString summaryItem;
        QString errorDetail;

        if (m_queueController) {
            const auto snap = m_queueController->taskSnapshot(id);
            if (snap.has_value()) {
                const bool isMove = (snap->type == TransferOperationType::Move);
                opName = isMove ? trLocal("Przenoszenie", "Move")
                                : trLocal("Kopiowanie", "Copy");

                if (!snap->sources.isEmpty()) {
                    summaryItem = sanitizeItemName(snap->sources.first());
                    if (snap->sources.size() > 1) {
                        summaryItem += trLocal(
                            QStringLiteral(" (+%1 więcej)").arg(snap->sources.size() - 1).toUtf8().constData(),
                            QStringLiteral(" (+%1 more)").arg(snap->sources.size() - 1).toUtf8().constData());
                    }
                }
                errorDetail = snap->errorText;
            }
        }

        if (opName.isEmpty()) {
            opName = trLocal("Transfer", "Transfer");
        }
        if (summaryItem.isEmpty()) {
            summaryItem = trLocal("element", "item");
        }
        if (errorDetail.isEmpty()) {
            errorDetail = trLocal("Wystąpił błąd podczas operacji.", "An error occurred during operation.");
        }

        NotificationPayload payload;
        payload.kind = NotificationKind::Error;
        payload.taskId = id;
        payload.title = trLocal("Ten komputer — Błąd transferu", "This PC — Transfer Error");
        payload.message = QStringLiteral("%1 (%2): %3").arg(opName, summaryItem, errorDetail);
        payload.iconName = QStringLiteral("dialog-error");

        dispatch(payload);
    }

    void handleTaskCompleted(quint64 id)
    {
        const bool observing = isUserObserving();
        THISPC_TRACE("TransferNotificationManager: handleTaskCompleted id=%llu, isUserObserving=%d",
                     static_cast<unsigned long long>(id), observing ? 1 : 0);
        // Success notifications are suppressed when the user is actively observing ThisPC.
        if (observing) {
            return;
        }

        CompletionRecord record;
        record.taskId = id;

        if (m_queueController) {
            const auto snap = m_queueController->taskSnapshot(id);
            if (snap.has_value()) {
                record.type = snap->type;
                record.itemCount = qMax(1, static_cast<int>(snap->sources.size()));
                if (!snap->sources.isEmpty()) {
                    record.summaryItemName = sanitizeItemName(snap->sources.first());
                }
            }
        }
        if (record.summaryItemName.isEmpty()) {
            record.summaryItemName = trLocal("element", "item");
        }

        m_pendingCompletions.append(record);

        // Krótkie zadania zbiorcze: debounce/coalesce window (250 ms) zapobiega zalewaniu pulpitu.
        if (!m_coalesceTimer->isActive()) {
            m_coalesceTimer->start(250);
        }
    }

    void flushPendingCompletions()
    {
        if (m_pendingCompletions.isEmpty()) {
            return;
        }

        const bool observing = isUserObserving();
        THISPC_TRACE("TransferNotificationManager: flushPendingCompletions count=%d, isUserObserving=%d",
                     static_cast<int>(m_pendingCompletions.size()), observing ? 1 : 0);

        // Recheck observer state before emission
        if (observing) {
            m_pendingCompletions.clear();
            return;
        }

        NotificationPayload payload;
        payload.kind = NotificationKind::Success;
        payload.iconName = QStringLiteral("document-save");

        if (m_pendingCompletions.size() == 1) {
            const auto &rec = m_pendingCompletions.first();
            payload.taskId = rec.taskId;
            payload.taskCount = 1;
            payload.title = trLocal("Ten komputer", "This PC");
            const bool isMove = (rec.type == TransferOperationType::Move);
            if (rec.itemCount == 1) {
                payload.message = isMove
                    ? trLocal("Przenoszenie ukończone: %1", "Move completed: %1").arg(rec.summaryItemName)
                    : trLocal("Kopiowanie ukończone: %1", "Copy completed: %1").arg(rec.summaryItemName);
            } else {
                payload.message = isMove
                    ? trLocal("Przeniesiono %1 elementów", "Moved %1 items").arg(rec.itemCount)
                    : trLocal("Skopiowano %1 elementów", "Copied %1 items").arg(rec.itemCount);
            }
        } else {
            payload.taskId = 0;
            payload.taskCount = static_cast<int>(m_pendingCompletions.size());
            payload.title = trLocal("Ten komputer", "This PC");
            payload.message = trLocal(
                "Ukończono %1 operacji transferu.",
                "%1 transfer operations completed.").arg(m_pendingCompletions.size());
        }

        m_pendingCompletions.clear();
        dispatch(payload);
    }

    void dispatch(const NotificationPayload &payload)
    {
        m_history.append(payload);
        ++m_dispatchedCount;
        if (payload.kind == NotificationKind::Success) {
            ++m_dispatchedSuccessCount;
        } else {
            ++m_dispatchedErrorCount;
        }

        THISPC_TRACE("TransferNotificationManager: dispatching notification kind=%d, title='%s', msg='%s'",
                     static_cast<int>(payload.kind), qPrintable(payload.title), qPrintable(payload.message));

        if (m_dispatchBackend) {
            try {
                m_dispatchBackend(payload);
            } catch (...) {
            }
            return;
        }

#ifdef THISPC_TEST_HARNESS
        // In headless regression test harnesses, do not attempt to invoke
        // external Plasma desktop D-Bus daemon plugins unless a dispatchBackend is set.
        return;
#else
        // Production KDE Plasma notification
        try {
            auto *notification = createKNotification(payload);
            notification->sendEvent();
            THISPC_TRACE("TransferNotificationManager: KNotification sent event successfully (eventId=%s, title=%s)",
                         qPrintable(notification->eventId()), qPrintable(payload.title));
        } catch (const std::exception &ex) {
            THISPC_TRACE("TransferNotificationManager: exception sending notification: %s", ex.what());
        } catch (...) {
            THISPC_TRACE("TransferNotificationManager: unknown exception sending notification");
        }
#endif
    }

    QPointer<QMainWindow> m_mainWindow;
    QPointer<QWidget> m_operationWindow;
    QPointer<TransferQueueController> m_queueController;
    QTimer *m_coalesceTimer = nullptr;
    QList<CompletionRecord> m_pendingCompletions;
    QSet<quint64> m_notifiedTaskIds;
    QList<NotificationPayload> m_history;
    DispatchBackend m_dispatchBackend;
    std::optional<bool> m_userObservingOverride;
    bool m_enabledFallback = true;
    int m_dispatchedCount = 0;
    int m_dispatchedSuccessCount = 0;
    int m_dispatchedErrorCount = 0;
};
