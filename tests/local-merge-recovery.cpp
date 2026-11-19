/*
 * Comprehensive Crash Recovery, Transaction Journal & Safe Cleanup tests
 * for FOLDER-MERGE-01 Stage 3B.1 (scenarios A..T, including real SIGKILL
 * child-process crash tests and SHA-256 integrity verification).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localmergeexecutionjob.h"
#include "localmergeplan.h"
#include "localmergetransaction.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks = 0;

static void verify(bool cond, const char *msg)
{
    ++checks;
    if (!cond) {
        qFatal("FAIL: %s", msg);
    }
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

static QByteArray sha256File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

static bool waitUntil(const std::function<bool()> &pred, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return pred();
}

static LocalMergePlan makePlan(const QString &srcSubdir, const QString &dstParent)
{
    LocalMergePlanJob job({QUrl::fromLocalFile(srcSubdir)}, QUrl::fromLocalFile(dstParent));
    job.setAutoDelete(false);
    QSignalSpy spy(&job, &KJob::result);
    job.start();
    verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "plan job finishes");
    verify(job.error() == KJob::NoError, "plan job succeeds");
    return job.plan();
}

// Helper: forks a child process that runs Overwrite and sends SIGKILL to itself at targetPhase
static void runSigkillAtPhase(
    const QString &srcSubdir,
    const QString &dstParent,
    const QString &relTarget,
    LocalAnchoredFileCopyWorker::FaultPhase targetPhase)
{
    const pid_t pid = ::fork();
    verify(pid >= 0, "fork succeeded");
    if (pid == 0) {
        LocalMergePlan plan = makePlan(srcSubdir, dstParent);
        approveAdditiveMergePlan(plan);
        approveFileOverwrite(plan, relTarget);
        LocalMergeExecutionJob execJob(plan);
        execJob.setAutoDelete(false);
        execJob.setFaultPhaseHook([targetPhase](
            LocalAnchoredFileCopyWorker::FaultPhase phase,
            const QString &, int, const QString &, const QString &) {
            if (phase == targetPhase) {
                ::kill(::getpid(), SIGKILL);
            }
        });
        QSignalSpy spy(&execJob, &KJob::result);
        execJob.start();
        waitUntil([&spy] { return spy.count() == 1; }, 5000);
        ::_exit(0);
    }
    int status = 0;
    verify(::waitpid(pid, &status, 0) == pid, "waitpid child");
    verify(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "child terminated by SIGKILL at target phase");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    verify(tmp.isValid(), "temp dir valid");

    using FaultPhase = LocalAnchoredFileCopyWorker::FaultPhase;

    // A, B, C, D: SIGKILL before RENAME_EXCHANGE (AfterJournalPrepared, AfterStagingCreated, AfterStagingDurable, AfterExchangeIntentDurable)
    const QList<FaultPhase> preExchangePhases = {
        FaultPhase::AfterJournalPrepared,
        FaultPhase::AfterStagingCreated,
        FaultPhase::AfterStagingDurable,
        FaultPhase::AfterExchangeIntentDurable,
    };
    for (int i = 0; i < preExchangePhases.size(); ++i) {
        const QString base = tmp.filePath(QStringLiteral("sigkill_pre_%1").arg(i));
        const QString src = base + QStringLiteral("/src/WTF");
        const QString dst = base + QStringLiteral("/dst/WTF");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "NEW-PRE")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "OLD-PRE")
            && writeFile(dst + QStringLiteral("/Third.txt"), "THIRD-KEEP"), "pre-exchange trees");

        runSigkillAtPhase(src, dstRoot, QStringLiteral("WTF/Config.wtf"), preExchangePhases[i]);

        // S: Idempotency check (inspect twice)
        const auto rep1 = LocalMergeRecoveryInspector::inspectDirectory(dst, false);
        const auto rep2 = LocalMergeRecoveryInspector::inspectDirectory(dst, false);
        verify(rep1.anyIncompleteFound
            && rep1.findings.size() == 1
            && rep2.findings.size() == 1
            && rep1.findings.first().status == LocalMergeRecoveryStatus::StagedOnlyAborted
            && rep1.findings.first().status == rep2.findings.first().status
            && rep1.findings.first().oldAvailable
            && !rep1.findings.first().manualInterventionRequired
            && sha256File(dst + QStringLiteral("/Config.wtf")) == QCryptographicHash::hash("OLD-PRE", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Third.txt")) == QCryptographicHash::hash("THIRD-KEEP", QCryptographicHash::Sha256),
               "SIGKILL pre-exchange preserves OLD intact and inspector is idempotent");

        // Resolve unambiguous pre-exchange staging
        const auto resolved = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(resolved.findings.size() == 1 && !resolved.anyManualInterventionRequired, "resolved pre-exchange");
        const auto afterClean = LocalMergeRecoveryInspector::inspectDirectory(dst, false);
        verify(!afterClean.anyIncompleteFound
            && sha256File(dst + QStringLiteral("/Config.wtf")) == QCryptographicHash::hash("OLD-PRE", QCryptographicHash::Sha256),
               "After resolving pre-exchange tx, directory is clean and OLD intact");
    }

    // E & F: CRASH-RECOVERY-01 — SIGKILL immediately AFTER RENAME_EXCHANGE, BEFORE moving OLD to .thispc-replaced.*
    {
        const QString base = tmp.filePath(QStringLiteral("sigkill_after_exch"));
        const QString src = base + QStringLiteral("/src/WTF");
        const QString dst = base + QStringLiteral("/dst/WTF");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "NEW-CRASH-EXCH")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "OLD-CRASH-EXCH")
            && writeFile(dst + QStringLiteral("/Third.txt"), "THIRD-KEEP"), "after-exchange trees");

        runSigkillAtPhase(src, dstRoot, QStringLiteral("WTF/Config.wtf"), FaultPhase::AfterExchange);

        // Inspect & promote OLD inside 0700 tx dir to .thispc-replaced.*
        const auto rep = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(rep.anyIncompleteFound
            && rep.findings.size() == 1
            && rep.findings.first().status == LocalMergeRecoveryStatus::ExchangeOccurredOldSaved
            && rep.findings.first().oldAvailable
            && rep.findings.first().newAvailable
            && rep.findings.first().manualInterventionRequired
            && rep.findings.first().recoveryOldPath.contains(QStringLiteral(".thispc-replaced."))
            && sha256File(dst + QStringLiteral("/Config.wtf")) == QCryptographicHash::hash("NEW-CRASH-EXCH", QCryptographicHash::Sha256)
            && sha256File(rep.findings.first().recoveryOldPath) == QCryptographicHash::hash("OLD-CRASH-EXCH", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/Third.txt")) == QCryptographicHash::hash("THIRD-KEEP", QCryptographicHash::Sha256),
               "CRASH-RECOVERY-01: SIGKILL after RENAME_EXCHANGE identifies & preserves OLD as .thispc-replaced.* with exact SHA-256");
    }

    // G & H: SIGKILL after PreviousFileSecured, before fsync(destParentFd)
    {
        const QString base = tmp.filePath(QStringLiteral("sigkill_secured"));
        const QString src = base + QStringLiteral("/src/WTF");
        const QString dst = base + QStringLiteral("/dst/WTF");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "NEW-SECURED")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "OLD-SECURED"), "secured trees");

        runSigkillAtPhase(src, dstRoot, QStringLiteral("WTF/Config.wtf"), FaultPhase::AfterPreviousFileSecured);
        const auto rep = LocalMergeRecoveryInspector::inspectDirectory(dst, false);
        verify(rep.findings.size() == 1
            && rep.findings.first().status == LocalMergeRecoveryStatus::ExchangeOccurredOldSaved
            && rep.findings.first().oldAvailable
            && sha256File(rep.findings.first().recoveryOldPath) == QCryptographicHash::hash("OLD-SECURED", QCryptographicHash::Sha256),
               "SIGKILL before fsync(destParentFd) preserves OLD and flags manual intervention");
    }

    // J: SIGKILL after DirectoryDurable, before cleanup
    {
        const QString base = tmp.filePath(QStringLiteral("sigkill_durable"));
        const QString src = base + QStringLiteral("/src/WTF");
        const QString dst = base + QStringLiteral("/dst/WTF");
        const QString dstRoot = base + QStringLiteral("/dst");
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "NEW-DURABLE")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "OLD-DURABLE"), "durable trees");

        runSigkillAtPhase(src, dstRoot, QStringLiteral("WTF/Config.wtf"), FaultPhase::AfterDirectoryDurable);
        const auto rep = LocalMergeRecoveryInspector::inspectDirectory(dst, false);
        verify(rep.findings.size() == 1
            && rep.findings.first().status == LocalMergeRecoveryStatus::DurableCommitPendingCleanup
            && !rep.findings.first().manualInterventionRequired
            && sha256File(dst + QStringLiteral("/Config.wtf")) == QCryptographicHash::hash("NEW-DURABLE", QCryptographicHash::Sha256),
               "SIGKILL after DirectoryDurable reports DurableCommitPendingCleanup");
    }

    // I: fsync(destParentFd) EIO populates txId, txPhase, previousFileState, newFileState, manualInterventionRequired
    {
        const QString base = tmp.filePath(QStringLiteral("eio_tx"));
        const QString src = base + QStringLiteral("/src/WTF");
        const QString dst = base + QStringLiteral("/dst/WTF");
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/Config.wtf"), "NEW-EIO")
            && writeFile(dst + QStringLiteral("/Config.wtf"), "OLD-EIO"), "eio trees");

        LocalMergePlan plan = makePlan(src, base + QStringLiteral("/dst"));
        approveAdditiveMergePlan(plan);
        verify(approveFileOverwrite(plan, QStringLiteral("WTF/Config.wtf")), "bind eio");
        LocalMergeExecutionJob job(plan);
        job.setAutoDelete(false);
        job.setSimulateFsyncEio(true);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&spy] { return spy.count() == 1; }, 5000), "eio finishes");
        const auto &rep = job.report();
        verify(rep.outcome == LocalMergeExecutionOutcome::FailedUncertainDurability
            && !rep.retryAllowed
            && rep.manualInterventionRequired
            && rep.replacedFiles.size() == 1
            && !rep.replacedFiles.first().transactionId.isEmpty()
            && rep.replacedFiles.first().transactionPhase == QStringLiteral("UncertainDurability")
            && rep.replacedFiles.first().previousFileState == QStringLiteral("SecuredBackupPreserved")
            && rep.replacedFiles.first().newFileState == QStringLiteral("PublishedUncertainDurability")
            && sha256File(rep.replacedFiles.first().recoveryPath) == QCryptographicHash::hash("OLD-EIO", QCryptographicHash::Sha256),
               "fsync EIO populates full transaction report fields and preserves OLD");
    }

    // K, L, M, N, O, P, T: Synthetic recovery inspector edge cases (fail-closed, never delete unknown files)
    {
        const QString dst = tmp.filePath(QStringLiteral("edge_cases/dst/WTF"));
        verify(QDir().mkpath(dst), "edge dir");

        // T: Unknown .thispc-part.* and .thispc-replaced.* must NEVER be deleted
        verify(writeFile(dst + QStringLiteral("/.thispc-part.unknown.1"), "UNKNOWN-PART")
            && writeFile(dst + QStringLiteral("/.thispc-replaced.unknown.1"), "UNKNOWN-REPL"), "unknown files");

        // K: Corrupted / truncated journal inside .thispc-tx.corrupt
        const QString txCorrupt = dst + QStringLiteral("/.thispc-tx.corrupt");
        verify(QDir().mkpath(txCorrupt)
            && ::chmod(QFile::encodeName(txCorrupt).constData(), 0700) == 0
            && writeFile(txCorrupt + QStringLiteral("/.thispc-merge-tx.corrupt"), "THISPC_MERGE_TX_V1\nTRUNCATED")
            && writeFile(txCorrupt + QStringLiteral("/stage"), "KEEP-CORRUPT-DATA"), "corrupt tx");

        // P: Foreign object inside .thispc-tx.foreign
        const QString txForeign = dst + QStringLiteral("/.thispc-tx.foreign");
        verify(QDir().mkpath(txForeign)
            && ::chmod(QFile::encodeName(txForeign).constData(), 0700) == 0
            && writeFile(txForeign + QStringLiteral("/alien.txt"), "ALIEN-DATA"), "foreign tx");

        // L & M & N & O: Missing staging/OLD or inode/parent mismatch
        const QString txMismatch = dst + QStringLiteral("/.thispc-tx.mismatch");
        verify(QDir().mkpath(txMismatch)
            && ::chmod(QFile::encodeName(txMismatch).constData(), 0700) == 0
            && writeFile(dst + QStringLiteral("/Target.txt"), "CURRENT-TARGET"), "mismatch dir");
        LocalMergeTxJournalRecord rec;
        rec.txId = QStringLiteral("mismatch");
        rec.phase = LocalMergeTxPhase::ExchangeIntentDurable;
        rec.sourcePath = QStringLiteral("WTF/Target.txt");
        rec.destinationPath = dst + QStringLiteral("/Target.txt");
        rec.destFileName = QStringLiteral("Target.txt");
        rec.replacedBackupName = QStringLiteral(".thispc-replaced.Target.txt.mismatch");
        rec.parentStat.valid = true;
        rec.parentStat.ino = 999999999ULL;  // mismatched parent/inode
        rec.expectedDestStat.valid = true;
        rec.expectedDestStat.ino = 888888888ULL;
        rec.stageStat.valid = true;
        rec.stageStat.ino = 777777777ULL;
        verify(writeFile(txMismatch + QStringLiteral("/.thispc-merge-tx.mismatch"), rec.serializeSigned()), "write mismatch journal");

        const auto rep = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(rep.anyIncompleteFound
            && rep.anyManualInterventionRequired
            && rep.findings.size() == 3
            && rep.preservedUntouchedFiles.size() == 2
            && sha256File(dst + QStringLiteral("/.thispc-part.unknown.1")) == QCryptographicHash::hash("UNKNOWN-PART", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/.thispc-replaced.unknown.1")) == QCryptographicHash::hash("UNKNOWN-REPL", QCryptographicHash::Sha256)
            && sha256File(txCorrupt + QStringLiteral("/stage")) == QCryptographicHash::hash("KEEP-CORRUPT-DATA", QCryptographicHash::Sha256)
            && sha256File(txForeign + QStringLiteral("/alien.txt")) == QCryptographicHash::hash("ALIEN-DATA", QCryptographicHash::Sha256),
               "K+L+M+N+O+P+T fail-closed: corrupted journals, foreign tx objects, mismatched inodes, and unknown .thispc-* files are NEVER deleted");
    }

    // Q & R: Parallel Overwrites to different files + two plans targeting same file
    {
        const QString base = tmp.filePath(QStringLiteral("parallel_tx"));
        const QString src = base + QStringLiteral("/src/WTF");
        const QString dst = base + QStringLiteral("/dst/WTF");
        verify(QDir().mkpath(src) && QDir().mkpath(dst)
            && writeFile(src + QStringLiteral("/A.txt"), "NEW-A")
            && writeFile(dst + QStringLiteral("/A.txt"), "OLD-A")
            && writeFile(src + QStringLiteral("/B.txt"), "NEW-B")
            && writeFile(dst + QStringLiteral("/B.txt"), "OLD-B"), "parallel trees");

        LocalMergePlan planA = makePlan(src, base + QStringLiteral("/dst"));
        planA.entries.erase(std::remove_if(planA.entries.begin(), planA.entries.end(), [](const LocalMergePlanEntry &e) {
            return e.relativePath == QLatin1String("WTF/B.txt");
        }), planA.entries.end());
        approveAdditiveMergePlan(planA);
        verify(approveFileOverwrite(planA, QStringLiteral("WTF/A.txt")), "bind A");

        LocalMergePlan planB = makePlan(src, base + QStringLiteral("/dst"));
        planB.entries.erase(std::remove_if(planB.entries.begin(), planB.entries.end(), [](const LocalMergePlanEntry &e) {
            return e.relativePath == QLatin1String("WTF/A.txt");
        }), planB.entries.end());
        approveAdditiveMergePlan(planB);
        verify(approveFileOverwrite(planB, QStringLiteral("WTF/B.txt")), "bind B");

        LocalMergeExecutionJob jobA(planA);
        LocalMergeExecutionJob jobB(planB);
        jobA.setAutoDelete(false);
        jobB.setAutoDelete(false);
        QSignalSpy spyA(&jobA, &KJob::result);
        QSignalSpy spyB(&jobB, &KJob::result);
        jobA.start();
        jobB.start();
        verify(waitUntil([&] { return spyA.count() == 1 && spyB.count() == 1; }, 5000), "parallel A & B finish");
        verify(jobA.error() == KJob::NoError && jobB.error() == KJob::NoError
            && sha256File(dst + QStringLiteral("/A.txt")) == QCryptographicHash::hash("NEW-A", QCryptographicHash::Sha256)
            && sha256File(dst + QStringLiteral("/B.txt")) == QCryptographicHash::hash("NEW-B", QCryptographicHash::Sha256),
               "Q: parallel transactions to different files both succeed with exact SHA-256");

        // R: Two transactions targeting the exact same file (second must fail-closed on inode mismatch!)
        verify(writeFile(src + QStringLiteral("/A.txt"), "NEW-A-2"), "update src A");
        LocalMergePlan planR1 = makePlan(src, base + QStringLiteral("/dst"));
        approveAdditiveMergePlan(planR1);
        verify(approveFileOverwrite(planR1, QStringLiteral("WTF/A.txt")), "bind R1");

        LocalMergePlan planR2 = makePlan(src, base + QStringLiteral("/dst"));
        approveAdditiveMergePlan(planR2);
        verify(approveFileOverwrite(planR2, QStringLiteral("WTF/A.txt")), "bind R2");

        LocalMergeExecutionJob jobR1(planR1);
        jobR1.setAutoDelete(false);
        QSignalSpy spyR1(&jobR1, &KJob::result);
        jobR1.start();
        verify(waitUntil([&] { return spyR1.count() == 1; }, 5000) && jobR1.error() == KJob::NoError, "R1 succeeds");

        LocalMergeExecutionJob jobR2(planR2);
        jobR2.setAutoDelete(false);
        QSignalSpy spyR2(&jobR2, &KJob::result);
        jobR2.start();
        verify(waitUntil([&] { return spyR2.count() == 1; }, 5000) && jobR2.error() != KJob::NoError
            && sha256File(dst + QStringLiteral("/A.txt")) == QCryptographicHash::hash("NEW-A-2", QCryptographicHash::Sha256),
               "R: second transaction targeting same already-replaced file fails closed on inode mismatch");
    }

    qInfo("PASS: %d Stage 3B.1 LocalMergeRecovery & Transaction Journal assertions verified", checks);
    return 0;
}
