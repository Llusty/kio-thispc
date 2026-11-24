/*
 * Focused core-only unit & lifecycle tests for PortableTransferJob and
 * portablecommon.h (0.46.0 Stage 2A.1).
 *
 * Strictly links only `thispc-core` (Qt6::Core + Qt6::Concurrent).
 * Zero KF6, KJob, KIO, GIO, QtGui, or QtWidgets dependencies.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "portablecommon.h"
#include "portabletransferjob.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "transferbandwidthlimiter.h"

#include <cstdio>
#include <functional>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char *expr, int line)
{
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL [line %d]: %s\n", line, expr);
        ++g_failures;
    }
}

#define PTJ_CHECK(expr) check((expr), #expr, __LINE__)

class FakePortableJob final : public PortableTransferJob
{
    Q_OBJECT

public:
    explicit FakePortableJob(TransferJobCapabilities caps = TransferJobCapability::Killable,
                             QObject *parent = nullptr)
        : PortableTransferJob(parent)
    {
        setCapabilities(caps);
    }

    int startCalls = 0;
    int cancelCalls = 0;
    int suspendCalls = 0;
    int resumeCalls = 0;
    bool acceptCancel = true;
    bool acceptSuspend = true;
    bool acceptResume = true;
    std::function<void(FakePortableJob *)> onStart;
    std::function<void(FakePortableJob *)> onCancel;
    std::shared_ptr<TransferBandwidthLimiter> attachedLimiter;
    quint64 attachedTaskId = 0;

    void setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter,
                             quint64 taskId = 0) override
    {
        attachedLimiter = std::move(limiter);
        attachedTaskId = taskId;
    }

    // Expose protected helpers for deterministic test driving
    void reportTotalBytes(qulonglong bytes) { setTotalBytes(bytes); }
    void reportProcessedBytes(qulonglong bytes) { setProcessedBytes(bytes); }
    void reportTotalFiles(qulonglong files) { setTotalFiles(files); }
    void reportProcessedFiles(qulonglong files) { setProcessedFiles(files); }
    void reportPercent(unsigned long pct) { setPercent(pct); }
    void reportSpeed(unsigned long bps) { emitSpeed(bps); }
    void reportCurrentItem(const QUrl &src, const QUrl &dst) { setCurrentItem(src, dst); }
    void reportInDecisionPhase(bool inDecision) { setInDecisionPhase(inDecision); }
    void reportCapabilities(TransferJobCapabilities caps) { setCapabilities(caps); }
    void reportOutcomeFlags(const TransferOutcomeFlags &flags) { setOutcomeFlags(flags); }
    void completeOk() { finishCompleted(); }
    void completeCancelled(const QString &msg = QString()) { finishCancelled(msg); }
    void completeFailed(TransferErrorCode code, const QString &msg) { finishFailed(code, msg); }

protected:
    void doStart() override
    {
        ++startCalls;
        if (onStart) {
            onStart(this);
        }
    }

    bool doCancel() override
    {
        ++cancelCalls;
        if (onCancel) {
            onCancel(this);
        }
        return acceptCancel;
    }

    bool doSuspend() override
    {
        ++suspendCalls;
        return acceptSuspend;
    }

    bool doResume() override
    {
        ++resumeCalls;
        return acceptResume;
    }
};

void flushEvents()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QString isolatedTempBase()
{
    const QString overrideDir = qEnvironmentVariable("THISPC_TEST_WORK_DIR");
    if (!overrideDir.isEmpty() && overrideDir.startsWith(QLatin1Char('/'))) {
        QDir().mkpath(overrideDir);
        return overrideDir;
    }
    return QDir::tempPath();
}

void testBasicLifecycleAndAutoDelete()
{
    QPointer<FakePortableJob> job = new FakePortableJob(
        TransferJobCapability::Killable | TransferJobCapability::Suspendable);
    PTJ_CHECK(job->state() == TransferJobState::NotStarted);
    PTJ_CHECK(!job->isStarted());
    PTJ_CHECK(!job->isTerminal());
    PTJ_CHECK(!job->isCancelRequested());
    PTJ_CHECK(!job->isSuspended());
    PTJ_CHECK(job->autoDelete());
    PTJ_CHECK(job->hasCapability(TransferJobCapability::Killable));
    PTJ_CHECK(job->hasCapability(TransferJobCapability::Suspendable));
    PTJ_CHECK(!job->hasCapability(TransferJobCapability::AsyncCancel));

    QList<TransferJobState> states;
    int finishedCount = 0;
    TransferJobState stateInFinished = TransferJobState::NotStarted;
    TransferErrorCode errorInFinished = TransferErrorCode::InternalError;
    bool aliveInFinished = false;

    QObject::connect(job, &PortableTransferJob::stateChanged,
                     [&](PortableTransferJob *, TransferJobState s) { states.append(s); });
    QObject::connect(job, &PortableTransferJob::finished,
                     [&](PortableTransferJob *j) {
                         ++finishedCount;
                         aliveInFinished = !job.isNull();
                         stateInFinished = j->state();
                         errorInFinished = j->errorCode();
                     });

    job->start();
    job->start(); // idempotent
    PTJ_CHECK(job->startCalls == 1);
    PTJ_CHECK(job->state() == TransferJobState::Running);
    PTJ_CHECK(job->isStarted());

    job->completeOk();
    // Duplicate terminal emissions must be ignored
    job->completeOk();
    job->completeFailed(TransferErrorCode::CannotWrite, QStringLiteral("ignored"));
    job->completeCancelled(QStringLiteral("ignored"));

    PTJ_CHECK(finishedCount == 1);
    PTJ_CHECK(aliveInFinished);
    PTJ_CHECK(stateInFinished == TransferJobState::Completed);
    PTJ_CHECK(errorInFinished == TransferErrorCode::NoError);
    PTJ_CHECK(states == QList<TransferJobState>({TransferJobState::Running, TransferJobState::Completed}));

    // Auto-delete must be deferred until event loop runs, not immediate inside callback
    PTJ_CHECK(!job.isNull());
    flushEvents();
    PTJ_CHECK(job.isNull());
}

void testManualOwnershipAndErrorCompletion()
{
    QObject owner;
    QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable, &owner);
    job->setAutoDelete(false);
    PTJ_CHECK(!job->autoDelete());

    int finishedCount = 0;
    QObject::connect(job, &PortableTransferJob::finished, [&](PortableTransferJob *) { ++finishedCount; });

    job->start();
    // Passing NoError to finishFailed must normalize to InternalError
    job->completeFailed(TransferErrorCode::NoError, QStringLiteral("synthetic failure"));
    PTJ_CHECK(finishedCount == 1);
    PTJ_CHECK(job->state() == TransferJobState::Failed);
    PTJ_CHECK(job->errorCode() == TransferErrorCode::InternalError);
    PTJ_CHECK(job->errorText() == QStringLiteral("synthetic failure"));

    flushEvents();
    PTJ_CHECK(!job.isNull()); // autoDelete == false preserves object

    // Explicit error code preservation
    QPointer<FakePortableJob> job2 = new FakePortableJob(TransferJobCapability::Killable, &owner);
    job2->setAutoDelete(false);
    job2->start();
    job2->completeFailed(TransferErrorCode::DiskFull, QStringLiteral("no space"));
    PTJ_CHECK(job2->errorCode() == TransferErrorCode::DiskFull);
    PTJ_CHECK(job2->errorText() == QStringLiteral("no space"));
}

void testAsyncCancellationWithCleanupBeforeFinished()
{
    QTemporaryDir sandbox(isolatedTempBase() + QStringLiteral("/thispc-ptj-test-XXXXXX"));
    PTJ_CHECK(sandbox.isValid());
    const QString partFile = sandbox.path() + QStringLiteral("/payload.bin.thispc-part");
    {
        QFile f(partFile);
        PTJ_CHECK(f.open(QIODevice::WriteOnly) && f.write("partial") == 7);
    }
    PTJ_CHECK(QFileInfo::exists(partFile));

    QPointer<FakePortableJob> job = new FakePortableJob(
        TransferJobCapability::Killable | TransferJobCapability::AsyncCancel);
    bool cleanupCompletedBeforeFinished = false;
    int finishedCount = 0;

    job->onCancel = [&](FakePortableJob *self) {
        // Simulate asynchronous worker cleanup before emitting terminal Cancelled
        QTimer::singleShot(15, self, [self, partFile]() {
            QFile::remove(partFile);
            self->completeCancelled(QStringLiteral("cancelled after cleanup"));
        });
    };

    QObject::connect(job, &PortableTransferJob::finished, [&](PortableTransferJob *j) {
        ++finishedCount;
        cleanupCompletedBeforeFinished = !QFileInfo::exists(partFile);
        PTJ_CHECK(j->state() == TransferJobState::Cancelled);
        PTJ_CHECK(j->errorCode() == TransferErrorCode::Cancelled);
        PTJ_CHECK(j->isCancelRequested());
    });

    job->start();
    PTJ_CHECK(job->requestCancel());
    // Immediately after requestCancel(), job must be Cancelling and NOT yet finished
    PTJ_CHECK(job->state() == TransferJobState::Cancelling);
    PTJ_CHECK(job->isCancelRequested());
    PTJ_CHECK(finishedCount == 0);
    PTJ_CHECK(QFileInfo::exists(partFile));

    // Idempotent second cancel request while Cancelling
    PTJ_CHECK(job->requestCancel());
    PTJ_CHECK(job->cancelCalls == 1);

    QEventLoop loop;
    QObject::connect(job, &PortableTransferJob::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();

    PTJ_CHECK(finishedCount == 1);
    PTJ_CHECK(cleanupCompletedBeforeFinished);
    PTJ_CHECK(!QFileInfo::exists(partFile));

    flushEvents();
    PTJ_CHECK(job.isNull());
}

void testCancelBeforeStartAndRejectedCancel()
{
    // Non-killable job rejects cancellation
    {
        FakePortableJob nonKillable(TransferJobCapability::None);
        nonKillable.setAutoDelete(false);
        PTJ_CHECK(!nonKillable.requestCancel());
        PTJ_CHECK(nonKillable.state() == TransferJobState::NotStarted);
    }

    // Killable job cancelled before start transitions cleanly to Cancelled
    {
        FakePortableJob unstarted(TransferJobCapability::Killable);
        unstarted.setAutoDelete(false);
        int finishedCount = 0;
        QObject::connect(&unstarted, &PortableTransferJob::finished, [&] { ++finishedCount; });
        PTJ_CHECK(unstarted.requestCancel());
        PTJ_CHECK(unstarted.state() == TransferJobState::Cancelled);
        PTJ_CHECK(unstarted.errorCode() == TransferErrorCode::Cancelled);
        PTJ_CHECK(finishedCount == 1);
        unstarted.start(); // must be no-op after terminal cancel
        PTJ_CHECK(unstarted.startCalls == 0);
    }

    // Rejected doCancel() reverts state from Cancelling back to Running
    {
        FakePortableJob stubborn(TransferJobCapability::Killable);
        stubborn.setAutoDelete(false);
        stubborn.acceptCancel = false;
        stubborn.start();
        PTJ_CHECK(!stubborn.requestCancel());
        PTJ_CHECK(stubborn.state() == TransferJobState::Running);
        PTJ_CHECK(!stubborn.isCancelRequested());
        stubborn.completeOk();
        PTJ_CHECK(!stubborn.requestCancel()); // terminal job rejects cancel
    }
}

void testSuspendResumeAndCapabilities()
{
    // Non-suspendable job (e.g. KIO semantics) must reject suspend/resume
    {
        FakePortableJob kioLike(TransferJobCapability::Killable);
        kioLike.setAutoDelete(false);
        kioLike.start();
        PTJ_CHECK(!kioLike.suspend());
        PTJ_CHECK(!kioLike.isSuspended());
        PTJ_CHECK(!kioLike.resume());
        kioLike.completeOk();
    }

    // Suspendable job transitions Running <-> Paused and emits suspendedChanged
    {
        FakePortableJob localCopy(
            TransferJobCapability::Killable
            | TransferJobCapability::Suspendable
            | TransferJobCapability::BandwidthLimited);
        localCopy.setAutoDelete(false);
        QList<bool> suspendedEvents;
        QObject::connect(&localCopy, &PortableTransferJob::suspendedChanged,
                         [&](PortableTransferJob *, bool s) { suspendedEvents.append(s); });

        PTJ_CHECK(!localCopy.suspend()); // NotStarted
        localCopy.start();
        PTJ_CHECK(localCopy.suspend());
        PTJ_CHECK(localCopy.state() == TransferJobState::Paused);
        PTJ_CHECK(localCopy.isSuspended());
        PTJ_CHECK(!localCopy.suspend()); // already paused
        PTJ_CHECK(localCopy.resume());
        PTJ_CHECK(localCopy.state() == TransferJobState::Running);
        PTJ_CHECK(!localCopy.isSuspended());
        PTJ_CHECK(!localCopy.resume()); // already running
        PTJ_CHECK(suspendedEvents == QList<bool>({true, false}));

        auto limiter = std::make_shared<TransferBandwidthLimiter>(nullptr);
        localCopy.setBandwidthLimiter(limiter, 77);
        PTJ_CHECK(localCopy.attachedLimiter == limiter);
        PTJ_CHECK(localCopy.attachedTaskId == 77);

        localCopy.completeOk();
    }
}

void testProgressSpeedAndOutcomeFlags()
{
    FakePortableJob job(TransferJobCapability::Killable);
    job.setAutoDelete(false);

    QList<qulonglong> processedEvents;
    QList<unsigned long> percentEvents;
    QList<unsigned long> speedEvents;
    QList<QPair<QUrl, QUrl>> itemEvents;

    QObject::connect(&job, &PortableTransferJob::processedBytesChanged,
                     [&](PortableTransferJob *, qulonglong b) { processedEvents.append(b); });
    QObject::connect(&job, &PortableTransferJob::percentChanged,
                     [&](PortableTransferJob *, unsigned long p) { percentEvents.append(p); });
    QObject::connect(&job, &PortableTransferJob::speedChanged,
                     [&](PortableTransferJob *, unsigned long s) { speedEvents.append(s); });
    QObject::connect(&job, &PortableTransferJob::currentItemChanged,
                     [&](PortableTransferJob *, const QUrl &s, const QUrl &d) {
                         itemEvents.append({s, d});
                     });

    job.start();
    job.reportTotalBytes(1000);
    job.reportProcessedBytes(250);
    job.reportProcessedBytes(1500); // clamped to 100%
    job.reportTotalFiles(3);
    job.reportProcessedFiles(2);
    job.reportSpeed(123456);
    const QUrl srcUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/a.bin"));
    const QUrl dstUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/b.bin"));
    job.reportCurrentItem(srcUrl, dstUrl);

    PTJ_CHECK(job.totalBytes() == 1000);
    PTJ_CHECK(job.processedBytes() == 1500);
    PTJ_CHECK(job.percent() == 100UL);
    PTJ_CHECK(job.totalFiles() == 3);
    PTJ_CHECK(job.processedFiles() == 2);
    PTJ_CHECK(job.speedBytesPerSecond() == 123456UL);
    PTJ_CHECK(job.currentSourceUrl() == srcUrl);
    PTJ_CHECK(job.currentDestinationUrl() == dstUrl);
    PTJ_CHECK(processedEvents == QList<qulonglong>({250, 1500}));
    PTJ_CHECK(percentEvents == QList<unsigned long>({25UL, 100UL}));
    PTJ_CHECK(speedEvents == QList<unsigned long>({123456UL}));
    PTJ_CHECK(itemEvents.size() == 1);

    TransferOutcomeFlags flags;
    flags.destinationPublished = true;
    flags.destinationReplaced = true;
    flags.directoryDurabilityConfirmed = true;
    flags.partialMoveOutcome = false;
    flags.batchPublishedCount = 2;
    flags.batchPartiallyCompleted = true;
    flags.mergeRetryBlocked = true;
    flags.mergeHistoryNote = QStringLiteral("merged 2 items");
    flags.systemError = 28;
    flags.backendErrorCode = 134;
    job.reportOutcomeFlags(flags);

    PTJ_CHECK(job.outcomeFlags().destinationPublished);
    PTJ_CHECK(job.outcomeFlags().destinationReplaced);
    PTJ_CHECK(job.outcomeFlags().directoryDurabilityConfirmed);
    PTJ_CHECK(job.outcomeFlags().batchPublishedCount == 2);
    PTJ_CHECK(job.outcomeFlags().mergeRetryBlocked);
    PTJ_CHECK(job.outcomeFlags().mergeHistoryNote == QStringLiteral("merged 2 items"));
    PTJ_CHECK(job.outcomeFlags().systemError == 28);
    PTJ_CHECK(job.outcomeFlags().backendErrorCode == 134);
    PTJ_CHECK(job.outcomeFlags().hasPartialPublication());

    job.completeOk();

    // Progress/speed mutations after terminal state must be suppressed
    job.reportProcessedBytes(2000);
    job.reportSpeed(999999);
    PTJ_CHECK(processedEvents.size() == 2);
    PTJ_CHECK(speedEvents.size() == 1);
}

void testNoHiddenEventLoopLocker(QCoreApplication &app)
{
    FakePortableJob unstarted(TransferJobCapability::Killable);
    unstarted.setAutoDelete(false);

    // If PortableTransferJob secretly held a QEventLoopLocker, quitting could hang or
    // interfere with event loop termination. Verify clean immediate exit.
    bool timerFired = false;
    QTimer::singleShot(0, &app, [&]() {
        timerFired = true;
        app.quit();
    });
    const int rc = app.exec();
    PTJ_CHECK(rc == 0);
    PTJ_CHECK(timerFired);
}

void testPortableCommonHelpers()
{
    // validNewName
    PTJ_CHECK(validNewName(QStringLiteral("document.pdf")));
    PTJ_CHECK(!validNewName(QString()));
    PTJ_CHECK(!validNewName(QStringLiteral("   ")));
    PTJ_CHECK(!validNewName(QStringLiteral(".")));
    PTJ_CHECK(!validNewName(QStringLiteral("..")));
    PTJ_CHECK(!validNewName(QStringLiteral("a/b")));

    // siblingUrlWithName
    const QUrl fileUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/folder/old.txt"));
    PTJ_CHECK(siblingUrlWithName(fileUrl, QStringLiteral("new.txt"))
              == QUrl::fromLocalFile(QStringLiteral("/tmp/folder/new.txt")));
    const QUrl dirWithSlash = QUrl::fromLocalFile(QStringLiteral("/tmp/folder/sub/"));
    PTJ_CHECK(siblingUrlWithName(dirWithSlash, QStringLiteral("renamed"))
              == QUrl::fromLocalFile(QStringLiteral("/tmp/folder/renamed")));

    // normalizedUrl / sameLocation / parentDirectoryUrl
    const QUrl u1 = QUrl::fromLocalFile(QStringLiteral("/tmp/a/b/"));
    const QUrl u2 = QUrl::fromLocalFile(QStringLiteral("/tmp/a/b"));
    PTJ_CHECK(sameLocation(u1, u2));
    PTJ_CHECK(parentDirectoryUrl(u2) == QUrl::fromLocalFile(QStringLiteral("/tmp/a")));

    // formatFileSize
    PTJ_CHECK(formatFileSize(0, true) == QStringLiteral("—"));
    PTJ_CHECK(formatFileSize(-1, false) == QStringLiteral("—"));
    PTJ_CHECK(formatFileSize(512, false).endsWith(QStringLiteral(" B")));
    PTJ_CHECK(formatFileSize(2048, false).endsWith(QStringLiteral(" KiB")));

    // trLocal / isPolish
    const QString translated = trLocal("Polski", "English");
    PTJ_CHECK(translated == (isPolish() ? QStringLiteral("Polski") : QStringLiteral("English")));

    // FileOperationContext::create
    const QList<QUrl> srcs = {
        QUrl::fromLocalFile(QStringLiteral("/tmp/src/f1.txt")),
        QUrl::fromLocalFile(QStringLiteral("/tmp/src/f2.txt")),
    };
    const QUrl dest = QUrl::fromLocalFile(QStringLiteral("/tmp/dst"));
    const FileOperationContext ctx = FileOperationContext::create(FileOperationType::Copy, srcs, dest);
    PTJ_CHECK(ctx.type == FileOperationType::Copy);
    PTJ_CHECK(ctx.sources.size() == 2);
    PTJ_CHECK(ctx.destinationDirectory == dest);
    PTJ_CHECK(ctx.sourceParentDirectories.size() == 1);
    PTJ_CHECK(ctx.sourceParentDirectories.contains(QUrl::fromLocalFile(QStringLiteral("/tmp/src"))));
}

void testMergeMetadataAndSafeRetryGuards()
{
    // 1. No publication and no recovery requirement
    {
        TransferOutcomeFlags clean;
        PTJ_CHECK(!clean.hasPartialPublication());
        PTJ_CHECK(!clean.blocksSafeRetry());
    }
    // 2. Published destination
    {
        TransferOutcomeFlags f;
        f.destinationPublished = true;
        PTJ_CHECK(!f.hasPartialPublication());
        PTJ_CHECK(f.blocksSafeRetry());
    }
    // 3. Replaced destination
    {
        TransferOutcomeFlags f;
        f.destinationReplaced = true;
        PTJ_CHECK(!f.hasPartialPublication());
        PTJ_CHECK(f.blocksSafeRetry());
    }
    // 4. Partial move
    {
        TransferOutcomeFlags f;
        f.partialMoveOutcome = true;
        PTJ_CHECK(f.hasPartialPublication());
        PTJ_CHECK(f.blocksSafeRetry());
    }
    // 5. Positive batch published count
    {
        TransferOutcomeFlags f;
        f.batchPublishedCount = 1;
        PTJ_CHECK(f.hasPartialPublication());
        PTJ_CHECK(f.blocksSafeRetry());
    }
    // 6. Partially completed batch
    {
        TransferOutcomeFlags f;
        f.batchPartiallyCompleted = true;
        PTJ_CHECK(f.hasPartialPublication());
        PTJ_CHECK(f.blocksSafeRetry());
    }
    // 7. Merge retry blocked
    {
        TransferOutcomeFlags f;
        f.mergeRetryBlocked = true;
        PTJ_CHECK(!f.hasPartialPublication());
        PTJ_CHECK(f.blocksSafeRetry());
    }
    // 8. Merge recovery required with zero known published items (fail-closed)
    {
        TransferOutcomeFlags f;
        f.mergeRecoveryRequired = true;
        f.mergeState = QStringLiteral("RecoveryRequired");
        f.batchPublishedCount = 0;
        f.mergeReplacedCount = 0;
        PTJ_CHECK(!f.hasPartialPublication());
        PTJ_CHECK(f.blocksSafeRetry());
    }
    // 9. Merge counts & replaced count metadata
    {
        TransferOutcomeFlags f;
        f.mergeState = QStringLiteral("CompletedWithSkips");
        f.mergeSkippedCount = 3;
        f.mergeReplacedCount = 2;
        PTJ_CHECK(f.hasPartialPublication());
        PTJ_CHECK(f.blocksSafeRetry());
        PTJ_CHECK(f.mergeSkippedCount == 3);
        PTJ_CHECK(f.mergeReplacedCount == 2);
    }
    // 10. Live decision-phase enter/exit and automatic clearing on terminalization
    {
        FakePortableJob job(TransferJobCapability::Killable);
        job.setAutoDelete(false);
        QList<bool> phaseEvents;
        QObject::connect(&job, &PortableTransferJob::decisionPhaseChanged,
                         [&](PortableTransferJob *, bool inDecision) { phaseEvents.append(inDecision); });

        PTJ_CHECK(!job.isInDecisionPhase());
        job.start();
        job.reportInDecisionPhase(true);
        job.reportInDecisionPhase(true); // duplicate ignored
        PTJ_CHECK(job.isInDecisionPhase());
        job.reportInDecisionPhase(false);
        PTJ_CHECK(!job.isInDecisionPhase());
        job.reportInDecisionPhase(true);
        PTJ_CHECK(job.isInDecisionPhase());

        // Terminalization must clear active decision phase before finished()
        bool phaseFalseInFinished = false;
        QObject::connect(&job, &PortableTransferJob::finished, [&](PortableTransferJob *j) {
            phaseFalseInFinished = !j->isInDecisionPhase();
        });
        job.completeOk();
        PTJ_CHECK(phaseFalseInFinished);
        PTJ_CHECK(!job.isInDecisionPhase());
        // Post-terminal mutation of decision phase must be ignored
        job.reportInDecisionPhase(true);
        PTJ_CHECK(!job.isInDecisionPhase());
        PTJ_CHECK(phaseEvents == QList<bool>({true, false, true, false}));
    }
}

void testPausedCancelAndSyncCompletions()
{
    // 1. Paused -> requestCancel() -> finishCancelled()
    {
        FakePortableJob job(TransferJobCapability::Killable | TransferJobCapability::Suspendable);
        job.setAutoDelete(false);
        int finishedCount = 0;
        QObject::connect(&job, &PortableTransferJob::finished, [&] { ++finishedCount; });

        job.start();
        PTJ_CHECK(job.suspend());
        PTJ_CHECK(job.state() == TransferJobState::Paused);
        PTJ_CHECK(job.requestCancel());
        PTJ_CHECK(job.state() == TransferJobState::Cancelling);
        PTJ_CHECK(!job.resume()); // cannot resume while Cancelling
        PTJ_CHECK(finishedCount == 0);

        job.completeCancelled(QStringLiteral("cancelled from paused"));
        PTJ_CHECK(finishedCount == 1);
        PTJ_CHECK(job.state() == TransferJobState::Cancelled);
        PTJ_CHECK(!job.resume());
    }

    // 2. Synchronous completion inside doStart()
    {
        QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable);
        int finishedCount = 0;
        job->onStart = [](FakePortableJob *self) {
            self->completeOk();
        };
        QObject::connect(job, &PortableTransferJob::finished, [&] { ++finishedCount; });
        job->start();
        PTJ_CHECK(finishedCount == 1);
        PTJ_CHECK(job->state() == TransferJobState::Completed);
        flushEvents();
        PTJ_CHECK(job.isNull());
    }

    // 3. Synchronous completion inside doCancel()
    {
        QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable);
        int finishedCount = 0;
        job->onCancel = [](FakePortableJob *self) {
            self->completeCancelled(QStringLiteral("sync cancel"));
        };
        QObject::connect(job, &PortableTransferJob::finished, [&] { ++finishedCount; });
        job->start();
        PTJ_CHECK(job->requestCancel());
        PTJ_CHECK(finishedCount == 1);
        PTJ_CHECK(job->state() == TransferJobState::Cancelled);
        flushEvents();
        PTJ_CHECK(job.isNull());
    }
}

void testReentrancyDeletionAndImmutability()
{
    // 4. stateChanged(Running) observer synchronously cancels before doStart()
    {
        FakePortableJob job(TransferJobCapability::Killable);
        job.setAutoDelete(false);
        job.onCancel = [](FakePortableJob *self) {
            self->completeCancelled(QStringLiteral("cancelled in stateChanged"));
        };
        QObject::connect(&job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *j, TransferJobState s) {
                             if (s == TransferJobState::Running) {
                                 j->requestCancel();
                             }
                         });
        job.start();
        PTJ_CHECK(job.startCalls == 0); // doStart() must NOT run after synchronous cancel
        PTJ_CHECK(job.state() == TransferJobState::Cancelled);
    }

    // 5. stateChanged(Running) observer synchronously deletes the job (ASan/UBSan test)
    {
        QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable);
        int startCallsObserved = 0;
        job->onStart = [&](FakePortableJob *) { ++startCallsObserved; };
        QObject::connect(job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *j, TransferJobState s) {
                             if (s == TransferJobState::Running) {
                                 delete j;
                             }
                         });
        job->start();
        PTJ_CHECK(job.isNull());
        PTJ_CHECK(startCallsObserved == 0);
    }

    // 6. Terminal stateChanged() observer synchronously deletes the job (no UAF in finished/deleteLater)
    {
        QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable);
        int finishedCount = 0;
        QObject::connect(job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *j, TransferJobState s) {
                             if (s == TransferJobState::Completed) {
                                 delete j;
                             }
                         });
        QObject::connect(job, &PortableTransferJob::finished, [&] { ++finishedCount; });
        job->start();
        job->completeOk();
        PTJ_CHECK(job.isNull());
        PTJ_CHECK(finishedCount == 0); // object ceased to exist before finished()
    }

    // 7. finished() observer synchronously deletes the job (no double-free / deleteLater on freed object)
    {
        QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable);
        int finishedCount = 0;
        QObject::connect(job, &PortableTransferJob::finished, [&](PortableTransferJob *j) {
            ++finishedCount;
            delete j;
        });
        job->start();
        job->completeOk();
        PTJ_CHECK(finishedCount == 1);
        PTJ_CHECK(job.isNull());
        flushEvents();
    }

    // 8. Parent QObject destruction invalidates QPointer cleanly
    {
        QPointer<FakePortableJob> job;
        {
            QObject parentOwner;
            job = new FakePortableJob(TransferJobCapability::Killable, &parentOwner);
            job->setAutoDelete(false);
            job->start();
            PTJ_CHECK(!job.isNull());
        }
        PTJ_CHECK(job.isNull());
    }

    // 9. Reentrant terminalization inside finished() is ignored
    {
        FakePortableJob job(TransferJobCapability::Killable);
        job.setAutoDelete(false);
        int finishedCount = 0;
        QObject::connect(&job, &PortableTransferJob::finished, [&](PortableTransferJob *) {
            ++finishedCount;
            job.completeFailed(TransferErrorCode::CannotWrite, QStringLiteral("reentrant"));
            job.completeCancelled(QStringLiteral("reentrant"));
        });
        job.start();
        job.completeOk();
        PTJ_CHECK(finishedCount == 1);
        PTJ_CHECK(job.state() == TransferJobState::Completed);
        PTJ_CHECK(job.errorCode() == TransferErrorCode::NoError);
    }

    // 10 & 11. Terminal outcome metadata & post-start capability immutability
    {
        FakePortableJob job(TransferJobCapability::Killable);
        job.setAutoDelete(false);
        TransferOutcomeFlags initial;
        initial.destinationPublished = true;
        initial.mergeState = QStringLiteral("Completed");
        job.reportOutcomeFlags(initial);

        job.start();
        // Attempting to change capabilities after start must be ignored
        job.reportCapabilities(TransferJobCapability::None);
        PTJ_CHECK(job.hasCapability(TransferJobCapability::Killable));

        job.completeOk();

        // Attempting to mutate outcome metadata after terminal state must be ignored
        TransferOutcomeFlags mutated;
        mutated.destinationPublished = false;
        mutated.mergeRecoveryRequired = true;
        mutated.mergeState = QStringLiteral("Tampered");
        job.reportOutcomeFlags(mutated);
        PTJ_CHECK(job.outcomeFlags().destinationPublished);
        PTJ_CHECK(!job.outcomeFlags().mergeRecoveryRequired);
        PTJ_CHECK(job.outcomeFlags().mergeState == QStringLiteral("Completed"));

        // Attempting to mutate capabilities after terminal state must be ignored
        job.reportCapabilities(TransferJobCapability::Suspendable);
        PTJ_CHECK(!job.hasCapability(TransferJobCapability::Suspendable));
    }
}

void testR2SignalReentrancyAndSyncDeletions()
{
    // 1. Delete job synchronously during stateChanged(Cancelling)
    {
        QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable);
        int cancelCallsObserved = 0;
        job->onCancel = [&](FakePortableJob *) { ++cancelCallsObserved; };
        QObject::connect(job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *j, TransferJobState s) {
                             if (s == TransferJobState::Cancelling) {
                                 delete j;
                             }
                         });
        job->start();
        PTJ_CHECK(job->requestCancel());
        PTJ_CHECK(job.isNull());
        PTJ_CHECK(cancelCallsObserved == 0);
    }

    // 2. Complete job synchronously during stateChanged(Cancelling)
    {
        FakePortableJob job(TransferJobCapability::Killable);
        job.setAutoDelete(false);
        int finishedCount = 0;
        QObject::connect(&job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *, TransferJobState s) {
                             if (s == TransferJobState::Cancelling) {
                                 job.completeOk();
                             }
                         });
        QObject::connect(&job, &PortableTransferJob::finished, [&] { ++finishedCount; });
        job.start();
        PTJ_CHECK(job.requestCancel());
        PTJ_CHECK(job.cancelCalls == 0);
        PTJ_CHECK(finishedCount == 1);
        PTJ_CHECK(job.state() == TransferJobState::Completed);
    }

    // 3. Delete job synchronously during stateChanged(Paused)
    {
        QPointer<FakePortableJob> job = new FakePortableJob(
            TransferJobCapability::Killable | TransferJobCapability::Suspendable);
        int suspendedSignals = 0;
        QObject::connect(job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *j, TransferJobState s) {
                             if (s == TransferJobState::Paused) {
                                 delete j;
                             }
                         });
        QObject::connect(job, &PortableTransferJob::suspendedChanged,
                         [&] { ++suspendedSignals; });
        job->start();
        PTJ_CHECK(!job->suspend());
        PTJ_CHECK(job.isNull());
        PTJ_CHECK(suspendedSignals == 0);
    }

    // 4. Delete job synchronously during stateChanged(Running) triggered by resume()
    {
        QPointer<FakePortableJob> job = new FakePortableJob(
            TransferJobCapability::Killable | TransferJobCapability::Suspendable);
        int suspendedSignals = 0;
        bool armedForResume = false;
        QObject::connect(job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *j, TransferJobState s) {
                             if (armedForResume && s == TransferJobState::Running) {
                                 delete j;
                             }
                         });
        QObject::connect(job, &PortableTransferJob::suspendedChanged,
                         [&] { ++suspendedSignals; });
        job->start();
        PTJ_CHECK(job->suspend());
        PTJ_CHECK(suspendedSignals == 1);
        armedForResume = true;
        PTJ_CHECK(!job->resume());
        PTJ_CHECK(job.isNull());
        PTJ_CHECK(suspendedSignals == 1); // no suspendedChanged(false) after deletion
    }

    // 5. Reentrantly request cancellation during suspend() and resume() stateChanged()
    {
        FakePortableJob job(TransferJobCapability::Killable | TransferJobCapability::Suspendable);
        job.setAutoDelete(false);
        QList<bool> suspendedSignals;
        QObject::connect(&job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *j, TransferJobState s) {
                             if (s == TransferJobState::Paused) {
                                 j->requestCancel();
                             }
                         });
        QObject::connect(&job, &PortableTransferJob::suspendedChanged,
                         [&](PortableTransferJob *, bool s) { suspendedSignals.append(s); });
        job.start();
        PTJ_CHECK(!job.suspend());
        PTJ_CHECK(job.state() == TransferJobState::Cancelling);
        PTJ_CHECK(suspendedSignals.isEmpty()); // no contradictory suspendedChanged(true)
        job.completeCancelled();
    }
    {
        FakePortableJob job(TransferJobCapability::Killable | TransferJobCapability::Suspendable);
        job.setAutoDelete(false);
        QList<bool> suspendedSignals;
        bool armedForResume = false;
        QObject::connect(&job, &PortableTransferJob::stateChanged,
                         [&](PortableTransferJob *j, TransferJobState s) {
                             if (armedForResume && s == TransferJobState::Running) {
                                 j->requestCancel();
                             }
                         });
        QObject::connect(&job, &PortableTransferJob::suspendedChanged,
                         [&](PortableTransferJob *, bool s) { suspendedSignals.append(s); });
        job.start();
        PTJ_CHECK(job.suspend());
        PTJ_CHECK(suspendedSignals == QList<bool>({true}));
        armedForResume = true;
        PTJ_CHECK(!job.resume());
        PTJ_CHECK(job.state() == TransferJobState::Cancelling);
        PTJ_CHECK(suspendedSignals == QList<bool>({true})); // no contradictory suspendedChanged(false)
        job.completeCancelled();
    }

    // 6. Delete job synchronously during totalBytesChanged
    {
        QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable);
        int percentSignals = 0;
        QObject::connect(job, &PortableTransferJob::totalBytesChanged,
                         [&](PortableTransferJob *j, qulonglong) { delete j; });
        QObject::connect(job, &PortableTransferJob::percentChanged,
                         [&] { ++percentSignals; });
        job->start();
        job->reportTotalBytes(1000);
        PTJ_CHECK(job.isNull());
        PTJ_CHECK(percentSignals == 0);
    }

    // 7. Delete job synchronously during processedBytesChanged
    {
        QPointer<FakePortableJob> job = new FakePortableJob(TransferJobCapability::Killable);
        int percentSignals = 0;
        job->start();
        job->reportTotalBytes(1000);
        QObject::connect(job, &PortableTransferJob::processedBytesChanged,
                         [&](PortableTransferJob *j, qulonglong) { delete j; });
        QObject::connect(job, &PortableTransferJob::percentChanged,
                         [&] { ++percentSignals; });
        job->reportProcessedBytes(500);
        PTJ_CHECK(job.isNull());
        PTJ_CHECK(percentSignals == 0);
    }

    // 8. Reentrantly finalize job during processedBytesChanged (no post-terminal percentChanged)
    {
        FakePortableJob job(TransferJobCapability::Killable);
        job.setAutoDelete(false);
        int percentSignals = 0;
        job.start();
        job.reportTotalBytes(1000);
        QObject::connect(&job, &PortableTransferJob::processedBytesChanged,
                         [&](PortableTransferJob *, qulonglong) { job.completeOk(); });
        QObject::connect(&job, &PortableTransferJob::percentChanged,
                         [&] { ++percentSignals; });
        job.reportProcessedBytes(500);
        PTJ_CHECK(job.isTerminal());
        PTJ_CHECK(job.state() == TransferJobState::Completed);
        PTJ_CHECK(percentSignals == 0);
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testBasicLifecycleAndAutoDelete();
    testManualOwnershipAndErrorCompletion();
    testAsyncCancellationWithCleanupBeforeFinished();
    testCancelBeforeStartAndRejectedCancel();
    testSuspendResumeAndCapabilities();
    testProgressSpeedAndOutcomeFlags();
    testNoHiddenEventLoopLocker(app);
    testPortableCommonHelpers();
    testMergeMetadataAndSafeRetryGuards();
    testPausedCancelAndSyncCompletions();
    testReentrancyDeletionAndImmutability();
    testR2SignalReentrancyAndSyncDeletions();

    if (g_failures > 0) {
        std::fprintf(stderr, "portable-transfer-job-test: FAILED (%d/%d checks failed)\n",
                     g_failures, g_checks);
        return 1;
    }

    std::printf("portable-transfer-job-test: %d checks passed\n", g_checks);
    return 0;
}

#include "portable-transfer-job-test.moc"
