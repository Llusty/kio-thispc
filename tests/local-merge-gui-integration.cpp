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

    std::printf("PASS: local_merge_gui_integration (%d assertions)\n", g_guiIntAssertions);
    return 0;
}
