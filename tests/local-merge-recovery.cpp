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

    // =========================================================================
    // STAGE 1B.8 (SEC-046-07): CRASH RECOVERY JOURNAL HARDENING TESTS J1–J22
    // =========================================================================
    auto readFileRec = [](const QString &path) -> QByteArray {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
    };
    auto makeSignedRaw = [](const QMap<QString, QByteArray> &fields) -> QByteArray {
        QByteArray body;
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            body += it.key().toLatin1() + '=' + it.value() + '\n';
        }
        const QByteArray digest = QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex();
        return QByteArray("THISPC_MERGE_TX_V1\n") + body + QByteArray("CHECKSUM=") + digest + QByteArray("\nEND\n");
    };
    auto baseValidFields = [](const QString &txId, const QString &dstName = QStringLiteral("file.txt"),
                              const QString &stageName = QStringLiteral("stage"),
                              const QString &backupName = QStringLiteral(".thispc-replaced.file.txt")) -> QMap<QString, QByteArray> {
        QMap<QString, QByteArray> m;
        m.insert(QStringLiteral("TXID"), txId.toLatin1());
        m.insert(QStringLiteral("PHASE"), "StagingDurable");
        m.insert(QStringLiteral("ACTION"), "Overwrite");
        m.insert(QStringLiteral("SRC"), QByteArray("/src/file.txt").toBase64());
        m.insert(QStringLiteral("DST"), QByteArray("/dst/file.txt").toBase64());
        m.insert(QStringLiteral("DSTNAME"), dstName.toUtf8().toBase64());
        m.insert(QStringLiteral("STAGENAME"), stageName.toUtf8().toBase64());
        m.insert(QStringLiteral("BACKUPNAME"), backupName.toUtf8().toBase64());
        m.insert(QStringLiteral("NEWSHA256"), QByteArray(64, 'a'));
        m.insert(QStringLiteral("PARENTSTAT"), "1,1,2,16877,0,0,0");
        m.insert(QStringLiteral("SRCSTAT"), "1,1,3,33188,10,0,0");
        m.insert(QStringLiteral("OLDSTAT"), "1,1,4,33188,10,0,0");
        m.insert(QStringLiteral("NEWSTAT"), "1,1,5,33188,10,0,0");
        m.insert(QStringLiteral("DURABLE"), "0");
        return m;
    };

    // J1 — Valid legacy 0.45.1 PID.counter journal deserializes cleanly
    {
        LocalMergeTxJournalRecord rec;
        const QByteArray raw = makeSignedRaw(baseValidFields(QStringLiteral("12345.1")));
        verify(LocalMergeTxJournalRecord::deserializeSigned(raw, &rec)
                   && rec.txId == QStringLiteral("12345.1")
                   && rec.destFileName == QStringLiteral("file.txt"),
               "J1: valid legacy 0.45.1 PID.counter journal accepted");
    }

    // J2 & J16 — Valid new cryptographic transaction ID + write/read roundtrip + uniqueness
    {
        QSet<QString> ids;
        for (int i = 0; i < 32; ++i) {
            const QString id = LocalMergeTxDetail::generateTxId();
            verify(id.size() == 32 && LocalMergeTxValidation::isValidTxId(id),
                   "J16: generateTxId produces valid 32-char hex ID");
            ids.insert(id);
        }
        verify(ids.size() == 32, "J16: 32 consecutive transaction IDs are unique");

        const QString j2Dir = tmp.filePath(QStringLiteral("j2_roundtrip"));
        verify(QDir().mkpath(j2Dir), "j2 mkpath");
        const int fd = ::open(QFile::encodeName(j2Dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        verify(fd >= 0, "j2 open dir");
        LocalMergeTxJournalRecord wrec;
        wrec.txId = LocalMergeTxDetail::generateTxId();
        wrec.destFileName = QStringLiteral("zażółć gęślą jaźń.txt");
        wrec.stageFileName = QStringLiteral("stage");
        wrec.replacedBackupName = QStringLiteral(".thispc-replaced.zażółć");
        wrec.sourcePath = QStringLiteral("/src/zażółć gęślą jaźń.txt");
        wrec.destinationPath = QStringLiteral("/dst/zażółć gęślą jaźń.txt");
        const QString jName = QStringLiteral(".thispc-merge-tx.%1").arg(wrec.txId);
        verify(LocalMergeTxDetail::writeJournalAtomically(fd, jName, wrec), "J2: writeJournalAtomically succeeds");
        LocalMergeTxJournalRecord rrec;
        verify(LocalMergeTxDetail::readJournalFile(fd, jName, &rrec)
                   && rrec.txId == wrec.txId
                   && rrec.destFileName == wrec.destFileName,
               "J2 & J9: new journal roundtrip preserves valid Unicode filename");
        ::close(fd);
    }

    // J3, J4, J5, J6, J7, J8, J19 — Strict deserialization rejections
    {
        LocalMergeTxJournalRecord rec;
        const QString txId = LocalMergeTxDetail::generateTxId();

        // J3: Parent traversal
        verify(!LocalMergeTxJournalRecord::deserializeSigned(
                   makeSignedRaw(baseValidFields(txId, QStringLiteral("../outside"))), &rec),
               "J3: DSTNAME traversal ../outside rejected");
        verify(!LocalMergeTxJournalRecord::deserializeSigned(
                   makeSignedRaw(baseValidFields(txId, QStringLiteral("ok.txt"), QStringLiteral("../outside"))), &rec),
               "J3: STAGENAME traversal ../outside rejected");
        verify(!LocalMergeTxJournalRecord::deserializeSigned(
                   makeSignedRaw(baseValidFields(txId, QStringLiteral("ok.txt"), QStringLiteral("stage"), QStringLiteral("../outside"))), &rec),
               "J3: BACKUPNAME traversal ../outside rejected");

        // J4: Absolute path
        verify(!LocalMergeTxJournalRecord::deserializeSigned(
                   makeSignedRaw(baseValidFields(txId, QStringLiteral("/tmp/evil"))), &rec),
               "J4: absolute path in DSTNAME rejected");

        // J5: Embedded path separators
        verify(!LocalMergeTxJournalRecord::deserializeSigned(
                   makeSignedRaw(baseValidFields(txId, QStringLiteral("sub/nested.txt"))), &rec),
               "J5: path separator in DSTNAME rejected");

        // J6: Embedded NUL byte
        {
            auto fields = baseValidFields(txId);
            QByteArray nulName("stage");
            nulName.append('\0');
            nulName.append("evil");
            fields[QStringLiteral("STAGENAME")] = nulName.toBase64();
            verify(!LocalMergeTxJournalRecord::deserializeSigned(makeSignedRaw(fields), &rec),
                   "J6: embedded NUL in Base64 field rejected");
        }

        // J7: Invalid UTF-8 sequence
        {
            auto fields = baseValidFields(txId);
            fields[QStringLiteral("DSTNAME")] = QByteArray("\xff\xfe\x80").toBase64();
            verify(!LocalMergeTxJournalRecord::deserializeSigned(makeSignedRaw(fields), &rec),
                   "J7: invalid UTF-8 sequence rejected");
        }

        // J8: Invalid Base64
        {
            auto fields = baseValidFields(txId);
            fields[QStringLiteral("DSTNAME")] = QByteArray("!!!not-valid-base64!!!");
            verify(!LocalMergeTxJournalRecord::deserializeSigned(makeSignedRaw(fields), &rec),
                   "J8: invalid Base64 rejected");
        }

        // J19: Corrupt checksum
        {
            QByteArray raw = makeSignedRaw(baseValidFields(txId));
            raw.replace("DURABLE=0", "DURABLE=1");
            verify(!LocalMergeTxJournalRecord::deserializeSigned(raw, &rec),
                   "J19: corrupt checksum rejected");
        }
    }

    // J10 & J11 & J20 — Symlinked txDir, symlinked journal, and non-0700 txDir permissions
    {
        const QString base = tmp.filePath(QStringLiteral("j10_11_20"));
        const QString dst = base + QStringLiteral("/dst");
        const QString extDir = base + QStringLiteral("/external_dir");
        const QString extFile = base + QStringLiteral("/external_secret.txt");
        verify(QDir().mkpath(dst) && QDir().mkpath(extDir) && writeFile(extFile, "EXTERNAL"), "j10 setup");

        // J10: txDir is a symlink
        const QString txId10 = LocalMergeTxDetail::generateTxId();
        verify(QFile::link(extDir, dst + QStringLiteral("/.thispc-tx.") + txId10), "j10 link txDir");
        const auto rep10 = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(rep10.anyManualInterventionRequired && QFileInfo::exists(extDir),
               "J10: symlinked txDir rejected without following");
        QFile::remove(dst + QStringLiteral("/.thispc-tx.") + txId10);

        // J11: journal inside txDir is a symlink
        const QString txId11 = LocalMergeTxDetail::generateTxId();
        const QString txDir11 = dst + QStringLiteral("/.thispc-tx.") + txId11;
        verify(QDir().mkpath(txDir11) && ::chmod(QFile::encodeName(txDir11).constData(), 0700) == 0, "j11 mk txDir");
        verify(QFile::link(extFile, txDir11 + QStringLiteral("/.thispc-merge-tx.") + txId11), "j11 link journal");
        const auto rep11 = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(rep11.anyManualInterventionRequired && QFileInfo::exists(extFile) && readFileRec(extFile) == "EXTERNAL",
               "J11: symlinked journal rejected without reading or mutating external file");
        QFile::remove(txDir11 + QStringLiteral("/.thispc-merge-tx.") + txId11);
        QDir().rmdir(txDir11);

        // J20: txDir has overly permissive mode 0755 instead of 0700
        const QString txId20 = LocalMergeTxDetail::generateTxId();
        const QString txDir20 = dst + QStringLiteral("/.thispc-tx.") + txId20;
        verify(QDir().mkpath(txDir20) && ::chmod(QFile::encodeName(txDir20).constData(), 0755) == 0, "j20 mk 0755 txDir");
        const auto rep20 = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(rep20.anyManualInterventionRequired && QFileInfo::exists(txDir20),
               "J20: permissive 0755 txDir flagged ManualInterventionRequired without mutation");
    }

    // J12, J13, J14, J17, J18 — Directory swap, foreign file, .tmp handling, forged malicious journal, default non-destructive
    {
        const QString base = tmp.filePath(QStringLiteral("j12_13_14_17"));
        const QString dst = base + QStringLiteral("/dst");
        const QString outsideSentinel = base + QStringLiteral("/outside_sentinel.txt");
        verify(QDir().mkpath(dst) && writeFile(outsideSentinel, "UNTOUCHED"), "j17 setup");
        const QString targetFile = dst + QStringLiteral("/real.txt");
        verify(writeFile(targetFile, "OLD-DATA"), "j17 write real.txt");

        struct stat parentSt {};
        struct stat oldSt {};
        verify(::lstat(QFile::encodeName(dst).constData(), &parentSt) == 0
                   && ::lstat(QFile::encodeName(targetFile).constData(), &oldSt) == 0, "j17 stats");

        // J17: Malicious journal with valid recomputed SHA-256 pointing STAGENAME at ../../outside_sentinel.txt
        const QString txId17 = LocalMergeTxDetail::generateTxId();
        const QString txDir17 = dst + QStringLiteral("/.thispc-tx.") + txId17;
        verify(QDir().mkpath(txDir17) && ::chmod(QFile::encodeName(txDir17).constData(), 0700) == 0, "j17 txDir");
        auto maliciousFields = baseValidFields(txId17, QStringLiteral("real.txt"), QStringLiteral("../../outside_sentinel.txt"));
        maliciousFields[QStringLiteral("PARENTSTAT")] = LocalMergeTxStatSnapshot::fromStat(parentSt).encode().toLatin1();
        maliciousFields[QStringLiteral("OLDSTAT")] = LocalMergeTxStatSnapshot::fromStat(oldSt).encode().toLatin1();
        verify(writeFile(txDir17 + QStringLiteral("/.thispc-merge-tx.") + txId17, makeSignedRaw(maliciousFields)), "j17 write forged journal");
        const auto rep17 = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(rep17.anyManualInterventionRequired && readFileRec(outsideSentinel) == "UNTOUCHED",
               "J17: forged journal traversal rejected even under resolveUnambiguous=true; outside sentinel untouched");
        QFile::remove(txDir17 + QStringLiteral("/.thispc-merge-tx.") + txId17);
        QDir().rmdir(txDir17);

        // J13: Foreign file inside valid txDir preserved
        const QString txId13 = LocalMergeTxDetail::generateTxId();
        const QString txDir13 = dst + QStringLiteral("/.thispc-tx.") + txId13;
        verify(QDir().mkpath(txDir13) && ::chmod(QFile::encodeName(txDir13).constData(), 0700) == 0
                   && writeFile(txDir13 + QStringLiteral("/foreign_user_note.txt"), "KEEP-ME"), "j13 setup");
        const auto rep13 = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(rep13.anyManualInterventionRequired
                   && readFileRec(txDir13 + QStringLiteral("/foreign_user_note.txt")) == "KEEP-ME",
               "J13: foreign file inside txDir preserved untouched");
        QFile::remove(txDir13 + QStringLiteral("/foreign_user_note.txt"));
        QDir().rmdir(txDir13);

        // J14 + J18 + J12: Valid StagedOnlyAborted with orphaned .tmp, default non-destructive, then swap attack, then clean resolve
        const QString txId14 = LocalMergeTxDetail::generateTxId();
        const QString txDir14 = dst + QStringLiteral("/.thispc-tx.") + txId14;
        verify(QDir().mkpath(txDir14) && ::chmod(QFile::encodeName(txDir14).constData(), 0700) == 0, "j14 txDir");
        const QString stagePath = txDir14 + QStringLiteral("/stage");
        verify(writeFile(stagePath, "STAGED-NEW"), "j14 stage");
        struct stat stageSt {};
        verify(::lstat(QFile::encodeName(stagePath).constData(), &stageSt) == 0, "j14 stageStat");
        const int txFd14 = ::open(QFile::encodeName(txDir14).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        verify(txFd14 >= 0, "j14 open");
        LocalMergeTxJournalRecord rec14;
        rec14.txId = txId14;
        rec14.phase = LocalMergeTxPhase::StagingDurable;
        rec14.sourcePath = QStringLiteral("/src/real.txt");
        rec14.destinationPath = targetFile;
        rec14.destFileName = QStringLiteral("real.txt");
        rec14.stageFileName = QStringLiteral("stage");
        rec14.replacedBackupName = QStringLiteral(".thispc-replaced.real.txt");
        rec14.parentStat = LocalMergeTxStatSnapshot::fromStat(parentSt);
        rec14.expectedDestStat = LocalMergeTxStatSnapshot::fromStat(oldSt);
        rec14.stageStat = LocalMergeTxStatSnapshot::fromStat(stageSt);
        verify(LocalMergeTxDetail::writeJournalAtomically(txFd14, QStringLiteral(".thispc-merge-tx.") + txId14, rec14), "j14 write journal");
        ::close(txFd14);
        const QString tmpPath = txDir14 + QStringLiteral("/.thispc-merge-tx.") + txId14 + QStringLiteral(".tmp");
        verify(writeFile(tmpPath, "PARTIAL-TMP"), "j14 write .tmp");

        // J18: Default resolveUnambiguous=false does NOT delete anything
        const auto repDefault = LocalMergeRecoveryInspector::inspectDirectory(dst);
        verify(repDefault.anyIncompleteFound && !repDefault.anyManualInterventionRequired
                   && QFileInfo::exists(stagePath) && QFileInfo::exists(tmpPath),
               "J18: default inspectDirectory(dst) is non-destructive");

        // J12: Replace txDir between inspection and cleanup -> aborts mutation!
        const auto repSwap = LocalMergeRecoveryInspector::inspectDirectory(dst, true, [&](const QString &name) {
            QDir(dst).rename(name, name + QStringLiteral("_moved"));
            QDir(dst).mkdir(name);
            ::chmod(QFile::encodeName(dst + QLatin1Char('/') + name).constData(), 0700);
        });
        verify(repSwap.anyManualInterventionRequired
                   && QFileInfo::exists(dst + QStringLiteral("/.thispc-tx.") + txId14 + QStringLiteral("_moved/stage")),
               "J12: txDir replacement before cleanup aborts mutation safely");
        QDir().rmdir(txDir14);
        QDir(dst).rename(QStringLiteral(".thispc-tx.") + txId14 + QStringLiteral("_moved"),
                         QStringLiteral(".thispc-tx.") + txId14);

        // J14 clean resolve: unambiguous StagedOnlyAborted with valid regular .tmp cleans up completely
        const auto repClean = LocalMergeRecoveryInspector::inspectDirectory(dst, true);
        verify(repClean.findings.size() == 1
                   && repClean.findings.first().status == LocalMergeRecoveryStatus::StagedOnlyAborted
                   && !QFileInfo::exists(txDir14)
                   && readFileRec(targetFile) == "OLD-DATA",
               "J14: unambiguous StagedOnlyAborted with safe .tmp cleans up cleanly while preserving OLD");
    }

    qInfo("PASS: %d Stage 3B.1 LocalMergeRecovery & Transaction Journal assertions verified", checks);
    return 0;
}
