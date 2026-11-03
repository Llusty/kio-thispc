/*
 * Stage 1 automated regression tests: KDE Plasma Notifications for ThisPC 0.45.0.
 *
 * Verifies:
 * 1. Sukces przy oknie aktywnym (suppressed).
 * 2. Sukces przy oknie nieaktywnym (dispatched).
 * 3. Sukces przy oknie zminimalizowanym (dispatched).
 * 4. Błąd operacji (dispatched even with active window, sanitized path).
 * 5. Anulowanie (no notification emitted).
 * 6. Wyłączenie powiadomień w ustawieniach (disabling setting stops notifications; persistence verified).
 * 7. Brak podwójnych powiadomień (idempotency: single terminal notification per task ID).
 * 8. Wiele szybko kończących się zadań (coalescing rapid completions into 1 batch notification).
 * 9. Zamknięcie aplikacji podczas operacji (safe destruction during active timer / queue).
 * 10. Brak wpływu na działanie kolejki, staging i stan terminalny (queue state transitions unchanged).
 * 11. Niedostępny backend powiadomień (fault tolerance: exception/failure in backend does not affect queue).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "transfernotificationmanager.h"
#include "transferqueuecontroller.h"
#include "transferqueuesettings.h"
#include "transferqueueui.h"
#include "operationmanager.h"

#include <QApplication>
#include <QMainWindow>
#include <QTest>
#include <QTemporaryDir>
#include <QElapsedTimer>

static int s_notifChecks = 0;
static void verifyNotif(bool condition, const char *description)
{
    if (!condition) {
        qFatal("FAIL: [notifications] %s", description);
    }
    ++s_notifChecks;
}

class NotifFakeJob final : public KJob
{
public:
    explicit NotifFakeJob()
    {
        setAutoDelete(true);
        setCapabilities(KJob::Killable | KJob::Suspendable);
    }

    void start() override { ++startCalls; }
    void finishOk() { emitResult(); }
    void finishError(int code, const QString &text)
    {
        setError(code);
        setErrorText(text);
        emitResult();
    }

    bool doKill() override
    {
        setError(KJob::KilledJobError);
        emitResult();
        return true;
    }

    int startCalls = 0;
};

static TransferTaskDescriptor makeNotifTask(
    const QString &title,
    TransferOperationType type,
    const QList<QUrl> &sources,
    const QUrl &dest,
    NotifFakeJob **outJob = nullptr)
{
    TransferTaskDescriptor desc;
    desc.title = title;
    desc.type = type;
    desc.backend = TransferBackendKind::LocalNative;
    desc.sources = sources;
    desc.destination = dest;
    desc.capabilities.canCancelRunning = true;
    desc.capabilities.canPause = true;
    desc.capabilities.canResume = true;
    desc.capabilities.supportsThrottling = false;
    desc.startMode = TransferJobStartMode::ControllerStarts;
    desc.jobFactory = [outJob]() -> KJob * {
        auto *j = new NotifFakeJob();
        if (outJob) *outJob = j;
        return j;
    };
    return desc;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    s_notifChecks = 0;
    qInfo("STARTING ThisPC 0.45.0 Stage 1: KDE Plasma Notifications test suite...");

    QTemporaryDir tempDir;
    verifyNotif(tempDir.isValid(), "Temporary directory initialized for test suite");
    const QString sandbox = tempDir.path();

    // -------------------------------------------------------------------------
    // Test 1: Sukces przy oknie aktywnym (Suppression)
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        TransferNotificationManager notifManager;
        notifManager.setQueueController(&controller);

        QList<NotificationPayload> captured;
        notifManager.setDispatchBackend([&captured](const NotificationPayload &p) {
            captured.append(p);
            return true;
        });

        // User is observing ThisPC (e.g. active window)
        notifManager.setUserObservingOverride(true);
        verifyNotif(notifManager.isUserObserving(), "T1: User is confirmed observing");

        NotifFakeJob *job = nullptr;
        const auto desc = makeNotifTask(
            QStringLiteral("Task 1"),
            TransferOperationType::Copy,
            {QUrl::fromLocalFile(sandbox + QStringLiteral("/file1.txt"))},
            QUrl::fromLocalFile(sandbox + QStringLiteral("/dest")),
            &job);

        const auto res = controller.enqueue(desc);
        verifyNotif(res.accepted(), "T1: Task enqueued successfully");
        verifyNotif(job != nullptr, "T1: Fake job instantiated");

        job->finishOk();
        QTest::qWait(50);
        notifManager.flushNow();

        verifyNotif(captured.isEmpty(), "T1: Success notification suppressed while user is actively observing ThisPC");
        verifyNotif(notifManager.dispatchedSuccessCount() == 0, "T1: Dispatched success count is 0");
    }

    // -------------------------------------------------------------------------
    // Test 2: Sukces przy oknie nieaktywnym (Dispatch)
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        TransferNotificationManager notifManager;
        notifManager.setQueueController(&controller);

        QList<NotificationPayload> captured;
        notifManager.setDispatchBackend([&captured](const NotificationPayload &p) {
            captured.append(p);
            return true;
        });

        // Window is inactive (user in another application)
        notifManager.setUserObservingOverride(false);
        verifyNotif(!notifManager.isUserObserving(), "T2: User is confirmed NOT observing (inactive window)");

        NotifFakeJob *job = nullptr;
        const auto desc = makeNotifTask(
            QStringLiteral("Task 2"),
            TransferOperationType::Copy,
            {QUrl::fromLocalFile(sandbox + QStringLiteral("/document.pdf"))},
            QUrl::fromLocalFile(sandbox + QStringLiteral("/backup")),
            &job);

        const auto res = controller.enqueue(desc);
        verifyNotif(res.accepted(), "T2: Task enqueued");
        verifyNotif(job != nullptr, "T2: Job running");

        job->finishOk();
        QTest::qWait(50);
        notifManager.flushNow();

        verifyNotif(captured.size() == 1, "T2: Exactly 1 notification dispatched when window is inactive");
        verifyNotif(captured.first().kind == NotificationKind::Success, "T2: Notification is Success kind");
        verifyNotif(captured.first().message.contains(QStringLiteral("document.pdf")),
                    "T2: Notification message contains item name without full path");
        verifyNotif(!captured.first().message.contains(sandbox),
                    "T2: Sensitive parent path is NOT exposed in notification");
        verifyNotif(notifManager.dispatchedSuccessCount() == 1, "T2: Dispatched success count is 1");
    }

    // -------------------------------------------------------------------------
    // Test 3: Sukces przy oknie zminimalizowanym (Dispatch)
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        TransferNotificationManager notifManager;
        notifManager.setQueueController(&controller);

        QList<NotificationPayload> captured;
        notifManager.setDispatchBackend([&captured](const NotificationPayload &p) {
            captured.append(p);
            return true;
        });

        // Simulate minimized window: mainWindow is minimized
        QMainWindow testWindow;
        testWindow.setWindowState(Qt::WindowMinimized);
        notifManager.setMainWindow(&testWindow);
        notifManager.setUserObservingOverride(std::nullopt); // Use live window inspection

        // When window is minimized, isUserObserving() must be false
        verifyNotif(!notifManager.isUserObserving(), "T3: Minimized window is correctly detected as NOT observing");

        NotifFakeJob *job = nullptr;
        const auto desc = makeNotifTask(
            QStringLiteral("Task 3"),
            TransferOperationType::Move,
            {QUrl::fromLocalFile(sandbox + QStringLiteral("/photo.jpg"))},
            QUrl::fromLocalFile(sandbox + QStringLiteral("/dest")),
            &job);

        const auto res = controller.enqueue(desc);
        verifyNotif(res.accepted(), "T3: Task enqueued");
        job->finishOk();
        QTest::qWait(50);
        notifManager.flushNow();

        verifyNotif(captured.size() == 1, "T3: Notification dispatched for minimized window");
        verifyNotif(captured.first().kind == NotificationKind::Success, "T3: Notification is Success kind");
        verifyNotif(captured.first().message.contains(QStringLiteral("photo.jpg")), "T3: Message contains item name");
    }

    // -------------------------------------------------------------------------
    // Test 4: Błąd operacji (Error dispatched even with active window, sanitized path)
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        TransferNotificationManager notifManager;
        notifManager.setQueueController(&controller);

        QList<NotificationPayload> captured;
        notifManager.setDispatchBackend([&captured](const NotificationPayload &p) {
            captured.append(p);
            return true;
        });

        // Even when user is actively observing, ERRORS must notify!
        notifManager.setUserObservingOverride(true);
        verifyNotif(notifManager.isUserObserving(), "T4: User is observing window");

        NotifFakeJob *job = nullptr;
        const auto desc = makeNotifTask(
            QStringLiteral("Task 4"),
            TransferOperationType::Copy,
            {QUrl::fromLocalFile(sandbox + QStringLiteral("/important.dat"))},
            QUrl::fromLocalFile(sandbox + QStringLiteral("/protected")),
            &job);

        const auto res = controller.enqueue(desc);
        verifyNotif(res.accepted(), "T4: Task enqueued");
        job->finishError(KJob::KilledJobError + 1, QStringLiteral("Brak uprawnień do zapisu"));
        QTest::qWait(50);

        verifyNotif(captured.size() == 1, "T4: Error notification dispatched even when window is active");
        verifyNotif(captured.first().kind == NotificationKind::Error, "T4: Notification is Error kind");
        verifyNotif(captured.first().iconName == QStringLiteral("dialog-error"), "T4: Error icon used");
        verifyNotif(captured.first().message.contains(QStringLiteral("important.dat")),
                    "T4: Error message includes file name");
        verifyNotif(!captured.first().message.contains(sandbox),
                    "T4: Full directory path is sanitized away");
        verifyNotif(captured.first().message.contains(QStringLiteral("Brak uprawnień do zapisu")),
                    "T4: Error diagnostics are included in notification");
        verifyNotif(notifManager.dispatchedErrorCount() == 1, "T4: Dispatched error count is 1");
    }

    // -------------------------------------------------------------------------
    // Test 5: Anulowanie (No desktop notification)
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        TransferNotificationManager notifManager;
        notifManager.setQueueController(&controller);

        QList<NotificationPayload> captured;
        notifManager.setDispatchBackend([&captured](const NotificationPayload &p) {
            captured.append(p);
            return true;
        });

        notifManager.setUserObservingOverride(false);

        NotifFakeJob *job = nullptr;
        const auto desc = makeNotifTask(
            QStringLiteral("Task 5"),
            TransferOperationType::Copy,
            {QUrl::fromLocalFile(sandbox + QStringLiteral("/cancel_me.bin"))},
            QUrl::fromLocalFile(sandbox + QStringLiteral("/dest")),
            &job);

        const auto res = controller.enqueue(desc);
        verifyNotif(res.accepted(), "T5: Task enqueued");

        // Cancel task
        const bool cancelOk = controller.cancelTask(res.id);
        verifyNotif(cancelOk, "T5: Cancel request accepted");
        QTest::qWait(50);
        notifManager.flushNow();

        verifyNotif(captured.isEmpty(), "T5: Cancelled task produces NO desktop notification");
        verifyNotif(notifManager.dispatchedCount() == 0, "T5: Dispatched count remains 0");
    }

    // -------------------------------------------------------------------------
    // Test 6: Wyłączenie powiadomień w ustawieniach (Persistence and suppression)
    // -------------------------------------------------------------------------
    {
        const QString settingsPath = sandbox + QStringLiteral("/notif_settings.ini");
        QFile::remove(settingsPath);

        // Verify default setting is true
        {
            QSettings s(settingsPath, QSettings::IniFormat);
            const TransferQueueConfig cfg = TransferQueueSettings::load(s);
            verifyNotif(cfg.desktopNotificationsEnabled, "T6: Default desktopNotificationsEnabled is true");
        }

        // Save disabled setting
        {
            QSettings s(settingsPath, QSettings::IniFormat);
            TransferQueueConfig cfg;
            cfg.desktopNotificationsEnabled = false;
            TransferQueueSettings::save(s, cfg);
            s.sync();
        }

        // Verify loaded disabled setting
        {
            QSettings s(settingsPath, QSettings::IniFormat);
            const TransferQueueConfig cfg = TransferQueueSettings::load(s);
            verifyNotif(!cfg.desktopNotificationsEnabled, "T6: desktopNotificationsEnabled persisted as false");

            TransferQueueController controller;
            controller.loadSettings(s);
            verifyNotif(!controller.desktopNotificationsEnabled(), "T6: Controller loaded disabled setting");

            TransferNotificationManager notifManager;
            notifManager.setQueueController(&controller);
            verifyNotif(!notifManager.isEnabled(), "T6: Notification manager is disabled via controller setting");

            QList<NotificationPayload> captured;
            notifManager.setDispatchBackend([&captured](const NotificationPayload &p) {
                captured.append(p);
                return true;
            });
            notifManager.setUserObservingOverride(false);

            NotifFakeJob *job = nullptr;
            const auto desc = makeNotifTask(
                QStringLiteral("Task 6"),
                TransferOperationType::Copy,
                {QUrl::fromLocalFile(sandbox + QStringLiteral("/f6.txt"))},
                QUrl::fromLocalFile(sandbox + QStringLiteral("/dest")),
                &job);

            const auto res = controller.enqueue(desc);
            job->finishOk();
            QTest::qWait(50);
            notifManager.flushNow();

            verifyNotif(captured.isEmpty(), "T6: When disabled, NO notifications are dispatched");
        }
    }

    // -------------------------------------------------------------------------
    // Test 7: Brak podwójnych powiadomień (Idempotency)
    // -------------------------------------------------------------------------
    {
        TransferNotificationManager notifManager;
        QList<NotificationPayload> captured;
        notifManager.setDispatchBackend([&captured](const NotificationPayload &p) {
            captured.append(p);
            return true;
        });

        notifManager.setUserObservingOverride(false);

        // Fire onTaskFinished for the same task ID multiple times
        const quint64 taskId = 999;
        notifManager.onTaskFinished(taskId, TransferTaskState::Completed);
        notifManager.onTaskFinished(taskId, TransferTaskState::Completed);
        notifManager.onTaskFinished(taskId, TransferTaskState::Completed);
        notifManager.flushNow();

        verifyNotif(captured.size() == 1, "T7: Exactly 1 notification dispatched despite 3 terminal calls (idempotent)");
        verifyNotif(notifManager.hasNotified(taskId), "T7: Task is recorded as notified");

        // Subsequent call after flush
        notifManager.onTaskFinished(taskId, TransferTaskState::Completed);
        notifManager.flushNow();
        verifyNotif(captured.size() == 1, "T7: Re-emission for already notified task ID is blocked");
    }

    // -------------------------------------------------------------------------
    // Test 8: Wiele szybko kończących się zadań (Coalescing / Flood prevention)
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        controller.setExecutionMode(TransferQueueMode::Parallel);
        controller.setParallelConcurrencyLimit(5);

        TransferNotificationManager notifManager;
        notifManager.setQueueController(&controller);

        QList<NotificationPayload> captured;
        notifManager.setDispatchBackend([&captured](const NotificationPayload &p) {
            captured.append(p);
            return true;
        });

        notifManager.setUserObservingOverride(false);

        QList<NotifFakeJob *> jobs;
        for (int i = 0; i < 5; ++i) {
            NotifFakeJob *job = nullptr;
            const auto desc = makeNotifTask(
                QStringLiteral("Batch Task %1").arg(i),
                TransferOperationType::Copy,
                {QUrl::fromLocalFile(sandbox + QStringLiteral("/batch_%1.txt").arg(i))},
                QUrl::fromLocalFile(sandbox + QStringLiteral("/dest")),
                &job);
            controller.enqueue(desc);
            jobs.append(job);
        }

        // All 5 finish in rapid succession
        for (auto *j : jobs) {
            if (j) j->finishOk();
        }

        // Wait for coalesce timer (250 ms) to fire automatically
        QElapsedTimer timer;
        timer.start();
        while (captured.isEmpty() && timer.elapsed() < 1000) {
            QTest::qWait(20);
            QCoreApplication::processEvents();
        }

        verifyNotif(captured.size() == 1, "T8: Rapid 5 tasks coalesced into exactly 1 summary notification");
        verifyNotif(captured.first().taskCount == 5, "T8: Notification taskCount reflects all 5 tasks");
        verifyNotif(captured.first().message.contains(QStringLiteral("5")),
                    "T8: Summary message mentions 5 completed tasks");
    }

    // -------------------------------------------------------------------------
    // Test 9: Zamknięcie aplikacji podczas operacji (Destruction safety)
    // -------------------------------------------------------------------------
    {
        auto controller = std::make_unique<TransferQueueController>();
        auto notifManager = std::make_unique<TransferNotificationManager>();
        notifManager->setQueueController(controller.get());
        notifManager->setUserObservingOverride(false);

        NotifFakeJob *job = nullptr;
        const auto desc = makeNotifTask(
            QStringLiteral("Shutdown Task"),
            TransferOperationType::Copy,
            {QUrl::fromLocalFile(sandbox + QStringLiteral("/shutdown.txt"))},
            QUrl::fromLocalFile(sandbox + QStringLiteral("/dest")),
            &job);

        controller->enqueue(desc);

        // Delete notifManager while task is active / timer may be pending
        notifManager.reset();
        verifyNotif(notifManager == nullptr, "T9: Notification manager cleanly destroyed");

        // Controller can finish or cancel without dangling pointers
        if (job) job->finishOk();
        controller.reset();
        verifyNotif(controller == nullptr, "T9: Queue controller cleanly destroyed without crash");
    }

    // -------------------------------------------------------------------------
    // Test 10: Brak wpływu na działanie kolejki, staging i stan terminalny
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        TransferNotificationManager notifManager;
        notifManager.setQueueController(&controller);

        NotifFakeJob *job = nullptr;
        const auto desc = makeNotifTask(
            QStringLiteral("Task 10"),
            TransferOperationType::Copy,
            {QUrl::fromLocalFile(sandbox + QStringLiteral("/clean.txt"))},
            QUrl::fromLocalFile(sandbox + QStringLiteral("/dest")),
            &job);

        const auto res = controller.enqueue(desc);
        verifyNotif(res.accepted(), "T10: Queue accepts task normally");
        verifyNotif(controller.activeCount() == 1, "T10: Task is active");

        job->finishOk();
        QTest::qWait(50);

        const auto snap = controller.taskSnapshot(res.id);
        verifyNotif(snap.has_value(), "T10: Task snapshot available");
        verifyNotif(snap->state == TransferTaskState::Completed, "T10: Terminal state is Completed");
        verifyNotif(controller.activeCount() == 0, "T10: Queue transitions to idle normally");
    }

    // -------------------------------------------------------------------------
    // Test 11: Niedostępny backend powiadomień (Fault tolerance)
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        TransferNotificationManager notifManager;
        notifManager.setQueueController(&controller);

        // Backend throws an exception or returns false
        notifManager.setDispatchBackend([](const NotificationPayload &) -> bool {
            throw std::runtime_error("Notification daemon unreachable or D-Bus crashed");
        });

        notifManager.setUserObservingOverride(false);

        NotifFakeJob *job = nullptr;
        const auto desc = makeNotifTask(
            QStringLiteral("Fault Tolerance Task"),
            TransferOperationType::Copy,
            {QUrl::fromLocalFile(sandbox + QStringLiteral("/fault.txt"))},
            QUrl::fromLocalFile(sandbox + QStringLiteral("/dest")),
            &job);

        const auto res = controller.enqueue(desc);
        verifyNotif(res.accepted(), "T11: Enqueued despite faulty notification backend");

        // Finishing job must NOT crash or fail the transfer
        bool noCrash = false;
        try {
            job->finishOk();
            QTest::qWait(50);
            notifManager.flushNow();
            noCrash = true;
        } catch (...) {
            noCrash = false;
        }

        verifyNotif(noCrash, "T11: Notification failure does NOT crash application or abort transfer");
        const auto snap = controller.taskSnapshot(res.id);
        verifyNotif(snap.has_value() && snap->state == TransferTaskState::Completed,
                    "T11: Transfer task state remains Completed successfully");
    }

    // -------------------------------------------------------------------------
    // Test 12: Produkcyjny obiekt KNotification (DefaultEvent, desktop-entry, urgency)
    // -------------------------------------------------------------------------
    {
        NotificationPayload successPayload;
        successPayload.kind = NotificationKind::Success;
        successPayload.title = QStringLiteral("Ten komputer");
        successPayload.message = QStringLiteral("Kopiowanie ukończone: test.txt");
        successPayload.iconName = QStringLiteral("document-save");

        auto *succNotif = TransferNotificationManager::createKNotification(successPayload);
        verifyNotif(succNotif != nullptr, "T12: Success KNotification instantiated");
        verifyNotif(succNotif->eventId() == QStringLiteral("notification"), "T12: Success eventId is 'notification'");
        verifyNotif((succNotif->flags() & KNotification::DefaultEvent) != 0, "T12: Success flags contain DefaultEvent");
        verifyNotif((succNotif->flags() & KNotification::Persistent) == 0, "T12: Success notification is not persistent (closes on timeout)");
        verifyNotif(succNotif->title() == QStringLiteral("Ten komputer"), "T12: Title matches payload");
        verifyNotif(succNotif->text() == QStringLiteral("Kopiowanie ukończone: test.txt"), "T12: Text matches payload");
        verifyNotif(succNotif->iconName() == QStringLiteral("document-save"), "T12: IconName matches payload");
        verifyNotif(succNotif->urgency() == KNotification::NormalUrgency, "T12: Normal urgency for success");
        verifyNotif(succNotif->hints().value(QStringLiteral("desktop-entry")).toString() == QStringLiteral("org.kde.thispcview"),
                    "T12: Hint 'desktop-entry' is org.kde.thispcview");
        delete succNotif;

        NotificationPayload errPayload;
        errPayload.kind = NotificationKind::Error;
        errPayload.title = QStringLiteral("Ten komputer — Błąd transferu");
        errPayload.message = QStringLiteral("Błąd transferu");
        errPayload.iconName = QStringLiteral("dialog-error");

        auto *errNotif = TransferNotificationManager::createKNotification(errPayload);
        verifyNotif(errNotif != nullptr, "T12: Error KNotification instantiated");
        verifyNotif(errNotif->eventId() == QStringLiteral("fatalerror"), "T12: Error eventId is 'fatalerror'");
        verifyNotif((errNotif->flags() & KNotification::DefaultEvent) != 0, "T12: Error flags contain DefaultEvent");
        verifyNotif(errNotif->urgency() == KNotification::HighUrgency, "T12: High urgency for error");
        verifyNotif(errNotif->hints().value(QStringLiteral("desktop-entry")).toString() == QStringLiteral("org.kde.thispcview"),
                    "T12: Error hint 'desktop-entry' is org.kde.thispcview");
        delete errNotif;
    }

    // -------------------------------------------------------------------------
    // Test 13: Bezpieczeństwo i trwałość sygnału desktopNotificationsEnabledChanged
    // -------------------------------------------------------------------------
    {
        TransferQueueController controller;
        int signalEmittedCount = 0;
        bool lastState = true;
        QObject::connect(&controller, &TransferQueueController::desktopNotificationsEnabledChanged,
                         [&](bool enabled) {
                             ++signalEmittedCount;
                             lastState = enabled;
                         });

        controller.setDesktopNotificationsEnabled(false);
        verifyNotif(signalEmittedCount == 1, "T13: desktopNotificationsEnabledChanged emitted on toggle false");
        verifyNotif(!lastState, "T13: Emitted state is false");
        verifyNotif(!controller.desktopNotificationsEnabled(), "T13: Controller reports false");

        // Idempotency: setting the same value does NOT emit duplicate signal
        controller.setDesktopNotificationsEnabled(false);
        verifyNotif(signalEmittedCount == 1, "T13: Duplicate setDesktopNotificationsEnabled(false) does not emit signal");

        controller.setDesktopNotificationsEnabled(true);
        verifyNotif(signalEmittedCount == 2, "T13: desktopNotificationsEnabledChanged emitted on toggle true");
        verifyNotif(lastState, "T13: Emitted state is true");
        verifyNotif(controller.desktopNotificationsEnabled(), "T13: Controller reports true");
    }

    qInfo("ALL 13 KDE Plasma Notification test scenarios passed (%d assertions)", s_notifChecks);
    return 0;
}
