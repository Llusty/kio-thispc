/*
 * Stage 1A + 1A-R1 regression suite: Lifecycle Hygiene, Safe Shutdown & Thread Ownership.
 *
 * Covers:
 * - AUD-A-07 / AUD-1A-03: Explicit ShutdownCoordinator + QEventLoopLocker + watchdog +
 *   direct OperationManager (cancellable & non-killable) + OperationWindow + queued transfers
 * - AUD-A-08 / AUD-1A-05: ChecksumJob thread/worker ownership, rapid restart from stateChanged,
 *   idle cancel reset, thread-safe cancellation (0 leak under LSan)
 * - AUD-A-09 / AUD-A-10 / AUD-1A-01 / AUD-1A-02 / AUD-1A-04: ApplicationWorkerPools mutex-protected
 *   state machine, >2 queued read overflow without QThreadPool::clear(), multi-window isolation,
 *   drain-during-window-open race, xattr UI error reporting, stress test
 * - AUD-A-11 / AUD-1A-06: StorageScanDialog & StorageTreemapCanvas non-blocking heap QMenu,
 *   consecutive menu retirement, model invalidation, external close
 * - AUD-A-13 / AUD-1A-07: closeEvent confirmation reentrancy guard, post-confirmation safety gate
 *   revalidation, re-shown window reset
 * - Real QApplication::exec() process integration tests spawned via QProcess (4 scenarios)
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "applicationlifecycle.h"
#include "checksumjob.h"
#include "duplicatefinderjob.h"
#include "metadataprovider.h"
#include "propertiesxattrs.h"
#include "propertiesxattrwidget.h"
#include "propertiesxattrwriter.h"
#include "storagescandialog.h"
#include "storagetreemapwidget.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace {

int g_checks = 0;

void verify(bool condition, const char *description)
{
    if (!condition) {
        qFatal("FAIL (lifecycle_hygiene): %s", description);
    }
    ++g_checks;
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
        QTest::qWait(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return predicate();
}

struct SyncLatch
{
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    bool released = false;

    void arriveAndWait()
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++arrived;
        cv.notify_all();
        cv.wait(lock, [this]() { return released; });
    }

    bool waitForArrivals(int expected, int timeoutMs = 3000)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this, expected]() { return arrived >= expected; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        cv.notify_all();
    }
};

class ControlledShutdownJob final : public KJob
{
public:
    explicit ControlledShutdownJob(bool killable = true, QObject *parent = nullptr)
        : KJob(parent)
    {
        setCapabilities(killable ? KJob::Killable : KJob::NoCapabilities);
    }

    bool cancelRequested() const { return m_cancelRequested; }
    void start() override {}

    void finishCancelled()
    {
        setError(KJob::KilledJobError);
        emitResult();
    }

    void finishOk()
    {
        emitResult();
    }

protected:
    bool doKill() override
    {
        m_cancelRequested = true;
        return false; // Asynchronous cancellation: finishes later via finishCancelled()
    }

private:
    bool m_cancelRequested = false;
};

QString writeTempFile(const QString &dirPath, const QString &name, const QByteArray &content)
{
    const QString path = dirPath + QLatin1Char('/') + name;
    QFile f(path);
    verify(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "open temp test file");
    verify(f.write(content) == content.size(), "write temp test file");
    f.close();
    return path;
}

// =============================================================================
// Real QApplication::exec() child scenarios spawned by QProcess (§10)
// =============================================================================
int runRealEventLoopChild(const QByteArray &scenario, QApplication &app)
{
    auto *pools = ApplicationWorkerPools::forApplication();
    verify(pools != nullptr, "child: ApplicationWorkerPools available");

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "child: tempDir valid");
    const QString sampleFile = writeTempFile(tempDir.path(), QStringLiteral("sample.txt"), "event-loop-test");

    if (scenario == "last-window-non-kjob") {
        // Real QApplication::exec() with NO KJob — only an accepted xattr write keeping
        // the event loop alive via ShutdownCoordinator's QEventLoopLocker.
        ThisPcWindow win(QUrl::fromLocalFile(tempDir.path()), false);
        win.show();
        auto *coord = win.shutdownCoordinatorForTesting();
        verify(coord != nullptr, "child1: ShutdownCoordinator present");

        auto latch = std::make_shared<SyncLatch>();
        std::atomic<bool> writeCompleted{false};
        bool aboutToQuitFired = false;
        QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
            aboutToQuitFired = true;
        });

        auto writeFuture = pools->runXattrWrite([latch, &writeCompleted]() -> int {
            latch->arriveAndWait();
            writeCompleted.store(true, std::memory_order_release);
            return 77;
        });

        QTimer::singleShot(20, &win, [&]() {
            verify(latch->waitForArrivals(1, 3000), "child1: write worker entered");
            win.close();
            verify(coord->isDraining(), "child1: coordinator draining");
            verify(coord->isEventLoopLocked(), "child1: QEventLoopLocker active");
            verify(win.isVisible(), "child1: window remains visible while draining");
            // Prove event loop stays alive for 60ms while draining, then release write
            QTimer::singleShot(60, &app, [latch, &writeCompleted, coord]() {
                verify(!writeCompleted.load(std::memory_order_acquire), "child1: still waiting for write");
                verify(coord->isDraining(), "child1: still draining before release");
                latch->release();
            });
        });

        const int rc = app.exec();
        verify(rc == 0, "child1: app.exec() returned 0");
        verify(writeCompleted.load(std::memory_order_acquire), "child1: write completed before exit");
        verify(writeFuture.isFinished() && writeFuture.result() == 77, "child1: write future intact");
        verify(coord->isCompleted(), "child1: coordinator completed");
        verify(!coord->isEventLoopLocked(), "child1: QEventLoopLocker released");
        verify(aboutToQuitFired, "child1: aboutToQuit fired");
        qInfo("CHILD_PASS: last-window-non-kjob");
        return 0;
    }

    if (scenario == "multi-window") {
        // Two real ThisPcWindows inside app.exec(): closing A must leave B fully functional
        // on process-wide ApplicationWorkerPools; closing B exits app.exec() cleanly.
        ThisPcWindow winA(QUrl::fromLocalFile(tempDir.path()), false);
        ThisPcWindow winB(QUrl::fromLocalFile(tempDir.path()), false);
        winA.show();
        winB.show();

        bool metaOnBWorked = false;
        bool writeOnBWorked = false;
        QTimer::singleShot(20, &app, [&]() {
            winA.close();
            verify(!winA.isVisible(), "child2: winA closed cleanly");
            verify(winB.isVisible(), "child2: winB still visible");
            verify(!pools->isShuttingDown(), "child2: shared pools NOT shut down while winB is active");

            auto *meta = new MetadataProvider(QUrl::fromLocalFile(sampleFile), &winB,
                [](const QString &) {
                    MetadataExtractionResult r;
                    r.userTags = {QStringLiteral("surviving-window")};
                    return r;
                });
            QObject::connect(meta, &MetadataProvider::dataChanged, &winB, [&, meta](const MetadataData &d) {
                if (d.state == MetadataState::Available) {
                    metaOnBWorked = true;
                    auto f = pools->runXattrWrite([&writeOnBWorked]() -> int {
                        writeOnBWorked = true;
                        return 1;
                    });
                    auto *w = new QFutureWatcher<int>(&winB);
                    QObject::connect(w, &QFutureWatcher<int>::finished, &winB, [&winB, w]() {
                        w->deleteLater();
                        winB.close();
                    });
                    w->setFuture(f);
                }
            });
            verify(meta->start(), "child2: metadata started on surviving winB");
        });

        const int rc = app.exec();
        verify(rc == 0, "child2: app.exec() returned 0");
        verify(metaOnBWorked, "child2: metadata succeeded on winB after winA closed");
        verify(writeOnBWorked, "child2: xattr write succeeded on winB after winA closed");
        qInfo("CHILD_PASS: multi-window");
        return 0;
    }

    if (scenario == "non-killable-direct-op") {
        // Real app.exec() with a non-killable direct OperationManager job, empty queue,
        // visible OperationWindow, and fast diagnostic watchdog.
        ThisPcWindow win(QUrl::fromLocalFile(tempDir.path()), false);
        win.show();
        auto *coord = win.shutdownCoordinatorForTesting();
        verify(coord != nullptr, "child3: coordinator present");
        coord->setDiagnosticInterval(35);

        auto *job = new ControlledShutdownJob(/*killable=*/false);
        job->setAutoDelete(true);
        win.m_operationManager->track(job, QStringLiteral("Non-killable direct op"));
        QObject::connect(job, &KJob::result, win.m_operationManager, [mgr = win.m_operationManager, job](KJob *) {
            mgr->finish(job, job->error() == 0, job->error() == KJob::KilledJobError, job->errorString());
        });
        win.m_operationManager->showDetailedWindow();
        QPointer<OperationWindow> opWin = win.m_operationManager->detailedWindow();
        verify(!opWin.isNull() && opWin->isVisible(), "child3: OperationWindow visible");

        int delayedCount = 0;
        QStringList lastDiags;
        QObject::connect(coord, &ShutdownCoordinator::shutdownDelayed, &win, [&](const QStringList &d) {
            ++delayedCount;
            lastDiags = d;
            if (delayedCount == 1 && job) {
                // Only finish after watchdog proves non-killable op was reported without killing it
                QTimer::singleShot(15, &app, [job]() {
                    job->finishOk();
                });
            }
        });

        win.setCloseConfirmationHandlerForTesting([](int activeCount) {
            verify(activeCount >= 1, "child3: confirmation saw active direct op");
            return true;
        });

        QTimer::singleShot(20, &win, [&]() {
            win.close();
            verify(coord->isDraining(), "child3: coordinator draining on non-killable op");
            verify(win.isVisible(), "child3: main window stayed visible during non-killable drain");
        });

        const int rc = app.exec();
        verify(rc == 0, "child3: app.exec() returned 0");
        verify(delayedCount >= 1, "child3: watchdog fired shutdownDelayed");
        verify(lastDiags.join(QLatin1Char('\n')).contains(QStringLiteral("non-killable")),
               "child3: diagnostics reported non-killable direct operation");
        verify(coord->isCompleted(), "child3: coordinator completed after job finished");
        verify(opWin.isNull() || !opWin->isVisible(), "child3: OperationWindow closed on completion");
        qInfo("CHILD_PASS: non-killable-direct-op");
        return 0;
    }

    if (scenario == "about-to-quit-drain") {
        // Real app.exec() exited directly via QCoreApplication::quit() (bypassing closeEvent)
        // while 2 reads are running, 2 reads are queued, and 1 xattr write is running.
        auto readLatch = std::make_shared<SyncLatch>();
        auto writeLatch = std::make_shared<SyncLatch>();
        std::atomic<int> queuedIoExecuted{0};
        std::atomic<bool> writeFinished{false};

        auto r1 = pools->runXattrRead([readLatch]() -> int { readLatch->arriveAndWait(); return 1; });
        auto r2 = pools->runXattrRead([readLatch]() -> int { readLatch->arriveAndWait(); return 2; });
        auto r3 = pools->runXattrRead([&queuedIoExecuted]() -> int { ++queuedIoExecuted; return 3; });
        auto r4 = pools->runXattrRead([&queuedIoExecuted]() -> int { ++queuedIoExecuted; return 4; });
        auto w1 = pools->runXattrWrite([writeLatch, &writeFinished]() -> int {
            writeLatch->arriveAndWait();
            writeFinished.store(true, std::memory_order_release);
            return 99;
        });

        QTimer::singleShot(20, &app, [readLatch, writeLatch]() {
            verify(readLatch->waitForArrivals(2, 3000), "child4: 2 reads running");
            verify(writeLatch->waitForArrivals(1, 3000), "child4: 1 write running");
            // Release latches shortly after quit() begins so drainForQuit() finishes cleanly
            std::thread([readLatch, writeLatch]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                readLatch->release();
                writeLatch->release();
            }).detach();
            QCoreApplication::quit();
        });

        const int rc = app.exec();
        verify(rc == 0, "child4: app.exec() returned 0");
        verify(writeFinished.load(std::memory_order_acquire), "child4: accepted write completed in aboutToQuit");
        verify(w1.isFinished() && w1.result() == 99, "child4: write future valid");
        verify(r1.isFinished() && r2.isFinished() && r3.isFinished() && r4.isFinished(),
               "child4: all 4 read futures reached terminal state");
        verify(r3.isCanceled() && r4.isCanceled(), "child4: queued read futures cooperatively cancelled");
        verify(queuedIoExecuted.load(std::memory_order_acquire) == 0,
               "child4: queued reads performed zero prohibited I/O");
        qInfo("CHILD_PASS: about-to-quit-drain");
        return 0;
    }

    if (scenario == "draining-mutation-guard") {
        // Real QApplication::exec() proving that during Draining on a non-killable
        // direct OperationManager job, user-initiated mutations (toolbar, context menu,
        // drag-drop, keyboard/queued callbacks) are all blocked while the existing
        // operation completes, watchdog fires, and app.exec() terminates with rc=0.
        interceptFileJobs = true;
        dispatch = {};

        ThisPcWindow win(QUrl::fromLocalFile(tempDir.path()), false);
        win.show();
        auto *coord = win.shutdownCoordinatorForTesting();
        verify(coord != nullptr, "child5: ShutdownCoordinator present");
        coord->setDiagnosticInterval(35);

        auto *job = new ControlledShutdownJob(/*killable=*/false);
        job->setAutoDelete(true);
        win.m_operationManager->track(job, QStringLiteral("Non-killable direct op R2"));
        QObject::connect(job, &KJob::result, win.m_operationManager, [mgr = win.m_operationManager, job](KJob *) {
            mgr->finish(job, job->error() == 0, job->error() == KJob::KilledJobError, job->errorString());
        });

        int delayedCount = 0;
        bool mutationAttemptsDone = false;
        QObject::connect(coord, &ShutdownCoordinator::shutdownDelayed, &win, [&](const QStringList &) {
            ++delayedCount;
            if (delayedCount == 1 && mutationAttemptsDone && job) {
                QTimer::singleShot(15, &app, [job]() { job->finishOk(); });
            }
        });

        win.setCloseConfirmationHandlerForTesting([](int) { return true; });

        QTimer::singleShot(20, &win, [&]() {
            win.close();
            verify(coord->isDraining(), "child5: entered Draining");
            verify(win.isVisible(), "child5: window stayed visible during Draining");
            verify(win.centralWidget() && !win.centralWidget()->isEnabled(),
                   "child5: centralWidget disabled during Draining");
            verify(win.statusBar() && !win.statusBar()->currentMessage().isEmpty(),
                   "child5: status bar displays closing status");

            // Attempt multiple mutations via queued/reentrant callbacks during Draining
            QTimer::singleShot(10, &win, [&]() {
                dispatch = {};
                win.createNewFolder();
                win.createNewFile(QStringLiteral("blocked.txt"), "x");
                win.pasteClipboard();
                win.trashSelected();
                win.deletePermanentlySelected();
                win.batchRenameSelected();
                win.renameSelected();
                win.showDirectoryContextMenu(QPoint(5, 5));
                win.handleDroppedUrls(
                    {QUrl::fromLocalFile(sampleFile)},
                    QUrl::fromLocalFile(tempDir.path()),
                    QPoint(10, 10),
                    Qt::NoModifier);
                verify(dispatch.kind.isEmpty(), "child5: zero mutations accepted during Draining");
                verify(win.findChildren<QMenu *>().isEmpty()
                       || !win.findChild<QMenu *>()->isVisible(),
                       "child5: context menu did not open during Draining");
                mutationAttemptsDone = true;
                if (delayedCount >= 1 && job) {
                    QTimer::singleShot(10, &app, [job]() { job->finishOk(); });
                }
            });
        });

        const int rc = app.exec();
        verify(rc == 0, "child5: app.exec() returned 0");
        verify(mutationAttemptsDone, "child5: mutation guard attempts executed");
        verify(delayedCount >= 1, "child5: watchdog fired during Draining");
        verify(dispatch.kind.isEmpty(), "child5: no mutation bypassed guard");
        verify(coord->isCompleted(), "child5: coordinator completed");
        qInfo("CHILD_PASS: draining-mutation-guard");
        return 0;
    }

    qFatal("Unknown child scenario: %s", scenario.constData());
    return 2;
}

void runChildScenarioInSubprocess(const QString &scenario)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(QCoreApplication::applicationFilePath(),
               {QStringLiteral("lifecycle_hygiene"), QStringLiteral("--child-scenario"), scenario});
    verify(proc.waitForStarted(5000), "subprocess started");
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        proc.waitForFinished(2000);
        qFatal("FAIL (lifecycle_hygiene): subprocess '%s' timed out! Output:\n%s",
               qPrintable(scenario), proc.readAll().constData());
    }
    const QByteArray output = proc.readAll();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        qFatal("FAIL (lifecycle_hygiene): subprocess '%s' failed (rc=%d)! Output:\n%s",
               qPrintable(scenario), proc.exitCode(), output.constData());
    }
    verify(output.contains(QByteArray("CHILD_PASS: ") + scenario.toUtf8()),
           "subprocess emitted CHILD_PASS marker");
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    if (argc > 3 && QByteArray(argv[2]) == "--child-scenario") {
        return runRealEventLoopChild(QByteArray(argv[3]), app);
    }

    auto *pools = ApplicationWorkerPools::forApplication();
    verify(pools != nullptr, "ApplicationWorkerPools::forApplication() available");

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "tempDir valid");

    // =========================================================================
    // 1. AUD-1A-01: Queued QtConcurrent read overflow (>2 tasks) without clear()
    // =========================================================================
    {
        const QString metaFile = writeTempFile(tempDir.path(), QStringLiteral("overflow.txt"), "overflow");

        // 1a. xattrReadPool overflow: 2 executing + 2 queued -> beginShutdownDrain()
        {
            auto latch = std::make_shared<SyncLatch>();
            std::atomic<int> queuedIoCalls{0};

            auto f1 = pools->runXattrRead([latch]() -> int { latch->arriveAndWait(); return 10; });
            auto f2 = pools->runXattrRead([latch]() -> int { latch->arriveAndWait(); return 20; });
            auto f3 = pools->runXattrRead([&queuedIoCalls]() -> int { ++queuedIoCalls; return 30; });
            auto f4 = pools->runXattrRead([&queuedIoCalls]() -> int { ++queuedIoCalls; return 40; });

            verify(latch->waitForArrivals(2, 3000), "1a. 2 xattr read workers entered pool");
            verify(pools->activeReadTaskCount() == 4, "1a. 4 read tasks tracked (2 running + 2 queued)");

            QSignalSpy drainedSpy(pools, &ApplicationWorkerPools::drainCompleted);
            const quint64 drainGen = pools->beginShutdownDrain();
            verify(drainedSpy.count() == 0, "1a. drain not yet complete while 2 workers blocked");

            // Track watchers for queued tasks f3 and f4 to prove finished() fires and watchers clean up
            QPointer<QFutureWatcher<int>> w3 = new QFutureWatcher<int>();
            QPointer<QFutureWatcher<int>> w4 = new QFutureWatcher<int>();
            int watcherFinished = 0;
            QObject::connect(w3, &QFutureWatcher<int>::finished, w3, [&]() { ++watcherFinished; w3->deleteLater(); });
            QObject::connect(w4, &QFutureWatcher<int>::finished, w4, [&]() { ++watcherFinished; w4->deleteLater(); });
            w3->setFuture(f3);
            w4->setFuture(f4);

            latch->release();
            verify(waitUntil([&]() {
                return f1.isFinished() && f2.isFinished() && f3.isFinished() && f4.isFinished()
                    && w3.isNull() && w4.isNull() && drainedSpy.count() >= 1;
            }), "1a. all 4 futures reached terminal state and watchers cleaned up");
            verify(watcherFinished == 2, "1a. both queued watchers received finished()");
            verify(f3.isCanceled() && f4.isCanceled(), "1a. queued futures cooperatively cancelled");
            verify(queuedIoCalls.load(std::memory_order_acquire) == 0,
                   "1a. queued tasks performed ZERO prohibited I/O after cancellation");
            verify(!pools->hasActiveTasksUpTo(drainGen), "1a. generation drained cleanly");

            pools->resetForTesting();
        }

        // 1b. metadataPool overflow with real MetadataProvider: cancelled queued provider must NOT
        // publish default/empty result as Available/NoMetadata!
        {
            auto latch = std::make_shared<SyncLatch>();
            std::atomic<int> queuedMetaCalls{0};
            auto b1 = pools->runMetadataTask([latch]() -> int { latch->arriveAndWait(); return 1; });
            auto b2 = pools->runMetadataTask([latch]() -> int { latch->arriveAndWait(); return 2; });
            verify(latch->waitForArrivals(2, 3000), "1b. 2 metadata pool threads occupied");

            MetadataProvider queuedProvider(QUrl::fromLocalFile(metaFile), nullptr,
                [&queuedMetaCalls](const QString &) {
                    ++queuedMetaCalls;
                    MetadataExtractionResult r;
                    r.userTags = {QStringLiteral("should-not-run")};
                    return r;
                });
            int dataChanges = 0;
            QObject::connect(&queuedProvider, &MetadataProvider::dataChanged, &queuedProvider,
                             [&](const MetadataData &) { ++dataChanges; });
            verify(queuedProvider.start(), "1b. queued MetadataProvider submitted");
            verify(dataChanges == 1 && queuedProvider.data().state == MetadataState::Loading,
                   "1b. queued MetadataProvider entered Loading");

            pools->beginShutdownDrain();
            latch->release();
            verify(waitUntil([&]() { return pools->activeTaskCount() == 0 && b1.isFinished() && b2.isFinished(); }),
                   "1b. metadataPool drained cleanly");
            verify(queuedMetaCalls.load(std::memory_order_acquire) == 0,
                   "1b. queued MetadataProvider extractor did NOT run after cancellation");
            verify(dataChanges == 1 && queuedProvider.data().state == MetadataState::Loading,
                   "1b. cancelled MetadataProvider did NOT publish bogus default result");
            verify(queuedProvider.findChildren<QFutureWatcherBase *>().isEmpty(),
                   "1b. queued MetadataProvider watcher deleted cleanly");

            pools->resetForTesting();
        }
    }

    // =========================================================================
    // 2. AUD-1A-02: Multi-window isolation, drain-during-new-window & xattr UI error
    // =========================================================================
    {
        const QString testFile = writeTempFile(tempDir.path(), QStringLiteral("mw.txt"), "multiwindow");

        // 2a. Close Window A during active write, open Window B while A is draining ->
        // Window A still completes shutdown, Window B stays fully operational!
        {
            ThisPcWindow winA(QUrl::fromLocalFile(tempDir.path()), false);
            winA.show();
            QCoreApplication::processEvents();
            auto *coordA = winA.shutdownCoordinatorForTesting();
            verify(coordA != nullptr, "2a. winA has ShutdownCoordinator");

            auto writeLatch = std::make_shared<SyncLatch>();
            auto writeFuture = pools->runXattrWrite([writeLatch]() -> int {
                writeLatch->arriveAndWait();
                return 55;
            });
            verify(writeLatch->waitForArrivals(1, 3000), "2a. write active before winA close");

            QSignalSpy spyA(coordA, &ShutdownCoordinator::shutdownCompleted);
            winA.close();
            verify(coordA->isDraining(), "2a. winA draining as last window");
            verify(coordA->isEventLoopLocked(), "2a. winA holding QEventLoopLocker");

            // Now open Window B WHILE Window A is still draining!
            ThisPcWindow winB(QUrl::fromLocalFile(tempDir.path()), false);
            winB.show();
            QCoreApplication::processEvents();
            verify(!pools->isShuttingDown(), "2a. winB re-opened pool submission without breaking winA drain");

            // Release Window A's write -> Window A MUST still receive drainCompleted and finish!
            writeLatch->release();
            verify(waitUntil([&]() { return coordA->isCompleted(); }),
                   "2a. winA completed shutdown despite winB opening mid-drain");
            verify(spyA.count() == 1, "2a. winA shutdownCompleted emitted once");
            verify(!coordA->isEventLoopLocked(), "2a. winA QEventLoopLocker released");
            verify(writeFuture.result() == 55, "2a. winA write result intact");

            // Verify Window B can still run metadata & xattr tasks cleanly
            MetadataProvider metaB(QUrl::fromLocalFile(testFile), nullptr, [](const QString &) {
                MetadataExtractionResult r;
                r.userTags = {QStringLiteral("ok-on-B")};
                return r;
            });
            verify(metaB.start(), "2a. winB metadata started");
            verify(waitUntil([&]() { return metaB.data().state == MetadataState::Available; }),
                   "2a. winB metadata succeeded");

            winB.close();
            verify(waitUntil([&]() { return winB.shutdownCoordinatorForTesting()->isCompleted(); }),
                   "2a. winB closed cleanly");
            pools->resetForTesting();
        }

        // 2b. Failed PropertiesXattrMutationController::submit() shows visible error in UI
        {
            PropertiesTargetCapabilities caps;
            caps.requestedUrl = QUrl::fromLocalFile(testFile);
            caps.normalizedUrl = caps.requestedUrl;
            caps.isLocal = true;
            caps.entryIdentity = {true, 1, 1, S_IFREG};
            caps.userXattrEditable = PropertiesCapabilityState::Supported;

            PropertiesXattrWidget widget(
                caps.requestedUrl, nullptr, caps.entryIdentity, caps,
                [](const PropertiesTargetCapabilities &, PropertiesXattrOperation,
                   const QByteArray &, const QByteArray &) {
                    return PropertiesXattrWriteResult{PropertiesXattrWriteStatus::Success, 0};
                },
                [](const QUrl &, const PropertiesObjectIdentity &) {
                    PropertiesXattrSnapshot s;
                    s.status = PropertiesXattrStatus::Empty;
                    return s;
                });
            widget.activate();
            verify(waitUntil([&]() {
                return widget.findChild<QPushButton *>(QStringLiteral("propertiesXattrAdd"))->isEnabled();
            }), "2b. xattr widget loaded and editable");

            // Put pools into shutdown so submit() returns false
            pools->beginShutdownDrain();
            verify(!widget.mutate(PropertiesXattrOperation::Add, QByteArrayLiteral("user.fail"), QByteArrayLiteral("v")),
                   "2b. mutate() returned false when submit rejected");
            auto *errLabel = widget.findChild<QLabel *>(QStringLiteral("propertiesXattrWriteError"));
            verify(errLabel != nullptr && !errLabel->text().isEmpty() && !errLabel->isHidden(),
                   "2b. failed xattr submission showed visible error message");
            pools->resetForTesting();
        }
    }

    // =========================================================================
    // 3. AUD-1A-03 & AUD-1A-07: Queued-only confirmation, direct ops, revalidation, re-show
    // =========================================================================
    {
        ThisPcWindow win(QUrl::fromLocalFile(tempDir.path()), false);
        win.show();
        QCoreApplication::processEvents();
        auto *coord = win.shutdownCoordinatorForTesting();
        verify(coord != nullptr && coord->isIdle(), "3. coordinator ready and Idle");

        auto *qc = win.m_queueController;
        if (!qc) {
            win.m_queueController = new TransferQueueController(&win);
            win.m_shutdownQueueWired = false;
            win.ensureShutdownCoordinator();
            qc = win.m_queueController;
        }

        // Pause queue and enqueue 1 queued-only task (activeCount == 0, queuedCount == 1)
        qc->setDispatchPaused(true);
        TransferTaskDescriptor desc;
        desc.type = TransferOperationType::Copy;
        desc.backend = TransferBackendKind::LocalNative;
        desc.title = QStringLiteral("Queued Only Task");
        desc.sources = {QUrl::fromLocalFile(tempDir.path() + QStringLiteral("/qsrc.bin"))};
        desc.destination = QUrl::fromLocalFile(tempDir.path() + QStringLiteral("/qdst"));
        desc.capabilities = defaultTransferCapabilities(desc.backend, desc.type);
        desc.jobFactory = []() -> KJob * {
            auto *j = new ControlledShutdownJob(true);
            j->setAutoDelete(true);
            return j;
        };
        verify(qc->enqueue(desc).accepted(), "3. queued-only task enqueued");
        verify(qc->activeCount() == 0 && qc->queuedCount() == 1, "3. activeCount==0 and queuedCount==1");

        // Close confirmation MUST fire for queued-only transfer AND block reentrant close()
        int confirmCalls = 0;
        win.setCloseConfirmationHandlerForTesting([&](int activeOps) -> bool {
            ++confirmCalls;
            verify(activeOps == 1, "3. queued-only transfer counted in activeOperations");
            verify(coord->isConfirming(), "3. coordinator Confirming during dialog");
            win.close(); // Reentrant close ignored
            return false;
        });
        verify(!win.close(), "3. close rejected for queued-only transfer");
        verify(confirmCalls == 1, "3. reentrant close did not double-prompt");
        verify(coord->isIdle(), "3. coordinator back to Idle");

        // AUD-1A-07: Post-confirmation safety gate revalidation (PropertiesWindow becomes busy
        // during modal QMessageBox -> close must be deferred even if user clicked Yes!)
        auto *propWin = new PropertiesWindow(&win);
        propWin->setAttribute(Qt::WA_DeleteOnClose);
        PropertiesLifecycle::instance().add(propWin);
        propWin->show();
        QCoreApplication::processEvents();

        bool simulatedBusyOnce = false;
        win.setCloseConfirmationHandlerForTesting([&](int) -> bool {
            ++confirmCalls;
            if (!simulatedBusyOnce) {
                simulatedBusyOnce = true;
                propWin->setBusy(true); // Becomes busy while modal QMessageBox is open!
            }
            return true;            // User clicks Yes
        });
        verify(!win.close(), "3. close deferred when PropertiesWindow became busy during confirmation");
        verify(coord->isIdle(), "3. coordinator remained Idle after post-confirmation gate deferred close");

        // Clearing busy lets PropertiesLifecycle trigger the deferred close cleanly!
        propWin->setBusy(false);
        verify(waitUntil([&]() { return coord->isCompleted() && !win.isVisible(); }),
               "3. queued-only transfer cancelled and window closed cleanly");

        // AUD-1A-07: Re-showing a closed window resets coordinator to Idle so subsequent close works
        win.show();
        QCoreApplication::processEvents();
        verify(coord->isIdle(), "3. re-shown window reset ShutdownCoordinator to Idle");
        win.close();
        verify(waitUntil([&]() { return coord->isCompleted() && !win.isVisible(); }),
               "3. re-shown window ran full closeEvent on second close");

        pools->resetForTesting();
    }

    // =========================================================================
    // 4. AUD-1A-04: Deterministic stress test for concurrent drain completion
    // =========================================================================
    {
        for (int iter = 0; iter < 40; ++iter) {
            auto f1 = pools->runXattrRead([]() -> int { return 1; });
            auto f2 = pools->runXattrWrite([]() -> int { return 2; });
            QSignalSpy spy(pools, &ApplicationWorkerPools::drainCompleted);
            const quint64 gen = pools->beginShutdownDrain();
            verify(waitUntil([&]() {
                return f1.isFinished() && f2.isFinished()
                    && !pools->hasActiveTasksUpTo(gen) && spy.count() >= 1;
            }), "4. stress iteration completed drain without missed wakeup");
            verify(f2.result() == 2, "4. stress iteration preserved accepted write");
            pools->resetForTesting();
        }
    }

    // =========================================================================
    // 5. AUD-1A-05: ChecksumJob rapid restart, idle cancel, DuplicateFinder (0 leak LSan)
    // =========================================================================
    {
        const QByteArray payload(128 * 1024, 'A');
        const QString f1 = writeTempFile(tempDir.path(), QStringLiteral("dup1.bin"), payload);
        const QString f2 = writeTempFile(tempDir.path(), QStringLiteral("dup2.bin"), payload);

        // 5a. Immediate restart directly from stateChanged(Completed), then immediate delete
        {
            auto *job = new ChecksumJob(QUrl::fromLocalFile(f1));
            int completions = 0;
            QObject::connect(job, &ChecksumJob::stateChanged, job, [&](const ChecksumData &d) {
                if (d.state == ChecksumState::Completed) {
                    ++completions;
                    if (completions < 3) {
                        verify(job->start(), "5a. rapid restart inside stateChanged succeeded");
                    }
                }
            });
            verify(job->start(), "5a. initial start");
            verify(waitUntil([&]() { return completions == 3; }), "5a. 3 rapid consecutive runs completed");
            // Trigger one more start and immediately destroy mid-flight
            verify(job->start(), "5a. 4th start before immediate destroy");
            delete job; // Must cleanly join & delete all tracked QThreads with 0 leak
        }

        // 5b. Idle cancel() after completion must NOT poison next start()
        {
            ChecksumJob job(QUrl::fromLocalFile(f1));
            verify(job.start(), "5b. first run started");
            verify(waitUntil([&]() { return job.data().state == ChecksumState::Completed; }),
                   "5b. first run completed");
            job.cancel(); // Idle cancel!
            job.cancel();
            verify(job.start(), "5b. second run after idle cancel started");
            verify(waitUntil([&]() { return job.data().state == ChecksumState::Completed; }),
                   "5b. second run completed (not poisoned by idle cancel)");
        }

        // 5c. Multi-file DuplicateFinderJob scan
        {
            StorageScanStats stats;
            stats.files = 2;
            auto makeEntry = [](const QString &path) {
                StorageScanEntry e;
                e.path = path;
                e.name = path.section(QLatin1Char('/'), -1);
                e.type = StorageEntryType::RegularFile;
                e.logicalSize = 128 * 1024;
                e.allocatedSize = 128 * 1024;
                struct stat st {};
                verify(::lstat(QFile::encodeName(path).constData(), &st) == 0, "5c. lstat");
                e.deviceId = static_cast<quint64>(st.st_dev);
                e.inode = static_cast<quint64>(st.st_ino);
                e.mtimeSec = st.st_mtim.tv_sec;
                e.mtimeNsec = st.st_mtim.tv_nsec;
                e.ctimeSec = st.st_ctim.tv_sec;
                e.ctimeNsec = st.st_ctim.tv_nsec;
                e.linkCount = 1;
                return e;
            };
            DuplicateFinderJob finder(tempDir.path(), {makeEntry(f1), makeEntry(f2)}, stats, StorageScanState::Completed);
            bool done = false;
            QObject::connect(&finder, &DuplicateFinderJob::finished, &finder, [&](const DuplicateFinderResult &res) {
                done = true;
                verify(res.groups.size() == 1 && res.groups.first().files.size() == 2,
                       "5c. DuplicateFinderJob found 1 group of 2");
            });
            verify(finder.start(), "5c. DuplicateFinderJob started");
            verify(waitUntil([&]() { return done; }), "5c. DuplicateFinderJob finished");
        }
    }

    // =========================================================================
    // 6. AUD-1A-06: Context-menu consecutive retirement, invalidation & external close
    // =========================================================================
    {
        const QString subDir = tempDir.path() + QStringLiteral("/ctxsub");
        QDir().mkpath(subDir);
        writeTempFile(subDir, QStringLiteral("x1.dat"), QByteArray(4096, 'Z'));
        writeTempFile(subDir, QStringLiteral("x2.dat"), QByteArray(4096, 'Z'));

        auto *dlg = new StorageScanDialog(QUrl::fromLocalFile(tempDir.path()));
        QPointer<StorageScanDialog> dlgGuard(dlg);
        dlg->show();
        verify(waitUntil([&]() { return dlg->job() && dlg->job()->state() == StorageScanState::Completed; }),
               "6. StorageScanDialog scan completed");

        // Consecutive context menus on filesTableView: second request retires first QMenu
        dlg->tabWidget()->setCurrentIndex(1);
        QCoreApplication::processEvents();
        const QRect fRect = dlg->filesTableView()->visualRect(dlg->filesModel()->index(0, 0));
        Q_EMIT dlg->filesTableView()->customContextMenuRequested(fRect.center());
        QCoreApplication::processEvents();
        QPointer<QMenu> firstMenu = dlg->findChild<QMenu *>();
        verify(!firstMenu.isNull(), "6. first QMenu opened");

        Q_EMIT dlg->filesTableView()->customContextMenuRequested(fRect.center());
        verify(waitUntil([&]() { return firstMenu.isNull(); }),
               "6. second context-menu request closed and retired first QMenu");
        QPointer<QMenu> secondMenu = dlg->findChild<QMenu *>();
        verify(!secondMenu.isNull(), "6. second QMenu active");

        // Clear model while menu is open, trigger action -> must use immutable snapshot safely
        QSignalSpy navSpy(dlg, &StorageScanDialog::navigateRequested);
        dlg->filesModel()->setResult({});
        secondMenu->actions().first()->trigger();
        QCoreApplication::processEvents();
        verify(navSpy.count() == 1, "6. immutable path snapshot worked after model clear");

        // Treemap consecutive context menu + external close
        dlg->tabWidget()->setCurrentIndex(4);
        dlg->resize(720, 560);
        QCoreApplication::processEvents();
        auto *canvas = dlg->treemapWidget()->findChild<StorageTreemapCanvas *>();
        verify(canvas != nullptr, "6. treemap canvas found");
        const QPoint center = canvas->rect().center();
        QContextMenuEvent ev1(QContextMenuEvent::Mouse, center, canvas->mapToGlobal(center));
        QCoreApplication::sendEvent(canvas, &ev1);
        QCoreApplication::processEvents();
        QPointer<QMenu> tm1 = canvas->findChild<QMenu *>();
        verify(!tm1.isNull(), "6. first treemap menu opened");

        QContextMenuEvent ev2(QContextMenuEvent::Mouse, center, canvas->mapToGlobal(center));
        QCoreApplication::sendEvent(canvas, &ev2);
        verify(waitUntil([&]() { return tm1.isNull(); }), "6. second treemap menu retired first");
        QPointer<QMenu> tm2 = canvas->findChild<QMenu *>();
        verify(!tm2.isNull(), "6. second treemap menu active");

        dlg->close();
        verify(waitUntil([&]() { return dlgGuard.isNull() && tm2.isNull(); }),
               "6. external close destroyed dialog and active treemap menu cleanly");
    }

    // =========================================================================
    // 7. AUD-1A-R1-NOTE-01 (Stage 1A-R2): Shutdown Interaction Safety Guard
    // =========================================================================
    {
        interceptFileJobs = true;
        dispatch = {};

        const QString r2File = writeTempFile(tempDir.path(), QStringLiteral("r2.txt"), "r2-guard");
        ThisPcWindow winA(QUrl::fromLocalFile(tempDir.path()), false);
        ThisPcWindow winB(QUrl::fromLocalFile(tempDir.path()), false);
        winA.show();
        winB.show();
        QCoreApplication::processEvents();

        auto *coordA = winA.shutdownCoordinatorForTesting();
        verify(coordA != nullptr && coordA->isIdle(), "7. winA coordinator Idle");

        auto *directJob = new ControlledShutdownJob(/*killable=*/false);
        directJob->setAutoDelete(true);
        winA.m_operationManager->track(directJob, QStringLiteral("Non-killable R2 op"));
        QObject::connect(directJob, &KJob::result, winA.m_operationManager,
                         [mgr = winA.m_operationManager, directJob](KJob *) {
                             mgr->finish(directJob, directJob->error() == 0,
                                         directJob->error() == KJob::KilledJobError,
                                         directJob->errorString());
                         });

        // 7a. Rejected close keeps controls enabled and interactive
        winA.setCloseConfirmationHandlerForTesting([](int) { return false; });
        verify(!winA.close(), "7a. rejected close keeps winA open");
        verify(coordA->isIdle(), "7a. coordinator stays Idle after rejected close");
        verify(winA.centralWidget() && winA.centralWidget()->isEnabled(),
               "7a. centralWidget remains enabled after rejected close");
        verify(winA.m_newFolderAction && winA.m_newFolderAction->isEnabled(),
               "7a. toolbar create action remains enabled after rejected close");

        // 7b. Accepted close enters Draining: blocks mutations on winA, preserves winB
        winA.setCloseConfirmationHandlerForTesting([](int) { return true; });
        winA.close();
        QCoreApplication::processEvents();
        verify(coordA->isDraining(), "7b. winA in Draining");
        verify(winA.isVisible(), "7b. winA remains visible while draining");
        verify(winA.centralWidget() && !winA.centralWidget()->isEnabled(),
               "7b. winA centralWidget disabled during Draining");
        verify(winB.centralWidget() && winB.centralWidget()->isEnabled(),
               "7b. surviving winB centralWidget stays enabled");
        verify(winB.m_newFolderAction && winB.m_newFolderAction->isEnabled(),
               "7b. surviving winB toolbar actions stay enabled");
        verify(!winA.m_newFolderAction->isEnabled() && !winA.m_pasteAction->isEnabled()
               && !winA.m_renameAction->isEnabled() && !winA.m_batchRenameAction->isEnabled()
               && !winA.m_trashAction->isEnabled() && !winA.m_undoAction->isEnabled()
               && !winA.m_redoAction->isEnabled(),
               "7b. all mutating toolbar actions disabled on closing winA");

        // Attempt direct/shortcut/context-menu/drop mutations on closing winA -> all blocked
        dispatch = {};
        winA.createNewFolder();
        winA.createNewFile(QStringLiteral("blocked2.txt"), "y");
        winA.pasteClipboard();
        winA.pasteClipboardInto(QUrl::fromLocalFile(tempDir.path()));
        winA.trashSelected();
        winA.deletePermanentlySelected();
        winA.batchRenameSelected();
        winA.renameSelected();
        winA.showDirectoryContextMenu(QPoint(5, 5));
        winA.handleDroppedUrls({QUrl::fromLocalFile(r2File)},
                               QUrl::fromLocalFile(tempDir.path()),
                               QPoint(10, 10), Qt::NoModifier);
        winA.m_undoAction->trigger();
        winA.m_redoAction->trigger();
        verify(dispatch.kind.isEmpty(), "7b. zero file mutations dispatched from closing winA");

        // Complete the previously accepted non-killable job -> winA finishes closing cleanly
        directJob->finishOk();
        verify(waitUntil([&]() { return coordA->isCompleted() && !winA.isVisible(); }),
               "7b. winA completed shutdown cleanly once non-killable op finished");

        // Re-showing winA restores interactive controls
        winA.show();
        QCoreApplication::processEvents();
        verify(coordA->isIdle(), "7b. re-shown winA reset to Idle");
        verify(winA.centralWidget() && winA.centralWidget()->isEnabled(),
               "7b. re-shown winA restored centralWidget enabled state");
        verify(winA.m_newFolderAction && winA.m_newFolderAction->isEnabled(),
               "7b. re-shown winA restored toolbar actions");

        winA.close();
        winB.close();
        verify(waitUntil([&]() {
            return coordA->isCompleted() && winB.shutdownCoordinatorForTesting()->isCompleted();
        }), "7b. both windows closed cleanly");
        pools->resetForTesting();
    }

    // =========================================================================
    // 8. §10 Mandatory Real QApplication::exec() Subprocess Integration Tests
    // =========================================================================
    runChildScenarioInSubprocess(QStringLiteral("last-window-non-kjob"));
    runChildScenarioInSubprocess(QStringLiteral("multi-window"));
    runChildScenarioInSubprocess(QStringLiteral("non-killable-direct-op"));
    runChildScenarioInSubprocess(QStringLiteral("about-to-quit-drain"));
    runChildScenarioInSubprocess(QStringLiteral("draining-mutation-guard"));

    qInfo("lifecycle_hygiene: ALL %d CHECKS PASSED (including 5 real QApplication::exec() subprocesses)", g_checks);
    return 0;
}
