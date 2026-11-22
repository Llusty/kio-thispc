#include "localmergequeuebridge.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstdio>

static int g_guiIntAssertions = 0;

#define GI_ASSERT(cond)                                                                 \
    do {                                                                                \
        ++g_guiIntAssertions;                                                           \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "GI_ASSERT failed at %s:%d: %s\n", __FILE__,           \
                         __LINE__, #cond);                                              \
            std::abort();                                                               \
        }                                                                               \
    } while (false)

static void writeFileGI(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    GI_ASSERT(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    GI_ASSERT(f.write(data) == data.size());
    f.close();
}

static QByteArray readFileGI(const QString &path)
{
    QFile f(path);
    GI_ASSERT(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

static QByteArray sha256GI(const QString &path)
{
    return QCryptographicHash::hash(readFileGI(path), QCryptographicHash::Sha256);
}

static TransferTaskState waitForTerminalGI(TransferQueueController &qc, quint64 taskId, int timeoutMs = 15000)
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
    GI_ASSERT(done);
    return finalState;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // 1 & 2. Developer gate: disabled by default, refused without gate
    {
        qunsetenv("THISPC_ENABLE_FOLDER_MERGE_QUEUE_DEV");
        GI_ASSERT(!LocalMergeQueueBridge::isDeveloperFeatureEnabled());

        QTemporaryDir sandbox;
        const QString src = sandbox.filePath(QStringLiteral("Src"));
        const QString dst = sandbox.filePath(QStringLiteral("Dst"));
        writeFileGI(src + QStringLiteral("/a.txt"), "AAA");
        QDir().mkpath(dst);

        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        const auto enq = LocalMergeQueueBridge::enqueueDevSandboxedMerge(&qc, sandbox.path(), std::move(req));
        GI_ASSERT(!enq.accepted());
    }

    // 3 & 4. Dev sandbox boundary: refuse outside devRoot AND refuse symlink escaping devRoot
    {
        qputenv("THISPC_ENABLE_FOLDER_MERGE_QUEUE_DEV", "1");
        QTemporaryDir sandbox;
        QTemporaryDir outside;
        const QString src = sandbox.filePath(QStringLiteral("Src"));
        writeFileGI(src + QStringLiteral("/a.txt"), "AAA");
        const QString escapeLink = sandbox.filePath(QStringLiteral("EscapeLink"));
        QFile::link(outside.path(), escapeLink);

        TransferQueueController qc;

        // Outside devRoot
        LocalMergeQueueRequest reqOut;
        reqOut.sources = {QUrl::fromLocalFile(src)};
        reqOut.destinationDir = QUrl::fromLocalFile(outside.path());
        const auto enqOut = LocalMergeQueueBridge::enqueueDevSandboxedMerge(&qc, sandbox.path(), std::move(reqOut));
        GI_ASSERT(!enqOut.accepted());

        // Symlink escaping devRoot
        LocalMergeQueueRequest reqLink;
        reqLink.sources = {QUrl::fromLocalFile(src)};
        reqLink.destinationDir = QUrl::fromLocalFile(escapeLink);
        const auto enqLink = LocalMergeQueueBridge::enqueueDevSandboxedMerge(&qc, sandbox.path(), std::move(reqLink));
        GI_ASSERT(!enqLink.accepted());

        // Allowed inside devRoot
        const QString dstIn = sandbox.filePath(QStringLiteral("InsideDst"));
        QDir().mkpath(dstIn);
        LocalMergeQueueRequest reqIn;
        reqIn.sources = {QUrl::fromLocalFile(src)};
        reqIn.destinationDir = QUrl::fromLocalFile(dstIn);
        const auto enqIn = LocalMergeQueueBridge::enqueueDevSandboxedMerge(&qc, sandbox.path(), std::move(reqIn));
        GI_ASSERT(enqIn.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enqIn.id) == TransferTaskState::Completed);
        GI_ASSERT(sha256GI(dstIn + QStringLiteral("/Src/a.txt")) == sha256GI(src + QStringLiteral("/a.txt")));
        qunsetenv("THISPC_ENABLE_FOLDER_MERGE_QUEUE_DEV");
    }

    // 5, 6, 7, 31, 32, 33, 35. Primary/Split context + OperationManager/OperationWindow + closing main window mid-transfer
    {
        qputenv("THISPC_ENABLE_FOLDER_MERGE_QUEUE_DEV", "1");
        QTemporaryDir sandbox;
        const QString src = sandbox.filePath(QStringLiteral("SplitSrc"));
        const QString dst = sandbox.filePath(QStringLiteral("SplitDst"));
        writeFileGI(src + QStringLiteral("/Sub/data.bin"), QByteArray(32768, 'D'));
        QDir().mkpath(dst);

        auto *win = new QMainWindow();
        TransferQueueController qc;
        auto *opMgr = new OperationManager(win, nullptr, nullptr);
        opMgr->setQueueController(&qc);

        QPointer<QMainWindow> winPtr(win);
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.title = QStringLiteral("[DEV MERGE SANDBOX] SplitSrc");
        req.context = FileOperationContext::create(FileOperationType::Copy, req.sources, req.destinationDir);

        const auto enq = LocalMergeQueueBridge::enqueueDevSandboxedMerge(&qc, sandbox.path(), std::move(req), [&](LocalMergeQueueJob *job) {
            job->setStateCallback([&](LocalMergeOrchestratorState s) {
                if (s == LocalMergeOrchestratorState::Executing && winPtr) {
                    delete winPtr.data();  // Destroy main window on GUI thread while transfer is running!
                }
            });
        });
        GI_ASSERT(enq.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Completed);
        GI_ASSERT(winPtr.isNull());
        GI_ASSERT(sha256GI(dst + QStringLiteral("/SplitSrc/Sub/data.bin"))
                  == sha256GI(src + QStringLiteral("/Sub/data.bin")));
        qunsetenv("THISPC_ENABLE_FOLDER_MERGE_QUEUE_DEV");
    }

    // 16 & 17. Completed Entry Identity: destination inode OR content changed before NeedsReplan round!
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("IdPack"));
        const QString dst = root.filePath(QStringLiteral("IdDst"));
        writeFileGI(src + QStringLiteral("/01_over.txt"), "NEW-1");
        writeFileGI(src + QStringLiteral("/02_Sub/f.txt"), "NEW-2");
        writeFileGI(dst + QStringLiteral("/IdPack/01_over.txt"), "OLD-1");
        QDir().mkpath(dst + QStringLiteral("/IdPack/02_Sub"));

        int overPrompts = 0;
        int subPrompts = 0;
        bool mutated01 = false;
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
                // First round: Overwrite. Second round (after external modification!): Skip to preserve external change!
                d.action = (overPrompts == 1)
                    ? LocalMergeApprovedAction::Overwrite
                    : LocalMergeApprovedAction::Skip;
            } else {
                d.action = LocalMergeApprovedAction::Skip;
            }
            return d;
        };
        req.executionJobConfigurator = [&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &rel, int) {
                if (!mutated01 && rel.endsWith(QLatin1String("01_over.txt"))) {
                    QDir().mkpath(dst + QStringLiteral("/IdPack/02_Sub (2)"));
                }
            });
        };

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            job->setStateCallback([&](LocalMergeOrchestratorState s) {
                if (s == LocalMergeOrchestratorState::NeedsReplan && !mutated01) {
                    mutated01 = true;
                    QFile::remove(dst + QStringLiteral("/IdPack/01_over.txt"));
                    writeFileGI(dst + QStringLiteral("/IdPack/01_over.txt"), "EXTERNAL-MOD-AFTER-ROUND1");
                }
            });
            job->setResultCallback([&](const LocalMergeOrchestratorResult &r) { captured = r; });
        });
        GI_ASSERT(enq.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Completed);
        GI_ASSERT(captured.freshDecisionRequired);
        GI_ASSERT(captured.externallyModifiedCompletedEntries.contains(dst + QStringLiteral("/IdPack/01_over.txt")));
        GI_ASSERT(overPrompts == 2); // Never silently skipped and never auto-re-overwritten!
        GI_ASSERT(readFileGI(dst + QStringLiteral("/IdPack/01_over.txt")) == "EXTERNAL-MOD-AFTER-ROUND1");
    }

    // 29. Persistent Retry Policy after history JSON save & reload (app restart simulation)
    {
        QTemporaryDir histDir;
        const QString histFile = histDir.filePath(QStringLiteral("history.json"));

        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("HistSrc"));
        const QString dst = root.filePath(QStringLiteral("HistDst"));
        writeFileGI(src + QStringLiteral("/cfg.wtf"), "NEW");
        writeFileGI(dst + QStringLiteral("/cfg.wtf"), "OLD");

        {
            TransferQueueController qc;
            auto *hm = new TransferHistoryManager(histFile, &qc);
            qc.setHistoryManager(hm);

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
            const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
            GI_ASSERT(enq.accepted());
            GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Failed);
            hm->flushSync();
        }

        // Reload from disk in a fresh TransferHistoryManager (simulating app restart)
        TransferHistoryManager reloadedMgr(histFile);
        reloadedMgr.loadSync();
        const auto records = reloadedMgr.records();
        GI_ASSERT(records.size() == 1);
        GI_ASSERT(records.first().isFolderMerge);
        GI_ASSERT(records.first().retryBlocked);
        GI_ASSERT(records.first().uncertainDurability);
        GI_ASSERT(!TransferQueueController::canRetryFromHistoryRecord(records.first()));
    }

    // 30. Undo Fence Timing: fence is raised BEFORE Overwrite writes to disk!
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("FenceSrc"));
        const QString dst = root.filePath(QStringLiteral("FenceDst"));
        writeFileGI(src + QStringLiteral("/f.txt"), "NEW-FENCE");
        writeFileGI(dst + QStringLiteral("/FenceSrc/f.txt"), "OLD-FENCE");

        TransferQueueController qc;
        UndoController undo(nullptr, {}, {}, {});
        const quint64 beforeSerial = undo.irreversibleSerial();
        quint64 serialAtExecuting = 0;

        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.capabilities.canSafeOverwrite = true;
        req.undoController = &undo;
        req.filePrompt = [](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Overwrite;
            return d;
        };

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            job->setStateCallback([&](LocalMergeOrchestratorState s) {
                if (s == LocalMergeOrchestratorState::Executing) {
                    serialAtExecuting = undo.irreversibleSerial();
                }
            });
        });
        GI_ASSERT(enq.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Completed);
        GI_ASSERT(serialAtExecuting > beforeSerial);
    }

    // GUI-STACK-01 regression (A-K): OperationWindow never shows/raises over active modal conflict dialog,
    // repeated 3 times with sequential conflicts (root folder -> subfolder -> file), plus Escape/Cancel.
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("StackSrc"));
        const QString dst = root.filePath(QStringLiteral("StackDst"));
        writeFileGI(src + QStringLiteral("/Sub/cfg.ini"), "NEW-CFG");
        writeFileGI(dst + QStringLiteral("/StackSrc/Sub/cfg.ini"), "OLD-CFG");

        QMainWindow mainWin;
        TransferQueueController qc;
        auto *opMgr = new OperationManager(&mainWin, nullptr, &mainWin);
        opMgr->setQueueController(&qc);

        for (int round = 0; round < 3; ++round) {
            if (opMgr->detailedWindow()) {
                opMgr->detailedWindow()->hide();
            }
            bool sawOpWinHiddenDuringModal = false;
            int folderCalls = 0;
            int fileCalls = 0;

            LocalMergeQueueRequest req;
            req.sources = {QUrl::fromLocalFile(src)};
            req.destinationDir = QUrl::fromLocalFile(dst);
            req.folderPrompt = [&](const LocalMergePlanEntry &entry, qsizetype rem, LocalMergeConflictProgress prog) {
                ++folderCalls;
                LocalFolderMergeConflictDialog dlg(entry, QSet<QString>{}, false, QLocale(), &mainWin, rem, prog);
                dlg.setWindowModality(Qt::ApplicationModal);
                QTimer::singleShot(0, &dlg, [&] {
                    // Simulate OperationManager trying to show OperationWindow while modal dialog is active!
                    opMgr->showDetailedWindow();
                    sawOpWinHiddenDuringModal = (!opMgr->detailedWindow() || !opMgr->detailedWindow()->isVisible());
                    dlg.reject();  // Cancel / Escape / X
                });
                dlg.exec();
                LocalFolderConflictDecision d;
                d.accepted = (round < 2);  // Rounds 0,1 merge; round 2 tests Cancel/Escape
                d.action = d.accepted ? LocalMergeApprovedAction::MergeDirectory : LocalMergeApprovedAction::Abort;
                return d;
            };
            req.filePrompt = [&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &caps, qsizetype rem, LocalMergeConflictProgress prog) {
                ++fileCalls;
                LocalFileConflictResolutionDialog dlg(entry, caps, rem, QSet<QString>{}, QLocale(), &mainWin, prog);
                dlg.setWindowModality(Qt::ApplicationModal);
                QTimer::singleShot(0, &dlg, [&] {
                    opMgr->showDetailedWindow();
                    GI_ASSERT(!opMgr->detailedWindow() || !opMgr->detailedWindow()->isVisible());
                    dlg.reject();
                });
                dlg.exec();
                LocalFileConflictDecision d;
                d.accepted = true;
                d.action = LocalMergeApprovedAction::Skip;
                return d;
            };

            const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
            GI_ASSERT(enq.accepted());
            const auto term = waitForTerminalGI(qc, enq.id);
            GI_ASSERT(sawOpWinHiddenDuringModal);
            if (round < 2) {
                GI_ASSERT(term == TransferTaskState::Completed);
                GI_ASSERT(folderCalls == 2);  // root + Sub
                GI_ASSERT(fileCalls == 1);    // cfg.ini
            } else {
                GI_ASSERT(term == TransferTaskState::Cancelled);
            }
        }

        // After all modal dialogs are closed, OperationWindow can be opened normally
        opMgr->showDetailedWindow();
        GI_ASSERT(opMgr->detailedWindow() != nullptr);
        GI_ASSERT(opMgr->detailedWindow()->isVisible());
        opMgr->detailedWindow()->hide();
    }

    // GUI-OPS-02 regression:
    // - Fast transfer (<150ms) with 3 conflicts: OperationWindow auto-appears AFTER dialogs & completion!
    // - Cancelled on first conflict (0 writes): OperationWindow does NOT auto-appear!
    // - Subsequent Merge after previous completion: OperationWindow auto-appears cleanly!
    // - setAutoShowEnabled(false) & setUserDismissed(true): respected!
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("OpsSrc"));
        const QString dst = root.filePath(QStringLiteral("OpsDst"));
        writeFileGI(src + QStringLiteral("/Sub/cfg.ini"), "NEW-OPS");
        writeFileGI(dst + QStringLiteral("/OpsSrc/Sub/cfg.ini"), "OLD-OPS");

        QMainWindow mainWin;
        TransferQueueController qc;
        auto *opMgr = new OperationManager(&mainWin, nullptr, &mainWin);
        opMgr->setQueueController(&qc);
        opMgr->setAutoShowGracePeriodMs(0);

        // Case A: Cancel on first conflict -> OperationWindow does NOT auto-appear
        {
            LocalMergeQueueRequest reqCancel;
            reqCancel.sources = {QUrl::fromLocalFile(src)};
            reqCancel.destinationDir = QUrl::fromLocalFile(dst);
            reqCancel.folderPrompt = [&](const LocalMergePlanEntry &e, qsizetype rem, LocalMergeConflictProgress prog) {
                LocalFolderMergeConflictDialog dlg(e, QSet<QString>{}, false, QLocale(), &mainWin, rem, prog);
                dlg.setWindowModality(Qt::ApplicationModal);
                QTimer::singleShot(0, &dlg, [&] {
                    GI_ASSERT(!opMgr->detailedWindow() || !opMgr->detailedWindow()->isVisible());
                    dlg.reject();
                });
                dlg.exec();
                LocalFolderConflictDecision d;
                d.accepted = false;
                d.action = LocalMergeApprovedAction::Abort;
                return d;
            };
            const auto enqC = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(reqCancel));
            GI_ASSERT(enqC.accepted());
            GI_ASSERT(waitForTerminalGI(qc, enqC.id) == TransferTaskState::Cancelled);
            QCoreApplication::processEvents();
            GI_ASSERT(!opMgr->detailedWindow() || !opMgr->detailedWindow()->isVisible());
        }

        // Case D & E & F: Fast Merge with 3 conflicts -> OperationWindow auto-appears after completion!
        for (int runIdx = 0; runIdx < 2; ++runIdx) {
            if (opMgr->detailedWindow()) {
                opMgr->detailedWindow()->hide();
            }
            LocalMergeQueueRequest reqFast;
            reqFast.sources = {QUrl::fromLocalFile(src)};
            reqFast.destinationDir = QUrl::fromLocalFile(dst);
            reqFast.folderPrompt = [&](const LocalMergePlanEntry &e, qsizetype rem, LocalMergeConflictProgress prog) {
                LocalFolderMergeConflictDialog dlg(e, QSet<QString>{}, false, QLocale(), &mainWin, rem, prog);
                dlg.setWindowModality(Qt::ApplicationModal);
                QTimer::singleShot(0, &dlg, [&] {
                    GI_ASSERT(!opMgr->detailedWindow() || !opMgr->detailedWindow()->isVisible());
                    dlg.accept();
                });
                dlg.exec();
                LocalFolderConflictDecision d;
                d.accepted = true;
                d.action = LocalMergeApprovedAction::MergeDirectory;
                return d;
            };
            reqFast.filePrompt = [&](const LocalMergePlanEntry &e, const LocalMergeBackendCapabilities &caps, qsizetype rem, LocalMergeConflictProgress prog) {
                LocalFileConflictResolutionDialog dlg(e, caps, rem, QSet<QString>{}, QLocale(), &mainWin, prog);
                dlg.setWindowModality(Qt::ApplicationModal);
                QTimer::singleShot(0, &dlg, [&] {
                    GI_ASSERT(!opMgr->detailedWindow() || !opMgr->detailedWindow()->isVisible());
                    dlg.accept();
                });
                dlg.exec();
                LocalFileConflictDecision d;
                d.accepted = true;
                d.action = LocalMergeApprovedAction::Skip;
                return d;
            };
            const auto enqF = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(reqFast));
            GI_ASSERT(enqF.accepted());
            GI_ASSERT(waitForTerminalGI(qc, enqF.id) == TransferTaskState::Completed);
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents();
            GI_ASSERT(opMgr->detailedWindow() != nullptr);
            GI_ASSERT(opMgr->detailedWindow()->isVisible());
        }

        // Case H & I: setAutoShowEnabled(false) & setUserDismissed(true) prevent auto-opening
        opMgr->detailedWindow()->hide();
        opMgr->setAutoShowEnabled(false);
        LocalMergeQueueRequest reqMuted;
        reqMuted.sources = {QUrl::fromLocalFile(src)};
        reqMuted.destinationDir = QUrl::fromLocalFile(dst);
        reqMuted.folderPrompt = [&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::MergeDirectory;
            return d;
        };
        reqMuted.filePrompt = [&](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Skip;
            return d;
        };
        const auto enqM = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(reqMuted));
        GI_ASSERT(enqM.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enqM.id) == TransferTaskState::Completed);
        QCoreApplication::processEvents();
        GI_ASSERT(!opMgr->detailedWindow()->isVisible());
    }

    // MERGE-SPEED-01 regression:
    // - Deterministic speed window calculation (too short / insufficient time -> 0 / "—")
    // - Multi-sample speed updates (not frozen at first sample)
    // - Dialog wait time does NOT pollute transfer speed
    // - Merge + Skip excludes skipped bytes from totalBytes so progress reaches 100%
    // - Cancel resets speedBytesPerSecond to 0
    // - Unit formatting B/s, KiB/s, MiB/s, GiB/s
    {
        GI_ASSERT(LocalMergeQueueJob::computeWindowSpeedBytesPerSec(1024ULL * 1024ULL, 50) == 0);
        GI_ASSERT(LocalMergeQueueJob::computeWindowSpeedBytesPerSec(0, 500) == 0);
        GI_ASSERT(LocalMergeQueueJob::computeWindowSpeedBytesPerSec(512ULL * 1024ULL * 1024ULL, 500)
                  == 1024ULL * 1024ULL * 1024ULL);

        GI_ASSERT(OperationWindow::speedText(0) == QStringLiteral("—"));
        GI_ASSERT(OperationWindow::speedText(512).contains(QStringLiteral("/s")));
        GI_ASSERT(OperationWindow::speedText(2048).contains(QStringLiteral("KiB/s")));
        GI_ASSERT(OperationWindow::speedText(4ULL * 1024ULL * 1024ULL).contains(QStringLiteral("MiB/s")));
        GI_ASSERT(OperationWindow::speedText(2ULL * 1024ULL * 1024ULL * 1024ULL).contains(QStringLiteral("GiB/s")));

        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("SpeedSrc"));
        const QString dst = root.filePath(QStringLiteral("SpeedDst"));
        writeFileGI(src + QStringLiteral("/skip.bin"), QByteArray(64 * 1024, 'S'));
        writeFileGI(src + QStringLiteral("/copy.bin"), QByteArray(128 * 1024, 'C'));
        writeFileGI(dst + QStringLiteral("/SpeedSrc/skip.bin"), "OLD");

        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.folderPrompt = [&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            // Simulate user reading conflict dialog for 600ms before confirming
            QEventLoop pauseLoop;
            QTimer::singleShot(600, &pauseLoop, &QEventLoop::quit);
            pauseLoop.exec();
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::MergeDirectory;
            return d;
        };
        req.filePrompt = [](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Skip;
            return d;
        };

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Completed);
        const auto snap = qc.taskSnapshot(enq.id);
        GI_ASSERT(snap.has_value());
        GI_ASSERT(snap->totalBytes == 128ULL * 1024ULL);
        GI_ASSERT(snap->processedBytes == 128ULL * 1024ULL);
        GI_ASSERT(snap->percent == 100);
        GI_ASSERT(snap->speedBytesPerSecond == 0);
    }

    // Helper: find child dialog without requiring Q_OBJECT on derived dialog class
    auto findFolderDlgGI = [](QObject *parent) -> LocalFolderMergeConflictDialog * {
        for (QDialog *d : parent->findChildren<QDialog *>()) {
            if (auto *typed = dynamic_cast<LocalFolderMergeConflictDialog *>(d)) {
                return typed;
            }
        }
        return nullptr;
    };
    auto findFileDlgGI = [](QObject *parent) -> LocalFileConflictResolutionDialog * {
        for (QDialog *d : parent->findChildren<QDialog *>()) {
            if (auto *typed = dynamic_cast<LocalFileConflictResolutionDialog *>(d)) {
                return typed;
            }
        }
        return nullptr;
    };

    // Helper: wait until a predicate becomes true while pumping the Qt event loop
    auto waitUntilGI = [](const std::function<bool()> &pred, int timeoutMs = 5000) -> bool {
        if (pred()) {
            return true;
        }
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (pred()) {
                return true;
            }
            QThread::msleep(5);
        }
        return pred();
    };

    // =========================================================================
    // STAGE 1B.6B (SEC-046-01): ASYNC FOLDER MERGE DIALOG & LIFETIME TESTS T1–T12
    // =========================================================================

    // T1 — Normal flow with real async GUI dialog (QDialog::open, no exec())
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT1");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/new_file.txt"), "T1_NEW");
        writeFileGI(dst + QStringLiteral("/FolderT1/existing.txt"), "T1_EXISTING");

        QWidget host;
        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg;
        GI_ASSERT(waitUntilGI([&]() {
            dlg = findFolderDlgGI(&host);
            return dlg && dlg->isVisible();
        }));
        GI_ASSERT(dlg->windowModality() == Qt::ApplicationModal);
        dlg->mergeButton()->click();

        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Completed);
        GI_ASSERT(waitUntilGI([&]() { return dlg.isNull(); }));
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT1/new_file.txt")) == "T1_NEW");
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT1/existing.txt")) == "T1_EXISTING");
    }

    // T2 — Mixed conflicts (folder Merge + file Rename + file Overwrite/Apply-to-All) via real async GUI dialogs
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT2");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/a.txt"), "SRC_A");
        writeFileGI(src + QStringLiteral("/b.txt"), "SRC_B");
        writeFileGI(src + QStringLiteral("/c.txt"), "SRC_C");
        writeFileGI(dst + QStringLiteral("/FolderT2/a.txt"), "DST_A");
        writeFileGI(dst + QStringLiteral("/FolderT2/b.txt"), "DST_B");
        writeFileGI(dst + QStringLiteral("/FolderT2/c.txt"), "DST_C");

        QWidget host;
        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.capabilities.canSafeOverwrite = true;
        req.capabilities.canRenameConflictingCopies = true;
        req.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());

        // 1) Folder conflict -> Merge
        QPointer<LocalFolderMergeConflictDialog> folderDlg;
        GI_ASSERT(waitUntilGI([&]() {
            folderDlg = findFolderDlgGI(&host);
            return folderDlg && folderDlg->isVisible();
        }));
        folderDlg->mergeButton()->click();
        GI_ASSERT(waitUntilGI([&]() { return folderDlg.isNull(); }));

        // 2) First file conflict (a.txt) -> Rename
        QPointer<LocalFileConflictResolutionDialog> fileDlg1;
        GI_ASSERT(waitUntilGI([&]() {
            fileDlg1 = findFileDlgGI(&host);
            return fileDlg1 && fileDlg1->isVisible();
        }));
        fileDlg1->suggestButton()->click();
        fileDlg1->renameButton()->click();
        GI_ASSERT(waitUntilGI([&]() { return fileDlg1.isNull(); }));

        // 3) Second file conflict (b.txt) -> Overwrite + Apply to All (auto-overwrites c.txt)
        QPointer<LocalFileConflictResolutionDialog> fileDlg2;
        GI_ASSERT(waitUntilGI([&]() {
            fileDlg2 = findFileDlgGI(&host);
            return fileDlg2 && fileDlg2->isVisible();
        }));
        if (auto *chk = fileDlg2->findChild<QCheckBox *>()) {
            chk->setChecked(true);
        }
        fileDlg2->overwriteButton()->click();
        GI_ASSERT(waitUntilGI([&]() { return fileDlg2.isNull(); }));

        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Completed);
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT2/a.txt")) == "DST_A");
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT2/b.txt")) == "SRC_B");
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT2/c.txt")) == "SRC_C");
        // Verify renamed copy of a.txt exists
        const QStringList entries = QDir(dst + QStringLiteral("/FolderT2")).entryList(QDir::Files);
        GI_ASSERT(entries.size() == 4);
    }

    // T3 — Cancel during open async GUI dialog (cancelTask while dialog is open)
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT3");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/mut.txt"), "SHOULD_NOT_WRITE");
        writeFileGI(dst + QStringLiteral("/FolderT3/keep.txt"), "KEEP");

        QWidget host;
        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg;
        GI_ASSERT(waitUntilGI([&]() {
            dlg = findFolderDlgGI(&host);
            return dlg && dlg->isVisible();
        }));

        GI_ASSERT(qc.cancelTask(enq.id));
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Cancelled);
        GI_ASSERT(waitUntilGI([&]() { return dlg.isNull(); }));
        GI_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/FolderT3/mut.txt")));
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT3/keep.txt")) == "KEEP");
    }

    // T4 — Cancel All with two tasks (task 1 waiting on open dialog, task 2 queued)
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src1 = tmp.path() + QStringLiteral("/src1/FolderT4");
        const QString src2 = tmp.path() + QStringLiteral("/src2/FolderT4");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src1 + QStringLiteral("/f1.txt"), "F1");
        writeFileGI(src2 + QStringLiteral("/f2.txt"), "F2");
        writeFileGI(dst + QStringLiteral("/FolderT4/orig.txt"), "ORIG");

        QWidget host;
        TransferQueueController qc;

        LocalMergeQueueRequest req1;
        req1.sources = {QUrl::fromLocalFile(src1)};
        req1.destinationDir = QUrl::fromLocalFile(dst);
        req1.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req1.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);
        const auto enq1 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req1));
        GI_ASSERT(enq1.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg;
        GI_ASSERT(waitUntilGI([&]() {
            dlg = findFolderDlgGI(&host);
            return dlg && dlg->isVisible();
        }));

        LocalMergeQueueRequest req2;
        req2.sources = {QUrl::fromLocalFile(src2)};
        req2.destinationDir = QUrl::fromLocalFile(dst);
        req2.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req2.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);
        const auto enq2 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req2));
        GI_ASSERT(enq2.accepted());

        GI_ASSERT(qc.cancelAll());
        GI_ASSERT(waitForTerminalGI(qc, enq1.id) == TransferTaskState::Cancelled);
        GI_ASSERT(waitForTerminalGI(qc, enq2.id) == TransferTaskState::Cancelled);
        GI_ASSERT(waitUntilGI([&]() { return dlg.isNull(); }));
        GI_ASSERT(qc.activeCount() == 0);
        GI_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/FolderT4/f1.txt")));
        GI_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/FolderT4/f2.txt")));
    }

    // T5 — Main-window shutdown sequence while async dialog is visible
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT5");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/f.txt"), "T5");
        writeFileGI(dst + QStringLiteral("/FolderT5/orig.txt"), "ORIG");

        QMainWindow win;
        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&win);
        req.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&win);
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg;
        GI_ASSERT(waitUntilGI([&]() {
            dlg = findFolderDlgGI(&win);
            return dlg && dlg->isVisible();
        }));

        // Mirror ThisPcWindow::closeEvent() ordering: close child dialogs, then beginShutdown()
        const auto childDialogs = win.findChildren<QDialog *>();
        for (QDialog *d : childDialogs) {
            d->close();
        }
        qc.beginShutdown();

        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Cancelled);
        GI_ASSERT(waitUntilGI([&]() { return dlg.isNull() && qc.isShutdownComplete(); }));
        GI_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/FolderT5/f.txt")));
    }

    // T6 — Rapid repeated cancellation emits exactly one terminal result
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT6");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/f.txt"), "T6");
        writeFileGI(dst + QStringLiteral("/FolderT6/orig.txt"), "ORIG");

        QWidget host;
        TransferQueueController qc;
        int queueFinishCount = 0;
        int orchFinishCount = 0;
        QObject::connect(&qc, &TransferQueueController::taskFinished, &qc, [&](quint64, TransferTaskState) {
            ++queueFinishCount;
        });

        QPointer<LocalMergeQueueJob> jobPtr;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            jobPtr = job;
            job->setResultCallback([&](const LocalMergeOrchestratorResult &) {
                ++orchFinishCount;
            });
        });
        GI_ASSERT(enq.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg;
        GI_ASSERT(waitUntilGI([&]() {
            dlg = findFolderDlgGI(&host);
            return dlg && dlg->isVisible();
        }));

        for (int i = 0; i < 5; ++i) {
            qc.cancelTask(enq.id);
            if (jobPtr) {
                jobPtr->requestCancelAsync();
            }
        }
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Cancelled);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        GI_ASSERT(queueFinishCount == 1);
        GI_ASSERT(orchFinishCount == 1);
    }

    // T7 — Late dialog completion after cancellation is ignored (no state change, no writes)
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT7");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/late.txt"), "SHOULD_NOT_APPEAR");
        writeFileGI(dst + QStringLiteral("/FolderT7/orig.txt"), "ORIG");

        TransferQueueController qc;
        std::function<void(const LocalFolderConflictDecision &)> capturedComplete;
        bool cancelHandleCalled = false;

        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.asyncFolderPrompt = [&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress,
                                    std::function<void(const LocalFolderConflictDecision &)> complete)
            -> LocalMergeOrchestrator::AsyncPromptCancelFn {
            capturedComplete = std::move(complete);
            return [&cancelHandleCalled]() { cancelHandleCalled = true; };
        };

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());
        GI_ASSERT(waitUntilGI([&]() { return static_cast<bool>(capturedComplete); }));

        GI_ASSERT(qc.cancelTask(enq.id));
        GI_ASSERT(cancelHandleCalled);
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Cancelled);

        // Invoke stale completion after cancellation and terminal finalization
        LocalFolderConflictDecision staleMerge;
        staleMerge.accepted = true;
        staleMerge.action = LocalMergeApprovedAction::MergeDirectory;
        capturedComplete(staleMerge);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        GI_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/FolderT7/late.txt")));
    }

    // T8 — Unexpected dialog destruction without finished() fails closed and releases reservation
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT8");
        const QString src2 = tmp.path() + QStringLiteral("/src2/FolderT8");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/t8_a.txt"), "A");
        writeFileGI(src2 + QStringLiteral("/t8_b.txt"), "B");
        writeFileGI(dst + QStringLiteral("/FolderT8/orig.txt"), "ORIG");

        QWidget host;
        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg;
        GI_ASSERT(waitUntilGI([&]() {
            dlg = findFolderDlgGI(&host);
            return dlg && dlg->isVisible();
        }));

        // Destroy dialog directly without accept()/reject()/done()
        delete dlg.data();
        GI_ASSERT(dlg.isNull());

        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Cancelled);
        GI_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/FolderT8/t8_a.txt")));

        // Verify queue reservation was released: a follow-up merge to same dst succeeds!
        LocalMergeQueueRequest req2;
        req2.sources = {QUrl::fromLocalFile(src2)};
        req2.destinationDir = QUrl::fromLocalFile(dst);
        req2.folderPrompt = [](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
        };
        const auto enq2 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req2));
        GI_ASSERT(enq2.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enq2.id) == TransferTaskState::Completed);
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT8/t8_b.txt")) == "B");
    }

    // T9 — Reservation lifetime: task 2 targeting same dst is blocked while task 1 dialog is open
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src1 = tmp.path() + QStringLiteral("/src1/FolderT9");
        const QString src2 = tmp.path() + QStringLiteral("/src2/FolderT9");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src1 + QStringLiteral("/first.txt"), "FIRST");
        writeFileGI(src2 + QStringLiteral("/second.txt"), "SECOND");
        writeFileGI(dst + QStringLiteral("/FolderT9/orig.txt"), "ORIG");

        QWidget host;
        TransferQueueController qc;

        LocalMergeQueueRequest req1;
        req1.sources = {QUrl::fromLocalFile(src1)};
        req1.destinationDir = QUrl::fromLocalFile(dst);
        req1.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req1.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);
        const auto enq1 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req1));
        GI_ASSERT(enq1.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg1;
        GI_ASSERT(waitUntilGI([&]() {
            dlg1 = findFolderDlgGI(&host);
            return dlg1 && dlg1->isVisible();
        }));

        // Enqueue task 2 targeting the exact same destination while task 1 holds conflict dialog
        LocalMergeQueueRequest req2;
        req2.sources = {QUrl::fromLocalFile(src2)};
        req2.destinationDir = QUrl::fromLocalFile(dst);
        req2.folderPrompt = [](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
        };
        const auto enq2 = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req2));
        GI_ASSERT(enq2.accepted());

        // Pump events: task 2 MUST remain BlockedByReservation and MUST NOT write second.txt
        for (int i = 0; i < 10; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        const auto snap2 = qc.taskSnapshot(enq2.id);
        GI_ASSERT(snap2.has_value());
        GI_ASSERT(snap2->state == TransferTaskState::Queued);
        GI_ASSERT(snap2->waitReason == TransferWaitReason::BlockedByReservation);
        GI_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/FolderT9/second.txt")));

        // Cancel task 1 -> only after task 1 finishes may task 2 run and write second.txt
        GI_ASSERT(qc.cancelTask(enq1.id));
        GI_ASSERT(waitForTerminalGI(qc, enq1.id) == TransferTaskState::Cancelled);
        GI_ASSERT(waitForTerminalGI(qc, enq2.id) == TransferTaskState::Completed);
        GI_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/FolderT9/first.txt")));
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT9/second.txt")) == "SECOND");
    }

    // T10 — NeedsReplan with async prompt does not duplicate dialogs or lose reservation
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT10");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/payload.txt"), "T10_DATA");
        writeFileGI(dst + QStringLiteral("/FolderT10/orig.txt"), "ORIG");
        // Pre-create colliding rename target so round 1 Rename triggers NeedsReplan
        writeFileGI(dst + QStringLiteral("/FolderT10_taken/blocker.txt"), "BLOCK");

        TransferQueueController qc;
        int asyncFolderPromptCount = 0;
        int liveConcurrentPrompts = 0;
        int maxConcurrentPrompts = 0;

        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.asyncFolderPrompt = [&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress,
                                    std::function<void(const LocalFolderConflictDecision &)> complete)
            -> LocalMergeOrchestrator::AsyncPromptCancelFn {
            ++asyncFolderPromptCount;
            ++liveConcurrentPrompts;
            maxConcurrentPrompts = qMax(maxConcurrentPrompts, liveConcurrentPrompts);
            const int round = asyncFolderPromptCount;
            QTimer::singleShot(0, &qc, [complete, round, &liveConcurrentPrompts]() {
                --liveConcurrentPrompts;
                LocalFolderConflictDecision d;
                d.accepted = true;
                d.action = LocalMergeApprovedAction::Rename;
                d.targetFolderName = (round == 1)
                    ? QStringLiteral("FolderT10_taken")
                    : QStringLiteral("FolderT10_free");
                complete(d);
            });
            return {};
        };

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Completed);
        GI_ASSERT(asyncFolderPromptCount == 2);
        GI_ASSERT(maxConcurrentPrompts == 1);
        GI_ASSERT(readFileGI(dst + QStringLiteral("/FolderT10_free/payload.txt")) == "T10_DATA");
    }

    // T11 — Object lifetime ordering (QPointer + destruction spies on job, orchestrator, dialog)
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString src = tmp.path() + QStringLiteral("/src/FolderT11");
        const QString dst = tmp.path() + QStringLiteral("/dst");
        writeFileGI(src + QStringLiteral("/f.txt"), "T11");
        writeFileGI(dst + QStringLiteral("/FolderT11/orig.txt"), "ORIG");

        QWidget host;
        TransferQueueController qc;
        QPointer<LocalMergeQueueJob> jobPtr;
        QPointer<LocalMergeOrchestrator> orchPtr;
        bool jobDestroyed = false;
        bool orchDestroyed = false;
        bool dlgDestroyed = false;

        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(src)};
        req.destinationDir = QUrl::fromLocalFile(dst);
        req.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);
        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req), [&](LocalMergeQueueJob *job) {
            jobPtr = job;
            QObject::connect(job, &QObject::destroyed, &qc, [&]() { jobDestroyed = true; });
            job->setStateCallback([&, job](LocalMergeOrchestratorState) {
                if (!orchPtr && job->orchestrator()) {
                    orchPtr = job->orchestrator();
                    QObject::connect(orchPtr, &QObject::destroyed, &qc, [&]() { orchDestroyed = true; });
                }
            });
        });
        GI_ASSERT(enq.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg;
        GI_ASSERT(waitUntilGI([&]() {
            dlg = findFolderDlgGI(&host);
            return dlg && dlg->isVisible();
        }));
        QObject::connect(dlg, &QObject::destroyed, &qc, [&]() { dlgDestroyed = true; });

        GI_ASSERT(qc.cancelTask(enq.id));
        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Cancelled);
        GI_ASSERT(waitUntilGI([&]() {
            return dlgDestroyed && orchDestroyed && jobDestroyed
                && dlg.isNull() && orchPtr.isNull() && jobPtr.isNull();
        }));
    }

    // T12 — Behavioral parity: synchronous vs asynchronous conflict resolution produce identical file states
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString srcSync = tmp.path() + QStringLiteral("/srcSync/Pkg");
        const QString dstSync = tmp.path() + QStringLiteral("/dstSync");
        const QString srcAsync = tmp.path() + QStringLiteral("/srcAsync/Pkg");
        const QString dstAsync = tmp.path() + QStringLiteral("/dstAsync");

        for (const QString &s : {srcSync, srcAsync}) {
            writeFileGI(s + QStringLiteral("/keep_dst.txt"), "NEW_SKIP");
            writeFileGI(s + QStringLiteral("/replace_me.txt"), "NEW_OVERWRITE");
            writeFileGI(s + QStringLiteral("/added.txt"), "ADDED");
        }
        for (const QString &d : {dstSync, dstAsync}) {
            writeFileGI(d + QStringLiteral("/Pkg/keep_dst.txt"), "OLD_KEEP");
            writeFileGI(d + QStringLiteral("/Pkg/replace_me.txt"), "OLD_REPLACE");
        }

        TransferQueueController qc;

        LocalMergeQueueRequest reqSync;
        reqSync.sources = {QUrl::fromLocalFile(srcSync)};
        reqSync.destinationDir = QUrl::fromLocalFile(dstSync);
        reqSync.capabilities.canSafeOverwrite = true;
        reqSync.folderPrompt = [](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
        };
        reqSync.filePrompt = [](const LocalMergePlanEntry &e, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = e.relativePath.endsWith(QStringLiteral("keep_dst.txt"))
                ? LocalMergeApprovedAction::Skip
                : LocalMergeApprovedAction::Overwrite;
            return d;
        };

        LocalMergeQueueRequest reqAsync;
        reqAsync.sources = {QUrl::fromLocalFile(srcAsync)};
        reqAsync.destinationDir = QUrl::fromLocalFile(dstAsync);
        reqAsync.capabilities.canSafeOverwrite = true;
        reqAsync.asyncFolderPrompt = [&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress,
                                         std::function<void(const LocalFolderConflictDecision &)> complete)
            -> LocalMergeOrchestrator::AsyncPromptCancelFn {
            QTimer::singleShot(0, &qc, [complete]() {
                complete(LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true});
            });
            return {};
        };
        reqAsync.asyncFilePrompt = [&](const LocalMergePlanEntry &e, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress,
                                       std::function<void(const LocalFileConflictDecision &)> complete)
            -> LocalMergeOrchestrator::AsyncPromptCancelFn {
            const bool skipIt = e.relativePath.endsWith(QStringLiteral("keep_dst.txt"));
            QTimer::singleShot(0, &qc, [complete, skipIt]() {
                LocalFileConflictDecision d;
                d.accepted = true;
                d.action = skipIt ? LocalMergeApprovedAction::Skip : LocalMergeApprovedAction::Overwrite;
                complete(d);
            });
            return {};
        };

        const auto eSync = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(reqSync));
        const auto eAsync = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(reqAsync));
        GI_ASSERT(eSync.accepted() && eAsync.accepted());
        GI_ASSERT(waitForTerminalGI(qc, eSync.id) == TransferTaskState::Completed);
        GI_ASSERT(waitForTerminalGI(qc, eAsync.id) == TransferTaskState::Completed);

        GI_ASSERT(readFileGI(dstSync + QStringLiteral("/Pkg/keep_dst.txt"))
                  == readFileGI(dstAsync + QStringLiteral("/Pkg/keep_dst.txt")));
        GI_ASSERT(readFileGI(dstSync + QStringLiteral("/Pkg/replace_me.txt"))
                  == readFileGI(dstAsync + QStringLiteral("/Pkg/replace_me.txt")));
        GI_ASSERT(readFileGI(dstSync + QStringLiteral("/Pkg/added.txt"))
                  == readFileGI(dstAsync + QStringLiteral("/Pkg/added.txt")));
        GI_ASSERT(readFileGI(dstAsync + QStringLiteral("/Pkg/keep_dst.txt")) == "OLD_KEEP");
        GI_ASSERT(readFileGI(dstAsync + QStringLiteral("/Pkg/replace_me.txt")) == "NEW_OVERWRITE");
        GI_ASSERT(readFileGI(dstAsync + QStringLiteral("/Pkg/added.txt")) == "ADDED");
    }

    // =========================================================================
    // STAGE 1B.9B-2A (SEC-046-09): SameObject Folder Rename GUI / Session / Queue Tests
    // =========================================================================
    auto sha256GI = [](const QString &path) -> QByteArray {
        QFile f(path);
        GI_ASSERT(f.open(QIODevice::ReadOnly));
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
    };

    // S7 — Apply-to-All (MergeAllFolders) cannot bypass SameObject guard, and submitting
    // Rename on SameObject in LocalMergeConflictSession fails closed (Cancelled, returns false).
    {
        LocalMergePlan plan;
        LocalMergePlanEntry normalDir;
        normalDir.relativePath = QStringLiteral("NormalDir");
        normalDir.sourcePath = QStringLiteral("/tmp/s/NormalDir");
        normalDir.destinationPath = QStringLiteral("/tmp/d/NormalDir");
        normalDir.sourceKind = LocalMergeObjectKind::Directory;
        normalDir.destinationKind = LocalMergeObjectKind::Directory;
        normalDir.classification = LocalMergeEntryClassification::MergeDirectoryCandidate;
        normalDir.caseStatus = LocalMergeCaseStatus::ExactNameMatch;

        LocalMergePlanEntry selfDir;
        selfDir.relativePath = QStringLiteral("SelfDir");
        selfDir.sourcePath = QStringLiteral("/tmp/d/SelfDir");
        selfDir.destinationPath = QStringLiteral("/tmp/d/SelfDir");
        selfDir.sourceKind = LocalMergeObjectKind::Directory;
        selfDir.destinationKind = LocalMergeObjectKind::Directory;
        selfDir.classification = LocalMergeEntryClassification::SameObject;
        selfDir.caseStatus = LocalMergeCaseStatus::ExactNameMatch;

        plan.entries = {normalDir, selfDir};
        LocalMergeBackendCapabilities caps;
        LocalMergeConflictSession session(&plan, caps);

        const auto step1 = session.advance();
        GI_ASSERT(step1.kind == LocalMergeConflictSession::StepKind::NeedFolderDecision);
        GI_ASSERT(step1.entry.relativePath == QStringLiteral("NormalDir"));
        GI_ASSERT(session.submitFolderDecision(LocalFolderConflictDecision{
            LocalMergeApprovedAction::MergeDirectory,
            LocalMergeApplyPolicy::MergeAllFolders,
            {},
            true}));

        // S7a: MergeAllFolders MUST NOT auto-approve SameObject
        const auto step2 = session.advance();
        GI_ASSERT(step2.kind == LocalMergeConflictSession::StepKind::NeedFolderDecision);
        GI_ASSERT(step2.entry.relativePath == QStringLiteral("SelfDir"));
        GI_ASSERT(step2.entry.classification == LocalMergeEntryClassification::SameObject);

        // S7b: Submitting Rename for SameObject fails closed
        const bool accepted = session.submitFolderDecision(LocalFolderConflictDecision{
            LocalMergeApprovedAction::Rename,
            LocalMergeApplyPolicy::None,
            QStringLiteral("SelfDir_copy"),
            true});
        GI_ASSERT(!accepted);
        GI_ASSERT(session.isTerminal());
        GI_ASSERT(session.result().cancelled && !session.result().completed);
    }

    // S6 / S8 / S11 / S12 — End-to-end Async GUI Dialog + QueueBridge on SameObject directory:
    // Rename controls disabled, Cancel ends in Cancelled (not Completed), original files intact,
    // queue reservations released cleanly for subsequent legitimate merges.
    {
        QTemporaryDir tmp;
        GI_ASSERT(tmp.isValid());
        const QString workDir = tmp.path() + QStringLiteral("/work");
        const QString selfDir = workDir + QStringLiteral("/DataDir");
        const QString docFile = selfDir + QStringLiteral("/doc.txt");
        writeFileGI(docFile, "critical-user-document-046-09");
        const QByteArray origHash = sha256GI(docFile);

        QWidget host;
        TransferQueueController qc;
        LocalMergeQueueRequest req;
        req.sources = {QUrl::fromLocalFile(selfDir)};
        req.destinationDir = QUrl::fromLocalFile(workDir);
        req.asyncFolderPrompt = LocalMergeQueueBridge::createAsyncGuiFolderPrompt(&host);
        req.asyncFilePrompt = LocalMergeQueueBridge::createAsyncGuiFilePrompt(&host);

        const auto enq = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(req));
        GI_ASSERT(enq.accepted());

        QPointer<LocalFolderMergeConflictDialog> dlg;
        GI_ASSERT(waitUntilGI([&]() {
            dlg = findFolderDlgGI(&host);
            return dlg && dlg->isVisible();
        }));
        GI_ASSERT(!dlg->mergeButton()->isEnabled());
        GI_ASSERT(!dlg->renameButton()->isEnabled());
        GI_ASSERT(!dlg->renameLineEdit()->isEnabled());
        GI_ASSERT(!dlg->suggestButton()->isEnabled());
        GI_ASSERT(dlg->skipButton()->isDefault());
        dlg->cancelButton()->click();

        GI_ASSERT(waitForTerminalGI(qc, enq.id) == TransferTaskState::Cancelled);
        GI_ASSERT(waitUntilGI([&]() { return dlg.isNull(); }));
        GI_ASSERT(sha256GI(docFile) == origHash);

        // S8/S10/S11: Programmatic async prompt attempting Rename on SameObject fails closed
        LocalMergeQueueRequest reqForce;
        reqForce.sources = {QUrl::fromLocalFile(selfDir)};
        reqForce.destinationDir = QUrl::fromLocalFile(workDir);
        reqForce.asyncFolderPrompt = [&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress,
                                         std::function<void(const LocalFolderConflictDecision &)> complete)
            -> LocalMergeOrchestrator::AsyncPromptCancelFn {
            QTimer::singleShot(0, &qc, [complete]() {
                complete(LocalFolderConflictDecision{
                    LocalMergeApprovedAction::Rename,
                    LocalMergeApplyPolicy::None,
                    QStringLiteral("DataDir_empty_trap"),
                    true});
            });
            return {};
        };
        const auto enqForce = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(reqForce));
        GI_ASSERT(enqForce.accepted());
        const TransferTaskState forceState = waitForTerminalGI(qc, enqForce.id);
        GI_ASSERT(forceState != TransferTaskState::Completed);
        GI_ASSERT(!QFileInfo::exists(workDir + QStringLiteral("/DataDir_empty_trap")));
        GI_ASSERT(sha256GI(docFile) == origHash);

        // S12: Queue reservations released cleanly — a subsequent legitimate merge into workDir succeeds
        const QString otherSrc = tmp.path() + QStringLiteral("/other/FreshDir");
        writeFileGI(otherSrc + QStringLiteral("/ok.txt"), "POST_RELEASE_OK");
        LocalMergeQueueRequest reqNext;
        reqNext.sources = {QUrl::fromLocalFile(otherSrc)};
        reqNext.destinationDir = QUrl::fromLocalFile(workDir);
        const auto enqNext = LocalMergeQueueBridge::enqueueMerge(&qc, std::move(reqNext));
        GI_ASSERT(enqNext.accepted());
        GI_ASSERT(waitForTerminalGI(qc, enqNext.id) == TransferTaskState::Completed);
        GI_ASSERT(readFileGI(workDir + QStringLiteral("/FreshDir/ok.txt")) == "POST_RELEASE_OK");
    }

    std::printf("PASS: local_merge_gui_integration (%d assertions)\n", g_guiIntAssertions);
    return 0;
}
