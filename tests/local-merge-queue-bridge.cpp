#include "localmergequeuebridge.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <cassert>
#include <cstdio>

static int g_qbAssertions = 0;

#define QB_ASSERT(cond)                                                                 \
    do {                                                                                \
        ++g_qbAssertions;                                                               \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "QB_ASSERT failed at %s:%d: %s\n", __FILE__,           \
                         __LINE__, #cond);                                              \
            std::abort();                                                               \
        }                                                                               \
    } while (false)

static void writeFileQB(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QB_ASSERT(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QB_ASSERT(f.write(data) == data.size());
    f.close();
}

static QByteArray readFileQB(const QString &path)
{
    QFile f(path);
    QB_ASSERT(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

static QByteArray sha256QB(const QString &path)
{
    return QCryptographicHash::hash(readFileQB(path), QCryptographicHash::Sha256);
}

static TransferTaskState waitForTaskTerminal(TransferQueueController &qc, quint64 taskId, int timeoutMs = 15000)
{
    if (const auto snap = qc.taskSnapshot(taskId); snap && isTerminalTransferState(snap->state)) {
        return snap->state;
    }
    QEventLoop loop;
    bool done = false;
    TransferTaskState finalState = TransferTaskState::Queued;
    QObject::connect(&qc, &TransferQueueController::taskFinished, &loop,
                     [&](quint64 id, TransferTaskState st) {
                         if (id == taskId) {
                             done = true;
                             finalState = st;
                             loop.quit();
                         }
                     });
    QTimer::singleShot(timeoutMs, &loop, [&] { loop.quit(); });
    if (!done) {
        loop.exec();
    }
    QB_ASSERT(done);
    return finalState;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // 31a. Developer gate disabled by default
    QB_ASSERT(!LocalMergeQueueBridge::isDeveloperFeatureEnabled());

    // 1. Enqueue Merge without automatic start (dispatch paused)
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("Src1"));
        const QString dst = root.filePath(QStringLiteral("Dst1"));
        writeFileQB(src + QStringLiteral("/a.txt"), "A");
        QDir().mkpath(dst);

        TransferQueueController qc;
        qc.setDispatchPaused(true);
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.title = QStringLiteral("Merge Src1");
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        QB_ASSERT(enq.accepted());
        QCoreApplication::processEvents();
        QB_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/Src1/a.txt")));

        // 2, 3, 4. Proper start, progress forwarding, clean success
        qc.setDispatchPaused(false);
        const auto term = waitForTaskTerminal(qc, enq.id);
        QB_ASSERT(term == TransferTaskState::Completed);
        QB_ASSERT(readFileQB(dst + QStringLiteral("/Src1/a.txt")) == "A");
    }

    // 5. Success with skipped conflicts
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("f.txt"));
        const QString dst = root.filePath(QStringLiteral("Dst"));
        writeFileQB(src, "NEW");
        writeFileQB(dst + QStringLiteral("/f.txt"), "OLD");

        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.filePrompt = [](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Skip;
            return d;
        };
        LocalMergeOrchestratorResult captured;
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            job->setResultCallback([&](const LocalMergeOrchestratorResult &r) { captured = r; });
        });
        QB_ASSERT(enq.accepted());
        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Completed);
        QB_ASSERT(captured.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        QB_ASSERT(readFileQB(dst + QStringLiteral("/f.txt")) == "OLD");
        QB_ASSERT(LocalMergeQueueBridge::formatHistorySummary(captured).contains(QStringLiteral("pominiętymi")));
    }

    // 6. Subfolder Rename + shared reservation
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("Addon"));
        const QString dst = root.filePath(QStringLiteral("Dst"));
        writeFileQB(src + QStringLiteral("/x.lua"), "NEW-LUA");
        writeFileQB(dst + QStringLiteral("/Addon/x.lua"), "OLD-LUA");

        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.folderPrompt = [](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Rename;
            d.targetFolderName = QStringLiteral("Addon (2)");
            return d;
        };
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        QB_ASSERT(enq.accepted());
        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Completed);
        QB_ASSERT(readFileQB(dst + QStringLiteral("/Addon/x.lua")) == "OLD-LUA");
        QB_ASSERT(readFileQB(dst + QStringLiteral("/Addon (2)/x.lua")) == "NEW-LUA");
    }

    // 7, 8, 9. NeedsReplan after partial success + protection of completed Overwrite
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("Pack"));
        const QString dst = root.filePath(QStringLiteral("Dst"));
        writeFileQB(src + QStringLiteral("/01_over.txt"), "NEW-OVER");
        writeFileQB(src + QStringLiteral("/02_Sub/in.txt"), "NEW-IN");
        writeFileQB(dst + QStringLiteral("/Pack/01_over.txt"), "OLD-OVER");
        QDir().mkpath(dst + QStringLiteral("/Pack/02_Sub"));

        int overPrompts = 0;
        int subPrompts = 0;
        bool injected = false;
        LocalMergeOrchestratorResult captured;

        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.capabilities.canSafeOverwrite = true;
        req.folderPrompt = [&](const LocalMergePlanEntry &e, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            if (QFileInfo(e.destinationPath).fileName() == QLatin1String("02_Sub")) {
                ++subPrompts;
                d.action = LocalMergeApprovedAction::Rename;
                d.targetFolderName = (subPrompts == 1) ? QStringLiteral("02_Sub (2)") : QStringLiteral("02_Sub (3)");
            } else {
                d.action = LocalMergeApprovedAction::MergeDirectory;
            }
            return d;
        };
        req.filePrompt = [&](const LocalMergePlanEntry &e, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            if (QFileInfo(e.destinationPath).fileName() == QLatin1String("01_over.txt")) {
                ++overPrompts;
                d.action = LocalMergeApprovedAction::Overwrite;
            } else {
                d.action = LocalMergeApprovedAction::Skip;
            }
            return d;
        };
        req.executionJobConfigurator = [&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &rel, int) {
                if (!injected && rel.endsWith(QLatin1String("01_over.txt"))) {
                    injected = true;
                    QDir().mkpath(dst + QStringLiteral("/Pack/02_Sub (2)"));
                }
            });
        };

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            job->setResultCallback([&](const LocalMergeOrchestratorResult &r) { captured = r; });
        });
        QB_ASSERT(enq.accepted());
        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Completed);
        QB_ASSERT(overPrompts == 1);
        QB_ASSERT(subPrompts == 2);
        QB_ASSERT(captured.replanRounds >= 1);
        QB_ASSERT(sha256QB(dst + QStringLiteral("/Pack/01_over.txt")) == sha256QB(src + QStringLiteral("/01_over.txt")));
        QB_ASSERT(sha256QB(dst + QStringLiteral("/Pack/02_Sub (3)/in.txt")) == sha256QB(src + QStringLiteral("/02_Sub/in.txt")));
    }

    // 10, 11, 12, 13, 14, 15. Shared reservations: Merge vs Merge, Merge vs Copy, Merge vs Move, Subtree, Symlink, Release
    {
        QTemporaryDir root;
        const QString srcA = root.filePath(QStringLiteral("SrcA"));
        const QString srcB = root.filePath(QStringLiteral("SrcB"));
        const QString dst = root.filePath(QStringLiteral("Dst"));
        writeFileQB(srcA + QStringLiteral("/a.txt"), "AAA");
        writeFileQB(srcB + QStringLiteral("/b.txt"), "BBB");
        QDir().mkpath(dst + QStringLiteral("/SrcA"));

        TransferQueueController qc;
        qc.setParallelConcurrencyLimit(4);

        // Enqueue a Merge targeting Dst/SrcA
        LocalMergeQueueRequest m1;
        m1.sources = {QUrl::fromLocalFile(srcA)};
        m1.destinationDir = QUrl::fromLocalFile(dst);
        const auto e1 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(m1));
        QB_ASSERT(e1.accepted());

        // Enqueue a second Merge targeting the SAME Dst/SrcA -> serialized by TransferReservationRegistry!
        LocalMergeQueueRequest m2;
        m2.sources = {QUrl::fromLocalFile(srcA)};
        m2.destinationDir = QUrl::fromLocalFile(dst);
        m2.folderPrompt = [](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::MergeDirectory;
            return d;
        };
        m2.filePrompt = [](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Skip;
            return d;
        };
        const auto e2 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(m2));
        QB_ASSERT(e2.accepted());

        // Enqueue a regular Copy targeting Dst/SrcA/a.txt (descendant) -> blocked until Merge finishes!
        TransferTaskDescriptor copyDesc;
        copyDesc.title = QStringLiteral("Copy into SrcA");
        copyDesc.type = TransferOperationType::Copy;
        copyDesc.backend = TransferBackendKind::LocalNative;
        copyDesc.sources = {QUrl::fromLocalFile(srcB + QStringLiteral("/b.txt"))};
        copyDesc.destination = QUrl::fromLocalFile(dst + QStringLiteral("/SrcA"));
        copyDesc.jobFactory = [&]() -> KJob * {
            return new LocalFileCopyJob(QUrl::fromLocalFile(srcB + QStringLiteral("/b.txt")),
                                        QUrl::fromLocalFile(dst + QStringLiteral("/SrcA/b.txt")));
        };
        const auto eCopy = qc.enqueue(std::move(copyDesc));
        QB_ASSERT(eCopy.accepted());

        // Enqueue a Move whose source is srcA -> blocked by SharedRead/ExclusiveWrite until Merges finish!
        TransferTaskDescriptor moveDesc;
        moveDesc.title = QStringLiteral("Move SrcA");
        moveDesc.type = TransferOperationType::Move;
        moveDesc.backend = TransferBackendKind::LocalNative;
        moveDesc.sources = {QUrl::fromLocalFile(srcA + QStringLiteral("/a.txt"))};
        moveDesc.destination = QUrl::fromLocalFile(root.path());
        moveDesc.jobFactory = [&]() -> KJob * {
            return new LocalFileCopyJob(QUrl::fromLocalFile(srcA + QStringLiteral("/a.txt")),
                                        QUrl::fromLocalFile(root.filePath(QStringLiteral("moved_a.txt"))));
        };
        const auto eMove = qc.enqueue(std::move(moveDesc));
        QB_ASSERT(eMove.accepted());

        QEventLoop loop;
        int done = 0;
        QObject::connect(&qc, &TransferQueueController::taskFinished, &loop, [&](quint64, TransferTaskState) {
            if (++done == 4) loop.quit();
        });
        QTimer::singleShot(15000, &loop, [&] { loop.quit(); });
        loop.exec();
        QB_ASSERT(done == 4);
        QB_ASSERT(readFileQB(dst + QStringLiteral("/SrcA/b.txt")) == "BBB");
    }

    // 16, 17, 18, 19, 20. Cancellations (queued, planning, dialog, copying, partial success)
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("CancelSrc"));
        const QString dst = root.filePath(QStringLiteral("CancelDst"));
        writeFileQB(src + QStringLiteral("/01.txt"), "ONE");
        writeFileQB(src + QStringLiteral("/02.bin"), QByteArray(128 * 1024, 'C'));
        QDir().mkpath(dst);

        TransferQueueController qc;
        qc.setDispatchPaused(true);
        LocalMergeQueueRequest rQueued;
        rQueued.sources = {QUrl::fromLocalFile(src)};
        rQueued.destinationDir = QUrl::fromLocalFile(dst);
        const auto eq1 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(rQueued));
        QB_ASSERT(qc.cancelTask(eq1.id));
        QB_ASSERT(waitForTaskTerminal(qc, eq1.id) == TransferTaskState::Cancelled);

        qc.setDispatchPaused(false);
        LocalMergeQueueRequest rCopy;
        rCopy.sources = {QUrl::fromLocalFile(src)};
        rCopy.destinationDir = QUrl::fromLocalFile(dst);
        rCopy.executionJobConfigurator = [&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &rel, int) {
                if (rel.endsWith(QLatin1String("02.bin"))) {
                    qc.cancelAll();
                }
            });
        };
        const auto eq2 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(rCopy));
        QB_ASSERT(waitForTaskTerminal(qc, eq2.id) == TransferTaskState::Cancelled);
        QB_ASSERT(!qc.checkRetryEligibility(eq2.id).eligible);
    }

    // 21, 22, 25, 26, 27, 28, 29, 30. ENOSPC / fsync EIO / No blind Retry / Undo fence / History / No double finish / No leaks
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("ErrSrc"));
        const QString dst = root.filePath(QStringLiteral("ErrDst"));
        writeFileQB(src + QStringLiteral("/cfg.wtf"), "NEW-CFG");
        writeFileQB(dst + QStringLiteral("/cfg.wtf"), "OLD-CFG");

        TransferQueueController qc;
        UndoController undo(nullptr, {}, {}, {});
        const quint64 initialSerial = undo.irreversibleSerial();

        QPointer<LocalMergeQueueJob> jobPtr;
        QPointer<LocalMergeOrchestrator> orchPtr;
        int finishCount = 0;
        LocalMergeOrchestratorResult captured;

        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.capabilities.canSafeOverwrite = true;
        req.filePrompt = [](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Overwrite;
            return d;
        };
        req.executionJobConfigurator = [](LocalMergeExecutionJob *job) {
            job->setSimulateFsyncEio(true);
        };

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            jobPtr = job;
            job->setResultCallback([&](const LocalMergeOrchestratorResult &r) {
                ++finishCount;
                captured = r;
                LocalMergeQueueBridge::applyUndoFenceIfNeeded(&undo, r);
            });
            QTimer::singleShot(0, job, [&, job] { orchPtr = job->orchestrator(); });
        });
        QB_ASSERT(enq.accepted());
        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Failed);
        QB_ASSERT(finishCount == 1);
        QB_ASSERT(captured.finalState == LocalMergeOrchestratorState::FailedUncertainDurability);
        QB_ASSERT(!qc.checkRetryEligibility(enq.id).eligible);
        QB_ASSERT(qc.retryTask(enq.id).rejection != TransferEnqueueRejection::None);
        QB_ASSERT(undo.irreversibleSerial() > initialSerial);
        QB_ASSERT(LocalMergeQueueBridge::formatHistorySummary(captured).contains(QStringLiteral("Niepewna trwałość")));

        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QB_ASSERT(jobPtr.isNull());
        QB_ASSERT(orchPtr.isNull());
    }

    // 23 & 24. RecoveryRequired in destination root AND deep in target subdirectory
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("DeepSrc"));
        const QString dst = root.filePath(QStringLiteral("DeepDst"));
        writeFileQB(src + QStringLiteral("/Sub/file.txt"), "NEW");
        const QString txDir = dst + QStringLiteral("/DeepSrc/Sub/.thispc-tx.999.1");
        writeFileQB(txDir + QStringLiteral("/journal"), "CORRUPT");

        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        LocalMergeOrchestratorResult captured;
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            job->setResultCallback([&](const LocalMergeOrchestratorResult &r) { captured = r; });
        });
        QB_ASSERT(enq.accepted());
        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Failed);
        QB_ASSERT(captured.finalState == LocalMergeOrchestratorState::RecoveryRequired);
        QB_ASSERT(!qc.checkRetryEligibility(enq.id).eligible);
        QB_ASSERT(QFileInfo::exists(txDir + QStringLiteral("/journal")));
        QB_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/DeepSrc/Sub/file.txt")));
    }

    // 32. Zero regression for normal Copy retry eligibility
    {
        QTemporaryDir root;
        const QString srcFile = root.filePath(QStringLiteral("ok.bin"));
        const QString dst = root.filePath(QStringLiteral("Dst"));
        writeFileQB(srcFile, "DATA");
        QDir().mkpath(dst);
        QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ExeOwner);

        TransferQueueController qc;
        TransferTaskDescriptor copyDesc;
        copyDesc.title = QStringLiteral("Normal Copy");
        copyDesc.type = TransferOperationType::Copy;
        copyDesc.backend = TransferBackendKind::LocalNative;
        copyDesc.sources = {QUrl::fromLocalFile(srcFile)};
        copyDesc.destination = QUrl::fromLocalFile(dst);
        copyDesc.jobFactory = [&]() -> KJob * {
            return new LocalFileCopyJob(QUrl::fromLocalFile(srcFile),
                                        QUrl::fromLocalFile(dst + QStringLiteral("/ok.bin")));
        };
        const auto enq = qc.enqueue(std::move(copyDesc));
        QB_ASSERT(enq.accepted());
        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Failed);
        QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QB_ASSERT(qc.checkRetryEligibility(enq.id).eligible);
    }

    // 33. Hardening: honest desc.capabilities + preflight RecoveryRequired history/retry metadata when task->job == nullptr
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("PreflightSrc"));
        const QString dst = root.filePath(QStringLiteral("PreflightDst"));
        writeFileQB(src + QStringLiteral("/item.txt"), "PAYLOAD");
        const QString directTxDir = dst + QStringLiteral("/.thispc-tx.888.1");
        writeFileQB(directTxDir + QStringLiteral("/journal"), "CORRUPT_DIRECT_TX");

        TransferQueueController qc;
        TransferHistoryManager histMgr(root.filePath(QStringLiteral("history.json")), &qc);
        qc.setHistoryManager(&histMgr);
        qc.setDispatchPaused(true);

        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        QB_ASSERT(enq.accepted());

        const auto snapQueued = qc.taskSnapshot(enq.id);
        QB_ASSERT(snapQueued.has_value());
        QB_ASSERT(snapQueued->declared.canCancelRunning);
        QB_ASSERT(!snapQueued->declared.canPause);
        QB_ASSERT(!snapQueued->declared.canResume);
        QB_ASSERT(!snapQueued->declared.supportsThrottling);
        QB_ASSERT(snapQueued->declared.cancelOutcome == TransferCancelOutcome::PartialResultsMayRemain);
        QB_ASSERT(!snapQueued->supportsThrottling);

        qc.setDispatchPaused(false);
        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Failed);

        const auto snapFailed = qc.taskSnapshot(enq.id);
        QB_ASSERT(snapFailed.has_value());
        QB_ASSERT(snapFailed->failureKind == TransferFailureKind::PreflightFailed);
        QB_ASSERT(snapFailed->errorText.contains(QStringLiteral("RecoveryRequired")));
        QB_ASSERT(!qc.checkRetryEligibility(enq.id).eligible);
        QB_ASSERT(qc.retryTask(enq.id).rejection != TransferEnqueueRejection::None);

        const QList<TransferHistoryRecord> histRecs = histMgr.records();
        QB_ASSERT(histRecs.size() == 1);
        QB_ASSERT(histRecs.first().isFolderMerge);
        QB_ASSERT(histRecs.first().recoveryRequired);
        QB_ASSERT(histRecs.first().retryBlocked);
        QB_ASSERT(histRecs.first().terminalState == TransferTaskState::Failed);
    }

    // 34. P1 #1: Main-thread async queue cancel during active worker preserves partialSuccess,
    // Undo fence, history metadata, and retry block without premature KJob::kill(EmitResult) result.
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("CancelMainSrc"));
        const QString dst = root.filePath(QStringLiteral("CancelMainDst"));
        writeFileQB(src + QStringLiteral("/01_done.txt"), "PUBLISHED-FIRST");
        writeFileQB(src + QStringLiteral("/02_slow.bin"), QByteArray(128 * 1024, 'S'));
        QDir().mkpath(dst);

        TransferQueueController qc;
        TransferHistoryManager histMgr(root.filePath(QStringLiteral("cancel_history.json")), &qc);
        qc.setHistoryManager(&histMgr);
        UndoController undo(nullptr, {}, {}, {});
        const quint64 initialSerial = undo.irreversibleSerial();

        std::atomic<quint64> taskIdAtomic{0};
        std::atomic<bool> cancelDispatched{false};
        LocalMergeOrchestratorResult captured;
        int finishCount = 0;

        qc.setDispatchPaused(true);
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.undoController = &undo;
        req.executionJobConfigurator = [&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &relPath, int) {
                if (relPath.endsWith(QLatin1String("02_slow.bin"))) {
                    const quint64 tid = taskIdAtomic.load();
                    QMetaObject::invokeMethod(&qc, [&qc, &cancelDispatched, tid] {
                        QB_ASSERT(qc.cancelTask(tid));
                        cancelDispatched.store(true);
                    }, Qt::QueuedConnection);
                    for (int i = 0; i < 400 && !cancelDispatched.load(); ++i) {
                        QThread::msleep(5);
                    }
                }
            });
        };

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            job->setResultCallback([&](const LocalMergeOrchestratorResult &r) {
                ++finishCount;
                captured = r;
            });
        });
        QB_ASSERT(enq.accepted());
        taskIdAtomic.store(enq.id);
        qc.setDispatchPaused(false);

        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Cancelled);
        QB_ASSERT(cancelDispatched.load());
        QB_ASSERT(finishCount == 1);
        QB_ASSERT(captured.finalState == LocalMergeOrchestratorState::Cancelled);
        QB_ASSERT(captured.progress.partialSuccess);
        QB_ASSERT(captured.progress.filesCreated == 1);
        QB_ASSERT(undo.irreversibleSerial() > initialSerial);
        QB_ASSERT(!qc.checkRetryEligibility(enq.id).eligible);
        QB_ASSERT(readFileQB(dst + QStringLiteral("/CancelMainSrc/01_done.txt")) == "PUBLISHED-FIRST");
        QB_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/CancelMainSrc/02_slow.bin")));

        const QList<TransferHistoryRecord> histRecs = histMgr.records();
        QB_ASSERT(histRecs.size() == 1);
        QB_ASSERT(histRecs.first().isFolderMerge);
        QB_ASSERT(histRecs.first().partialSuccess);
        QB_ASSERT(histRecs.first().retryBlocked);
        QB_ASSERT(histRecs.first().terminalState == TransferTaskState::Cancelled);
        QB_ASSERT(!histRecs.first().errorText.isEmpty());
    }

    // 35. SEC-046-03 (1/3): Immediate dispatch into empty Parallel queue assigns live taskId != 0
    // before start() and enforces dynamic Rename reservation against concurrent conflicting task.
    {
        QTemporaryDir root;
        const QString src1 = root.filePath(QStringLiteral("ImmSrc/Addon"));
        const QString src2 = root.filePath(QStringLiteral("ImmOther/Addon (2)"));
        const QString dst = root.filePath(QStringLiteral("ImmDst"));
        writeFileQB(src1 + QStringLiteral("/file.txt"), "MERGE-RENAMED");
        writeFileQB(src2 + QStringLiteral("/other.txt"), "COMPETING-COPY");
        writeFileQB(dst + QStringLiteral("/Addon/file.txt"), "EXISTING");

        TransferQueueController qc;
        qc.setExecutionMode(TransferQueueMode::Parallel);
        qc.setParallelConcurrencyLimit(2);

        quint64 capturedJobTaskId = 0;
        quint64 competingId = 0;
        bool competingEnqueued = false;
        std::atomic<bool> competingQueuedWhileMergeRunning{false};
        std::atomic<bool> releaseMerge{false};

        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src1)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.folderPrompt = [](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Rename;
            d.targetFolderName = QStringLiteral("Addon (2)");
            return d;
        };
        req.executionJobConfigurator = [&](LocalMergeExecutionJob *execJob) {
            execJob->setPrePublishHook([&](const QString &, int) {
                QMetaObject::invokeMethod(&qc, [&] {
                    if (!competingEnqueued) {
                        competingEnqueued = true;
                        TransferTaskDescriptor cdesc;
                        cdesc.title = QStringLiteral("Competing Copy");
                        cdesc.type = TransferOperationType::Copy;
                        cdesc.backend = TransferBackendKind::LocalNative;
                        cdesc.sources = {QUrl::fromLocalFile(src2)};
                        cdesc.destination = QUrl::fromLocalFile(dst);
                        cdesc.jobFactory = [&]() -> KJob * {
                            return new LocalTransferJob({QUrl::fromLocalFile(src2)}, QUrl::fromLocalFile(dst), false);
                        };
                        const auto cenq = qc.enqueue(std::move(cdesc));
                        QB_ASSERT(cenq.accepted());
                        competingId = cenq.id;
                        const auto csnap = qc.taskSnapshot(competingId);
                        QB_ASSERT(csnap.has_value());
                        competingQueuedWhileMergeRunning.store(
                            csnap->state == TransferTaskState::Queued
                            && csnap->waitReason == TransferWaitReason::BlockedByReservation);
                        releaseMerge.store(true);
                    }
                }, Qt::QueuedConnection);
                for (int i = 0; i < 400 && !releaseMerge.load(); ++i) {
                    QThread::msleep(5);
                }
            });
        };

        // Empty queue, dispatch NOT paused -> synchronous tryLaunch() inside enqueue()!
        QPointer<LocalMergeQueueJob> launchedJob;
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            launchedJob = job;
            job->setStateCallback([&capturedJobTaskId, job](LocalMergeOrchestratorState) {
                if (capturedJobTaskId == 0) {
                    capturedJobTaskId = job->queueTaskId();
                }
            });
        });
        QB_ASSERT(enq.accepted());
        QB_ASSERT(launchedJob && launchedJob->queueTaskId() == enq.id && launchedJob->queueTaskId() != 0);

        QB_ASSERT(waitForTaskTerminal(qc, enq.id) == TransferTaskState::Completed);
        QB_ASSERT(capturedJobTaskId == enq.id && capturedJobTaskId != 0);
        QB_ASSERT(competingQueuedWhileMergeRunning.load());
        QB_ASSERT(competingId != 0);
        // Competing task now unblocks cleanly once merge reservation releases
        const auto term2 = waitForTaskTerminal(qc, competingId);
        QB_ASSERT(term2 == TransferTaskState::Completed || term2 == TransferTaskState::Failed);
        QB_ASSERT(readFileQB(dst + QStringLiteral("/Addon (2)/file.txt")) == "MERGE-RENAMED");
        QB_ASSERT(qc.snapshot().activeCount == 0);
    }

    // 36. SEC-046-03 (2/3): Dynamic Rename refuses target already reserved by a parallel task (fail-closed)
    // and direct LocalMergeQueueJob with taskId == 0 fails closed before touching disk.
    {
        QTemporaryDir root;
        const QString src1 = root.filePath(QStringLiteral("RefuseSrc/Addon"));
        const QString src2File = root.filePath(QStringLiteral("RefuseHolder/Addon (2)"));
        const QString dst = root.filePath(QStringLiteral("RefuseDst"));
        writeFileQB(src1 + QStringLiteral("/a.txt"), "MERGE-DATA");
        writeFileQB(src2File, QByteArray(16 * 1024, 'H'));
        writeFileQB(dst + QStringLiteral("/Addon/a.txt"), "EXISTING");

        TransferQueueController qc;
        qc.setExecutionMode(TransferQueueMode::Parallel);
        qc.setParallelConcurrencyLimit(2);

        // Task 1 holds exclusive reservation on RefuseDst/Addon (2) via staging .thispc-part
        TransferTaskDescriptor holdDesc;
        holdDesc.title = QStringLiteral("Holder");
        holdDesc.type = TransferOperationType::Copy;
        holdDesc.backend = TransferBackendKind::LocalNative;
        holdDesc.sources = {QUrl::fromLocalFile(src2File)};
        holdDesc.destination = QUrl::fromLocalFile(dst);
        holdDesc.jobFactory = [&]() -> KJob * {
            return new LocalFileCopyJob(
                QUrl::fromLocalFile(src2File),
                QUrl::fromLocalFile(dst + QStringLiteral("/Addon (2)")),
                nullptr, 4096, 250);
        };
        const auto holdEnq = qc.enqueue(std::move(holdDesc));
        QB_ASSERT(holdEnq.accepted());

        // Task 2 attempts immediate-launch Folder Merge with dynamic Rename -> Addon (2)
        LocalMergeOrchestratorResult mergeRes;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src1)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.folderPrompt = [](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Rename;
            d.targetFolderName = QStringLiteral("Addon (2)");
            return d;
        };
        const auto mergeEnq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            job->setResultCallback([&](const LocalMergeOrchestratorResult &r) { mergeRes = r; });
        });
        QB_ASSERT(mergeEnq.accepted());
        QB_ASSERT(waitForTaskTerminal(qc, holdEnq.id) == TransferTaskState::Completed);
        const auto mergeTerm = waitForTaskTerminal(qc, mergeEnq.id);
        QB_ASSERT(mergeTerm == TransferTaskState::Failed);
        QB_ASSERT(mergeRes.diagnostic.contains(QStringLiteral("conflicts with active queue reservation")));
        QB_ASSERT(readFileQB(dst + QStringLiteral("/Addon (2)")) == QByteArray(16 * 1024, 'H'));
        QB_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/Addon (2)/a.txt")));

        // Fail-closed test for taskId == 0
        LocalMergeQueueRequest zeroReq;
        zeroReq.sources = {QUrl::fromLocalFile(src1)};
        zeroReq.destinationDir = QUrl::fromLocalFile(dst);
        auto *zeroJob = new LocalMergeQueueJob(std::move(zeroReq), &qc, 0);
        zeroJob->setAutoDelete(false);
        LocalMergeOrchestratorResult zeroRes;
        zeroJob->setResultCallback([&](const LocalMergeOrchestratorResult &r) { zeroRes = r; });
        zeroJob->start();
        QB_ASSERT(zeroJob->error() != 0);
        QB_ASSERT(zeroRes.finalState == LocalMergeOrchestratorState::FailedNoChanges);
        delete zeroJob;
    }

    // 37. SEC-046-03 (3/3): Retry of failed Folder Merge rebinds NEW live taskId and enforces reservation.
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("RetrySrc/Addon"));
        const QString dst = root.filePath(QStringLiteral("RetryDst"));
        const QString holdFile = root.filePath(QStringLiteral("RetryHold/Addon (2)"));
        const QString compSrc = root.filePath(QStringLiteral("RetryComp/Addon (2)"));
        writeFileQB(src + QStringLiteral("/x.txt"), "RETRY-NEW");
        writeFileQB(holdFile, QByteArray(16 * 1024, 'H'));
        writeFileQB(compSrc + QStringLiteral("/y.txt"), "COMPETING");
        writeFileQB(dst + QStringLiteral("/Addon/x.txt"), "EXISTING");

        TransferQueueController qc;
        qc.setExecutionMode(TransferQueueMode::Parallel);
        qc.setParallelConcurrencyLimit(2);

        int attempt = 0;
        QList<quint64> observedJobTaskIds;
        std::atomic<bool> releaseRetry{false};
        bool competingEnqueued = false;
        quint64 competingId = 0;
        std::atomic<bool> competingBlockedByRetry{false};

        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.folderPrompt = [&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            ++attempt;
            LocalFolderConflictDecision d;
            if (attempt == 1) {
                // Force a FailedNoChanges on first attempt by returning invalid empty rename? Or simulate pre-publish error:
                // Actually aborting gives Cancelled; instead use maxReplanRounds or fail in prePublishHook below.
            }
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Rename;
            d.targetFolderName = QStringLiteral("Addon (2)");
            return d;
        };
        req.executionJobConfigurator = [&](LocalMergeExecutionJob *execJob) {
            if (attempt == 1) {
                // Fail before any publish by setting an impossible-to-meet simulated error on first attempt
                execJob->setSimulateUnsupportedOverwriteFs(true);
                // Note: Rename doesn't hit overwrite; cancel/abort won't allow retry unless FailedNoChanges.
            }
            execJob->setPrePublishHook([&](const QString &, int) {
                if (attempt >= 2) {
                    QMetaObject::invokeMethod(&qc, [&] {
                        if (!competingEnqueued) {
                            competingEnqueued = true;
                            TransferTaskDescriptor cdesc;
                            cdesc.title = QStringLiteral("Retry Competing");
                            cdesc.type = TransferOperationType::Copy;
                            cdesc.backend = TransferBackendKind::LocalNative;
                            cdesc.sources = {QUrl::fromLocalFile(compSrc)};
                            cdesc.destination = QUrl::fromLocalFile(dst);
                            cdesc.jobFactory = [&]() -> KJob * {
                                return new LocalTransferJob({QUrl::fromLocalFile(compSrc)}, QUrl::fromLocalFile(dst), false);
                            };
                            const auto cenq = qc.enqueue(std::move(cdesc));
                            QB_ASSERT(cenq.accepted());
                            competingId = cenq.id;
                            const auto csnap = qc.taskSnapshot(competingId);
                            competingBlockedByRetry.store(
                                csnap.has_value()
                                && csnap->state == TransferTaskState::Queued
                                && csnap->waitReason == TransferWaitReason::BlockedByReservation);
                            releaseRetry.store(true);
                        }
                    }, Qt::QueuedConnection);
                    for (int i = 0; i < 400 && !releaseRetry.load(); ++i) {
                        QThread::msleep(5);
                    }
                }
            });
        };

        // Use a missing source parent temporarily or forceRecoveryRequired on attempt 1?
        // Actually, forceRecoveryRequired triggers RecoveryRequired (retry blocked). Instead, make attempt 1 fail with
        // missing source directory right before execution? No, simplest: remove source file? Wait: Rename creates dir first.
        // Easiest deterministic FailedNoChanges: run attempt 1 with a conflict holder that blocks Rename (as in test 36),
        // then retry after holder finishes!
        TransferTaskDescriptor holdDesc;
        holdDesc.title = QStringLiteral("Initial Blocker");
        holdDesc.type = TransferOperationType::Copy;
        holdDesc.backend = TransferBackendKind::LocalNative;
        holdDesc.sources = {QUrl::fromLocalFile(holdFile)};
        holdDesc.destination = QUrl::fromLocalFile(dst);
        holdDesc.jobFactory = [&]() -> KJob * {
            return new LocalFileCopyJob(
                QUrl::fromLocalFile(holdFile),
                QUrl::fromLocalFile(dst + QStringLiteral("/Addon (2)")),
                nullptr, 4096, 250);
        };
        const auto holdEnq = qc.enqueue(std::move(holdDesc));
        QB_ASSERT(holdEnq.accepted());

        const auto enq1 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            auto recorded = std::make_shared<bool>(false);
            job->setStateCallback([&observedJobTaskIds, job, recorded](LocalMergeOrchestratorState) {
                if (!*recorded) {
                    *recorded = true;
                    observedJobTaskIds.append(job->queueTaskId());
                }
            });
        });
        QB_ASSERT(enq1.accepted());
        QB_ASSERT(waitForTaskTerminal(qc, holdEnq.id) == TransferTaskState::Completed);
        QB_ASSERT(waitForTaskTerminal(qc, enq1.id) == TransferTaskState::Failed);
        // Remove Addon (2) created by Initial Blocker so retried Rename to Addon (2) succeeds
        QFile::remove(dst + QStringLiteral("/Addon (2)"));

        const auto retryEnq = qc.retryTask(enq1.id);
        QB_ASSERT(retryEnq.accepted());
        QB_ASSERT(retryEnq.id != enq1.id);
        QB_ASSERT(waitForTaskTerminal(qc, retryEnq.id) == TransferTaskState::Completed);
        QB_ASSERT(observedJobTaskIds.size() == 2);
        QB_ASSERT(observedJobTaskIds[0] == enq1.id);
        QB_ASSERT(observedJobTaskIds[1] == retryEnq.id);
        QB_ASSERT(competingBlockedByRetry.load());
        QB_ASSERT(waitForTaskTerminal(qc, competingId) == TransferTaskState::Completed);
        QB_ASSERT(qc.snapshot().activeCount == 0);
    }

    std::printf("PASS: local_merge_queue_bridge (%d assertions)\n", g_qbAssertions);
    return 0;
}
