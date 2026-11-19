#include "localmergeorchestrator.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cassert>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

static QByteArray readLinkRawOrch(const QString &path)
{
    QByteArray buf(512, Qt::Uninitialized);
    const ssize_t n = ::readlink(QFile::encodeName(path).constData(), buf.data(), buf.size() - 1);
    if (n < 0) return {};
    buf.resize(n);
    return buf;
}

static int g_orchAssertions = 0;

#define ORCH_ASSERT(cond)                                                               \
    do {                                                                                \
        ++g_orchAssertions;                                                             \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "ORCH_ASSERT failed at %s:%d: %s\n", __FILE__,         \
                         __LINE__, #cond);                                              \
            std::abort();                                                               \
        }                                                                               \
    } while (false)

static void writeTextFileOrch(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    ORCH_ASSERT(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ORCH_ASSERT(f.write(data) == data.size());
    f.close();
}

static QByteArray readAllOrch(const QString &path)
{
    QFile f(path);
    ORCH_ASSERT(f.open(QIODevice::ReadOnly));
    return f.readAll();
}

static QByteArray sha256Orch(const QString &path)
{
    return QCryptographicHash::hash(readAllOrch(path), QCryptographicHash::Sha256);
}

static LocalMergeOrchestratorResult runToCompletion(LocalMergeOrchestrator &orch, int timeoutMs = 15000)
{
    QEventLoop loop;
    bool finished = false;
    LocalMergeOrchestratorResult out;
    orch.setFinishedCallback([&](const LocalMergeOrchestratorResult &res) {
        out = res;
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, [&] { loop.quit(); });
    orch.start();
    if (!finished) {
        loop.exec();
    }
    ORCH_ASSERT(finished);
    return out;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // 1. Clean additive merge (no conflicts)
    {
        QTemporaryDir root;
        ORCH_ASSERT(root.isValid());
        const QString srcDir = root.filePath(QStringLiteral("Src1"));
        const QString dstDir = root.filePath(QStringLiteral("Dst1"));
        writeTextFileOrch(srcDir + QStringLiteral("/Sub/a.txt"), "hello-additive-1");
        writeTextFileOrch(srcDir + QStringLiteral("/Sub/b.bin"), QByteArray(8192, 'Z'));
        QDir().mkpath(dstDir + QStringLiteral("/Src1"));

        LocalMergeReservationRegistry reg;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir), &reg, 1);
        const auto res = runToCompletion(orch);

        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(reg.holderCount() == 0);
        ORCH_ASSERT(sha256Orch(dstDir + QStringLiteral("/Src1/Sub/a.txt")) == sha256Orch(srcDir + QStringLiteral("/Sub/a.txt")));
        ORCH_ASSERT(sha256Orch(dstDir + QStringLiteral("/Src1/Sub/b.bin")) == sha256Orch(srcDir + QStringLiteral("/Sub/b.bin")));
        ORCH_ASSERT(QFileInfo::exists(srcDir + QStringLiteral("/Sub/a.txt")));
        ORCH_ASSERT(res.undoSummary.reversible);
        ORCH_ASSERT(!res.undoSummary.requiresIrreversibleSerial);
        ORCH_ASSERT(res.undoSummary.existingDirectoriesNeverDelete);
        ORCH_ASSERT(res.undoSummary.createdFiles.size() == 2);
        ORCH_ASSERT(res.progress.filesCreated == 2);
        ORCH_ASSERT(res.progress.existingDirectoriesMerged >= 1);
    }

    // 2. Mixed conflicts (Merge + Skip + Overwrite + Rename)
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("Pack"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/keep.txt"), "NEW-KEEP");
        writeTextFileOrch(srcDir + QStringLiteral("/over.txt"), "NEW-OVERWRITE");
        writeTextFileOrch(srcDir + QStringLiteral("/ren.txt"), "NEW-RENAME");
        writeTextFileOrch(dstDir + QStringLiteral("/Pack/keep.txt"), "OLD-KEEP");
        writeTextFileOrch(dstDir + QStringLiteral("/Pack/over.txt"), "OLD-OVERWRITE");
        writeTextFileOrch(dstDir + QStringLiteral("/Pack/ren.txt"), "OLD-RENAME");

        LocalMergeBackendCapabilities caps;
        caps.canSafeOverwrite = true;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir), nullptr, 2, caps);
        orch.setFolderPrompt([](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::MergeDirectory;
            return d;
        });
        orch.setFilePrompt([](const LocalMergePlanEntry &e, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            const QString name = QFileInfo(e.destinationPath).fileName();
            if (name == QLatin1String("keep.txt")) {
                d.action = LocalMergeApprovedAction::Skip;
            } else if (name == QLatin1String("over.txt")) {
                d.action = LocalMergeApprovedAction::Overwrite;
            } else {
                d.action = LocalMergeApprovedAction::Rename;
                d.targetFileName = QStringLiteral("ren (2).txt");
            }
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/Pack/keep.txt")) == "OLD-KEEP");
        ORCH_ASSERT(sha256Orch(dstDir + QStringLiteral("/Pack/over.txt")) == sha256Orch(srcDir + QStringLiteral("/over.txt")));
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/Pack/ren.txt")) == "OLD-RENAME");
        ORCH_ASSERT(sha256Orch(dstDir + QStringLiteral("/Pack/ren (2).txt")) == sha256Orch(srcDir + QStringLiteral("/ren.txt")));
        ORCH_ASSERT(res.progress.replacedCount == 1);
        ORCH_ASSERT(res.progress.skippedCount == 1);
        ORCH_ASSERT(!res.undoSummary.reversible);
        ORCH_ASSERT(res.undoSummary.requiresIrreversibleSerial);
    }

    // 3. Skip subtree (Skip folder)
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("Root"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/Sub/inside.txt"), "SHOULD-NOT-COPY");
        writeTextFileOrch(srcDir + QStringLiteral("/top.txt"), "TOP");
        QDir().mkpath(dstDir + QStringLiteral("/Root/Sub"));

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir));
        orch.setFolderPrompt([](const LocalMergePlanEntry &e, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            if (QFileInfo(e.destinationPath).fileName() == QLatin1String("Sub")) {
                d.action = LocalMergeApprovedAction::Skip;
            } else {
                d.action = LocalMergeApprovedAction::MergeDirectory;
            }
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        ORCH_ASSERT(!QFileInfo::exists(dstDir + QStringLiteral("/Root/Sub/inside.txt")));
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/Root/top.txt")) == "TOP");
        ORCH_ASSERT(res.progress.skippedSubtreeCount >= 1);
    }

    // 4. ApplyToAll Merge folders + ApplyToAll File Skip
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("A"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/S1/f1.txt"), "N1");
        writeTextFileOrch(srcDir + QStringLiteral("/S2/f2.txt"), "N2");
        writeTextFileOrch(srcDir + QStringLiteral("/S2/new.txt"), "NEW");
        writeTextFileOrch(dstDir + QStringLiteral("/A/S1/f1.txt"), "O1");
        writeTextFileOrch(dstDir + QStringLiteral("/A/S2/f2.txt"), "O2");

        int folderPrompts = 0;
        int filePrompts = 0;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir));
        orch.setFolderPrompt([&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            ++folderPrompts;
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::MergeDirectory;
            d.applyPolicy = LocalMergeApplyPolicy::MergeAllFolders;
            return d;
        });
        orch.setFilePrompt([&](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            ++filePrompts;
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Skip;
            d.applyPolicy = LocalMergeApplyPolicy::SkipAllFileConflicts;
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        ORCH_ASSERT(folderPrompts == 1);
        ORCH_ASSERT(filePrompts == 1);
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/A/S1/f1.txt")) == "O1");
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/A/S2/f2.txt")) == "O2");
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/A/S2/new.txt")) == "NEW");
    }

    // 5. Subtree Rename (folder rename)
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("Addon"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/data.lua"), "NEW-LUA");
        writeTextFileOrch(dstDir + QStringLiteral("/Addon/data.lua"), "OLD-LUA");

        LocalMergeReservationRegistry reg;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir), &reg, 5);
        orch.setFolderPrompt([](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Rename;
            d.targetFolderName = QStringLiteral("Addon (2)");
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/Addon/data.lua")) == "OLD-LUA");
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/Addon (2)/data.lua")) == "NEW-LUA");
        ORCH_ASSERT(reg.holderCount() == 0);
    }

    // 6. File Rename
    {
        QTemporaryDir root;
        const QString srcFile = root.filePath(QStringLiteral("note.txt"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcFile, "NEW-NOTE");
        writeTextFileOrch(dstDir + QStringLiteral("/note.txt"), "OLD-NOTE");

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcFile)}, QUrl::fromLocalFile(dstDir));
        orch.setFilePrompt([](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Rename;
            d.targetFileName = QStringLiteral("note (copy).txt");
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/note.txt")) == "OLD-NOTE");
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/note (copy).txt")) == "NEW-NOTE");
    }

    // 7. Cancel in AwaitingUserDecision (folder prompt)
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("F"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/x.txt"), "X");
        QDir().mkpath(dstDir + QStringLiteral("/F"));

        LocalMergeReservationRegistry reg;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir), &reg, 7);
        orch.setFolderPrompt([](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = false;
            d.action = LocalMergeApprovedAction::Abort;
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Cancelled);
        ORCH_ASSERT(!QFileInfo::exists(dstDir + QStringLiteral("/F/x.txt")));
        ORCH_ASSERT(res.progress.retryAllowed);
        ORCH_ASSERT(reg.holderCount() == 0);
    }

    // 8. Cancel in AwaitingUserDecision (file prompt)
    {
        QTemporaryDir root;
        const QString srcFile = root.filePath(QStringLiteral("c.txt"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcFile, "NEW");
        writeTextFileOrch(dstDir + QStringLiteral("/c.txt"), "OLD");

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcFile)}, QUrl::fromLocalFile(dstDir));
        orch.setFilePrompt([](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = false;
            d.action = LocalMergeApprovedAction::Abort;
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Cancelled);
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/c.txt")) == "OLD");
    }

    // 9. Cancel during Planning
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("Big"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/a.txt"), "A");
        QDir().mkpath(dstDir);

        LocalMergeReservationRegistry reg;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir), &reg, 9);
        orch.setStateChangedCallback([&](LocalMergeOrchestratorState s) {
            if (s == LocalMergeOrchestratorState::Planning) {
                orch.cancel();
            }
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Cancelled);
        ORCH_ASSERT(orch.stateHistory().contains(LocalMergeOrchestratorState::Cancelling));
        ORCH_ASSERT(reg.holderCount() == 0);
    }

    // 10. Cancel during Executing (mid-transfer)
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("Stream"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/first.txt"), "FIRST");
        writeTextFileOrch(srcDir + QStringLiteral("/big.bin"), QByteArray(256 * 1024, 'B'));
        QDir().mkpath(dstDir);

        LocalMergeReservationRegistry reg;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir), &reg, 10);
        orch.setExecutionJobConfigurator([&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &relativePath, int) {
                if (relativePath.endsWith(QLatin1String("big.bin"))) {
                    orch.cancel();
                }
            });
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Cancelled);
        ORCH_ASSERT(orch.stateHistory().contains(LocalMergeOrchestratorState::Cancelling));
        ORCH_ASSERT(reg.holderCount() == 0);
    }

    // 11 & 12. NeedsReplan continuation after partial success: never re-runs completed overwrite!
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("ReplanPack"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/01_over.txt"), "NEW-OVER-1");
        writeTextFileOrch(srcDir + QStringLiteral("/02_Sub/inside.txt"), "NEW-INSIDE");
        writeTextFileOrch(dstDir + QStringLiteral("/ReplanPack/01_over.txt"), "OLD-OVER-1");
        QDir().mkpath(dstDir + QStringLiteral("/ReplanPack/02_Sub"));

        int prompt01Count = 0;
        int folderPromptCount = 0;
        bool injectedCollision = false;

        LocalMergeBackendCapabilities caps;
        caps.canSafeOverwrite = true;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir), nullptr, 11, caps);
        orch.setFolderPrompt([&](const LocalMergePlanEntry &e, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            if (QFileInfo(e.destinationPath).fileName() == QLatin1String("02_Sub")) {
                ++folderPromptCount;
                d.action = LocalMergeApprovedAction::Rename;
                d.targetFolderName = (folderPromptCount == 1)
                    ? QStringLiteral("02_Sub (2)")
                    : QStringLiteral("02_Sub (3)");
            } else {
                d.action = LocalMergeApprovedAction::MergeDirectory;
            }
            return d;
        });
        orch.setFilePrompt([&](const LocalMergePlanEntry &e, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            if (QFileInfo(e.destinationPath).fileName() == QLatin1String("01_over.txt")) {
                ++prompt01Count;
                d.action = LocalMergeApprovedAction::Overwrite;
            } else {
                d.action = LocalMergeApprovedAction::Skip;
            }
            return d;
        });
        orch.setExecutionJobConfigurator([&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &relativePath, int) {
                if (!injectedCollision && relativePath.endsWith(QLatin1String("01_over.txt"))) {
                    injectedCollision = true;
                    QDir().mkpath(dstDir + QStringLiteral("/ReplanPack/02_Sub (2)"));
                }
            });
        });

        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(res.replanRounds >= 1);
        ORCH_ASSERT(orch.stateHistory().contains(LocalMergeOrchestratorState::NeedsReplan));
        ORCH_ASSERT(prompt01Count == 1); // Never re-prompted or re-overwritten!
        ORCH_ASSERT(folderPromptCount == 2);
        ORCH_ASSERT(sha256Orch(dstDir + QStringLiteral("/ReplanPack/01_over.txt"))
                    == sha256Orch(srcDir + QStringLiteral("/01_over.txt")));
        ORCH_ASSERT(sha256Orch(dstDir + QStringLiteral("/ReplanPack/02_Sub (3)/inside.txt"))
                    == sha256Orch(srcDir + QStringLiteral("/02_Sub/inside.txt")));
        ORCH_ASSERT(res.progress.replacedCount == 1);
    }

    // 13. Pre-flight RecoveryRequired (existing .thispc-tx.* directory preserved!)
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("Src"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/a.txt"), "A");
        const QString txDir = dstDir + QStringLiteral("/.thispc-tx.unknown");
        writeTextFileOrch(txDir + QStringLiteral("/marker.txt"), "KEEP-ME");

        LocalMergeReservationRegistry reg;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir), &reg, 13);
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::RecoveryRequired);
        ORCH_ASSERT(res.progress.manualInterventionRequired);
        ORCH_ASSERT(!res.progress.retryAllowed);
        ORCH_ASSERT(res.undoSummary.requiresIrreversibleSerial);
        ORCH_ASSERT(QFileInfo::exists(txDir + QStringLiteral("/marker.txt")));
        ORCH_ASSERT(reg.holderCount() == 0);
    }

    // 14. Pre-flight RecoveryRequired (forced)
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("Src"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/a.txt"), "A");
        QDir().mkpath(dstDir);

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir));
        orch.setForceRecoveryRequired(true);
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::RecoveryRequired);
        ORCH_ASSERT(!QFileInfo::exists(dstDir + QStringLiteral("/Src/a.txt")));
    }

    // 15. FailedNoChanges (missing source)
    {
        QTemporaryDir root;
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        QDir().mkpath(dstDir);

        LocalMergeReservationRegistry reg;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(root.filePath(QStringLiteral("Missing")))},
                                    QUrl::fromLocalFile(dstDir), &reg, 15);
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::FailedNoChanges);
        ORCH_ASSERT(!res.progress.partialSuccess);
        ORCH_ASSERT(res.progress.retryAllowed);
        ORCH_ASSERT(!res.undoSummary.requiresIrreversibleSerial);
        ORCH_ASSERT(reg.holderCount() == 0);
    }

    // 16. FailedPartialSuccess (error on second entry after first succeeded)
    {
        QTemporaryDir root;
        const QString srcDir = root.filePath(QStringLiteral("Partial"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcDir + QStringLiteral("/01_ok.txt"), "OK");
        writeTextFileOrch(srcDir + QStringLiteral("/02_fail.txt"), "FAIL");
        QDir().mkpath(dstDir);

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcDir)}, QUrl::fromLocalFile(dstDir));
        orch.setExecutionJobConfigurator([&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &relativePath, int) {
                if (relativePath.endsWith(QLatin1String("01_ok.txt"))) {
                    QFile::remove(srcDir + QStringLiteral("/02_fail.txt"));
                }
            });
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::FailedPartialSuccess);
        ORCH_ASSERT(res.progress.partialSuccess);
        ORCH_ASSERT(!res.progress.retryAllowed);
        ORCH_ASSERT(res.undoSummary.requiresIrreversibleSerial);
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/Partial/01_ok.txt")) == "OK");
    }

    // 17. FailedUncertainDurability (fsync EIO on overwrite)
    {
        QTemporaryDir root;
        const QString srcFile = root.filePath(QStringLiteral("cfg.wtf"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcFile, "NEW-CFG");
        writeTextFileOrch(dstDir + QStringLiteral("/cfg.wtf"), "OLD-CFG");

        LocalMergeBackendCapabilities caps;
        caps.canSafeOverwrite = true;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcFile)}, QUrl::fromLocalFile(dstDir), nullptr, 17, caps);
        orch.setFilePrompt([](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Overwrite;
            return d;
        });
        orch.setExecutionJobConfigurator([](LocalMergeExecutionJob *job) {
            job->setSimulateFsyncEio(true);
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::FailedUncertainDurability);
        ORCH_ASSERT(res.progress.manualInterventionRequired);
        ORCH_ASSERT(!res.progress.retryAllowed);
        ORCH_ASSERT(res.undoSummary.requiresIrreversibleSerial);
    }

    // 18. NTFS Overwrite capability disabled
    {
        QTemporaryDir root;
        const QString srcFile = root.filePath(QStringLiteral("ntfs.txt"));
        const QString dstDir = root.filePath(QStringLiteral("Dest"));
        writeTextFileOrch(srcFile, "NEW");
        writeTextFileOrch(dstDir + QStringLiteral("/ntfs.txt"), "OLD");

        LocalMergeBackendCapabilities caps;
        caps.canSafeOverwrite = false;
        caps.filesystemLabel = QStringLiteral("ntfs3");
        bool sawDisabledOverwrite = false;

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(srcFile)}, QUrl::fromLocalFile(dstDir), nullptr, 18, caps);
        orch.setFilePrompt([&](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &c, qsizetype, LocalMergeConflictProgress) {
            sawDisabledOverwrite = !c.canSafeOverwrite;
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Skip;
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(sawDisabledOverwrite);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        ORCH_ASSERT(readAllOrch(dstDir + QStringLiteral("/ntfs.txt")) == "OLD");
    }

    // 19, 20, 21, 22, 23. Reservation registry tests (exact, ancestor/descendant, prefix trap, symlink, dev/ino, src vs dst)
    {
        QTemporaryDir root;
        const QString dstA = root.filePath(QStringLiteral("DstA"));
        const QString dstA2 = root.filePath(QStringLiteral("DstA2"));
        const QString dstASub = dstA + QStringLiteral("/Sub");
        const QString symDstA = root.filePath(QStringLiteral("LinkToDstA"));
        QDir().mkpath(dstASub);
        QDir().mkpath(dstA2);
        QFile::link(dstA, symDstA);

        LocalMergeReservationRegistry reg;
        LocalMergeReservationScope s1;
        s1.holderId = 100;
        s1.sourcePaths = {root.filePath(QStringLiteral("Src1"))};
        s1.destinationPaths = {dstA};
        s1.transactionParents = {dstA};
        ORCH_ASSERT(reg.tryAcquire(s1));

        // Exact same destination blocked
        LocalMergeReservationScope sSame;
        sSame.holderId = 101;
        sSame.sourcePaths = {root.filePath(QStringLiteral("Src2"))};
        sSame.destinationPaths = {dstA};
        ORCH_ASSERT(!reg.tryAcquire(sSame));

        // Descendant blocked
        LocalMergeReservationScope sChild;
        sChild.holderId = 102;
        sChild.destinationPaths = {dstASub};
        ORCH_ASSERT(!reg.tryAcquire(sChild));

        // Ancestor blocked
        LocalMergeReservationScope sParent;
        sParent.holderId = 103;
        sParent.destinationPaths = {root.path()};
        ORCH_ASSERT(!reg.tryAcquire(sParent));

        // Symlink alias blocked
        LocalMergeReservationScope sLink;
        sLink.holderId = 104;
        sLink.destinationPaths = {symDstA};
        ORCH_ASSERT(!reg.tryAcquire(sLink));

        // Source overlapping active destination blocked
        LocalMergeReservationScope sSrcOverlap;
        sSrcOverlap.holderId = 105;
        sSrcOverlap.sourcePaths = {dstASub};
        sSrcOverlap.destinationPaths = {dstA2};
        ORCH_ASSERT(!reg.tryAcquire(sSrcOverlap));

        // Prefix trap (/DstA vs /DstA2) MUST NOT conflict!
        LocalMergeReservationScope sPrefixSafe;
        sPrefixSafe.holderId = 106;
        sPrefixSafe.sourcePaths = {root.filePath(QStringLiteral("Src3"))};
        sPrefixSafe.destinationPaths = {dstA2};
        sPrefixSafe.transactionParents = {dstA2};
        ORCH_ASSERT(reg.tryAcquire(sPrefixSafe));

        // dev/ino identity check
        ORCH_ASSERT(LocalMergeReservationRegistry::pathsOverlap(dstA, symDstA));
        ORCH_ASSERT(!LocalMergeReservationRegistry::pathsOverlap(dstA, dstA2));

        ORCH_ASSERT(reg.release(100));
        ORCH_ASSERT(reg.release(106));
        ORCH_ASSERT(reg.holderCount() == 0);

        // Now sSame succeeds
        ORCH_ASSERT(reg.tryAcquire(sSame));
        ORCH_ASSERT(reg.release(101));
    }

    // 24. Two independent trees run concurrently in the same reservation registry
    {
        QTemporaryDir root;
        const QString srcA = root.filePath(QStringLiteral("SrcA"));
        const QString srcB = root.filePath(QStringLiteral("SrcB"));
        const QString dstA = root.filePath(QStringLiteral("DstA"));
        const QString dstB = root.filePath(QStringLiteral("DstB"));
        writeTextFileOrch(srcA + QStringLiteral("/a.txt"), "AAA");
        writeTextFileOrch(srcB + QStringLiteral("/b.txt"), "BBB");
        QDir().mkpath(dstA);
        QDir().mkpath(dstB);

        LocalMergeReservationRegistry reg;
        LocalMergeOrchestrator orchA({QUrl::fromLocalFile(srcA)}, QUrl::fromLocalFile(dstA), &reg, 201);
        LocalMergeOrchestrator orchB({QUrl::fromLocalFile(srcB)}, QUrl::fromLocalFile(dstB), &reg, 202);

        QEventLoop loop;
        int done = 0;
        LocalMergeOrchestratorResult resA, resB;
        orchA.setFinishedCallback([&](const LocalMergeOrchestratorResult &r) {
            resA = r;
            if (++done == 2) loop.quit();
        });
        orchB.setFinishedCallback([&](const LocalMergeOrchestratorResult &r) {
            resB = r;
            if (++done == 2) loop.quit();
        });
        orchA.start();
        orchB.start();
        QTimer::singleShot(15000, &loop, [&] { loop.quit(); });
        loop.exec();
        ORCH_ASSERT(done == 2);
        ORCH_ASSERT(resA.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(resB.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(reg.holderCount() == 0);
        ORCH_ASSERT(readAllOrch(dstA + QStringLiteral("/SrcA/a.txt")) == "AAA");
        ORCH_ASSERT(readAllOrch(dstB + QStringLiteral("/SrcB/b.txt")) == "BBB");
    }

    // 25. Duplicate execution refusal
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("DupSrc"));
        const QString dst = root.filePath(QStringLiteral("DupDst"));
        writeTextFileOrch(src + QStringLiteral("/f.txt"), "DUP");
        QDir().mkpath(dst);

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dst));
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        orch.start();
        ORCH_ASSERT(orch.result().duplicateExecutionBlocked);
        ORCH_ASSERT(!orch.executePreparedPlan());
    }

    // 26. Non-blocking Qt event loop during Planning/Executing
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("LoopSrc"));
        const QString dst = root.filePath(QStringLiteral("LoopDst"));
        writeTextFileOrch(src + QStringLiteral("/f.txt"), QByteArray(16384, 'L'));
        QDir().mkpath(dst);

        int ticks = 0;
        QTimer ticker;
        ticker.setInterval(0);
        QObject::connect(&ticker, &QTimer::timeout, [&] { ++ticks; });
        ticker.start();

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dst));
        const auto res = runToCompletion(orch);
        ticker.stop();
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(ticks >= 1);
    }

    // 27 & 28. Progress snapshot & Undo/Redo contract A-H
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("StatsSrc"));
        const QString dst = root.filePath(QStringLiteral("StatsDst"));
        writeTextFileOrch(src + QStringLiteral("/Sub/1.txt"), "12345");
        writeTextFileOrch(src + QStringLiteral("/Sub/2.txt"), "67890");
        QDir().mkpath(dst + QStringLiteral("/StatsSrc"));

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dst));
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(res.progress.filesPlanned == 2);
        ORCH_ASSERT(res.progress.filesCreated == 2);
        ORCH_ASSERT(res.progress.directoriesCreated == 1);
        ORCH_ASSERT(res.progress.existingDirectoriesMerged == 1);
        ORCH_ASSERT(res.progress.bytesPlanned == 10);
        ORCH_ASSERT(res.progress.bytesProcessed == 10);
        ORCH_ASSERT(res.undoSummary.reversible);
        ORCH_ASSERT(res.undoSummary.existingDirectoriesNeverDelete);
        ORCH_ASSERT(res.undoSummary.createdFiles.size() == 2);
        ORCH_ASSERT(res.undoSummary.createdDirectories.size() == 1);
    }

    // 29. R1 Reproduction: Symlinks (relative, absolute, dir-symlink, dangling) & mixed dir during Merge; no fake Completed on blocked
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("SymMergeSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString dst = dstParent + QStringLiteral("/SymMergeSrc");
        const QString outDir = root.filePath(QStringLiteral("OutsideDir"));
        ORCH_ASSERT(QDir().mkpath(src + QStringLiteral("/Sub")));
        ORCH_ASSERT(QDir().mkpath(dst));
        ORCH_ASSERT(QDir().mkpath(outDir));
        writeTextFileOrch(src + QStringLiteral("/real.txt"), "real-payload");
        writeTextFileOrch(src + QStringLiteral("/Sub/nested.txt"), "nested-payload");
        writeTextFileOrch(outDir + QStringLiteral("/secret.txt"), "do-not-recurse");
        writeTextFileOrch(dst + QStringLiteral("/keep.txt"), "keep-existing");
        ORCH_ASSERT(::symlink("real.txt", QFile::encodeName(src + QStringLiteral("/rel.link"))) == 0);
        ORCH_ASSERT(::symlink(QFile::encodeName(src + QStringLiteral("/real.txt")).constData(),
                              QFile::encodeName(src + QStringLiteral("/abs.link")).constData()) == 0);
        ORCH_ASSERT(::symlink(QFile::encodeName(outDir).constData(),
                              QFile::encodeName(src + QStringLiteral("/dir.link")).constData()) == 0);
        ORCH_ASSERT(::symlink("non_existent_target",
                              QFile::encodeName(src + QStringLiteral("/dangling.link")).constData()) == 0);

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::MergeDirectory;
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(res.executionReport.objectsBlocked.isEmpty());
        ORCH_ASSERT(readLinkRawOrch(dst + QStringLiteral("/rel.link")) == "real.txt");
        ORCH_ASSERT(readLinkRawOrch(dst + QStringLiteral("/abs.link")) == QFile::encodeName(src + QStringLiteral("/real.txt")));
        ORCH_ASSERT(readLinkRawOrch(dst + QStringLiteral("/dir.link")) == QFile::encodeName(outDir));
        ORCH_ASSERT(readLinkRawOrch(dst + QStringLiteral("/dangling.link")) == "non_existent_target");
        struct stat dirLinkSt {};
        ORCH_ASSERT(::lstat(QFile::encodeName(dst + QStringLiteral("/dir.link")).constData(), &dirLinkSt) == 0
                    && S_ISLNK(dirLinkSt.st_mode));
        ORCH_ASSERT(res.executionReport.filesCreated.size() == 2);
        ORCH_ASSERT(res.executionReport.symlinksCreated.size() == 4);
        ORCH_ASSERT(res.progress.filesPlanned == 6);
        ORCH_ASSERT(res.progress.filesCreated == 6);
        ORCH_ASSERT(res.progress.directoriesCreated == 1);
        ORCH_ASSERT(QDir(outDir).removeRecursively());
        ORCH_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/dir.link/secret.txt")));
        ORCH_ASSERT(QFileInfo(dst + QStringLiteral("/dir.link")).isSymLink());
        ORCH_ASSERT(sha256Orch(dst + QStringLiteral("/real.txt")) == sha256Orch(src + QStringLiteral("/real.txt")));
        ORCH_ASSERT(sha256Orch(dst + QStringLiteral("/Sub/nested.txt")) == sha256Orch(src + QStringLiteral("/Sub/nested.txt")));
    }

    // 30. R2 Reproduction: NeedsReplan after directoriesCreated & renamed subdirectory preserves children across rounds
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("ReplanSubtreeSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString dst = dstParent + QStringLiteral("/ReplanSubtreeSrc");
        QDir().mkpath(src + QStringLiteral("/01_BrandNew"));
        QDir().mkpath(src + QStringLiteral("/02_ConflictSub"));
        QDir().mkpath(src + QStringLiteral("/03_RenamedSub"));
        QDir().mkpath(dst + QStringLiteral("/02_ConflictSub"));
        QDir().mkpath(dst + QStringLiteral("/03_RenamedSub"));
        writeTextFileOrch(src + QStringLiteral("/01_BrandNew/child1.txt"), "c1");
        writeTextFileOrch(src + QStringLiteral("/02_ConflictSub/trigger.txt"), "t1");
        writeTextFileOrch(src + QStringLiteral("/03_RenamedSub/r1.txt"), "r1");
        writeTextFileOrch(dst + QStringLiteral("/02_ConflictSub/existing.txt"), "keep2");
        writeTextFileOrch(dst + QStringLiteral("/03_RenamedSub/existing.txt"), "keep3");

        int roundSeen = 0;
        int rootMergePrompts = 0;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([&](const LocalMergePlanEntry &entry, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            if (entry.relativePath == QStringLiteral("ReplanSubtreeSrc")) {
                ++rootMergePrompts;
                d.action = LocalMergeApprovedAction::MergeDirectory;
            } else if (entry.relativePath.endsWith(QStringLiteral("02_ConflictSub"))) {
                d.action = LocalMergeApprovedAction::Rename;
                d.targetFolderName = (roundSeen == 0)
                    ? QStringLiteral("02_ConflictSub_alt")
                    : QStringLiteral("02_ConflictSub_alt2");
            } else if (entry.relativePath.endsWith(QStringLiteral("03_RenamedSub"))) {
                d.action = LocalMergeApprovedAction::Rename;
                d.targetFolderName = (roundSeen <= 1)
                    ? QStringLiteral("03_RenamedSub_copy")
                    : QStringLiteral("03_RenamedSub_copy2");
            } else {
                d.action = LocalMergeApprovedAction::MergeDirectory;
            }
            return d;
        });
        orch.setExecutionJobConfigurator([&](LocalMergeExecutionJob *job) {
            const int curRound = roundSeen++;
            job->setPrePublishHook([&, curRound](const QString &relPath, int) {
                if (curRound == 0 && relPath == QStringLiteral("ReplanSubtreeSrc/01_BrandNew/child1.txt")) {
                    writeTextFileOrch(src + QStringLiteral("/01_BrandNew/child2.txt"), "c2-after-replan1");
                    QDir().mkpath(dst + QStringLiteral("/02_ConflictSub_alt"));
                } else if (curRound == 1 && relPath == QStringLiteral("ReplanSubtreeSrc/02_ConflictSub_alt2/trigger.txt")) {
                    writeTextFileOrch(src + QStringLiteral("/02_ConflictSub/child_in_renamed.txt"), "in-renamed");
                    QDir().mkpath(dst + QStringLiteral("/03_RenamedSub_copy"));
                }
            });
        });

        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(res.replanRounds == 2);
        ORCH_ASSERT(rootMergePrompts == 1);
        ORCH_ASSERT(res.executionReport.objectsBlocked.isEmpty());
        ORCH_ASSERT(res.progress.directoriesCreated == 3);
        ORCH_ASSERT(res.progress.filesCreated == 5);
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/01_BrandNew/child1.txt")) == "c1");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/01_BrandNew/child2.txt")) == "c2-after-replan1");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/02_ConflictSub_alt2/trigger.txt")) == "t1");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/02_ConflictSub_alt2/child_in_renamed.txt")) == "in-renamed");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/03_RenamedSub_copy2/r1.txt")) == "r1");
    }

    // 31. R1 + R3: (a) blocking plan fails fast before dialogs; (b) directory Rename preserves symlinks without dereference
    {
        // (a) Blocking FIFO in source -> fails in onPlanReady() with 0 prompts
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("BlkSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString dst = dstParent + QStringLiteral("/BlkSrc");
        ORCH_ASSERT(QDir().mkpath(src) && QDir().mkpath(dst));
        ORCH_ASSERT(::mkfifo(QFile::encodeName(src + QStringLiteral("/named.pipe")).constData(), 0644) == 0);
        writeTextFileOrch(dst + QStringLiteral("/keep.txt"), "keep");

        int folderPrompts = 0;
        int filePrompts = 0;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([&](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            ++folderPrompts;
            return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
        });
        orch.setFilePrompt([&](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            ++filePrompts;
            return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::FailedNoChanges);
        ORCH_ASSERT(folderPrompts == 0 && filePrompts == 0);
        ORCH_ASSERT(!res.diagnostic.isEmpty());
    }
    {
        // (b) Directory Rename containing relative, absolute, dir-to-outside, and dangling symlinks
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("SymRenSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString conflictDst = dstParent + QStringLiteral("/SymRenSrc");
        const QString renamedDst = dstParent + QStringLiteral("/SymRenSrc_renamed");
        const QString outDir = root.filePath(QStringLiteral("OutsideDir"));
        ORCH_ASSERT(QDir().mkpath(src) && QDir().mkpath(conflictDst) && QDir().mkpath(outDir));
        writeTextFileOrch(src + QStringLiteral("/real.txt"), "ren-payload");
        writeTextFileOrch(conflictDst + QStringLiteral("/keep.txt"), "existing-keep");
        writeTextFileOrch(outDir + QStringLiteral("/secret.txt"), "do-not-deref");
        ORCH_ASSERT(::symlink("real.txt", QFile::encodeName(src + QStringLiteral("/rel.link")).constData()) == 0);
        ORCH_ASSERT(::symlink(QFile::encodeName(src + QStringLiteral("/real.txt")).constData(),
                              QFile::encodeName(src + QStringLiteral("/abs.link")).constData()) == 0);
        ORCH_ASSERT(::symlink(QFile::encodeName(outDir).constData(),
                              QFile::encodeName(src + QStringLiteral("/dir.link")).constData()) == 0);
        ORCH_ASSERT(::symlink("missing_target",
                              QFile::encodeName(src + QStringLiteral("/dangling.link")).constData()) == 0);

        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Rename;
            d.targetFolderName = QStringLiteral("SymRenSrc_renamed");
            return d;
        });
        const auto res = runToCompletion(orch);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(res.executionReport.objectsBlocked.isEmpty());
        ORCH_ASSERT(res.executionReport.symlinksCreated.size() == 4);
        ORCH_ASSERT(res.progress.filesCreated == 5);
        ORCH_ASSERT(readLinkRawOrch(renamedDst + QStringLiteral("/rel.link")) == "real.txt");
        ORCH_ASSERT(readLinkRawOrch(renamedDst + QStringLiteral("/abs.link")) == QFile::encodeName(src + QStringLiteral("/real.txt")));
        ORCH_ASSERT(readLinkRawOrch(renamedDst + QStringLiteral("/dir.link")) == QFile::encodeName(outDir));
        ORCH_ASSERT(readLinkRawOrch(renamedDst + QStringLiteral("/dangling.link")) == "missing_target");
        struct stat stDirLnk {};
        ORCH_ASSERT(::lstat(QFile::encodeName(renamedDst + QStringLiteral("/dir.link")).constData(), &stDirLnk) == 0
                    && S_ISLNK(stDirLnk.st_mode));
        ORCH_ASSERT(QDir(outDir).removeRecursively());
        ORCH_ASSERT(!QFileInfo::exists(renamedDst + QStringLiteral("/dir.link/secret.txt")));
        ORCH_ASSERT(readAllOrch(conflictDst + QStringLiteral("/keep.txt")) == "existing-keep");
    }

    // 32. R2 external modification of completed file & directory between replan rounds fails closed / requires fresh decision
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("ModReplanSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString dst = dstParent + QStringLiteral("/ModReplanSrc");
        QDir().mkpath(src + QStringLiteral("/01_NewDir"));
        QDir().mkpath(src + QStringLiteral("/02_ConflictSub"));
        QDir().mkpath(dst + QStringLiteral("/02_ConflictSub"));
        writeTextFileOrch(src + QStringLiteral("/01_NewDir/inside.txt"), "IN");
        writeTextFileOrch(src + QStringLiteral("/01_first.txt"), "FIRST");
        writeTextFileOrch(src + QStringLiteral("/02_ConflictSub/trigger.txt"), "T1");
        writeTextFileOrch(dst + QStringLiteral("/02_ConflictSub/existing.txt"), "EXISTING");

        int subPrompts = 0;
        int promptsOnFirst = 0;
        bool externalMutationDone = false;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([&](const LocalMergePlanEntry &entry, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            if (entry.relativePath.endsWith(QStringLiteral("02_ConflictSub"))) {
                d.action = LocalMergeApprovedAction::Rename;
                d.targetFolderName = (subPrompts++ == 0)
                    ? QStringLiteral("02_ConflictSub_alt")
                    : QStringLiteral("02_ConflictSub_alt2");
            } else {
                d.action = LocalMergeApprovedAction::MergeDirectory;
            }
            return d;
        });
        orch.setFilePrompt([&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            if (entry.relativePath.endsWith(QStringLiteral("01_first.txt"))) {
                ++promptsOnFirst;
            }
            LocalFileConflictDecision d;
            d.accepted = true;
            d.action = LocalMergeApprovedAction::Skip;
            return d;
        });
        orch.setExecutionJobConfigurator([&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &relPath, int) {
                if (relPath.endsWith(QStringLiteral("01_first.txt"))
                    && !QFileInfo::exists(dst + QStringLiteral("/02_ConflictSub_alt"))) {
                    QDir().mkpath(dst + QStringLiteral("/02_ConflictSub_alt"));
                }
            });
        });
        orch.setStateChangedCallback([&](LocalMergeOrchestratorState st) {
            if (st == LocalMergeOrchestratorState::NeedsReplan && !externalMutationDone) {
                externalMutationDone = true;
                writeTextFileOrch(dst + QStringLiteral("/01_first.txt"), "EXTERNALLY_MODIFIED");
                QDir(dst + QStringLiteral("/01_NewDir")).removeRecursively();
                QDir().mkpath(dst + QStringLiteral("/01_NewDir"));
            }
        });

        const auto res = runToCompletion(orch);
        ORCH_ASSERT(externalMutationDone);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        ORCH_ASSERT(res.freshDecisionRequired);
        ORCH_ASSERT(res.externallyModifiedCompletedEntries.size() >= 2);
        ORCH_ASSERT(promptsOnFirst == 1);
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/01_first.txt")) == "EXTERNALLY_MODIFIED");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/02_ConflictSub_alt2/trigger.txt")) == "T1");
    }

    // 33. R3 explicit Skip on DestinationSymlinkConflict records in skippedEntries and keeps objectsBlocked empty
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("SkipSymSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString dst = dstParent + QStringLiteral("/SkipSymSrc");
        QDir().mkpath(src);
        QDir().mkpath(dst);
        writeTextFileOrch(src + QStringLiteral("/a.txt"), "SRC-REG");
        writeTextFileOrch(src + QStringLiteral("/new.txt"), "NEW-FILE");
        writeTextFileOrch(dst + QStringLiteral("/existing_target.txt"), "KEEP-TARGET");
        ORCH_ASSERT(::symlink("existing_target.txt",
                              QFile::encodeName(dst + QStringLiteral("/a.txt")).constData()) == 0);

        int filePromptCount = 0;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
        });
        orch.setFilePrompt([&](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            ++filePromptCount;
            return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
        });

        const auto res = runToCompletion(orch);
        ORCH_ASSERT(filePromptCount == 1);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        ORCH_ASSERT(res.executionReport.objectsBlocked.isEmpty());
        ORCH_ASSERT(res.executionReport.skippedEntries.size() == 1);
        ORCH_ASSERT(res.executionReport.skippedEntries.first().relativePath == QStringLiteral("SkipSymSrc/a.txt"));
        ORCH_ASSERT(readLinkRawOrch(dst + QStringLiteral("/a.txt")) == "existing_target.txt");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/new.txt")) == "NEW-FILE");
    }

    // 34. P1 #1: Standalone orchestrator cancel during Planning completes cleanly and releases reservation (no hang)
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("PlanCancelSrc"));
        const QString dst = root.filePath(QStringLiteral("PlanCancelDst"));
        QDir().mkpath(src);
        QDir().mkpath(dst);
        for (int i = 0; i < 8; ++i) {
            writeTextFileOrch(src + QStringLiteral("/f%1.txt").arg(i), "DATA");
        }

        LocalMergeReservationRegistry registry;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dst), &registry, 77);
        bool cancelledInPlanning = false;
        orch.setStateChangedCallback([&](LocalMergeOrchestratorState st) {
            if (st == LocalMergeOrchestratorState::Planning && !cancelledInPlanning) {
                cancelledInPlanning = true;
                QTimer::singleShot(0, &orch, [&orch] { orch.cancel(); });
            }
        });

        const auto res = runToCompletion(orch, 5000);
        ORCH_ASSERT(cancelledInPlanning);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Cancelled);
        ORCH_ASSERT(registry.holderCount() == 0);
        ORCH_ASSERT(!QFileInfo::exists(dst + QStringLiteral("/PlanCancelSrc")));
    }

    // 35. P1 #2: Directory-source Skip on TypeConflictDirOverFile & DestinationSymlinkConflict avoids objectsBlocked
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("DirSkipSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString dst = dstParent + QStringLiteral("/DirSkipSrc");
        const QString symTarget = root.filePath(QStringLiteral("SymTarget"));
        QDir().mkpath(src + QStringLiteral("/02_DirVsFile"));
        QDir().mkpath(src + QStringLiteral("/03_DirVsSym/Nested"));
        QDir().mkpath(dst);
        QDir().mkpath(symTarget);
        writeTextFileOrch(src + QStringLiteral("/01_ok.txt"), "OK");
        writeTextFileOrch(src + QStringLiteral("/02_DirVsFile/child.txt"), "SHOULD_SKIP");
        writeTextFileOrch(src + QStringLiteral("/03_DirVsSym/Nested/deep.txt"), "SHOULD_SKIP2");
        writeTextFileOrch(dst + QStringLiteral("/02_DirVsFile"), "EXISTING_REG_FILE");
        ORCH_ASSERT(::symlink(QFile::encodeName(symTarget).constData(),
                              QFile::encodeName(dst + QStringLiteral("/03_DirVsSym")).constData()) == 0);

        int filePrompts = 0;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
        });
        orch.setFilePrompt([&](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            ++filePrompts;
            return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
        });

        const auto res = runToCompletion(orch);
        ORCH_ASSERT(filePrompts == 2);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        ORCH_ASSERT(res.executionReport.objectsBlocked.isEmpty());
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/01_ok.txt")) == "OK");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/02_DirVsFile")) == "EXISTING_REG_FILE");
        ORCH_ASSERT(readLinkRawOrch(dst + QStringLiteral("/03_DirVsSym")) == QFile::encodeName(symTarget));
        ORCH_ASSERT(!QFileInfo::exists(symTarget + QStringLiteral("/Nested")));
    }

    // 36. P1 #3: Renamed regular file is recorded in m_completedEntryRecords and NOT re-prompted after NeedsReplan
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("RenameFileReplanSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString dst = dstParent + QStringLiteral("/RenameFileReplanSrc");
        QDir().mkpath(src + QStringLiteral("/02_Sub"));
        QDir().mkpath(dst + QStringLiteral("/02_Sub"));
        writeTextFileOrch(src + QStringLiteral("/01_conflict.txt"), "NEW-RENAMED-DATA");
        writeTextFileOrch(dst + QStringLiteral("/01_conflict.txt"), "KEEP-ORIGINAL");
        writeTextFileOrch(src + QStringLiteral("/02_Sub/x.txt"), "X-DATA");
        writeTextFileOrch(dst + QStringLiteral("/02_Sub/old.txt"), "OLD-SUB");

        int promptsOn01 = 0;
        int subPrompts = 0;
        bool triggeredReplan = false;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([&](const LocalMergePlanEntry &entry, qsizetype, LocalMergeConflictProgress) {
            LocalFolderConflictDecision d;
            d.accepted = true;
            if (entry.relativePath.endsWith(QStringLiteral("02_Sub"))) {
                d.action = LocalMergeApprovedAction::Rename;
                d.targetFolderName = (subPrompts++ == 0)
                    ? QStringLiteral("02_Sub_alt")
                    : QStringLiteral("02_Sub_alt2");
            } else {
                d.action = LocalMergeApprovedAction::MergeDirectory;
            }
            return d;
        });
        orch.setFilePrompt([&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            if (entry.relativePath.endsWith(QStringLiteral("01_conflict.txt"))) {
                ++promptsOn01;
                return LocalFileConflictDecision{LocalMergeApprovedAction::Rename, LocalMergeApplyPolicy::None, QStringLiteral("01_conflict_copy.txt"), true};
            }
            return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
        });
        orch.setExecutionJobConfigurator([&](LocalMergeExecutionJob *job) {
            job->setPrePublishHook([&](const QString &relPath, int) {
                if (!triggeredReplan && relPath.endsWith(QStringLiteral("01_conflict.txt"))) {
                    triggeredReplan = true;
                    QDir().mkpath(dst + QStringLiteral("/02_Sub_alt"));
                }
            });
        });

        const auto res = runToCompletion(orch);
        ORCH_ASSERT(triggeredReplan);
        ORCH_ASSERT(res.replanRounds == 1);
        ORCH_ASSERT(promptsOn01 == 1);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::Completed);
        ORCH_ASSERT(res.executionReport.objectsBlocked.isEmpty());
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/01_conflict.txt")) == "KEEP-ORIGINAL");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/01_conflict_copy.txt")) == "NEW-RENAMED-DATA");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/02_Sub_alt2/x.txt")) == "X-DATA");
    }

    // 37. P2 #1: (a) CaseInsensitiveCollision Rename for regular file succeeds; (b) differing-case sibling symlink prompts/Skips without NeedsReplan loop
    {
        QTemporaryDir root;
        const QString src = root.filePath(QStringLiteral("CaseSrc"));
        const QString dstParent = root.filePath(QStringLiteral("Dest"));
        const QString dst = dstParent + QStringLiteral("/CaseSrc");
        QDir().mkpath(src);
        QDir().mkpath(dst);
        writeTextFileOrch(src + QStringLiteral("/Config.txt"), "NEW-CASE-CFG");
        writeTextFileOrch(dst + QStringLiteral("/config.txt"), "OLD-LOWER-CFG");
        writeTextFileOrch(src + QStringLiteral("/symtarget.txt"), "SYM-TARGET");
        ORCH_ASSERT(::symlink("symtarget.txt",
                              QFile::encodeName(src + QStringLiteral("/Link.lnk")).constData()) == 0);
        ORCH_ASSERT(::symlink("existing_lower",
                              QFile::encodeName(dst + QStringLiteral("/link.lnk")).constData()) == 0);

        int cfgPrompts = 0;
        int lnkPrompts = 0;
        LocalMergeOrchestrator orch({QUrl::fromLocalFile(src)}, QUrl::fromLocalFile(dstParent));
        orch.setFolderPrompt([](const LocalMergePlanEntry &, qsizetype, LocalMergeConflictProgress) {
            return LocalFolderConflictDecision{LocalMergeApprovedAction::MergeDirectory, LocalMergeApplyPolicy::None, {}, true};
        });
        orch.setFilePrompt([&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
            if (QFileInfo(entry.sourcePath).fileName() == QLatin1String("Config.txt")) {
                ++cfgPrompts;
                return LocalFileConflictDecision{LocalMergeApprovedAction::Rename, LocalMergeApplyPolicy::None, QStringLiteral("Config_copy.txt"), true};
            }
            if (QFileInfo(entry.sourcePath).fileName() == QLatin1String("Link.lnk")) {
                ++lnkPrompts;
                return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
            }
            return LocalFileConflictDecision{LocalMergeApprovedAction::Skip, LocalMergeApplyPolicy::None, {}, true};
        });

        const auto res = runToCompletion(orch);
        ORCH_ASSERT(cfgPrompts == 1);
        ORCH_ASSERT(lnkPrompts == 1);
        ORCH_ASSERT(res.replanRounds == 0);
        ORCH_ASSERT(res.finalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        ORCH_ASSERT(res.executionReport.objectsBlocked.isEmpty());
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/config.txt")) == "OLD-LOWER-CFG");
        ORCH_ASSERT(readAllOrch(dst + QStringLiteral("/Config_copy.txt")) == "NEW-CASE-CFG");
        ORCH_ASSERT(readLinkRawOrch(dst + QStringLiteral("/link.lnk")) == "existing_lower");
    }

    std::printf("PASS: local_merge_orchestrator (%d assertions)\n", g_orchAssertions);
    return 0;
}
