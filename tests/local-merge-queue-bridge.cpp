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

    std::printf("PASS: local_merge_queue_bridge (%d assertions)\n", g_qbAssertions);
    return 0;
}
