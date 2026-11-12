/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localbatchmovejob.h"
#include "localfilemovejob.h"
#include "transferqueuecontroller.h"
#include "transfernotificationmanager.h"
#include "kiostagedtransferjob.h"
#include "undocontroller.h"

#include <KIO/FileUndoManager>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#ifndef MSDOS_SUPER_MAGIC
#define MSDOS_SUPER_MAGIC 0x4d44
#endif
#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

template<typename Predicate>
static bool waitUntil(Predicate predicate, int timeoutMilliseconds = 8000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(3);
    }
    return predicate();
}

static bool writeFileBytes(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size();
}

static QByteArray sha256File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

static struct stat lstatEntry(const QString &path, bool *okOut = nullptr)
{
    struct stat st {};
    const bool ok = (::lstat(QFile::encodeName(path).constData(), &st) == 0);
    if (okOut) *okOut = ok;
    return st;
}

static void resetBatchTestHooks()
{
    simulateBatchMoveUnsupportedFsMagic() = 0;
    simulateBatchMoveOpenat2Unsupported() = false;
    simulateBatchMoveStatxMntIdUnsupported() = false;
    simulateBatchMoveRenameNoReplaceUnsupported() = false;
    simulateBatchMoveExdev() = false;
    simulateBatchMoveBeforeStep() = {};
    simulateBatchMoveBeforeRename() = {};
    simulateBatchMoveAfterRenameBeforeFsync() = {};
    simulateBatchMoveDestinationFsyncErrorAtStep() = {-1, 0};
    simulateBatchMoveSourceFsyncErrorAtStep() = {-1, 0};
    simulateBatchMovePostRenameVerifyFailAtStep() = -1;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    resetBatchTestHooks();

    const QString btrfsParent = qEnvironmentVariable("THISPC_TEST_FILES");
    verify(!btrfsParent.isEmpty(), "THISPC_TEST_FILES environment path provided");
    QTemporaryDir btrfsRoot(btrfsParent + QStringLiteral("/batch-move-XXXXXX"));
    verify(btrfsRoot.isValid(), "Btrfs test root directory created");

    struct statfs rootSfs {};
    verify(::statfs(QFile::encodeName(btrfsRoot.path()).constData(), &rootSfs) == 0
               && static_cast<unsigned long>(rootSfs.f_type) == BTRFS_SUPER_MAGIC,
           "Test root resides on real Btrfs filesystem");

    const QString srcRoot = btrfsRoot.path() + QStringLiteral("/src");
    const QString dstRoot = btrfsRoot.path() + QStringLiteral("/dst");
    verify(QDir().mkpath(srcRoot) && QDir().mkpath(dstRoot), "src/dst directories created");

    // -------------------------------------------------------------------------
    // 1. Positive: BM-P1 — 3 regular files (Unicode, spaces, hidden), multi-dir, Undo & Redo
    // -------------------------------------------------------------------------
    {
        const QString srcSubA = srcRoot + QStringLiteral("/subA");
        const QString srcSubB = srcRoot + QStringLiteral("/subB");
        const QString dstSub = dstRoot + QStringLiteral("/pos1");
        verify(QDir().mkpath(srcSubA) && QDir().mkpath(srcSubB) && QDir().mkpath(dstSub),
               "BM-P1: directories created");

        const QString f1 = srcSubA + QString::fromUtf8("/zażółć gęślą.txt");
        const QString f2 = srcSubB + QStringLiteral("/spacja nazwa.bin");
        const QString f3 = srcSubA + QStringLiteral("/.ukryty_plik");
        const QByteArray d1("alpha-zażółć-payload");
        const QByteArray d2("beta-space-binary-payload-123456");
        const QByteArray d3("hidden-file-data");
        verify(writeFileBytes(f1, d1) && writeFileBytes(f2, d2) && writeFileBytes(f3, d3),
               "BM-P1: source files created");

        const struct stat st1 = lstatEntry(f1);
        const struct stat st2 = lstatEntry(f2);
        const struct stat st3 = lstatEntry(f3);
        const QByteArray h1 = QCryptographicHash::hash(d1, QCryptographicHash::Sha256);
        const QByteArray h2 = QCryptographicHash::hash(d2, QCryptographicHash::Sha256);
        const QByteArray h3 = QCryptographicHash::hash(d3, QCryptographicHash::Sha256);

        const QList<QUrl> srcs = {QUrl::fromLocalFile(f1), QUrl::fromLocalFile(f2), QUrl::fromLocalFile(f3)};
        const QUrl dstUrl = QUrl::fromLocalFile(dstSub);
        const MoveOperationClassification pol = MoveSafetyPolicy::evaluate(srcs, dstUrl, true);
        verify(pol.allowed && pol.decision == MovePolicyDecision::AllowedBatchSameVolumeBtrfsRegularFiles,
               "BM-P1: MoveSafetyPolicy qualifies multi-dir regular files on Btrfs");

        LocalMoveHistory history;
        auto *job = new LocalBatchMoveJob(srcs, dstUrl);
        job->setAutoDelete(false);
        history.recordWhenCompleted(job, 101);
        QSignalSpy spy(job, &KJob::result);
        job->start();
        verify(waitUntil([&] { return spy.count() == 1; }), "BM-P1: batch move finishes");

        const QString t1 = dstSub + QString::fromUtf8("/zażółć gęślą.txt");
        const QString t2 = dstSub + QStringLiteral("/spacja nazwa.bin");
        const QString t3 = dstSub + QStringLiteral("/.ukryty_plik");
        verify(job->error() == KJob::NoError
                   && job->outcome() == BatchMoveOutcome::FullSuccess
                   && job->publishedCount() == 3
                   && job->durableCount() == 3
                   && job->failedCount() == 0
                   && job->skippedCount() == 0
                   && !job->partialMoveOutcome()
                   && !job->property("thispcPartialMoveOutcome").toBool()
                   && !job->property("thispcBatchPartiallyCompleted").toBool(),
               "BM-P1: batch result reports FullSuccess 3/3 durable");
        verify(!QFileInfo::exists(f1) && !QFileInfo::exists(f2) && !QFileInfo::exists(f3),
               "BM-P1: all source paths removed after atomic rename");
        verify(!QFileInfo::exists(t1 + QStringLiteral(".thispc-part"))
                   && !QFileInfo::exists(t2 + QStringLiteral(".thispc-part")),
               "BM-P1: zero staging artifacts created");

        const struct stat dt1 = lstatEntry(t1);
        const struct stat dt2 = lstatEntry(t2);
        const struct stat dt3 = lstatEntry(t3);
        verify(dt1.st_dev == st1.st_dev && dt1.st_ino == st1.st_ino
                   && dt2.st_dev == st2.st_dev && dt2.st_ino == st2.st_ino
                   && dt3.st_dev == st3.st_dev && dt3.st_ino == st3.st_ino,
               "BM-P1: all inodes and devices preserved atomically");
        verify(sha256File(t1) == h1 && sha256File(t2) == h2 && sha256File(t3) == h3,
               "BM-P1: all SHA-256 checksums intact");
        verify(history.canUndo() && history.undoSerial() == 101 && !history.canRedo(),
               "BM-P1: single compound Undo record registered");

        // Compound Undo returns all 3 files across their two distinct source directories in reverse order
        KJob *undoJob = history.undo();
        verify(undoJob != nullptr, "BM-P1: compound Undo job started");
        QSignalSpy undoSpy(undoJob, &KJob::result);
        verify(waitUntil([&] { return undoSpy.count() == 1; }), "BM-P1: Undo completes");
        const auto *undoBatch = qobject_cast<LocalBatchMoveJob *>(undoJob);
        verify(undoBatch && undoJob->error() == KJob::NoError
                   && undoBatch->outcome() == BatchMoveOutcome::FullSuccess
                   && undoBatch->durableCount() == 3
                   && undoBatch->batchResult().items.first().sourceUrl == QUrl::fromLocalFile(t3),
               "BM-P1: Undo executed in reverse order cleanly");
        const struct stat ut1 = lstatEntry(f1);
        const struct stat ut2 = lstatEntry(f2);
        const struct stat ut3 = lstatEntry(f3);
        verify(ut1.st_ino == st1.st_ino && ut2.st_ino == st2.st_ino && ut3.st_ino == st3.st_ino
                   && sha256File(f1) == h1 && sha256File(f2) == h2 && sha256File(f3) == h3
                   && !QFileInfo::exists(t1) && !QFileInfo::exists(t2) && !QFileInfo::exists(t3),
               "BM-P1: Undo restored all files to multi-parent locations with original inodes & SHA-256");
        verify(!history.canUndo() && history.canRedo(), "BM-P1: Redo enabled after Undo");

        // Compound Redo restores destination cleanly in forward order
        KJob *redoJob = history.redo();
        verify(redoJob != nullptr, "BM-P1: compound Redo job started");
        QSignalSpy redoSpy(redoJob, &KJob::result);
        verify(waitUntil([&] { return redoSpy.count() == 1; }), "BM-P1: Redo completes");
        const auto *redoBatch = qobject_cast<LocalBatchMoveJob *>(redoJob);
        verify(redoBatch && redoJob->error() == KJob::NoError
                   && redoBatch->outcome() == BatchMoveOutcome::FullSuccess
                   && redoBatch->durableCount() == 3
                   && redoBatch->batchResult().items.first().sourceUrl == QUrl::fromLocalFile(f1)
                   && lstatEntry(t1).st_ino == st1.st_ino
                   && lstatEntry(t2).st_ino == st2.st_ino
                   && lstatEntry(t3).st_ino == st3.st_ino,
               "BM-P1: Redo moved all 3 files forward again with original inodes intact");
        delete redoJob;
        delete undoJob;
        delete job;
    }

    // -------------------------------------------------------------------------
    // 2. Preflight Negatives: BM-N1, BM-N2, dupes, hardlinks, symlinks, dirs, FIFO, socket, cross-FS, kernel gaps
    // -------------------------------------------------------------------------
    {
        const QString pfSrcA = srcRoot + QStringLiteral("/pfA");
        const QString pfSrcB = srcRoot + QStringLiteral("/pfB");
        const QString pfDst = dstRoot + QStringLiteral("/pfDst");
        verify(QDir().mkpath(pfSrcA) && QDir().mkpath(pfSrcB) && QDir().mkpath(pfDst),
               "Preflight dirs created");

        const QString p1 = pfSrcA + QStringLiteral("/a.txt");
        const QString p2 = pfSrcA + QStringLiteral("/b.txt");
        verify(writeFileBytes(p1, "A") && writeFileBytes(p2, "B"), "Preflight base files");

        // BM-N1: destination collision on 2nd file blocks ENTIRE batch before moving file 1
        const QString colDst = pfDst + QStringLiteral("/b.txt");
        verify(writeFileBytes(colDst, "EXISTING"), "BM-N1: destination collision created");
        {
            LocalBatchMoveJob colJob({QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p2)}, QUrl::fromLocalFile(pfDst));
            colJob.setAutoDelete(false);
            QSignalSpy spy(&colJob, &KJob::result);
            colJob.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N1: finished");
            verify(colJob.error() != KJob::NoError
                       && colJob.outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && colJob.batchError() == BatchMoveError::DestinationExists
                       && colJob.publishedCount() == 0
                       && colJob.durableCount() == 0
                       && QFileInfo::exists(p1) && QFileInfo::exists(p2)
                       && !QFileInfo::exists(pfDst + QStringLiteral("/a.txt"))
                       && sha256File(colDst) == QCryptographicHash::hash("EXISTING", QCryptographicHash::Sha256),
                   "BM-N1: preflight destination collision prevents ALL moves and preserves target");
        }
        verify(QFile::remove(colDst), "BM-N1: remove collision");

        // BM-N2: same target basename from two different source folders -> CollisionInBatch
        const QString p2DupName = pfSrcB + QStringLiteral("/a.txt");
        verify(writeFileBytes(p2DupName, "A-FROM-B"), "BM-N2: same-basename file created");
        {
            const auto pol = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p2DupName)},
                QUrl::fromLocalFile(pfDst), true);
            verify(!pol.allowed && pol.decision == MovePolicyDecision::RefusedLocalContainment,
                   "BM-N2: policy rejects two sources mapping to same destination name");
            LocalBatchMoveJob dupNameJob(
                {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p2DupName)},
                QUrl::fromLocalFile(pfDst));
            dupNameJob.setAutoDelete(false);
            QSignalSpy spy(&dupNameJob, &KJob::result);
            dupNameJob.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N2: finished");
            verify(dupNameJob.batchError() == BatchMoveError::CollisionInBatch
                       && dupNameJob.publishedCount() == 0
                       && QFileInfo::exists(p1) && QFileInfo::exists(p2DupName)
                       && !QFileInfo::exists(pfDst + QStringLiteral("/a.txt")),
                   "BM-N2: zero files moved on target name collision");
        }

        // Duplicate source URLs
        {
            LocalBatchMoveJob dupUrlJob(
                {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p1)},
                QUrl::fromLocalFile(pfDst));
            dupUrlJob.setAutoDelete(false);
            QSignalSpy spy(&dupUrlJob, &KJob::result);
            dupUrlJob.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "DupURL: finished");
            verify(dupUrlJob.batchError() == BatchMoveError::CollisionInBatch && dupUrlJob.publishedCount() == 0
                       && QFileInfo::exists(p1) && !QFileInfo::exists(pfDst + QStringLiteral("/a.txt")),
                   "DupURL: duplicate source URL rejected before start");
        }

        // Unsupported hardlinked file (nlink > 1) in batch -> fail-closed
        const QString hlA = pfSrcA + QStringLiteral("/hl.txt");
        const QString hlB = pfSrcB + QStringLiteral("/hl_other.txt");
        verify(writeFileBytes(hlA, "HL")
                   && ::link(QFile::encodeName(hlA).constData(), QFile::encodeName(hlB).constData()) == 0,
               "Hardlink pair created");
        {
            const auto pol = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(hlA)},
                QUrl::fromLocalFile(pfDst), true);
            verify(!pol.allowed && pol.decision == MovePolicyDecision::RefusedLocalContainment
                       && QFileInfo::exists(p1) && QFileInfo::exists(hlA),
                   "Preflight rejects hardlinked file (nlink > 1) before any move");
        }

        // Symlink, directory, FIFO, Unix socket mixed with valid file -> fail-closed
        const QString symPath = pfSrcA + QStringLiteral("/sym.lnk");
        const QString dirPath = pfSrcA + QStringLiteral("/subdir");
        const QString fifoPath = pfSrcA + QStringLiteral("/pipe.fifo");
        const QString sockPath = pfSrcA + QStringLiteral("/unix.sock");
        verify(::symlink(QFile::encodeName(p2).constData(), QFile::encodeName(symPath).constData()) == 0, "Symlink created");
        verify(QDir().mkdir(dirPath), "Subdir created");
        verify(::mkfifo(QFile::encodeName(fifoPath).constData(), 0600) == 0, "FIFO created");
        const int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        verify(sfd >= 0, "Unix socket fd created");
        {
            const QByteArray oldCwd = QFile::encodeName(QDir::currentPath());
            verify(::chdir(QFile::encodeName(pfSrcA).constData()) == 0, "chdir to pfSrcA for Unix socket");
            struct sockaddr_un addr {};
            addr.sun_family = AF_UNIX;
            std::memcpy(addr.sun_path, "unix.sock", 10);
            verify(::bind(sfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0, "Unix socket bound");
            verify(::chdir(oldCwd.constData()) == 0, "restore cwd after Unix socket bind");
        }
        ::close(sfd);

        for (const QString &bad : {symPath, dirPath, fifoPath, sockPath}) {
            const auto pol = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(bad)},
                QUrl::fromLocalFile(pfDst), true);
            verify(!pol.allowed && pol.decision == MovePolicyDecision::RefusedLocalContainment,
                   "Preflight rejects batch containing symlink/dir/FIFO/socket");
            verify(QFileInfo::exists(p1) && !QFileInfo::exists(pfDst + QStringLiteral("/a.txt")),
                   "Rejected special/dir/symlink batch left zero changes");
        }

        // Non-Btrfs simulated FS (VFAT/NTFS-3G), missing openat2/statx/renameat2, simulated EXDEV, remote & mixed
        for (unsigned long magic : {static_cast<unsigned long>(MSDOS_SUPER_MAGIC), static_cast<unsigned long>(FUSE_SUPER_MAGIC)}) {
            simulateBatchMoveUnsupportedFsMagic() = magic;
            const auto pol = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p2)},
                QUrl::fromLocalFile(pfDst), true);
            simulateBatchMoveUnsupportedFsMagic() = 0;
            verify(!pol.allowed && pol.decision == MovePolicyDecision::RefusedLocalContainment
                       && !QFileInfo::exists(pfDst + QStringLiteral("/a.txt")),
                   "Non-Btrfs (VFAT/NTFS-3G) batch rejected fail-closed before execution");
        }

        simulateBatchMoveOpenat2Unsupported() = true;
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p2)}, QUrl::fromLocalFile(pfDst), true).allowed,
               "Missing openat2 causes fail-closed preflight refusal");
        resetBatchTestHooks();

        simulateBatchMoveStatxMntIdUnsupported() = true;
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p2)}, QUrl::fromLocalFile(pfDst), true).allowed,
               "Missing statx STATX_MNT_ID causes fail-closed preflight refusal");
        resetBatchTestHooks();

        {
            simulateBatchMoveRenameNoReplaceUnsupported() = true;
            LocalBatchMoveJob noReplaceJob({QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p2)}, QUrl::fromLocalFile(pfDst));
            noReplaceJob.setAutoDelete(false);
            QSignalSpy spy(&noReplaceJob, &KJob::result);
            noReplaceJob.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "NoReplaceUnsupported finished");
            resetBatchTestHooks();
            verify(noReplaceJob.batchError() == BatchMoveError::UnsupportedKernelFeature
                       && noReplaceJob.publishedCount() == 0
                       && QFileInfo::exists(p1) && QFileInfo::exists(p2),
                   "Missing RENAME_NOREPLACE fails closed without fallback");
        }
        {
            simulateBatchMoveExdev() = true;
            LocalBatchMoveJob exdevJob({QUrl::fromLocalFile(p1), QUrl::fromLocalFile(p2)}, QUrl::fromLocalFile(pfDst));
            exdevJob.setAutoDelete(false);
            QSignalSpy spy(&exdevJob, &KJob::result);
            exdevJob.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Exdev finished");
            resetBatchTestHooks();
            verify(exdevJob.batchError() == BatchMoveError::MountOrSubvolumeBoundary
                       && exdevJob.publishedCount() == 0
                       && QFileInfo::exists(p1) && QFileInfo::exists(p2),
                   "EXDEV fails closed without Copy+Delete fallback");
        }

        // Real tmpfs (/tmp) cross-FS to Btrfs refused fail-closed
        QTemporaryDir tmpfsDir;
        const QString t1 = tmpfsDir.filePath(QStringLiteral("tmp1.txt"));
        const QString t2 = tmpfsDir.filePath(QStringLiteral("tmp2.txt"));
        verify(writeFileBytes(t1, "T1") && writeFileBytes(t2, "T2"), "tmpfs files created");
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(t1), QUrl::fromLocalFile(t2)}, QUrl::fromLocalFile(pfDst), true).allowed,
               "Real tmpfs -> Btrfs batch rejected fail-closed");
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(p1), QUrl(QStringLiteral("sftp://host/f"))}, QUrl::fromLocalFile(pfDst), true).allowed,
               "Mixed local + remote batch rejected fail-closed");
    }

    // -------------------------------------------------------------------------
    // 3. Execution Negatives & Durability Gate: BM-N5, BM-N6, BM-N7, BM-N8
    // -------------------------------------------------------------------------
    {
        const QString exSrc = srcRoot + QStringLiteral("/exSrc");
        const QString exDst = dstRoot + QStringLiteral("/exDst");
        verify(QDir().mkpath(exSrc) && QDir().mkpath(exDst), "Execution test dirs created");

        // BM-N5a: 2nd source replaced/modified AFTER Preflight (before step 1) -> step 0 preserved, step 1 & 2 stopped, Retry blocked
        const QString s0 = exSrc + QStringLiteral("/m1.txt");
        const QString s1 = exSrc + QStringLiteral("/m2.txt");
        const QString s2 = exSrc + QStringLiteral("/m3.txt");
        const QString d0 = exDst + QStringLiteral("/m1.txt");
        const QString d1 = exDst + QStringLiteral("/m2.txt");
        const QString d2 = exDst + QStringLiteral("/m3.txt");
        verify(writeFileBytes(s0, "ONE") && writeFileBytes(s1, "TWO") && writeFileBytes(s2, "THREE"),
               "BM-N5a: sources created");
        {
            TransferQueueController q;
            UndoController undo(nullptr, [](bool) {}, [] {}, [](const QString &, int) {});
            undo.setQueueController(&q);
            LocalMoveHistory *hist = undo.m_nativeHistory;
            // Record an earlier move to verify Undo fence activates after partial batch
            const QString preS = exSrc + QStringLiteral("/pre_move.txt");
            const QString preD = exDst + QStringLiteral("/pre_move.txt");
            verify(writeFileBytes(preS, "PRE"), "BM-N5a: pre-move file");
            LocalFileMoveJob preJob(QUrl::fromLocalFile(preS), QUrl::fromLocalFile(preD), nullptr, LocalFileMoveStrategy::Automatic);
            preJob.setAutoDelete(false);
            undo.recordNativeTransfer(&preJob, true);
            QSignalSpy preSpy(&preJob, &KJob::result);
            preJob.start();
            verify(waitUntil([&] { return preSpy.count() == 1; }) && undo.nativeUndoAvailable(),
                   "BM-N5a: earlier move Undo is available before partial batch");

            bool swapped = false;
            simulateBatchMoveBeforeStep() = [&](int step) {
                if (step == 1 && !swapped) {
                    swapped = true;
                    QFile::remove(s1);
                    writeFileBytes(s1, "ALIEN_SWAP");
                }
            };
            const QList<QUrl> batchUrls = {QUrl::fromLocalFile(s0), QUrl::fromLocalFile(s1), QUrl::fromLocalFile(s2)};
            TransferTaskDescriptor desc;
            desc.title = QStringLiteral("BM-N5a");
            desc.type = TransferOperationType::Move;
            desc.backend = TransferBackendKind::LocalNative;
            desc.sources = batchUrls;
            desc.destination = QUrl::fromLocalFile(exDst);
            desc.startMode = TransferJobStartMode::ControllerStarts;
            desc.jobFactory = [&]() -> KJob * {
                auto *j = new LocalBatchMoveJob(batchUrls, QUrl::fromLocalFile(exDst), &q);
                undo.recordNativeTransfer(j, true);
                return j;
            };
            const auto enq = q.enqueue(std::move(desc));
            verify(waitUntil([&] {
                const auto s = q.taskSnapshot(enq.id);
                return s.has_value() && s->state == TransferTaskState::Failed;
            }), "BM-N5a: task transitions to Failed after mid-batch source swap");
            resetBatchTestHooks();

            verify(!QFileInfo::exists(s0) && sha256File(d0) == QCryptographicHash::hash("ONE", QCryptographicHash::Sha256),
                   "BM-N5a: file 0 preserved at destination without destructive rollback");
            verify(!QFileInfo::exists(d1) && sha256File(s1) == QCryptographicHash::hash("ALIEN_SWAP", QCryptographicHash::Sha256),
                   "BM-N5a: swapped file 1 stopped cleanly at source");
            verify(!QFileInfo::exists(d2) && sha256File(s2) == QCryptographicHash::hash("THREE", QCryptographicHash::Sha256),
                   "BM-N5a: file 2 untouched at source");
            const auto elig = q.checkRetryEligibility(enq.id);
            verify(!elig.eligible, "BM-N5a: Retry blocked after partial batch publication");
            verify(!hist->canUndo() && !undo.nativeUndoAvailable(),
                   "BM-N5a: partial batch not recorded and earlier Undo fenced by m_irreversibleSerial");
        }

        // BM-N5b: Destination target 2 appears AFTER Preflight (before step 1) -> stops cleanly preserving step 0 & target 2
        const QString c0 = exSrc + QStringLiteral("/c1.txt");
        const QString c1 = exSrc + QStringLiteral("/c2.txt");
        const QString cd0 = exDst + QStringLiteral("/c1.txt");
        const QString cd1 = exDst + QStringLiteral("/c2.txt");
        verify(writeFileBytes(c0, "C1") && writeFileBytes(c1, "C2"), "BM-N5b: sources");
        {
            simulateBatchMoveBeforeStep() = [&](int step) {
                if (step == 1) {
                    writeFileBytes(cd1, "INTRUDER");
                }
            };
            LocalBatchMoveJob midCol({QUrl::fromLocalFile(c0), QUrl::fromLocalFile(c1)}, QUrl::fromLocalFile(exDst));
            midCol.setAutoDelete(false);
            QSignalSpy spy(&midCol, &KJob::result);
            midCol.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N5b: finished");
            resetBatchTestHooks();
            verify(midCol.outcome() == BatchMoveOutcome::PartialSuccessCleanStop
                       && midCol.publishedCount() == 1
                       && midCol.durableCount() == 1
                       && midCol.failedCount() == 1
                       && sha256File(cd0) == QCryptographicHash::hash("C1", QCryptographicHash::Sha256)
                       && sha256File(cd1) == QCryptographicHash::hash("INTRUDER", QCryptographicHash::Sha256)
                       && sha256File(c1) == QCryptographicHash::hash("C2", QCryptographicHash::Sha256),
                   "BM-N5b: mid-batch target race stops without overwriting intruder and keeps file 0");
        }

        // BM-N6: Cancellation between file 0 and file 1 -> file 0 stays durable, file 1 & 2 stay at source
        const QString k0 = exSrc + QStringLiteral("/k1.txt");
        const QString k1 = exSrc + QStringLiteral("/k2.txt");
        const QString k2 = exSrc + QStringLiteral("/k3.txt");
        verify(writeFileBytes(k0, "K1") && writeFileBytes(k1, "K2") && writeFileBytes(k2, "K3"),
               "BM-N6: cancel test files");
        {
            LocalBatchMoveJob cancelMid({QUrl::fromLocalFile(k0), QUrl::fromLocalFile(k1), QUrl::fromLocalFile(k2)},
                                        QUrl::fromLocalFile(exDst));
            cancelMid.setAutoDelete(false);
            std::atomic<bool> atGate{false};
            simulateBatchMoveBeforeStep() = [&](int step) {
                if (step == 1) {
                    atGate.store(true, std::memory_order_release);
                    while (!cancelMid.isCancelRequested()) {
                        QThread::msleep(1);
                    }
                }
            };
            QSignalSpy spy(&cancelMid, &KJob::result);
            cancelMid.start();
            verify(waitUntil([&] { return atGate.load(std::memory_order_acquire); }),
                   "BM-N6: paused before step 1");
            verify(cancelMid.kill(KJob::EmitResult), "BM-N6: mid-batch kill accepted");
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N6: cancelled signal emitted");
            resetBatchTestHooks();
            verify(cancelMid.error() == KJob::KilledJobError
                       && cancelMid.outcome() == BatchMoveOutcome::PartialSuccessCleanStop
                       && cancelMid.publishedCount() == 1
                       && cancelMid.durableCount() == 1
                       && cancelMid.skippedCount() == 2
                       && cancelMid.property("thispcBatchPartiallyCompleted").toBool()
                       && !QFileInfo::exists(k0)
                       && sha256File(exDst + QStringLiteral("/k1.txt")) == QCryptographicHash::hash("K1", QCryptographicHash::Sha256)
                       && QFileInfo::exists(k1) && QFileInfo::exists(k2),
                   "BM-N6: mid-batch cancel stops steps 1..2 and preserves durable file 0");
        }

        // BM-N7: Cancellation arriving in window AFTER SYS_renameat2 of final file before fsync -> NOT misreported Cancelled
        const QString pr0 = exSrc + QStringLiteral("/pr1.txt");
        const QString pr1 = exSrc + QStringLiteral("/pr2.txt");
        verify(writeFileBytes(pr0, "PR1") && writeFileBytes(pr1, "PR2"), "BM-N7: files created");
        {
            LocalBatchMoveJob postRenameJob({QUrl::fromLocalFile(pr0), QUrl::fromLocalFile(pr1)},
                                            QUrl::fromLocalFile(exDst));
            postRenameJob.setAutoDelete(false);
            std::atomic<bool> atPostRename{false};
            std::atomic<bool> releaseGate{false};
            simulateBatchMoveAfterRenameBeforeFsync() = [&](int step) {
                if (step == 1) {
                    atPostRename.store(true, std::memory_order_release);
                    while (!releaseGate.load(std::memory_order_acquire)) {
                        QThread::msleep(1);
                    }
                }
            };
            QSignalSpy spy(&postRenameJob, &KJob::result);
            postRenameJob.start();
            verify(waitUntil([&] { return atPostRename.load(std::memory_order_acquire); }),
                   "BM-N7: paused after renameat2 of last file");
            QThread *releaser = QThread::create([&] {
                QThread::msleep(15);
                releaseGate.store(true, std::memory_order_release);
            });
            releaser->start();
            verify(!postRenameJob.kill(KJob::EmitResult),
                   "BM-N7: doKill refuses to mark FullSuccess post-rename batch as Cancelled");
            releaser->wait();
            delete releaser;
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N7: finishes");
            resetBatchTestHooks();
            verify(postRenameJob.error() == KJob::NoError
                       && postRenameJob.outcome() == BatchMoveOutcome::FullSuccess
                       && postRenameJob.durableCount() == 2
                       && sha256File(exDst + QStringLiteral("/pr2.txt")) == QCryptographicHash::hash("PR2", QCryptographicHash::Sha256),
                   "BM-N7: final outcome preserves truth (FullSuccess) after late kill");
        }

        // BM-N8: fsync error on 2nd file (dstParent AND srcParent variants) + PostRenameVerifyFail
        for (const int mode : {0, 1, 2}) {
            const QString tag = QString::number(mode);
            const QString fA = exSrc + QStringLiteral("/fs_%1_a.txt").arg(tag);
            const QString fB = exSrc + QStringLiteral("/fs_%1_b.txt").arg(tag);
            const QString fC = exSrc + QStringLiteral("/fs_%1_c.txt").arg(tag);
            verify(writeFileBytes(fA, "FA") && writeFileBytes(fB, "FB") && writeFileBytes(fC, "FC"),
                   "BM-N8: durability fixtures created");
            if (mode == 0) simulateBatchMoveDestinationFsyncErrorAtStep() = {1, EIO};
            else if (mode == 1) simulateBatchMoveSourceFsyncErrorAtStep() = {1, EIO};
            else simulateBatchMovePostRenameVerifyFailAtStep() = 1;

            TransferQueueController qDur;
            LocalMoveHistory histDur;
            const QList<QUrl> uList = {QUrl::fromLocalFile(fA), QUrl::fromLocalFile(fB), QUrl::fromLocalFile(fC)};
            TransferTaskDescriptor d;
            d.title = QStringLiteral("BM-N8");
            d.type = TransferOperationType::Move;
            d.backend = TransferBackendKind::LocalNative;
            d.sources = uList;
            d.destination = QUrl::fromLocalFile(exDst);
            d.startMode = TransferJobStartMode::ControllerStarts;
            d.jobFactory = [&]() -> KJob * {
                auto *j = new LocalBatchMoveJob(uList, QUrl::fromLocalFile(exDst), &qDur);
                histDur.recordWhenCompleted(j, 77);
                return j;
            };
            const auto enq = qDur.enqueue(std::move(d));
            verify(waitUntil([&] {
                const auto s = qDur.taskSnapshot(enq.id);
                return s.has_value() && s->state == TransferTaskState::Failed;
            }), "BM-N8: durability/postcheck failure transitions queue task to Failed (never Completed)");
            resetBatchTestHooks();

            const auto snap = qDur.taskSnapshot(enq.id);
            verify(snap.has_value() && snap->state == TransferTaskState::Failed
                       && !qDur.checkRetryEligibility(enq.id).eligible
                       && !histDur.canUndo(),
                   "BM-N8: Retry and Undo strictly blocked after unconfirmed durability");
            verify(sha256File(exDst + QStringLiteral("/fs_%1_a.txt").arg(tag)) == QCryptographicHash::hash("FA", QCryptographicHash::Sha256)
                       && sha256File(exDst + QStringLiteral("/fs_%1_b.txt").arg(tag)) == QCryptographicHash::hash("FB", QCryptographicHash::Sha256)
                       && !QFileInfo::exists(fA) && !QFileInfo::exists(fB)
                       && sha256File(fC) == QCryptographicHash::hash("FC", QCryptographicHash::Sha256),
                   "BM-N8: published files A & B preserved at dest without rollback; file C never started");
        }
    }

    // -------------------------------------------------------------------------
    // 4. Retry after 0 published & Undo safety: BM-N9, BM-N10
    // -------------------------------------------------------------------------
    {
        const QString rSrc = srcRoot + QStringLiteral("/rSrc");
        const QString rDst = dstRoot + QStringLiteral("/rDst");
        verify(QDir().mkpath(rSrc) && QDir().mkpath(rDst), "BM-N9/10 dirs");

        // BM-N9: FullRefusalBeforeExecution (0 published) allows Retry ONCE blocker removed
        const QString rf1 = rSrc + QStringLiteral("/r1.txt");
        const QString rf2 = rSrc + QStringLiteral("/r2.txt");
        const QString rd2 = rDst + QStringLiteral("/r2.txt");
        verify(writeFileBytes(rf1, "R1") && writeFileBytes(rf2, "R2") && writeFileBytes(rd2, "BLK"),
               "BM-N9: retry fixtures");
        {
            TransferQueueController qR;
            TransferTaskDescriptor d;
            d.title = QStringLiteral("BM-N9");
            d.type = TransferOperationType::Move;
            d.backend = TransferBackendKind::LocalNative;
            d.sources = {QUrl::fromLocalFile(rf1), QUrl::fromLocalFile(rf2)};
            d.destination = QUrl::fromLocalFile(rDst);
            d.startMode = TransferJobStartMode::ControllerStarts;
            d.jobFactory = [&]() -> KJob * {
                return new LocalBatchMoveJob({QUrl::fromLocalFile(rf1), QUrl::fromLocalFile(rf2)},
                                             QUrl::fromLocalFile(rDst), &qR);
            };
            const auto enq = qR.enqueue(std::move(d));
            verify(waitUntil([&] {
                const auto s = qR.taskSnapshot(enq.id);
                return s.has_value() && s->state == TransferTaskState::Failed;
            }), "BM-N9: preflight collision fails closed with 0 moved");
            verify(!qR.checkRetryEligibility(enq.id).eligible,
                   "BM-N9: Retry blocked while destination blocker exists");
            verify(QFile::remove(rd2), "BM-N9: blocker removed");
            verify(qR.checkRetryEligibility(enq.id).eligible,
                   "BM-N9: Retry eligible once blocker removed and publishedCount == 0");
            const auto retried = qR.retryTask(enq.id);
            verify(retried.accepted(), "BM-N9: retried task accepted");
            verify(waitUntil([&] {
                const auto s = qR.taskSnapshot(retried.id);
                return s.has_value() && s->state == TransferTaskState::Completed;
            }), "BM-N9: retried batch completes cleanly");
            verify(sha256File(rDst + QStringLiteral("/r1.txt")) == QCryptographicHash::hash("R1", QCryptographicHash::Sha256)
                       && sha256File(rd2) == QCryptographicHash::hash("R2", QCryptographicHash::Sha256),
                   "BM-N9: retried files moved accurately");
        }

        // BM-N10: Undo blocks on (a) collision at old source, (b) inode swap, (c) SAME-inode content edit + utimensat
        const QString u1 = rSrc + QStringLiteral("/u1.txt");
        const QString u2 = rSrc + QStringLiteral("/u2.txt");
        const QString ud1 = rDst + QStringLiteral("/u1.txt");
        const QString ud2 = rDst + QStringLiteral("/u2.txt");
        verify(writeFileBytes(u1, "UU1") && writeFileBytes(u2, "UU2"), "BM-N10: files");
        LocalMoveHistory histU;
        {
            LocalBatchMoveJob j({QUrl::fromLocalFile(u1), QUrl::fromLocalFile(u2)}, QUrl::fromLocalFile(rDst));
            j.setAutoDelete(false);
            histU.recordWhenCompleted(&j, 55);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }) && histU.canUndo(),
                   "BM-N10: batch recorded for Undo tests");
        }

        // (a) Collision in old source folder -> Undo preflight refuses completely (neither file moved back!)
        verify(writeFileBytes(u1, "NEW_AT_SRC"), "BM-N10a: collision at old source created");
        {
            KJob *uj = histU.undo();
            QSignalSpy spy(uj, &KJob::result);
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N10a: Undo finished");
            verify(uj->error() != KJob::NoError && QFileInfo::exists(ud1) && QFileInfo::exists(ud2)
                       && sha256File(u1) == QCryptographicHash::hash("NEW_AT_SRC", QCryptographicHash::Sha256)
                       && histU.canUndo(),
                   "BM-N10a: Undo collision refuses before moving ANY file and preserves all data");
            delete uj;
        }
        verify(QFile::remove(u1), "BM-N10a: collision cleared");

        // (c) SAME-INODE same-size content modification + mtime restoration attempt -> ctime changed, caught!
        {
            const struct stat beforeEdit = lstatEntry(ud2);
            const QByteArray fdBytes = QFile::encodeName(ud2);
            const int editFd = ::open(fdBytes.constData(), O_RDWR);
            verify(editFd >= 0 && ::pwrite(editFd, "XX2", 3, 0) == 3,
                   "BM-N10c: in-place same-size write on ud2");
            struct timespec ts[2] = {beforeEdit.st_atim, beforeEdit.st_mtim};
            ::futimens(editFd, ts); // try to preserve mtime while modifying content!
            ::close(editFd);
            const struct stat afterEdit = lstatEntry(ud2);
            verify(afterEdit.st_ino == beforeEdit.st_ino && afterEdit.st_size == beforeEdit.st_size,
                   "BM-N10c: inode and byte size stayed identical despite content edit");

            KJob *uj = histU.undo();
            QSignalSpy spy(uj, &KJob::result);
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N10c: Undo finished");
            verify(uj->error() != KJob::NoError && QFileInfo::exists(ud1) && QFileInfo::exists(ud2)
                       && !QFileInfo::exists(u1) && !QFileInfo::exists(u2),
                   "BM-N10c: same-inode content edit (even with restored mtime) detected via ctime & blocked by Undo preflight");
            delete uj;
        }

        // (b) Inode swap on ud1 before Undo -> caught by Undo preflight without touching ud2
        {
            const struct stat oldUd1 = lstatEntry(ud1);
            verify(QFile::remove(ud1) && writeFileBytes(ud1, "SWAPPED_INODE"),
                   "BM-N10b: replaced ud1 with new inode");
            const struct stat swappedUd1 = lstatEntry(ud1);
            verify(swappedUd1.st_ino != oldUd1.st_ino || swappedUd1.st_size != oldUd1.st_size,
                   "BM-N10b: replacement identity differs from recorded batch");
            KJob *uj = histU.undo();
            QSignalSpy spy(uj, &KJob::result);
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N10b: Undo finished");
            verify(uj->error() != KJob::NoError
                       && sha256File(ud1) == QCryptographicHash::hash("SWAPPED_INODE", QCryptographicHash::Sha256)
                       && sha256File(ud2) == QCryptographicHash::hash("XX2", QCryptographicHash::Sha256)
                       && !QFileInfo::exists(u1) && !QFileInfo::exists(u2),
                   "BM-N10b: inode swap before Undo detected in preflight without partial mutation");
            delete uj;
        }

        // (d) Mid-Undo interruption (after Undo moves 1 file back, 2nd fails) -> fences history & leaves no false success
        const QString m1 = rSrc + QStringLiteral("/m1u.txt");
        const QString m2 = rSrc + QStringLiteral("/m2u.txt");
        const QString md1 = rDst + QStringLiteral("/m1u.txt");
        const QString md2 = rDst + QStringLiteral("/m2u.txt");
        verify(writeFileBytes(m1, "M1") && writeFileBytes(m2, "M2"), "BM-N10d: files");
        LocalMoveHistory histMid;
        // Older valid move before batch
        const QString oldS = rSrc + QStringLiteral("/old.txt");
        const QString oldD = rDst + QStringLiteral("/old.txt");
        verify(writeFileBytes(oldS, "OLD"), "BM-N10d: older move source");
        {
            LocalFileMoveJob oj(QUrl::fromLocalFile(oldS), QUrl::fromLocalFile(oldD), nullptr, LocalFileMoveStrategy::Automatic);
            oj.setAutoDelete(false);
            histMid.recordWhenCompleted(&oj, 1);
            QSignalSpy spy(&oj, &KJob::result);
            oj.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N10d: older move done");
            LocalBatchMoveJob bj({QUrl::fromLocalFile(m1), QUrl::fromLocalFile(m2)}, QUrl::fromLocalFile(rDst));
            bj.setAutoDelete(false);
            histMid.recordWhenCompleted(&bj, 2);
            QSignalSpy bspy(&bj, &KJob::result);
            bj.start();
            verify(waitUntil([&] { return bspy.count() == 1; }) && histMid.count() == 2,
                   "BM-N10d: batch appended after older move");
        }
        simulateBatchMoveBeforeStep() = [&](int step) {
            if (step == 1) {
                writeFileBytes(m1, "UNDO_INTRUDER"); // note: Undo runs reverse [md2->m2, md1->m1], step 1 is m1!
            }
        };
        {
            KJob *uj = histMid.undo();
            QSignalSpy spy(uj, &KJob::result);
            verify(waitUntil([&] { return spy.count() == 1; }), "BM-N10d: mid-Undo abort finishes");
            resetBatchTestHooks();
            verify(uj->error() != KJob::NoError
                       && sha256File(m2) == QCryptographicHash::hash("M2", QCryptographicHash::Sha256)
                       && sha256File(m1) == QCryptographicHash::hash("UNDO_INTRUDER", QCryptographicHash::Sha256)
                       && sha256File(md1) == QCryptographicHash::hash("M1", QCryptographicHash::Sha256)
                       && !histMid.canUndo(),
                   "BM-N10d: mid-Undo collision stops cleanly, preserves already-undone file & blocks older Undo");
            delete uj;
        }
    }

    // -------------------------------------------------------------------------
    // 5. Lifecycle & Notification Sanity: BM-N11, BM-N12
    // -------------------------------------------------------------------------
    {
        const QString lcSrc = srcRoot + QStringLiteral("/lcSrc");
        const QString lcDst = dstRoot + QStringLiteral("/lcDst");
        verify(QDir().mkpath(lcSrc) && QDir().mkpath(lcDst), "BM-N11: dirs");
        const QString l1 = lcSrc + QStringLiteral("/l1.txt");
        const QString l2 = lcSrc + QStringLiteral("/l2.txt");
        verify(writeFileBytes(l1, "L1") && writeFileBytes(l2, "L2"), "BM-N11: files");

        // Shutdown during queue batch move + notification verification
        auto *q = new TransferQueueController;
        std::atomic<bool> gate{false};
        simulateBatchMoveBeforeStep() = [&](int step) {
            if (step == 1) {
                gate.store(true, std::memory_order_release);
                for (int k = 0; k < 200; ++k) {
                    QThread::msleep(1);
                }
            }
        };
        TransferTaskDescriptor d;
        d.title = QStringLiteral("BM-N11");
        d.type = TransferOperationType::Move;
        d.backend = TransferBackendKind::LocalNative;
        d.sources = {QUrl::fromLocalFile(l1), QUrl::fromLocalFile(l2)};
        d.destination = QUrl::fromLocalFile(lcDst);
        d.startMode = TransferJobStartMode::ControllerStarts;
        d.jobFactory = [&]() -> KJob * {
            return new LocalBatchMoveJob({QUrl::fromLocalFile(l1), QUrl::fromLocalFile(l2)},
                                         QUrl::fromLocalFile(lcDst), q);
        };
        q->enqueue(std::move(d));
        verify(waitUntil([&] { return gate.load(std::memory_order_acquire); }),
               "BM-N11: batch worker reached step 1 before shutdown");
        q->beginShutdown();
        verify(waitUntil([&] { return !q->hasNonTerminalTasks(); }),
               "BM-N11: beginShutdown cleanly terminated batch move without hang or UAF");
        delete q;
        resetBatchTestHooks();
        verify(sha256File(lcDst + QStringLiteral("/l1.txt")) == QCryptographicHash::hash("L1", QCryptographicHash::Sha256),
               "BM-N11: already published file 0 preserved across shutdown");

        // Notification manager single summary for batch move
        QWidget dummyWin;
        TransferQueueController qNotif;
        TransferNotificationManager notif(nullptr, &dummyWin);
        notif.setQueueController(&qNotif);
        notif.setEnabled(true);
        QList<NotificationPayload> payloads;
        notif.setDispatchBackend([&](const NotificationPayload &p) {
            payloads.append(p);
            return true;
        });
        const QString n1 = lcSrc + QStringLiteral("/n1.txt");
        const QString n2 = lcSrc + QStringLiteral("/n2.txt");
        verify(writeFileBytes(n1, "N1") && writeFileBytes(n2, "N2"), "Notification batch files");
        TransferTaskDescriptor nd;
        nd.title = QStringLiteral("Notif Batch");
        nd.type = TransferOperationType::Move;
        nd.backend = TransferBackendKind::LocalNative;
        nd.sources = {QUrl::fromLocalFile(n1), QUrl::fromLocalFile(n2)};
        nd.destination = QUrl::fromLocalFile(lcDst);
        nd.startMode = TransferJobStartMode::ControllerStarts;
        nd.jobFactory = [&]() -> KJob * {
            return new LocalBatchMoveJob({QUrl::fromLocalFile(n1), QUrl::fromLocalFile(n2)},
                                         QUrl::fromLocalFile(lcDst), &qNotif);
        };
        const auto enqN = qNotif.enqueue(std::move(nd));
        verify(waitUntil([&] {
            const auto s = qNotif.taskSnapshot(enqN.id);
            return s.has_value() && s->state == TransferTaskState::Completed;
        }), "Notification batch move completes");
        verify(waitUntil([&] { return payloads.size() == 1; }, 2000),
               "NotificationManager emits single coalesced summary for batch move");
        verify(payloads.first().kind == NotificationKind::Success && payloads.first().taskCount == 1,
               "Batch notification has Success kind and single taskCount");
    }

    resetBatchTestHooks();
    qInfo("PASS: %d LocalBatchMoveJob assertions", checks);
    return 0;
}
