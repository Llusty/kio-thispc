/*
 * Isolated KDE compatibility adapter tests for KJobTransferAdapter
 * (0.46.0 Stage 2A.2-1).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "kjobtransferadapter.h"

#include <QHash>
#include "transferbandwidthlimiter.h"

#include <KIO/CopyJob>
#include <KIO/FileCopyJob>
#include <KIO/Global>
#include <KJob>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void checkImpl(bool cond, const char *expr, int line)
{
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL [line %d]: %s\n", line, expr);
    }
}

#define CHECK(expr) checkImpl(static_cast<bool>(expr), #expr, __LINE__)

void pumpEvents(int ms = 15)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Deterministic fake KJob for isolated adapter testing.
class FakeKJob : public KJob
{
    Q_OBJECT

public:
    explicit FakeKJob(Capabilities caps = Killable, QObject *parent = nullptr)
        : KJob(parent)
    {
        setCapabilities(caps);
    }

    ~FakeKJob() override
    {
        if (onDestroyedHook) {
            onDestroyedHook();
        }
    }

    int startCalls = 0;
    int killCalls = 0;
    int suspendCalls = 0;
    int resumeCalls = 0;
    bool suspendResult = true;
    bool resumeResult = true;
    bool killResult = true;
    bool syncCompleteOnStart = false;
    bool syncFinishOnKill = true;
    int syncError = 0;
    QString syncErrorText;
    std::function<void()> onStartHook;
    std::function<void()> onDestroyedHook;

    void start() override
    {
        ++startCalls;
        if (onStartHook) {
            onStartHook();
        }
        if (syncCompleteOnStart) {
            finishNow(syncError, syncErrorText);
        }
    }

    void finishNow(int err = 0, const QString &errText = {})
    {
        setError(err);
        setErrorText(errText);
        emitResult();
    }

    void setErrorWithoutEmit(int err, const QString &errText = {})
    {
        setError(err);
        setErrorText(errText);
    }

    void reportTotalBytes(qulonglong bytes) { setTotalAmount(KJob::Bytes, bytes); }
    void reportProcessedBytes(qulonglong bytes) { setProcessedAmount(KJob::Bytes, bytes); }
    void reportTotalFiles(qulonglong files) { setTotalAmount(KJob::Files, files); }
    void reportProcessedFiles(qulonglong files) { setProcessedAmount(KJob::Files, files); }
    void reportPercent(unsigned long p) { setPercent(p); }
    void reportSpeed(unsigned long bps) { emitSpeed(bps); }
    void reportDescription(const QString &title,
                           const QPair<QString, QString> &f1,
                           const QPair<QString, QString> &f2)
    {
        Q_EMIT description(this, title, f1, f2);
    }
    void updateCapabilities(Capabilities caps) { setCapabilities(caps); }

protected:
    bool doKill() override
    {
        ++killCalls;
        if (!killResult) {
            return false;
        }
        if (syncFinishOnKill) {
            setError(KJob::KilledJobError);
            setErrorText(QStringLiteral("Killed by test"));
        }
        return true;
    }

    bool doSuspend() override
    {
        ++suspendCalls;
        return suspendResult;
    }

    bool doResume() override
    {
        ++resumeCalls;
        return resumeResult;
    }
};

// Fake KJob that additionally exposes Q_INVOKABLE setBandwidthLimiter.
class LimiterFakeKJob : public FakeKJob
{
    Q_OBJECT

public:
    explicit LimiterFakeKJob(Capabilities caps = Killable | Suspendable, QObject *parent = nullptr)
        : FakeKJob(caps, parent)
    {
    }

    std::shared_ptr<TransferBandwidthLimiter> receivedLimiter;
    quint64 receivedTaskId = 0;
    int limiterCalls = 0;

    Q_INVOKABLE void setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter,
                                         quint64 taskId = 0)
    {
        ++limiterCalls;
        receivedLimiter = std::move(limiter);
        receivedTaskId = taskId;
    }
};

// 1-2: Ownership validation (safe construction + rejection of unsafe arrangements).
void testOwnershipValidation()
{
    QString reason;

    // Null job rejected
    CHECK(KJobTransferAdapter::create(nullptr, {}, nullptr, &reason) == nullptr);
    CHECK(!reason.isEmpty());

    // Externally parented KJob rejected
    QObject externalParent;
    auto *parentedJob = new FakeKJob(KJob::Killable, &externalParent);
    reason.clear();
    CHECK(KJobTransferAdapter::create(parentedJob, {}, nullptr, &reason) == nullptr);
    CHECK(reason.contains(QStringLiteral("parent")));

    // Already-errored KJob rejected
    auto *erroredJob = new FakeKJob(KJob::Killable);
    erroredJob->setErrorWithoutEmit(KIO::ERR_DISK_FULL, QStringLiteral("Disk full"));
    reason.clear();
    CHECK(KJobTransferAdapter::create(erroredJob, {}, nullptr, &reason) == nullptr);
    CHECK(reason.contains(QStringLiteral("error")));
    delete erroredJob;

    // Different-thread KJob rejected
    QThread workerThread;
    workerThread.start();
    auto *foreignJob = new FakeKJob(KJob::Killable);
    foreignJob->moveToThread(&workerThread);
    reason.clear();
    CHECK(KJobTransferAdapter::create(foreignJob, {}, nullptr, &reason) == nullptr);
    CHECK(reason.contains(QStringLiteral("thread")));
    QMetaObject::invokeMethod(foreignJob, [foreignJob]() { delete foreignJob; }, Qt::BlockingQueuedConnection);
    workerThread.quit();
    workerThread.wait();

    // Valid owned-job construction + double-wrap rejection
    auto *validJob = new FakeKJob(KJob::Killable);
    reason.clear();
    auto *adapter = KJobTransferAdapter::create(validJob, {}, nullptr, &reason);
    CHECK(adapter != nullptr);
    CHECK(reason.isEmpty());
    CHECK(!validJob->isAutoDelete());
    CHECK(adapter->wrappedJob() == validJob);

    // Double wrapping same job rejected
    reason.clear();
    CHECK(KJobTransferAdapter::create(validJob, {}, nullptr, &reason) == nullptr);
    CHECK(reason.contains(QStringLiteral("already owned")));

    delete adapter;
}

// 3-4: ControllerStarts (1 start) vs FactoryStarts (0 starts).
void testStartModes()
{
    // ControllerStarts
    auto *ctrlJob = new FakeKJob(KJob::Killable);
    KJobTransferAdapterOptions ctrlOpts;
    ctrlOpts.startMode = KJobStartMode::ControllerStarts;
    auto *ctrlAdapter = KJobTransferAdapter::create(ctrlJob, ctrlOpts);
    CHECK(ctrlAdapter != nullptr);
    ctrlAdapter->setAutoDelete(false);
    CHECK(ctrlJob->startCalls == 0);
    ctrlAdapter->start();
    ctrlAdapter->start(); // duplicate start ignored
    CHECK(ctrlJob->startCalls == 1);
    CHECK(ctrlAdapter->state() == TransferJobState::Running);
    ctrlJob->finishNow(0);
    CHECK(ctrlAdapter->state() == TransferJobState::Completed);
    delete ctrlAdapter;

    // FactoryStarts
    auto *facJob = new FakeKJob(KJob::Killable);
    KJobTransferAdapterOptions facOpts;
    facOpts.startMode = KJobStartMode::FactoryStarts;
    auto *facAdapter = KJobTransferAdapter::create(facJob, facOpts);
    CHECK(facAdapter != nullptr);
    facAdapter->setAutoDelete(false);
    facAdapter->start();
    CHECK(facJob->startCalls == 0);
    CHECK(facAdapter->state() == TransferJobState::Running);
    facJob->finishNow(0);
    CHECK(facAdapter->state() == TransferJobState::Completed);
    delete facAdapter;
}

// 5: FactoryStarts job completes after adapter construction but before adapter->start().
void testBufferedCompletionBeforeStart()
{
    auto *facJob = new FakeKJob(KJob::Killable);
    facJob->setProperty("thispcDestinationPublished", true);
    KJobTransferAdapterOptions facOpts;
    facOpts.startMode = KJobStartMode::FactoryStarts;
    auto *adapter = KJobTransferAdapter::create(facJob, facOpts);
    CHECK(adapter != nullptr);
    adapter->setAutoDelete(false);

    int finishedCount = 0;
    QObject::connect(adapter, &PortableTransferJob::finished, [&](PortableTransferJob *) {
        ++finishedCount;
    });

    // KJob completes before adapter->start()
    facJob->finishNow(0);
    CHECK(finishedCount == 0);
    CHECK(adapter->state() == TransferJobState::NotStarted);

    // Calling start() delivers the buffered terminal result cleanly
    adapter->start();
    CHECK(finishedCount == 1);
    CHECK(adapter->state() == TransferJobState::Completed);
    CHECK(adapter->outcomeFlags().destinationPublished);
    CHECK(adapter->outcomeFlags().blocksSafeRetry());
    delete adapter;
}

// 6-7-8-11: Synchronous completion, asynchronous completion, success & duplicate terminal prevention.
void testSyncAndAsyncCompletion()
{
    // Synchronous completion inside start()
    auto *syncJob = new FakeKJob(KJob::Killable);
    syncJob->syncCompleteOnStart = true;
    auto *syncAdapter = KJobTransferAdapter::create(syncJob);
    CHECK(syncAdapter != nullptr);
    syncAdapter->setAutoDelete(false);

    int syncFinished = 0;
    QObject::connect(syncAdapter, &PortableTransferJob::finished, [&](PortableTransferJob *) {
        ++syncFinished;
    });
    syncAdapter->start();
    CHECK(syncFinished == 1);
    CHECK(syncAdapter->state() == TransferJobState::Completed);
    CHECK(syncAdapter->errorCode() == TransferErrorCode::NoError);

    // Duplicate terminal callback prevention
    syncJob->finishNow(KIO::ERR_DISK_FULL, QStringLiteral("Duplicate"));
    CHECK(syncFinished == 1);
    CHECK(syncAdapter->state() == TransferJobState::Completed);
    delete syncAdapter;

    // Asynchronous completion
    auto *asyncJob = new FakeKJob(KJob::Killable);
    auto *asyncAdapter = KJobTransferAdapter::create(asyncJob);
    CHECK(asyncAdapter != nullptr);
    asyncAdapter->setAutoDelete(false);

    int asyncFinished = 0;
    QObject::connect(asyncAdapter, &PortableTransferJob::finished, [&](PortableTransferJob *) {
        ++asyncFinished;
    });
    asyncAdapter->start();
    CHECK(asyncAdapter->state() == TransferJobState::Running);
    QTimer::singleShot(5, asyncJob, [asyncJob]() { asyncJob->finishNow(0); });
    pumpEvents(30);
    CHECK(asyncFinished == 1);
    CHECK(asyncAdapter->state() == TransferJobState::Completed);
    delete asyncAdapter;
}

// 9-10: Error mapping, raw backend code preservation, and failure-after-cancel-request accuracy.
void testErrorAndCancelMapping()
{
    CHECK(KJobTransferAdapter::mapKJobError(0) == TransferErrorCode::NoError);
    CHECK(KJobTransferAdapter::mapKJobError(KJob::KilledJobError) == TransferErrorCode::Cancelled);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_USER_CANCELED) == TransferErrorCode::Cancelled);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_DOES_NOT_EXIST) == TransferErrorCode::DoesNotExist);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_FILE_ALREADY_EXIST) == TransferErrorCode::AlreadyExists);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_ACCESS_DENIED) == TransferErrorCode::CannotRead);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_WRITE_ACCESS_DENIED) == TransferErrorCode::CannotWrite);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_CANNOT_OPEN_FOR_READING) == TransferErrorCode::CannotOpenForReading);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_CANNOT_OPEN_FOR_WRITING) == TransferErrorCode::CannotOpenForWriting);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_CANNOT_MKDIR) == TransferErrorCode::CannotMkdir);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_CANNOT_RENAME) == TransferErrorCode::CannotRename);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_CANNOT_CHMOD) == TransferErrorCode::CannotChmod);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_DISK_FULL) == TransferErrorCode::DiskFull);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_IS_DIRECTORY) == TransferErrorCode::IsDirectory);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_UNSUPPORTED_ACTION) == TransferErrorCode::UnsupportedAction);
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_UNSUPPORTED_PROTOCOL) == TransferErrorCode::UnsupportedProtocol);
    CHECK(KJobTransferAdapter::mapKJobError(99999) == TransferErrorCode::BackendError);

    // User cancellation via KIO::ERR_USER_CANCELED
    auto *cancelJob = new FakeKJob(KJob::Killable);
    auto *cancelAdapter = KJobTransferAdapter::create(cancelJob);
    cancelAdapter->setAutoDelete(false);
    cancelAdapter->start();
    cancelJob->finishNow(KIO::ERR_USER_CANCELED, QStringLiteral("User cancelled"));
    CHECK(cancelAdapter->state() == TransferJobState::Cancelled);
    CHECK(cancelAdapter->errorCode() == TransferErrorCode::Cancelled);
    CHECK(cancelAdapter->outcomeFlags().backendErrorCode == KIO::ERR_USER_CANCELED);
    delete cancelAdapter;

    // Unrelated failure after cancel request MUST NOT be misclassified as Cancelled
    auto *failJob = new FakeKJob(KJob::Killable);
    KJobTransferAdapterOptions opts;
    opts.cancelHandler = [](KJob *) { return true; }; // async cancel accepted
    auto *failAdapter = KJobTransferAdapter::create(failJob, opts);
    failAdapter->setAutoDelete(false);
    failAdapter->start();
    CHECK(failAdapter->requestCancel());
    CHECK(failAdapter->state() == TransferJobState::Cancelling);
    failJob->finishNow(KIO::ERR_DISK_FULL, QStringLiteral("No space left on device"));
    CHECK(failAdapter->state() == TransferJobState::Failed);
    CHECK(failAdapter->errorCode() == TransferErrorCode::DiskFull);
    CHECK(failAdapter->errorText() == QStringLiteral("No space left on device"));
    CHECK(failAdapter->outcomeFlags().backendErrorCode == KIO::ERR_DISK_FULL);
    delete failAdapter;
}

// 12-13-14: Unexpected KJob destruction, adapter destruction during active job,
// and strict destruction order (wrapped KJob destroyed BEFORE adapter::destroyed).
void testLifetimeAndDestructionOrder()
{
    // 12: KJob unexpectedly destroyed externally while running
    auto *extJob = new FakeKJob(KJob::Killable);
    auto *extAdapter = KJobTransferAdapter::create(extJob);
    extAdapter->setAutoDelete(false);
    extAdapter->start();
    delete extJob; // hostile external deletion
    CHECK(extAdapter->wrappedJob() == nullptr);
    CHECK(extAdapter->state() == TransferJobState::Failed);
    CHECK(extAdapter->errorCode() == TransferErrorCode::InternalError);
    CHECK(extAdapter->outcomeFlags().blocksSafeRetry());
    delete extAdapter; // must not double-free

    // 13: Adapter destroyed while job is active (AUD-2A2-01: detached retainer,
    // non-blocking, cleanupHandle completes ONLY after wrapped KJob is destroyed)
    bool activeJobDestroyed = false;
    int activeJobKillCalls = 0;
    auto *activeJob = new FakeKJob(KJob::Killable);
    activeJob->syncFinishOnKill = false;
    activeJob->onDestroyedHook = [&]() {
        activeJobDestroyed = true;
        activeJobKillCalls = activeJob->killCalls;
    };
    auto *activeAdapter = KJobTransferAdapter::create(activeJob);
    activeAdapter->setAutoDelete(false);
    auto activeCleanup = activeAdapter->cleanupHandle();
    bool activeCleanupSignalled = false;
    QObject::connect(activeCleanup.get(), &PortableTransferCleanupHandle::cleanupCompleted,
                     [&]() { activeCleanupSignalled = true; });
    activeAdapter->start();
    delete activeAdapter;
    CHECK(!activeCleanup->isCleanupComplete());
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() >= 1);
    pumpEvents(25);
    CHECK(activeJobDestroyed);
    CHECK(activeJobKillCalls == 1);
    CHECK(activeCleanup->isCleanupComplete());
    CHECK(activeCleanupSignalled);
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 0);

    // 14: Deferred autoDelete destruction order — wrapped KJob MUST be destroyed
    // BEFORE QObject::destroyed(adapter) fires!
    std::vector<QString> order;
    auto *ordJob = new FakeKJob(KJob::Killable);
    ordJob->onDestroyedHook = [&]() { order.push_back(QStringLiteral("kjob")); };
    QPointer<KJobTransferAdapter> ordAdapter = KJobTransferAdapter::create(ordJob);
    CHECK(ordAdapter->autoDelete());
    QObject::connect(ordAdapter.data(), &QObject::destroyed, [&]() {
        order.push_back(QStringLiteral("adapter"));
    });
    ordAdapter->start();
    ordJob->finishNow(0);
    pumpEvents(25);
    CHECK(ordAdapter.isNull());
    CHECK(order.size() == 2);
    if (order.size() == 2) {
        CHECK(order[0] == QStringLiteral("kjob"));
        CHECK(order[1] == QStringLiteral("adapter"));
    }
}

// 15-16-17: Source/target context, byte/file progress, percentage, and speed.
void testProgressSpeedAndContext()
{
    auto *job = new FakeKJob(KJob::Killable);
    auto *adapter = KJobTransferAdapter::create(job);
    adapter->setAutoDelete(false);

    qulonglong lastTotalBytes = 0;
    qulonglong lastProcessedBytes = 0;
    qulonglong lastTotalFiles = 0;
    qulonglong lastProcessedFiles = 0;
    unsigned long lastPercent = 0;
    unsigned long lastSpeed = 0;
    QUrl lastSrc;
    QUrl lastDst;

    QObject::connect(adapter, &PortableTransferJob::totalBytesChanged,
                     [&](PortableTransferJob *, qulonglong b) { lastTotalBytes = b; });
    QObject::connect(adapter, &PortableTransferJob::processedBytesChanged,
                     [&](PortableTransferJob *, qulonglong b) { lastProcessedBytes = b; });
    QObject::connect(adapter, &PortableTransferJob::totalFilesChanged,
                     [&](PortableTransferJob *, qulonglong f) { lastTotalFiles = f; });
    QObject::connect(adapter, &PortableTransferJob::processedFilesChanged,
                     [&](PortableTransferJob *, qulonglong f) { lastProcessedFiles = f; });
    QObject::connect(adapter, &PortableTransferJob::percentChanged,
                     [&](PortableTransferJob *, unsigned long p) { lastPercent = p; });
    QObject::connect(adapter, &PortableTransferJob::speedChanged,
                     [&](PortableTransferJob *, unsigned long s) { lastSpeed = s; });
    QObject::connect(adapter, &PortableTransferJob::currentItemChanged,
                     [&](PortableTransferJob *, const QUrl &s, const QUrl &d) {
                         lastSrc = s;
                         lastDst = d;
                     });

    adapter->start();
    job->reportTotalBytes(1000);
    job->reportProcessedBytes(250);
    job->reportTotalFiles(5);
    job->reportProcessedFiles(2);
    job->reportSpeed(123456);

    const QUrl srcUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/src/a.bin"));
    const QUrl dstUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/dst/a.bin"));
    job->setProperty("thispcCurrentSourceUrl", srcUrl);
    job->setProperty("thispcCurrentDestinationUrl", dstUrl);
    job->reportDescription(QStringLiteral("Copying"),
                           {QStringLiteral("Source"), QStringLiteral("/tmp/src/a.bin")},
                           {QStringLiteral("Destination"), QStringLiteral("/tmp/dst/a.bin")});

    CHECK(lastTotalBytes == 1000);
    CHECK(lastProcessedBytes == 250);
    CHECK(lastTotalFiles == 5);
    CHECK(lastProcessedFiles == 2);
    CHECK(lastPercent == 25);
    CHECK(lastSpeed == 123456);
    CHECK(lastSrc == srcUrl);
    CHECK(lastDst == dstUrl);
    CHECK(adapter->lastDescriptionTitle() == QStringLiteral("Copying"));
    CHECK(adapter->lastDescriptionField1().second == QStringLiteral("/tmp/src/a.bin"));
    CHECK(adapter->lastDescriptionField2().second == QStringLiteral("/tmp/dst/a.bin"));

    job->finishNow(0);
    delete adapter;
}

// 18-19: Pause capability accuracy + real KIO::CopyJob NEVER marked Suspendable.
void testPauseAndKioCopyJobCapability()
{
    // Opt-in NOT set -> even KJob::Suspendable is NOT advertised blindly
    auto *noOptJob = new FakeKJob(KJob::Killable | KJob::Suspendable);
    auto *noOptAdapter = KJobTransferAdapter::create(noOptJob);
    noOptAdapter->setAutoDelete(false);
    CHECK(!noOptAdapter->hasCapability(TransferJobCapability::Suspendable));
    noOptAdapter->start();
    CHECK(!noOptAdapter->suspend());
    CHECK(noOptJob->suspendCalls == 0);
    noOptJob->finishNow(0);
    delete noOptAdapter;

    // Opt-in set on verified local-style fake job -> Suspendable works
    auto *pauseJob = new FakeKJob(KJob::Killable | KJob::Suspendable);
    KJobTransferAdapterOptions pauseOpts;
    pauseOpts.allowExactPause = true;
    auto *pauseAdapter = KJobTransferAdapter::create(pauseJob, pauseOpts);
    pauseAdapter->setAutoDelete(false);
    CHECK(pauseAdapter->hasCapability(TransferJobCapability::Suspendable));
    pauseAdapter->start();
    CHECK(pauseAdapter->suspend());
    CHECK(pauseAdapter->isSuspended());
    CHECK(pauseJob->suspendCalls == 1);
    CHECK(pauseAdapter->resume());
    CHECK(!pauseAdapter->isSuspended());
    CHECK(pauseJob->resumeCalls == 1);
    pauseJob->finishNow(0);
    delete pauseAdapter;

    // Real KIO::CopyJob -> NEVER marked Suspendable even when allowExactPause = true!
    QTemporaryDir tempDir;
    CHECK(tempDir.isValid());
    const QString srcPath = tempDir.filePath(QStringLiteral("source.txt"));
    const QString dstPath = tempDir.filePath(QStringLiteral("dest.txt"));
    {
        QFile f(srcPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("thispc-adapter-kio-test");
    }
    KIO::CopyJob *copyJob = KIO::copy(QUrl::fromLocalFile(srcPath),
                                      QUrl::fromLocalFile(dstPath),
                                      KIO::HideProgressInfo);
    KJobTransferAdapterOptions kioOpts;
    kioOpts.startMode = KJobStartMode::FactoryStarts;
    kioOpts.allowExactPause = true; // explicitly attempt to enable pause
    auto *kioAdapter = KJobTransferAdapter::create(copyJob, kioOpts);
    CHECK(kioAdapter != nullptr);
    kioAdapter->setAutoDelete(false);
    CHECK(!kioAdapter->hasCapability(TransferJobCapability::Suspendable));
    CHECK(!kioAdapter->hasCapability(TransferJobCapability::BandwidthLimited));

    QEventLoop loop;
    QObject::connect(kioAdapter, &PortableTransferJob::finished, &loop, &QEventLoop::quit);
    kioAdapter->start();
    if (!kioAdapter->isTerminal()) {
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    CHECK(kioAdapter->isTerminal());
    CHECK(kioAdapter->state() == TransferJobState::Completed);
    CHECK(QFile::exists(dstPath));
    delete kioAdapter;
}

// 20-21-22: Custom async cancellation, KJob::kill fallback, and cancel-request vs completion.
void testCancellationPaths()
{
    // Custom async cancellation: stays Cancelling until wrapped job finishes cleanup
    auto *asyncJob = new FakeKJob(KJob::Killable);
    int customCancelCalls = 0;
    KJobTransferAdapterOptions customOpts;
    customOpts.cancelHandler = [&](KJob *j) -> bool {
        ++customCancelCalls;
        CHECK(j == asyncJob);
        return true; // async cleanup in progress
    };
    auto *customAdapter = KJobTransferAdapter::create(asyncJob, customOpts);
    customAdapter->setAutoDelete(false);
    CHECK(customAdapter->hasCapability(TransferJobCapability::AsyncCancel));
    customAdapter->start();
    CHECK(customAdapter->requestCancel());
    CHECK(customAdapter->requestCancel()); // idempotent; must not call handler twice
    CHECK(customCancelCalls == 1);
    CHECK(asyncJob->killCalls == 0);
    CHECK(customAdapter->state() == TransferJobState::Cancelling);
    CHECK(!customAdapter->isTerminal());

    // Now wrapped job finishes cleanup and emits cancelled result
    asyncJob->finishNow(KIO::ERR_USER_CANCELED, QStringLiteral("Cancelled after cleanup"));
    CHECK(customAdapter->state() == TransferJobState::Cancelled);
    CHECK(customAdapter->isTerminal());
    delete customAdapter;

    // Synchronous result inside cancelHandler
    auto *syncCancelJob = new FakeKJob(KJob::Killable);
    KJobTransferAdapterOptions syncCancelOpts;
    syncCancelOpts.cancelHandler = [](KJob *j) -> bool {
        static_cast<FakeKJob *>(j)->finishNow(KIO::ERR_USER_CANCELED, QStringLiteral("Sync cancel"));
        return true;
    };
    auto *syncCancelAdapter = KJobTransferAdapter::create(syncCancelJob, syncCancelOpts);
    syncCancelAdapter->setAutoDelete(false);
    syncCancelAdapter->start();
    CHECK(syncCancelAdapter->requestCancel());
    CHECK(syncCancelAdapter->state() == TransferJobState::Cancelled);
    delete syncCancelAdapter;

    // KJob::kill fallback
    auto *killJob = new FakeKJob(KJob::Killable);
    auto *killAdapter = KJobTransferAdapter::create(killJob);
    killAdapter->setAutoDelete(false);
    CHECK(killAdapter->hasCapability(TransferJobCapability::Killable));
    CHECK(!killAdapter->hasCapability(TransferJobCapability::AsyncCancel));
    killAdapter->start();
    CHECK(killAdapter->requestCancel());
    CHECK(killJob->killCalls == 1);
    CHECK(killAdapter->state() == TransferJobState::Cancelled);
    delete killAdapter;
}

// 23: Bandwidth limiter shared_ptr lifetime and unsupported job accuracy.
void testBandwidthLimiter()
{
    auto limiter = std::make_shared<TransferBandwidthLimiter>();
    const auto weakLimiter = std::weak_ptr<TransferBandwidthLimiter>(limiter);

    // Job with Q_INVOKABLE setBandwidthLimiter
    auto *limJob = new LimiterFakeKJob(KJob::Killable);
    auto *limAdapter = KJobTransferAdapter::create(limJob);
    limAdapter->setAutoDelete(false);
    CHECK(limAdapter->hasCapability(TransferJobCapability::BandwidthLimited));
    limAdapter->setBandwidthLimiter(limiter, 42);
    CHECK(limJob->limiterCalls == 1);
    CHECK(limJob->receivedTaskId == 42);
    CHECK(limJob->receivedLimiter.get() == limiter.get());

    // Release caller's shared_ptr — adapter + job keep limiter alive
    limiter.reset();
    CHECK(!weakLimiter.expired());
    limJob->finishNow(0);
    delete limAdapter;
    CHECK(weakLimiter.expired());

    // Plain FakeKJob without setBandwidthLimiter does NOT advertise BandwidthLimited
    auto *plainJob = new FakeKJob(KJob::Killable);
    auto *plainAdapter = KJobTransferAdapter::create(plainJob);
    plainAdapter->setAutoDelete(false);
    CHECK(!plainAdapter->hasCapability(TransferJobCapability::BandwidthLimited));
    delete plainAdapter;
}

// 24-25-26-27-28: Folder Merge metadata, live decision phase, recovery with 0 published,
// and fail-closed invariant for missing vs explicit-false vs type-invalid properties.
void testFolderMergeMetadataAndFailClosed()
{
    // 24 + 25 + 28: Full Folder Merge metadata + live decision phase
    auto *mergeJob = new FakeKJob(KJob::Killable);
    auto *mergeAdapter = KJobTransferAdapter::create(mergeJob);
    mergeAdapter->setAutoDelete(false);

    std::vector<bool> phaseHistory;
    QObject::connect(mergeAdapter, &PortableTransferJob::decisionPhaseChanged,
                     [&](PortableTransferJob *, bool inPhase) {
                         phaseHistory.push_back(inPhase);
                     });

    mergeAdapter->start();
    mergeJob->setProperty("thispcMergeInDecisionPhase", true);
    CHECK(mergeAdapter->isInDecisionPhase());
    mergeJob->setProperty("thispcMergeInDecisionPhase", false);
    CHECK(!mergeAdapter->isInDecisionPhase());
    mergeJob->setProperty("thispcMergeInDecisionPhase", true);
    CHECK(mergeAdapter->isInDecisionPhase());

    mergeJob->setProperty("thispcDestinationPublished", true);
    mergeJob->setProperty("thispcDestinationReplaced", true);
    mergeJob->setProperty("thispcDirectoryDurabilityConfirmed", true);
    mergeJob->setProperty("thispcPartialMoveOutcome", true);
    mergeJob->setProperty("thispcBatchPublishedCount", 3);
    mergeJob->setProperty("thispcBatchPartiallyCompleted", true);
    mergeJob->setProperty("thispcMergeRetryBlocked", true);
    mergeJob->setProperty("thispcMergeRecoveryRequired", true);
    mergeJob->setProperty("thispcMergeState", QStringLiteral("RecoveryRequired"));
    mergeJob->setProperty("thispcMergeSkippedCount", 2);
    mergeJob->setProperty("thispcMergeReplacedCount", 1);
    mergeJob->setProperty("thispcMergeHistoryNote", QStringLiteral("Partial merge interrupted"));
    mergeJob->setProperty("thispcSystemError", 28);

    // Check metadata is copied BEFORE finished() signal fires
    bool flagsSeenInFinished = false;
    QObject::connect(mergeAdapter, &PortableTransferJob::finished, [&](PortableTransferJob *j) {
        const auto &f = j->outcomeFlags();
        flagsSeenInFinished = f.destinationPublished
            && f.destinationReplaced
            && f.directoryDurabilityConfirmed
            && f.partialMoveOutcome
            && f.batchPublishedCount == 3
            && f.batchPartiallyCompleted
            && f.mergeRetryBlocked
            && f.mergeRecoveryRequired
            && f.mergeState == QStringLiteral("RecoveryRequired")
            && f.mergeSkippedCount == 2
            && f.mergeReplacedCount == 1
            && f.mergeHistoryNote == QStringLiteral("Partial merge interrupted")
            && f.systemError == 28
            && f.blocksSafeRetry();
    });

    mergeJob->finishNow(KIO::ERR_DISK_FULL, QStringLiteral("Disk full during merge"));
    CHECK(flagsSeenInFinished);
    CHECK(!mergeAdapter->isInDecisionPhase()); // auto-reset on terminal
    CHECK(phaseHistory.size() == 4); // true, false, true, false (terminal reset)
    delete mergeAdapter;

    // 26: Recovery-required with zero published items still blocks Safe Retry
    auto *recJob = new FakeKJob(KJob::Killable);
    recJob->setProperty("thispcMergeRecoveryRequired", true);
    recJob->setProperty("thispcBatchPublishedCount", 0);
    recJob->setProperty("thispcMergeState", QStringLiteral("RecoveryRequired"));
    const TransferOutcomeFlags recFlags = KJobTransferAdapter::extractOutcomeFlags(recJob);
    CHECK(recFlags.mergeRecoveryRequired);
    CHECK(recFlags.batchPublishedCount == 0);
    CHECK(recFlags.blocksSafeRetry());
    delete recJob;

    // 27a: Genuinely absent properties vs explicit false/0 -> retry-safe
    auto *cleanJob = new FakeKJob(KJob::Killable);
    const TransferOutcomeFlags absentFlags = KJobTransferAdapter::extractOutcomeFlags(cleanJob);
    CHECK(!absentFlags.blocksSafeRetry());

    cleanJob->setProperty("thispcDestinationPublished", false);
    cleanJob->setProperty("thispcMergeRetryBlocked", false);
    cleanJob->setProperty("thispcMergeRecoveryRequired", false);
    cleanJob->setProperty("thispcBatchPublishedCount", 0);
    const TransferOutcomeFlags explicitFalseFlags = KJobTransferAdapter::extractOutcomeFlags(cleanJob);
    CHECK(!explicitFalseFlags.blocksSafeRetry());
    delete cleanJob;

    // 27b: Type-invalid safety-critical metadata -> FAIL CLOSED (blocksSafeRetry() == true)
    auto *badBoolJob = new FakeKJob(KJob::Killable);
    badBoolJob->setProperty("thispcMergeRetryBlocked", QStringLiteral("false")); // wrong type!
    const TransferOutcomeFlags badBoolFlags = KJobTransferAdapter::extractOutcomeFlags(badBoolJob);
    CHECK(badBoolFlags.mergeRetryBlocked);
    CHECK(badBoolFlags.blocksSafeRetry());
    delete badBoolJob;

    auto *badRecoveryJob = new FakeKJob(KJob::Killable);
    badRecoveryJob->setProperty("thispcMergeRecoveryRequired", 123); // wrong type!
    const TransferOutcomeFlags badRecoveryFlags = KJobTransferAdapter::extractOutcomeFlags(badRecoveryJob);
    CHECK(badRecoveryFlags.mergeRecoveryRequired);
    CHECK(badRecoveryFlags.blocksSafeRetry());
    delete badRecoveryJob;

    auto *negCountJob = new FakeKJob(KJob::Killable);
    negCountJob->setProperty("thispcBatchPublishedCount", -1); // negative count!
    const TransferOutcomeFlags negCountFlags = KJobTransferAdapter::extractOutcomeFlags(negCountJob);
    CHECK(negCountFlags.mergeRetryBlocked);
    CHECK(negCountFlags.blocksSafeRetry());
    delete negCountJob;
}

// 29-30: Reentrant signal observers (deleting adapter inside decisionPhaseChanged,
// processedBytesChanged, and finished) without UAF or leaks.
void testReentrantObservers()
{
    // Delete adapter inside decisionPhaseChanged
    auto *job1 = new FakeKJob(KJob::Killable);
    QPointer<KJobTransferAdapter> adapter1 = KJobTransferAdapter::create(job1);
    adapter1->setAutoDelete(false);
    QObject::connect(adapter1.data(), &PortableTransferJob::decisionPhaseChanged,
                     [&](PortableTransferJob *j, bool) { delete j; });
    adapter1->start();
    job1->setProperty("thispcMergeInDecisionPhase", true);
    CHECK(adapter1.isNull());

    // Delete adapter inside processedBytesChanged
    auto *job2 = new FakeKJob(KJob::Killable);
    QPointer<KJobTransferAdapter> adapter2 = KJobTransferAdapter::create(job2);
    adapter2->setAutoDelete(false);
    QObject::connect(adapter2.data(), &PortableTransferJob::processedBytesChanged,
                     [&](PortableTransferJob *j, qulonglong) { delete j; });
    adapter2->start();
    job2->reportTotalBytes(500);
    job2->reportProcessedBytes(100);
    CHECK(adapter2.isNull());

    // Delete adapter inside finished()
    auto *job3 = new FakeKJob(KJob::Killable);
    QPointer<KJobTransferAdapter> adapter3 = KJobTransferAdapter::create(job3);
    adapter3->setAutoDelete(true); // autoDelete must not double-delete after sync delete
    QObject::connect(adapter3.data(), &PortableTransferJob::finished,
                     [&](PortableTransferJob *j) { delete j; });
    adapter3->start();
    job3->finishNow(0);
    pumpEvents(15);
    CHECK(adapter3.isNull());
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 0);
}

// Fake KJob with mismatched setBandwidthLimiter parameter type (AUD-2A2-05).
class BadLimiterFakeKJob : public FakeKJob
{
    Q_OBJECT

public:
    explicit BadLimiterFakeKJob(QObject *parent = nullptr)
        : FakeKJob(KJob::Killable, parent)
    {
    }

    int calls = 0;

    Q_INVOKABLE void setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter>, QString)
    {
        ++calls;
    }
};

// Deterministic KJob owning a real worker QThread for AUD-2A2-01 lifetime tests.
class RealWorkerKJob : public KJob
{
    Q_OBJECT

public:
    explicit RealWorkerKJob(int stepDelayMs = 8, int maxSteps = 50, QObject *parent = nullptr)
        : KJob(parent)
        , m_stepDelayMs(stepDelayMs)
        , m_maxSteps(maxSteps)
    {
        setCapabilities(KJob::Killable);
    }

    ~RealWorkerKJob() override
    {
        m_cancelRequested.store(true, std::memory_order_release);
        if (m_worker) {
            m_worker->wait();
            delete m_worker;
            m_worker = nullptr;
        }
        if (onDestroyedHook) {
            onDestroyedHook();
        }
    }

    std::atomic<bool> workerRunning{false};
    std::atomic<bool> workerExited{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<int> stepsCompleted{0};
    std::function<void()> onDestroyedHook;

    void start() override
    {
        if (m_worker) {
            return;
        }
        m_worker = QThread::create([this]() {
            workerRunning.store(true, std::memory_order_release);
            for (int i = 0; i < m_maxSteps; ++i) {
                if (m_cancelRequested.load(std::memory_order_acquire)) {
                    break;
                }
                QThread::msleep(static_cast<unsigned long>(m_stepDelayMs));
                stepsCompleted.fetch_add(1, std::memory_order_acq_rel);
            }
            const bool cancelled = m_cancelRequested.load(std::memory_order_acquire);
            workerRunning.store(false, std::memory_order_release);
            workerExited.store(true, std::memory_order_release);
            QMetaObject::invokeMethod(
                this,
                [this, cancelled]() {
                    if (cancelled) {
                        setError(KIO::ERR_USER_CANCELED);
                        setErrorText(QStringLiteral("Worker cancelled"));
                    }
                    emitResult();
                },
                Qt::QueuedConnection);
        });
        m_worker->start();
    }

    bool requestCancelAsync()
    {
        m_cancelRequested.store(true, std::memory_order_release);
        return true;
    }

protected:
    bool doKill() override
    {
        m_cancelRequested.store(true, std::memory_order_release);
        return true;
    }

private:
    int m_stepDelayMs = 8;
    int m_maxSteps = 50;
    QThread *m_worker = nullptr;
};

// AUD-2A2-01: Real worker thread lifetime, non-blocking GUI destroy, and
// cleanupHandle() tracking across both active teardown and reentrant callbacks.
void testAud2A2_01_RealWorkerAndCleanupHandle()
{
    // 1. Active real-worker job destroyed via delete adapter -> non-blocking,
    //    cleanupHandle stays false until worker thread finishes and KJob is destroyed.
    bool workerJobDestroyed = false;
    auto *wJob = new RealWorkerKJob(10, 80);
    wJob->onDestroyedHook = [&]() { workerJobDestroyed = true; };
    KJobTransferAdapterOptions opts;
    opts.cancelHandler = [wJob](KJob *) { return wJob->requestCancelAsync(); };
    auto *wAdapter = KJobTransferAdapter::create(wJob, opts);
    CHECK(wAdapter != nullptr);
    wAdapter->setAutoDelete(false);
    auto handle = wAdapter->cleanupHandle();
    CHECK(!handle->isCleanupComplete());

    int cleanupSignals = 0;
    QObject::connect(handle.get(), &PortableTransferCleanupHandle::cleanupCompleted,
                     [&]() { ++cleanupSignals; });

    wAdapter->start();
    QElapsedTimer waitStart;
    waitStart.start();
    while (!wJob->workerRunning.load(std::memory_order_acquire) && waitStart.elapsed() < 1000) {
        QThread::msleep(2);
    }
    CHECK(wJob->workerRunning.load(std::memory_order_acquire));

    QElapsedTimer destroyTimer;
    destroyTimer.start();
    delete wAdapter;
    const qint64 destroyElapsedMs = destroyTimer.elapsed();
    // Must not block GUI thread waiting for the worker loop
    CHECK(destroyElapsedMs < 100);
    CHECK(!handle->isCleanupComplete());
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 1);

    // Wait boundedly for async worker unwind + retainer destruction
    QEventLoop loop;
    if (!handle->isCleanupComplete()) {
        QObject::connect(handle.get(), &PortableTransferCleanupHandle::cleanupCompleted,
                         &loop, &QEventLoop::quit);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    CHECK(handle->isCleanupComplete());
    CHECK(cleanupSignals == 1);
    CHECK(workerJobDestroyed);
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 0);

    // 2. Reentrant delete inside finished(): cleanupHandle must NOT complete
    //    until rawJob->deleteLater() actually destroys rawJob.
    bool rawDestroyed = false;
    auto *reJob = new FakeKJob(KJob::Killable);
    reJob->onDestroyedHook = [&]() { rawDestroyed = true; };
    QPointer<KJobTransferAdapter> reAdapter = KJobTransferAdapter::create(reJob);
    reAdapter->setAutoDelete(false);
    auto reHandle = reAdapter->cleanupHandle();
    QObject::connect(reAdapter.data(), &PortableTransferJob::finished,
                     [&](PortableTransferJob *j) { delete j; });
    reAdapter->start();
    reJob->finishNow(0);
    CHECK(reAdapter.isNull());
    CHECK(!rawDestroyed);
    CHECK(!reHandle->isCleanupComplete());
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 1);
    pumpEvents(25);
    CHECK(rawDestroyed);
    CHECK(reHandle->isCleanupComplete());
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 0);
}

// AUD-2A2-02: FactoryStarts cancellation before PortableTransferJob::start().
void testAud2A2_02_FactoryStartsPreStartCancel()
{
    // 1. Async cancelHandler: stays Cancelling until backend emits result;
    //    simulated backend stops work upon cancellation.
    auto *facJob = new FakeKJob(KJob::Killable);
    int cancelCalls = 0;
    bool backendActive = true;
    int simulatedWorkAfterCancel = 0;
    KJobTransferAdapterOptions opts;
    opts.startMode = KJobStartMode::FactoryStarts;
    opts.cancelHandler = [&](KJob *) -> bool {
        ++cancelCalls;
        backendActive = false;
        return true;
    };
    auto *adapter = KJobTransferAdapter::create(facJob, opts);
    CHECK(adapter != nullptr);
    adapter->setAutoDelete(false);

    int finishedCount = 0;
    QObject::connect(adapter, &PortableTransferJob::finished, [&](PortableTransferJob *) {
        ++finishedCount;
        if (backendActive) {
            ++simulatedWorkAfterCancel;
        }
    });

    // Cancel BEFORE start()
    CHECK(adapter->requestCancel());
    CHECK(adapter->requestCancel()); // idempotent
    CHECK(cancelCalls == 1);
    CHECK(adapter->state() == TransferJobState::Cancelling);
    CHECK(!adapter->isTerminal());
    CHECK(finishedCount == 0);
    CHECK(!backendActive);

    facJob->finishNow(KIO::ERR_USER_CANCELED, QStringLiteral("Cancelled before start"));
    CHECK(adapter->state() == TransferJobState::Cancelled);
    CHECK(finishedCount == 1);
    CHECK(simulatedWorkAfterCancel == 0);
    delete adapter;

    // 2. Backend fails with unrelated error after pre-start cancel -> remains Failed
    auto *failJob = new FakeKJob(KJob::Killable);
    KJobTransferAdapterOptions failOpts;
    failOpts.startMode = KJobStartMode::FactoryStarts;
    failOpts.cancelHandler = [](KJob *) { return true; };
    auto *failAdapter = KJobTransferAdapter::create(failJob, failOpts);
    failAdapter->setAutoDelete(false);
    CHECK(failAdapter->requestCancel());
    CHECK(failAdapter->state() == TransferJobState::Cancelling);
    failJob->finishNow(KIO::ERR_CANNOT_WRITE, QStringLiteral("Write failed"));
    CHECK(failAdapter->state() == TransferJobState::Failed);
    CHECK(failAdapter->errorCode() == TransferErrorCode::CannotWrite);
    delete failAdapter;

    // 3. Synchronous result inside pre-start cancelHandler
    auto *syncJob = new FakeKJob(KJob::Killable);
    KJobTransferAdapterOptions syncOpts;
    syncOpts.startMode = KJobStartMode::FactoryStarts;
    syncOpts.cancelHandler = [](KJob *j) {
        static_cast<FakeKJob *>(j)->finishNow(KIO::ERR_USER_CANCELED, QStringLiteral("Sync pre-start cancel"));
        return true;
    };
    auto *syncAdapter = KJobTransferAdapter::create(syncJob, syncOpts);
    syncAdapter->setAutoDelete(false);
    int syncFinished = 0;
    QObject::connect(syncAdapter, &PortableTransferJob::finished, [&](PortableTransferJob *) { ++syncFinished; });
    CHECK(syncAdapter->requestCancel());
    CHECK(syncAdapter->state() == TransferJobState::Cancelled);
    CHECK(syncFinished == 1);
    delete syncAdapter;

    // 4. Buffered result already arrived before pre-start cancel -> delivers buffered outcome
    auto *bufJob = new FakeKJob(KJob::Killable);
    bufJob->setProperty("thispcDestinationPublished", true);
    KJobTransferAdapterOptions bufOpts;
    bufOpts.startMode = KJobStartMode::FactoryStarts;
    auto *bufAdapter = KJobTransferAdapter::create(bufJob, bufOpts);
    bufAdapter->setAutoDelete(false);
    bufJob->finishNow(0);
    CHECK(bufAdapter->requestCancel());
    CHECK(bufAdapter->state() == TransferJobState::Completed);
    CHECK(bufAdapter->outcomeFlags().destinationPublished);
    delete bufAdapter;

    // 5. ControllerStarts NotStarted cancellation remains immediate (unchanged)
    auto *ctrlJob = new FakeKJob(KJob::Killable);
    auto *ctrlAdapter = KJobTransferAdapter::create(ctrlJob);
    ctrlAdapter->setAutoDelete(false);
    CHECK(ctrlAdapter->requestCancel());
    CHECK(ctrlAdapter->state() == TransferJobState::Cancelled);
    CHECK(ctrlJob->startCalls == 0);
    CHECK(ctrlJob->killCalls == 0);
    delete ctrlAdapter;
}

// AUD-2A2-03: Strict pause capability across KIO jobs, staged transfer, dynamic fallback,
// and KJob::suspended / KJob::resumed signal synchronization.
void testAud2A2_03_PauseAndFallback()
{
    // Staged transfer property blocks Suspendable even with allowExactPause = true
    auto *stagedJob = new FakeKJob(KJob::Killable | KJob::Suspendable);
    stagedJob->setProperty("thispcStagedTransfer", true);
    KJobTransferAdapterOptions opts;
    opts.allowExactPause = true;
    auto *stagedAdapter = KJobTransferAdapter::create(stagedJob, opts);
    stagedAdapter->setAutoDelete(false);
    CHECK(!stagedAdapter->hasCapability(TransferJobCapability::Suspendable));
    delete stagedAdapter;

    // Real KIO::FileCopyJob never marked Suspendable
    QTemporaryDir tempDir;
    CHECK(tempDir.isValid());
    const QString srcPath = tempDir.filePath(QStringLiteral("fc-src.txt"));
    const QString dstPath = tempDir.filePath(QStringLiteral("fc-dst.txt"));
    {
        QFile f(srcPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("file-copy-test");
    }
    KIO::FileCopyJob *fcJob = KIO::file_copy(QUrl::fromLocalFile(srcPath),
                                             QUrl::fromLocalFile(dstPath),
                                             -1,
                                             KIO::HideProgressInfo);
    KJobTransferAdapterOptions fcOpts;
    fcOpts.startMode = KJobStartMode::FactoryStarts;
    fcOpts.allowExactPause = true;
    auto *fcAdapter = KJobTransferAdapter::create(fcJob, fcOpts);
    CHECK(fcAdapter != nullptr);
    fcAdapter->setAutoDelete(false);
    CHECK(!fcAdapter->hasCapability(TransferJobCapability::Suspendable));
    QEventLoop loop;
    QObject::connect(fcAdapter, &PortableTransferJob::finished, &loop, &QEventLoop::quit);
    fcAdapter->start();
    if (!fcAdapter->isTerminal()) {
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    CHECK(fcAdapter->state() == TransferJobState::Completed);
    delete fcAdapter;

    // Local-to-KIO dynamic fallback: job drops KJob::Suspendable mid-run
    auto *fbJob = new FakeKJob(KJob::Killable | KJob::Suspendable);
    auto *fbAdapter = KJobTransferAdapter::create(fbJob, opts);
    fbAdapter->setAutoDelete(false);
    CHECK(fbAdapter->hasCapability(TransferJobCapability::Suspendable));
    fbAdapter->start();
    fbJob->updateCapabilities(KJob::Killable); // simulated KIO fallback
    CHECK(!fbAdapter->suspend());
    CHECK(fbJob->suspendCalls == 0);
    CHECK(!fbAdapter->isSuspended());
    fbJob->finishNow(0);
    delete fbAdapter;

    // External KJob::suspended / resumed signals keep adapter state synced
    auto *sigJob = new FakeKJob(KJob::Killable | KJob::Suspendable);
    auto *sigAdapter = KJobTransferAdapter::create(sigJob, opts);
    sigAdapter->setAutoDelete(false);
    sigAdapter->start();
    CHECK(sigJob->suspend());
    CHECK(sigAdapter->isSuspended());
    CHECK(sigJob->resume());
    CHECK(!sigAdapter->isSuspended());
    sigJob->finishNow(0);
    delete sigAdapter;
}

// AUD-2A2-04: Fail-closed decision phase on invalid types/removal + eventFilter propagation.
void testAud2A2_04_DecisionPhaseAndEventFilter()
{
    auto *job = new FakeKJob(KJob::Killable);
    auto *adapter = KJobTransferAdapter::create(job);
    adapter->setAutoDelete(false);
    adapter->start();
    CHECK(!adapter->isInDecisionPhase());

    // Invalid string type -> fails closed to true
    job->setProperty("thispcMergeInDecisionPhase", QStringLiteral("false"));
    CHECK(adapter->isInDecisionPhase());

    // Explicit valid bool false clears it
    job->setProperty("thispcMergeInDecisionPhase", false);
    CHECK(!adapter->isInDecisionPhase());

    // Invalid int type -> fails closed to true
    job->setProperty("thispcMergeInDecisionPhase", 0);
    CHECK(adapter->isInDecisionPhase());

    // Removing property while active -> retains true (fail-closed)
    job->setProperty("thispcMergeInDecisionPhase", QVariant());
    CHECK(adapter->isInDecisionPhase());

    job->finishNow(0);
    CHECK(!adapter->isInDecisionPhase());
    delete adapter;

    // Secondary event filter still receives DynamicPropertyChange when adapter
    // is deleted reentrantly inside decisionPhaseChanged (eventFilter returns false).
    class ProbeFilter : public QObject
    {
    public:
        int seenCount = 0;
        bool eventFilter(QObject *w, QEvent *e) override
        {
            if (e && e->type() == QEvent::DynamicPropertyChange) {
                ++seenCount;
            }
            return QObject::eventFilter(w, e);
        }
    };

    auto *job2 = new FakeKJob(KJob::Killable);
    ProbeFilter probe;
    job2->installEventFilter(&probe); // installed BEFORE adapter -> runs AFTER adapter
    QPointer<KJobTransferAdapter> adapter2 = KJobTransferAdapter::create(job2);
    adapter2->setAutoDelete(false);
    QObject::connect(adapter2.data(), &PortableTransferJob::decisionPhaseChanged,
                     [&](PortableTransferJob *j, bool) { delete j; });
    adapter2->start();
    probe.seenCount = 0;
    job2->setProperty("thispcMergeInDecisionPhase", true);
    CHECK(adapter2.isNull());
    CHECK(probe.seenCount == 1);
    pumpEvents(25);
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 0);
}

// AUD-2A2-05: ERR_CANNOT_WRITE, mergeState fallback safety, buffered-result progress
// suppression, byte-authoritative percent, and bandwidth-limiter signature validation.
void testAud2A2_05_ConsistencyAndFailClosed()
{
    CHECK(KJobTransferAdapter::mapKJobError(KIO::ERR_CANNOT_WRITE) == TransferErrorCode::CannotWrite);

    // Merge state strings alone force retry block even without bool flags
    for (const QString &state : {QStringLiteral("RecoveryRequired"),
                                 QStringLiteral("FailedPartialSuccess"),
                                 QStringLiteral("FailedUncertainDurability")}) {
        auto *j = new FakeKJob(KJob::Killable);
        j->setProperty("thispcMergeState", state);
        const TransferOutcomeFlags f = KJobTransferAdapter::extractOutcomeFlags(j);
        CHECK(f.blocksSafeRetry());
        if (state == QStringLiteral("RecoveryRequired")) {
            CHECK(f.mergeRecoveryRequired);
        }
        delete j;
    }

    // Late progress after FactoryStarts buffered result is ignored;
    // when totalBytes > 0, raw percentChanged does not override byte-derived percent.
    auto *pJob = new FakeKJob(KJob::Killable);
    KJobTransferAdapterOptions pOpts;
    pOpts.startMode = KJobStartMode::FactoryStarts;
    auto *pAdapter = KJobTransferAdapter::create(pJob, pOpts);
    pAdapter->setAutoDelete(false);
    pJob->reportTotalBytes(1000);
    pJob->reportProcessedBytes(400);
    CHECK(pAdapter->percent() == 40);
    pJob->reportPercent(95); // must NOT override byte-derived 40%
    CHECK(pAdapter->percent() == 40);

    pJob->finishNow(0); // buffered before start()
    pJob->reportProcessedBytes(900); // late after result -> ignored
    CHECK(pAdapter->processedBytes() == 400);
    pAdapter->start();
    CHECK(pAdapter->state() == TransferJobState::Completed);
    delete pAdapter;

    // When totalBytes == 0, raw percentChanged still works
    auto *noBytesJob = new FakeKJob(KJob::Killable);
    auto *noBytesAdapter = KJobTransferAdapter::create(noBytesJob);
    noBytesAdapter->setAutoDelete(false);
    noBytesAdapter->start();
    noBytesJob->reportPercent(73);
    CHECK(noBytesAdapter->percent() == 73);
    noBytesJob->finishNow(0);
    delete noBytesAdapter;

    // Mismatched 2nd parameter type on setBandwidthLimiter is NOT advertised
    auto *badLimJob = new BadLimiterFakeKJob();
    auto *badLimAdapter = KJobTransferAdapter::create(badLimJob);
    badLimAdapter->setAutoDelete(false);
    CHECK(!badLimAdapter->hasCapability(TransferJobCapability::BandwidthLimited));
    badLimAdapter->setBandwidthLimiter(std::make_shared<TransferBandwidthLimiter>(), 9);
    CHECK(badLimJob->calls == 0);
    delete badLimAdapter;
}

// AUD-2A2-R1-01: Active non-killable job & job refusing cancellation must NOT
// be destroyed prematurely by KJobCleanupRetainer; cleanup completes only after
// natural backend completion and actual destruction.
void testAud2A2_R1_01_RefusedCancelAndNonKillable()
{
    // 1. Active job without KJob::Killable
    bool nonKillableDestroyed = false;
    auto *nkJob = new FakeKJob(KJob::NoCapabilities);
    nkJob->onDestroyedHook = [&]() { nonKillableDestroyed = true; };
    auto *nkAdapter = KJobTransferAdapter::create(nkJob);
    CHECK(nkAdapter != nullptr);
    nkAdapter->setAutoDelete(false);
    auto nkHandle = nkAdapter->cleanupHandle();
    int nkSignals = 0;
    QObject::connect(nkHandle.get(), &PortableTransferCleanupHandle::cleanupCompleted,
                     [&]() { ++nkSignals; });
    nkAdapter->start();
    delete nkAdapter;

    // Pump events while job is still active: must NOT be destroyed prematurely!
    pumpEvents(25);
    CHECK(!nonKillableDestroyed);
    CHECK(!nkHandle->isCleanupComplete());
    CHECK(nkSignals == 0);
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 1);

    // Natural completion -> retainer schedules deleteLater -> destroyed & complete
    nkJob->finishNow(0);
    pumpEvents(25);
    CHECK(nonKillableDestroyed);
    CHECK(nkHandle->isCleanupComplete());
    CHECK(nkSignals == 1);
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 0);

    // 2. Active job whose cancelHandler refuses cancellation (returns false)
    bool refusedDestroyed = false;
    int refusedCancelCalls = 0;
    auto *refJob = new FakeKJob(KJob::Killable);
    refJob->onDestroyedHook = [&]() { refusedDestroyed = true; };
    KJobTransferAdapterOptions refOpts;
    refOpts.cancelHandler = [&](KJob *) -> bool {
        ++refusedCancelCalls;
        return false; // cancel refused while mid-commit
    };
    auto *refAdapter = KJobTransferAdapter::create(refJob, refOpts);
    refAdapter->setAutoDelete(false);
    auto refHandle = refAdapter->cleanupHandle();
    refAdapter->start();
    delete refAdapter;

    pumpEvents(25);
    CHECK(refusedCancelCalls == 1);
    CHECK(!refusedDestroyed);
    CHECK(!refHandle->isCleanupComplete());
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 1);

    refJob->finishNow(0);
    pumpEvents(25);
    CHECK(refusedDestroyed);
    CHECK(refHandle->isCleanupComplete());
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 0);

    // 3. Real worker thread refusing cancellation: delete adapter is non-blocking,
    //    worker runs to natural completion, then cleanupHandle completes cleanly.
    bool rwDestroyed = false;
    auto *rwJob = new RealWorkerKJob(5, 6); // ~30 ms natural completion
    rwJob->onDestroyedHook = [&]() { rwDestroyed = true; };
    KJobTransferAdapterOptions rwOpts;
    rwOpts.cancelHandler = [](KJob *) { return false; }; // refuse cancel
    auto *rwAdapter = KJobTransferAdapter::create(rwJob, rwOpts);
    rwAdapter->setAutoDelete(false);
    auto rwHandle = rwAdapter->cleanupHandle();
    rwAdapter->start();

    QElapsedTimer waitRun;
    waitRun.start();
    while (!rwJob->workerRunning.load(std::memory_order_acquire) && waitRun.elapsed() < 1000) {
        QThread::msleep(2);
    }
    CHECK(rwJob->workerRunning.load(std::memory_order_acquire));

    QElapsedTimer destroyTimer;
    destroyTimer.start();
    delete rwAdapter;
    CHECK(destroyTimer.elapsed() < 100); // non-blocking on GUI thread
    CHECK(!rwDestroyed);
    CHECK(!rwHandle->isCleanupComplete());

    QEventLoop loop;
    if (!rwHandle->isCleanupComplete()) {
        QObject::connect(rwHandle.get(), &PortableTransferCleanupHandle::cleanupCompleted,
                         &loop, &QEventLoop::quit);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    CHECK(rwHandle->isCleanupComplete());
    CHECK(rwDestroyed);
    CHECK(KJobTransferAdapter::pendingDetachedCleanupCount() == 0);
}

// AUD-2A2-R1-03: Resuming an already-paused job after KJob::Suspendable is dropped.
void testAud2A2_R1_03_ResumeAfterCapabilityChange()
{
    auto *job = new FakeKJob(KJob::Killable | KJob::Suspendable);
    KJobTransferAdapterOptions opts;
    opts.allowExactPause = true;
    auto *adapter = KJobTransferAdapter::create(job, opts);
    adapter->setAutoDelete(false);

    std::vector<bool> suspendedSignals;
    QObject::connect(adapter, &PortableTransferJob::suspendedChanged,
                     [&](PortableTransferJob *, bool s) { suspendedSignals.push_back(s); });

    adapter->start();
    CHECK(adapter->suspend());
    CHECK(adapter->isSuspended());

    // Capability changes while already Paused
    job->updateCapabilities(KJob::Killable);

    // Backend rejects resume -> adapter stays Paused, no fake resume signal
    job->resumeResult = false;
    CHECK(!adapter->resume());
    CHECK(adapter->isSuspended());
    CHECK(suspendedSignals.size() == 1);

    // Backend accepts resume -> adapter cleanly exits Paused to Running
    job->resumeResult = true;
    CHECK(adapter->resume());
    CHECK(!adapter->isSuspended());
    CHECK(adapter->state() == TransferJobState::Running);
    CHECK(suspendedSignals.size() == 2);
    CHECK(suspendedSignals[0] == true);
    CHECK(suspendedSignals[1] == false);

    // Subsequent suspend is still blocked because Suspendable was dropped
    CHECK(!adapter->suspend());
    CHECK(!adapter->isSuspended());

    job->finishNow(0);
    delete adapter;
}

// AUD-2A2-R1-02: Subprocess event-loop shutdown verification (Modes B, C, D)
// verifying actual retainer & backend destructor execution and zero LSan leaks.
int runShutdownSubprocessMode(const QString &mode, int argc, char **argv)
{
    bool backendDestroyed = false;
    int cleanupSignals = 0;
    std::shared_ptr<PortableTransferCleanupHandle> handle;

    {
        QCoreApplication app(argc, argv);

        if (mode == QStringLiteral("quit-in-cleanup")) {
            // Mode B/E: cleanupCompleted() synchronously triggers QCoreApplication::quit()
            auto *job = new FakeKJob(KJob::Killable);
            job->onDestroyedHook = [&]() { backendDestroyed = true; };
            auto *adapter = KJobTransferAdapter::create(job);
            adapter->setAutoDelete(false);
            handle = adapter->cleanupHandle();
            QObject::connect(handle.get(), &PortableTransferCleanupHandle::cleanupCompleted, [&]() {
                ++cleanupSignals;
                QCoreApplication::quit();
            });
            adapter->start();
            QTimer::singleShot(0, &app, [adapter]() { delete adapter; });
            app.exec();
        } else if (mode == QStringLiteral("about-to-quit-active")) {
            // Mode C/F: aboutToQuit fires while a non-killable active job is retained;
            // aboutToQuit does not block, and ~QCoreApplication destroys retainer & job.
            auto *job = new FakeKJob(KJob::NoCapabilities);
            job->onDestroyedHook = [&]() { backendDestroyed = true; };
            auto *adapter = KJobTransferAdapter::create(job);
            adapter->setAutoDelete(false);
            handle = adapter->cleanupHandle();
            QObject::connect(handle.get(), &PortableTransferCleanupHandle::cleanupCompleted, [&]() {
                ++cleanupSignals;
            });
            adapter->start();
            delete adapter;
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
            app.exec();
        } else if (mode == QStringLiteral("after-exec-detach")) {
            // Mode D: adapter deleted AFTER QCoreApplication::exec() returns;
            // ~QCoreApplication destroys parented retainer & backend without leak.
            auto *job = new FakeKJob(KJob::Killable);
            job->onDestroyedHook = [&]() { backendDestroyed = true; };
            auto *adapter = KJobTransferAdapter::create(job);
            adapter->setAutoDelete(false);
            handle = adapter->cleanupHandle();
            QObject::connect(handle.get(), &PortableTransferCleanupHandle::cleanupCompleted, [&]() {
                ++cleanupSignals;
            });
            adapter->start();
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
            app.exec();
            delete adapter; // detached AFTER exec() returned
        } else {
            return 2;
        }
    }

    const bool ok = backendDestroyed
        && handle
        && handle->isCleanupComplete()
        && cleanupSignals == 1
        && KJobTransferAdapter::pendingDetachedCleanupCount() == 0;
    std::printf("SHUTDOWN_SUBPROCESS[%s]: backendDestroyed=%d cleanupComplete=%d signals=%d pending=%d\n",
                qPrintable(mode),
                backendDestroyed ? 1 : 0,
                (handle && handle->isCleanupComplete()) ? 1 : 0,
                cleanupSignals,
                KJobTransferAdapter::pendingDetachedCleanupCount());
    return ok ? 0 : 1;
}

void testAud2A2_R1_02_EventLoopShutdownSubprocesses()
{
    const QString exe = QCoreApplication::applicationFilePath();
    for (const QString &mode : {QStringLiteral("quit-in-cleanup"),
                                QStringLiteral("about-to-quit-active"),
                                QStringLiteral("after-exec-detach")}) {
        QProcess proc;
        proc.start(exe, {QStringLiteral("--shutdown-mode=%1").arg(mode)});
        CHECK(proc.waitForStarted(5000));
        CHECK(proc.waitForFinished(10000));
        CHECK(proc.exitStatus() == QProcess::NormalExit);
        CHECK(proc.exitCode() == 0);
    }
}

} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QStringLiteral("--shutdown-mode="))) {
            return runShutdownSubprocessMode(arg.mid(16), argc, argv);
        }
    }

    QCoreApplication app(argc, argv);

    testOwnershipValidation();
    testStartModes();
    testBufferedCompletionBeforeStart();
    testSyncAndAsyncCompletion();
    testErrorAndCancelMapping();
    testLifetimeAndDestructionOrder();
    testProgressSpeedAndContext();
    testPauseAndKioCopyJobCapability();
    testCancellationPaths();
    testBandwidthLimiter();
    testFolderMergeMetadataAndFailClosed();
    testReentrantObservers();
    testAud2A2_01_RealWorkerAndCleanupHandle();
    testAud2A2_02_FactoryStartsPreStartCancel();
    testAud2A2_03_PauseAndFallback();
    testAud2A2_04_DecisionPhaseAndEventFilter();
    testAud2A2_05_ConsistencyAndFailClosed();
    testAud2A2_R1_01_RefusedCancelAndNonKillable();
    testAud2A2_R1_03_ResumeAfterCapabilityChange();
    testAud2A2_R1_02_EventLoopShutdownSubprocesses();

    std::printf("KJobTransferAdapter tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

#include "kjob-transfer-adapter-test.moc"
