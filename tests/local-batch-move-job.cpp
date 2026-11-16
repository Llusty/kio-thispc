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
    simulateBatchMoveExt4Filesystem() = false;
    simulateBatchMoveExt4StatxMntIdUniqueMissing() = false;
    simulateBatchMoveExt4FlagsError() = false;
    simulateBatchMoveExt4CasefoldAllDirs() = false;
    simulateBatchMoveExt4CasefoldInode() = 0;
    simulateBatchMoveExt4Encrypt() = false;
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

        for (const QString &bad : {symPath, fifoPath, sockPath}) {
            const auto pol = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(bad)},
                QUrl::fromLocalFile(pfDst), true);
            verify(!pol.allowed && pol.decision == MovePolicyDecision::RefusedLocalContainment,
                   "Preflight rejects batch containing symlink/FIFO/socket");
            verify(QFileInfo::exists(p1) && !QFileInfo::exists(pfDst + QStringLiteral("/a.txt")),
                   "Rejected special/symlink batch left zero changes");
        }
        {
            BatchMovePreflight regOnlyPf;
            verify(!LocalBatchMoveDetail::qualifyFastForPolicy(
                       {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(dirPath)},
                       QUrl::fromLocalFile(pfDst), &regOnlyPf),
                   "Regular-files-only preflight helper rejects directory");
            const auto polDir = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(p1), QUrl::fromLocalFile(dirPath)},
                QUrl::fromLocalFile(pfDst), true);
            verify(polDir.allowed && polDir.decision == MovePolicyDecision::AllowedBatchSameVolumeBtrfsMixed,
                   "H.2B.2B policy qualifies mixed file+dir batch on same Btrfs subvolume");
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

    // -------------------------------------------------------------------------
    // 6. Stage 5.3.2H.2B.2A: Mixed & Directory Batch Move Foundation
    // -------------------------------------------------------------------------
    {
        auto runMixedBatch = [](const QList<QUrl> &sources, const QUrl &dstDir) {
            auto *job = new LocalBatchMoveJob(sources, dstDir);
            job->setAllowDirectories(true);
            QEventLoop loop;
            QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
            job->start();
            loop.exec();
            return job;
        };

        auto isDirPresent = [](const QString &path) {
            QFileInfo fi(path);
            return fi.exists() && fi.isDir() && !fi.isSymLink();
        };
        auto inodeOf = [](const QString &path) -> quint64 {
            return static_cast<quint64>(lstatEntry(path).st_ino);
        };

        const QString h2b2aSrc = srcRoot + QStringLiteral("/h2b2a_src");
        const QString h2b2aSrcAlt = srcRoot + QStringLiteral("/h2b2a_src_alt");
        const QString h2b2aDst = dstRoot + QStringLiteral("/h2b2a_dst");
        verify(QDir().mkpath(h2b2aSrc) && QDir().mkpath(h2b2aSrcAlt) && QDir().mkpath(h2b2aDst),
               "H2B2A: base dirs created");

        // P1: Two directories from one source parent (with multi-level tree, Unicode, spaces, preserved inodes & SHA-256)
        {
            const QString d1 = h2b2aSrc + QStringLiteral("/katalog zażółć spacja");
            const QString d2 = h2b2aSrc + QStringLiteral("/.ukryty_dir");
            const QString d1Deep = d1 + QStringLiteral("/sub1/sub2");
            verify(QDir().mkpath(d1Deep) && QDir().mkpath(d2), "H2B2A-P1: mkpath");
            const QString f1 = d1 + QStringLiteral("/root.txt");
            const QString f2 = d1Deep + QStringLiteral("/głęboki plik.bin");
            const QString f3 = d2 + QStringLiteral("/hidden.dat");
            verify(writeFileBytes(f1, "R1") && writeFileBytes(f2, "DEEP_DATA_12345") && writeFileBytes(f3, "HID"),
                   "H2B2A-P1: files");
            const quint64 inoD1 = inodeOf(d1);
            const quint64 inoD2 = inodeOf(d2);
            const quint64 inoF2 = inodeOf(f2);
            const QByteArray shaF2 = sha256File(f2);

            auto *job = runMixedBatch({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                                      QUrl::fromLocalFile(h2b2aDst));
            const BatchMoveResult res = job->batchResult();
            verify(job->error() == KJob::NoError
                       && res.outcome == BatchMoveOutcome::FullSuccess
                       && res.totalCount == 2 && res.publishedCount == 2 && res.durableCount == 2
                       && res.items.size() == 2
                       && res.items.at(0).kind == BatchMoveItemKind::Directory
                       && res.items.at(1).kind == BatchMoveItemKind::Directory
                       && res.items.at(0).movedDirSnapshot.scannedRegularFiles == 2
                       && res.items.at(0).movedDirSnapshot.scannedSubdirectories == 2
                       && res.items.at(0).movedDirSnapshot.scannedBytes == 17,
                   "H2B2A-P1: two directories moved atomically with accurate tree snapshot");
            const QString dstD1 = h2b2aDst + QStringLiteral("/katalog zażółć spacja");
            const QString dstD2 = h2b2aDst + QStringLiteral("/.ukryty_dir");
            verify(!QFileInfo::exists(d1) && !QFileInfo::exists(d2)
                       && isDirPresent(dstD1) && isDirPresent(dstD2)
                       && inodeOf(dstD1) == inoD1 && inodeOf(dstD2) == inoD2
                       && inodeOf(dstD1 + QStringLiteral("/sub1/sub2/głęboki plik.bin")) == inoF2
                       && sha256File(dstD1 + QStringLiteral("/sub1/sub2/głęboki plik.bin")) == shaF2,
                   "H2B2A-P1: inodes and SHA-256 preserved, no staging");
            delete job;
        }

        // P2: Two directories from different source parents
        {
            const QString dA = h2b2aSrc + QStringLiteral("/diffParentA");
            const QString dB = h2b2aSrcAlt + QStringLiteral("/diffParentB");
            verify(QDir().mkpath(dA) && QDir().mkpath(dB), "H2B2A-P2: dirs");
            verify(writeFileBytes(dA + QStringLiteral("/a.txt"), "AAA")
                       && writeFileBytes(dB + QStringLiteral("/b.txt"), "BBBB"),
                   "H2B2A-P2: files");
            const quint64 inoA = inodeOf(dA);
            const quint64 inoB = inodeOf(dB);
            auto *job = runMixedBatch({QUrl::fromLocalFile(dA), QUrl::fromLocalFile(dB)},
                                      QUrl::fromLocalFile(h2b2aDst));
            verify(job->error() == KJob::NoError
                       && job->outcome() == BatchMoveOutcome::FullSuccess
                       && inodeOf(h2b2aDst + QStringLiteral("/diffParentA")) == inoA
                       && inodeOf(h2b2aDst + QStringLiteral("/diffParentB")) == inoB,
                   "H2B2A-P2: two directories from different source parents moved");
            delete job;
        }

        // P3: Mixed batch: 1 file + 1 directory
        {
            const QString mf = h2b2aSrc + QStringLiteral("/mix_file.txt");
            const QString md = h2b2aSrc + QStringLiteral("/mix_dir");
            verify(QDir().mkpath(md), "H2B2A-P3: dir");
            verify(writeFileBytes(mf, "MIX_FILE") && writeFileBytes(md + QStringLiteral("/in.txt"), "IN_DIR"),
                   "H2B2A-P3: files");
            const quint64 inoMf = inodeOf(mf);
            const quint64 inoMd = inodeOf(md);
            auto *job = runMixedBatch({QUrl::fromLocalFile(mf), QUrl::fromLocalFile(md)},
                                      QUrl::fromLocalFile(h2b2aDst));
            const BatchMoveResult res = job->batchResult();
            verify(job->error() == KJob::NoError
                       && res.outcome == BatchMoveOutcome::FullSuccess
                       && res.items.at(0).kind == BatchMoveItemKind::RegularFile
                       && res.items.at(1).kind == BatchMoveItemKind::Directory
                       && inodeOf(h2b2aDst + QStringLiteral("/mix_file.txt")) == inoMf
                       && inodeOf(h2b2aDst + QStringLiteral("/mix_dir")) == inoMd,
                   "H2B2A-P3: mixed file + directory batch moved cleanly");
            delete job;
        }

        // P4: Mixed batch: 2 files + 1 directory from different parents, total Bytes checked
        {
            const QString f1 = h2b2aSrc + QStringLiteral("/p4_f1.txt");
            const QString f2 = h2b2aSrcAlt + QStringLiteral("/p4_f2.txt");
            const QString d3 = h2b2aSrc + QStringLiteral("/p4_dir");
            verify(QDir().mkpath(d3 + QStringLiteral("/sub")), "H2B2A-P4: dir");
            verify(writeFileBytes(f1, "12345")
                       && writeFileBytes(f2, "123456")
                       && writeFileBytes(d3 + QStringLiteral("/sub/x.txt"), "1234567"),
                   "H2B2A-P4: files");
            auto *job = runMixedBatch(
                {QUrl::fromLocalFile(f1), QUrl::fromLocalFile(f2), QUrl::fromLocalFile(d3)},
                QUrl::fromLocalFile(h2b2aDst));
            verify(job->error() == KJob::NoError
                       && job->totalCount() == 3 && job->durableCount() == 3
                       && job->totalAmount(KJob::Bytes) == 18
                       && job->processedAmount(KJob::Bytes) == 18,
                   "H2B2A-P4: 2 files + 1 directory batch moved with exact total byte accounting");
            delete job;
        }

        // N1: Destination collision in mixed batch -> FullRefusalBeforeExecution (0 moved)
        {
            const QString nf = h2b2aSrc + QStringLiteral("/n1_f.txt");
            const QString nd = h2b2aSrc + QStringLiteral("/n1_dir");
            const QString existingDstDir = h2b2aDst + QStringLiteral("/n1_dir");
            verify(QDir().mkpath(nd) && QDir().mkpath(existingDstDir), "H2B2A-N1: dirs");
            verify(writeFileBytes(nf, "N1F") && writeFileBytes(nd + QStringLiteral("/x.txt"), "N1D"), "H2B2A-N1: files");
            auto *job = runMixedBatch({QUrl::fromLocalFile(nf), QUrl::fromLocalFile(nd)},
                                      QUrl::fromLocalFile(h2b2aDst));
            verify(job->error() != KJob::NoError
                       && job->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && job->batchError() == BatchMoveError::DestinationExists
                       && job->publishedCount() == 0
                       && QFileInfo::exists(nf) && isDirPresent(nd),
                   "H2B2A-N1: destination collision refuses entire mixed batch before moving file 0");
            delete job;
        }

        // N2: Nested sources (dir + child dir, and dir + child file) -> FullRefusalBeforeExecution
        {
            const QString parentD = h2b2aSrc + QStringLiteral("/n2_parent");
            const QString childD = parentD + QStringLiteral("/child_dir");
            const QString childF = parentD + QStringLiteral("/child_file.txt");
            verify(QDir().mkpath(childD) && writeFileBytes(childF, "CF"), "H2B2A-N2: fixtures");

            auto *job1 = runMixedBatch({QUrl::fromLocalFile(parentD), QUrl::fromLocalFile(childD)},
                                       QUrl::fromLocalFile(h2b2aDst));
            verify(job1->error() != KJob::NoError
                       && job1->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && job1->batchError() == BatchMoveError::CollisionInBatch
                       && isDirPresent(parentD) && isDirPresent(childD),
                   "H2B2A-N2a: nested directories in batch refused fail-closed");
            delete job1;

            auto *job2 = runMixedBatch({QUrl::fromLocalFile(parentD), QUrl::fromLocalFile(childF)},
                                       QUrl::fromLocalFile(h2b2aDst));
            verify(job2->error() != KJob::NoError
                       && job2->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && job2->batchError() == BatchMoveError::CollisionInBatch
                       && isDirPresent(parentD) && QFileInfo::exists(childF),
                   "H2B2A-N2b: directory + descendant file in batch refused fail-closed");
            delete job2;
        }

        // N3: Source directory as ancestor of destination -> CycleDetected
        {
            const QString cycA = h2b2aSrc + QStringLiteral("/n3_cycA");
            const QString cycB = h2b2aSrc + QStringLiteral("/n3_cycB");
            const QString innerDst = cycA + QStringLiteral("/inner_dst");
            verify(QDir().mkpath(innerDst) && QDir().mkpath(cycB), "H2B2A-N3: dirs");
            auto *job = runMixedBatch({QUrl::fromLocalFile(cycA), QUrl::fromLocalFile(cycB)},
                                      QUrl::fromLocalFile(innerDst));
            verify(job->error() != KJob::NoError
                       && job->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && job->batchError() == BatchMoveError::CycleDetected
                       && job->publishedCount() == 0
                       && isDirPresent(cycA) && isDirPresent(cycB),
                   "H2B2A-N3: destination inside source directory refused as CycleDetected");
            delete job;
        }

        // N4: Symlink in deep subdirectory -> ContainmentViolation, 0 moved
        {
            const QString okFile = h2b2aSrc + QStringLiteral("/n4_ok.txt");
            const QString symDir = h2b2aSrc + QStringLiteral("/n4_symdir");
            verify(QDir().mkpath(symDir + QStringLiteral("/a/b")) && writeFileBytes(okFile, "OK4"),
                   "H2B2A-N4: fixtures");
            verify(::symlink("/etc/hosts", QFile::encodeName(symDir + QStringLiteral("/a/b/bad.lnk")).constData()) == 0,
                   "H2B2A-N4: symlink created");
            auto *job = runMixedBatch({QUrl::fromLocalFile(okFile), QUrl::fromLocalFile(symDir)},
                                      QUrl::fromLocalFile(h2b2aDst));
            verify(job->error() != KJob::NoError
                       && job->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && job->batchError() == BatchMoveError::ContainmentViolation
                       && job->publishedCount() == 0 && QFileInfo::exists(okFile),
                   "H2B2A-N4: deep symlink in directory refuses entire mixed batch");
            delete job;
        }

        // N5: Hardlink inside directory -> ContainmentViolation, 0 moved
        {
            const QString okFile = h2b2aSrc + QStringLiteral("/n5_ok.txt");
            const QString hlDir = h2b2aSrc + QStringLiteral("/n5_hldir");
            const QString hlOrig = h2b2aSrcAlt + QStringLiteral("/n5_orig.txt");
            verify(QDir().mkpath(hlDir) && writeFileBytes(okFile, "OK5") && writeFileBytes(hlOrig, "HL"),
                   "H2B2A-N5: fixtures");
            verify(::link(QFile::encodeName(hlOrig).constData(),
                          QFile::encodeName(hlDir + QStringLiteral("/hard.txt")).constData()) == 0,
                   "H2B2A-N5: hardlink created");
            auto *job = runMixedBatch({QUrl::fromLocalFile(okFile), QUrl::fromLocalFile(hlDir)},
                                      QUrl::fromLocalFile(h2b2aDst));
            verify(job->error() != KJob::NoError
                       && job->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && job->batchError() == BatchMoveError::ContainmentViolation
                       && job->publishedCount() == 0 && QFileInfo::exists(okFile),
                   "H2B2A-N5: hardlinked file inside directory refuses mixed batch");
            delete job;
        }

        // N6: FIFO and Unix socket inside directory -> ContainmentViolation
        {
            const QString okFile = h2b2aSrc + QStringLiteral("/n6_ok.txt");
            const QString fifoDir = h2b2aSrc + QStringLiteral("/n6_fifodir");
            const QString sockDir = h2b2aSrc + QStringLiteral("/n6_sockdir");
            verify(QDir().mkpath(fifoDir) && QDir().mkpath(sockDir) && writeFileBytes(okFile, "OK6"),
                   "H2B2A-N6: dirs");
            verify(::mkfifo(QFile::encodeName(fifoDir + QStringLiteral("/pipe")).constData(), 0600) == 0,
                   "H2B2A-N6: fifo created");
            auto *jobFifo = runMixedBatch({QUrl::fromLocalFile(okFile), QUrl::fromLocalFile(fifoDir)},
                                          QUrl::fromLocalFile(h2b2aDst));
            verify(jobFifo->error() != KJob::NoError
                       && jobFifo->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && jobFifo->batchError() == BatchMoveError::ContainmentViolation
                       && QFileInfo::exists(okFile),
                   "H2B2A-N6a: FIFO inside directory refuses mixed batch");
            delete jobFifo;

            const QString prevCwd = QDir::currentPath();
            verify(QDir::setCurrent(sockDir), "H2B2A-N6: chdir");
            const int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un addr {};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, "test.sock", sizeof(addr.sun_path) - 1);
            const bool bound = (sfd >= 0)
                && (::bind(sfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0);
            if (sfd >= 0) ::close(sfd);
            QDir::setCurrent(prevCwd);
            verify(bound, "H2B2A-N6: socket bound");
            auto *jobSock = runMixedBatch({QUrl::fromLocalFile(okFile), QUrl::fromLocalFile(sockDir)},
                                          QUrl::fromLocalFile(h2b2aDst));
            verify(jobSock->error() != KJob::NoError
                       && jobSock->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && jobSock->batchError() == BatchMoveError::ContainmentViolation
                       && QFileInfo::exists(okFile),
                   "H2B2A-N6b: Unix socket inside directory refuses mixed batch");
            delete jobSock;
        }

        // N7: Mount point / EXDEV boundary / non-Btrfs filesystem -> MountOrSubvolumeBoundary / UnsupportedFilesystem
        {
            const QString d1 = h2b2aSrc + QStringLiteral("/n7_d1");
            const QString d2 = h2b2aSrc + QStringLiteral("/n7_d2");
            verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2A-N7: dirs");

            simulateBatchMoveUnsupportedFsMagic() = MSDOS_SUPER_MAGIC;
            auto *jobFs = runMixedBatch({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                                        QUrl::fromLocalFile(h2b2aDst));
            verify(jobFs->error() != KJob::NoError
                       && jobFs->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && jobFs->batchError() == BatchMoveError::UnsupportedFilesystem,
                   "H2B2A-N7a: non-Btrfs filesystem refuses directory batch");
            delete jobFs;
            resetBatchTestHooks();

            simulateBatchMoveExdev() = true;
            auto *jobExdev = runMixedBatch({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                                           QUrl::fromLocalFile(h2b2aDst));
            verify(jobExdev->error() != KJob::NoError
                       && jobExdev->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && jobExdev->batchError() == BatchMoveError::MountOrSubvolumeBoundary,
                   "H2B2A-N7b: EXDEV subvolume/mount boundary refuses directory batch");
            delete jobExdev;
            resetBatchTestHooks();

            // Active mount point from /proc/self/mountinfo
            QString activeMount;
            QFile mi(QStringLiteral("/proc/self/mountinfo"));
            if (mi.open(QIODevice::ReadOnly | QIODevice::Text)) {
                while (!mi.atEnd()) {
                    const QByteArray line = mi.readLine().trimmed();
                    const QList<QByteArray> parts = line.split(' ');
                    if (parts.size() >= 5 && parts.at(4) != "/") {
                        const QString mp = QFile::decodeName(parts.at(4));
                        if (QFileInfo::exists(mp) && QFileInfo(mp).isDir()) {
                            activeMount = mp;
                            break;
                        }
                    }
                }
            }
            if (!activeMount.isEmpty()) {
                auto *jobMp = runMixedBatch({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(activeMount)},
                                            QUrl::fromLocalFile(h2b2aDst));
                verify(jobMp->error() != KJob::NoError
                           && jobMp->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution,
                       "H2B2A-N7c: active mount point refused fail-closed");
                delete jobMp;
            }
        }

        // N8: Directory replaced after Phase 2 Preflight -> PartialSuccessCleanStop, item 0 preserved
        {
            const QString d1 = h2b2aSrc + QStringLiteral("/n8_d1");
            const QString d2 = h2b2aSrc + QStringLiteral("/n8_d2");
            verify(QDir().mkpath(d1) && QDir().mkpath(d2)
                       && writeFileBytes(d1 + QStringLiteral("/a.txt"), "N8A"),
                   "H2B2A-N8: dirs");
            simulateBatchMoveBeforeStep() = [&](int step) {
                if (step == 1) {
                    QDir().rmdir(d2);
                    QDir().mkdir(d2);
                }
            };
            auto *job = runMixedBatch({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                                      QUrl::fromLocalFile(h2b2aDst));
            resetBatchTestHooks();
            const BatchMoveResult res = job->batchResult();
            verify(job->error() != KJob::NoError
                       && res.outcome == BatchMoveOutcome::PartialSuccessCleanStop
                       && res.publishedCount == 1 && res.durableCount == 1
                       && res.items.at(0).state == BatchMoveItemState::DurableCompleted
                       && res.items.at(1).state == BatchMoveItemState::FailedBeforePublish
                       && isDirPresent(h2b2aDst + QStringLiteral("/n8_d1")),
                   "H2B2A-N8: directory replaced after Preflight stops step 1 cleanly without rolling back step 0");
            delete job;
        }

        // N9: New symlink inserted inside dir 2 tree before step 1 rename -> revalidation catches it!
        {
            const QString d1 = h2b2aSrc + QStringLiteral("/n9_d1");
            const QString d2 = h2b2aSrc + QStringLiteral("/n9_d2");
            verify(QDir().mkpath(d1) && QDir().mkpath(d2 + QStringLiteral("/sub"))
                       && writeFileBytes(d1 + QStringLiteral("/a.txt"), "N9A"),
                   "H2B2A-N9: dirs");
            simulateBatchMoveBeforeStep() = [&](int step) {
                if (step == 1) {
                    ::symlink("/etc/passwd",
                              QFile::encodeName(d2 + QStringLiteral("/sub/injected.lnk")).constData());
                }
            };
            auto *job = runMixedBatch({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                                      QUrl::fromLocalFile(h2b2aDst));
            resetBatchTestHooks();
            const BatchMoveResult res = job->batchResult();
            verify(job->error() != KJob::NoError
                       && res.outcome == BatchMoveOutcome::PartialSuccessCleanStop
                       && res.publishedCount == 1 && res.durableCount == 1
                       && res.items.at(1).state == BatchMoveItemState::FailedBeforePublish
                       && res.items.at(1).error == BatchMoveError::ContainmentViolation
                       && isDirPresent(h2b2aDst + QStringLiteral("/n9_d1"))
                       && isDirPresent(d2),
                   "H2B2A-N9: mid-batch symlink injection caught by re-inspection before step 1 rename");
            delete job;
        }

        // N10: Collision on step 1 during execution -> PartialSuccessCleanStop
        {
            const QString d1 = h2b2aSrc + QStringLiteral("/n10_d1");
            const QString d2 = h2b2aSrc + QStringLiteral("/n10_d2");
            verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2A-N10: dirs");
            simulateBatchMoveBeforeStep() = [&](int step) {
                if (step == 1) {
                    QDir().mkdir(h2b2aDst + QStringLiteral("/n10_d2"));
                }
            };
            auto *job = runMixedBatch({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                                      QUrl::fromLocalFile(h2b2aDst));
            resetBatchTestHooks();
            verify(job->error() != KJob::NoError
                       && job->outcome() == BatchMoveOutcome::PartialSuccessCleanStop
                       && job->publishedCount() == 1
                       && job->batchResult().items.at(1).error == BatchMoveError::DestinationExists
                       && isDirPresent(h2b2aDst + QStringLiteral("/n10_d1"))
                       && isDirPresent(d2),
                   "H2B2A-N10: mid-execution collision on dir 2 stops cleanly without rollback");
            delete job;
        }

        // N11: fsync error on directory step -> PartialOutcomeWithUnconfirmedDurability + Retry blocked
        {
            const QString d1 = h2b2aSrc + QStringLiteral("/n11_d1");
            const QString d2 = h2b2aSrc + QStringLiteral("/n11_d2");
            verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2A-N11: dirs");
            TransferQueueController q;
            simulateBatchMoveDestinationFsyncErrorAtStep() = {1, EIO};
            TransferTaskDescriptor desc;
            desc.title = QStringLiteral("H2B2A-N11");
            desc.type = TransferOperationType::Move;
            desc.backend = TransferBackendKind::LocalNative;
            desc.sources = {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)};
            desc.destination = QUrl::fromLocalFile(h2b2aDst);
            desc.startMode = TransferJobStartMode::ControllerStarts;
            desc.jobFactory = [&]() -> KJob * {
                auto *j = new LocalBatchMoveJob(
                    {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                    QUrl::fromLocalFile(h2b2aDst), &q);
                j->setAllowDirectories(true);
                return j;
            };
            const auto enq = q.enqueue(std::move(desc));
            verify(waitUntil([&] {
                const auto s = q.taskSnapshot(enq.id);
                return s.has_value() && s->state == TransferTaskState::Failed;
            }), "H2B2A-N11: fsync error fails queue task");
            resetBatchTestHooks();
            const auto snap = q.taskSnapshot(enq.id);
            verify(snap.has_value()
                       && snap->state == TransferTaskState::Failed
                       && !q.checkRetryEligibility(enq.id).eligible
                       && isDirPresent(h2b2aDst + QStringLiteral("/n11_d1"))
                       && isDirPresent(h2b2aDst + QStringLiteral("/n11_d2")),
                   "H2B2A-N11: fsync failure marks partial outcome, preserves published dirs, blocks Retry");
        }

        // N12: Cancellation after publishing 1st directory -> PartialSuccessCleanStop + Retry blocked
        {
            const QString d1 = h2b2aSrc + QStringLiteral("/n12_d1");
            const QString d2 = h2b2aSrc + QStringLiteral("/n12_d2");
            verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2A-N12: dirs");
            TransferQueueController q;
            LocalBatchMoveJob *activeJob = nullptr;
            simulateBatchMoveBeforeStep() = [&](int step) {
                if (step == 1 && activeJob) {
                    activeJob->kill(KJob::EmitResult);
                }
            };
            TransferTaskDescriptor desc;
            desc.title = QStringLiteral("H2B2A-N12");
            desc.type = TransferOperationType::Move;
            desc.backend = TransferBackendKind::LocalNative;
            desc.sources = {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)};
            desc.destination = QUrl::fromLocalFile(h2b2aDst);
            desc.startMode = TransferJobStartMode::ControllerStarts;
            desc.jobFactory = [&]() -> KJob * {
                activeJob = new LocalBatchMoveJob(
                    {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                    QUrl::fromLocalFile(h2b2aDst), &q);
                activeJob->setAllowDirectories(true);
                return activeJob;
            };
            const auto enq = q.enqueue(std::move(desc));
            verify(waitUntil([&] {
                const auto s = q.taskSnapshot(enq.id);
                return s.has_value() && isTerminalTransferState(s->state);
            }), "H2B2A-N12: cancelled task reaches terminal state");
            resetBatchTestHooks();
            const auto snap = q.taskSnapshot(enq.id);
            verify(snap.has_value()
                       && snap->state == TransferTaskState::Cancelled
                       && !q.checkRetryEligibility(enq.id).eligible
                       && isDirPresent(h2b2aDst + QStringLiteral("/n12_d1"))
                       && isDirPresent(d2),
                   "H2B2A-N12: cancel after dir 0 preserves dir 0, leaves dir 1, blocks Retry");
        }

        // N13: Cancellation after rename of 2nd dir before fsync -> fsync STILL runs and completes durability
        {
            const QString d1 = h2b2aSrc + QStringLiteral("/n13_d1");
            const QString d2 = h2b2aSrc + QStringLiteral("/n13_d2");
            verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2A-N13: dirs");
            auto *job = new LocalBatchMoveJob(
                {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                QUrl::fromLocalFile(h2b2aDst));
            job->setAllowDirectories(true);
            QThread *killerThread = nullptr;
            simulateBatchMoveAfterRenameBeforeFsync() = [&](int step) {
                if (step == 1) {
                    killerThread = QThread::create([job] {
                        QMetaObject::invokeMethod(job, [job] { job->kill(KJob::EmitResult); }, Qt::QueuedConnection);
                    });
                    killerThread->start();
                    QThread::msleep(15);
                }
            };
            QEventLoop loop;
            QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
            job->start();
            loop.exec();
            if (killerThread) {
                killerThread->wait();
                delete killerThread;
            }
            resetBatchTestHooks();
            verify(job->publishedCount() == 2 && job->durableCount() == 2
                       && isDirPresent(h2b2aDst + QStringLiteral("/n13_d1"))
                       && isDirPresent(h2b2aDst + QStringLiteral("/n13_d2")),
                   "H2B2A-N13: cancel after 2nd rename still completes fsync durability gate");
            delete job;
        }

        // GUI / Production Policy Containment: MoveSafetyPolicy & default LocalBatchMoveJob still block dir/mixed batches
        {
            const QString g1 = h2b2aSrc + QStringLiteral("/gui_d1");
            const QString g2 = h2b2aSrc + QStringLiteral("/gui_d2");
            const QString gf = h2b2aSrc + QStringLiteral("/gui_f1.txt");
            verify(QDir().mkpath(g1) && QDir().mkpath(g2) && writeFileBytes(gf, "GF"), "H2B2A-GUI: fixtures");

            const auto evalTwoDirs = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(g1), QUrl::fromLocalFile(g2)},
                QUrl::fromLocalFile(h2b2aDst),
                true);
            verify(evalTwoDirs.allowed
                       && evalTwoDirs.decision == MovePolicyDecision::AllowedBatchSameVolumeBtrfsMixed,
                   "H2B2B-GUI1: MoveSafetyPolicy::evaluate() allows qualified 2-directory Btrfs batch move");

            const auto evalMixed = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(gf), QUrl::fromLocalFile(g1)},
                QUrl::fromLocalFile(h2b2aDst),
                true);
            verify(evalMixed.allowed
                       && evalMixed.decision == MovePolicyDecision::AllowedBatchSameVolumeBtrfsMixed,
                   "H2B2B-GUI2: MoveSafetyPolicy::evaluate() allows qualified mixed file+dir Btrfs batch move");

            // Default LocalBatchMoveJob (allowDirectories == false) still fails closed without explicit opt-in
            auto *defaultJob = new LocalBatchMoveJob(
                {QUrl::fromLocalFile(gf), QUrl::fromLocalFile(g1)},
                QUrl::fromLocalFile(h2b2aDst));
            QEventLoop loop;
            QObject::connect(defaultJob, &KJob::result, &loop, &QEventLoop::quit);
            defaultJob->start();
            loop.exec();
            verify(defaultJob->error() != KJob::NoError
                       && defaultJob->outcome() == BatchMoveOutcome::FullRefusalBeforeExecution
                       && QFileInfo::exists(gf) && isDirPresent(g1),
                   "H2B2A-GUI3: default LocalBatchMoveJob without setAllowDirectories(true) fails closed");
            delete defaultJob;
        }

        // H.2B.2B Undo/Redo, Partial Undo Fence, Directory Identity & Policy Tests
        {
            const QString h2b2bSrc = srcRoot + QStringLiteral("/h2b2b_src");
            const QString h2b2bDst = dstRoot + QStringLiteral("/h2b2b_dst");
            verify(QDir().mkpath(h2b2bSrc) && QDir().mkpath(h2b2bDst), "H2B2B: base dirs");

            // UR1: Mixed batch (2 files + 2 multi-level dirs with Unicode/spaces) -> compound Undo & Redo
            {
                const QString f1 = h2b2bSrc + QStringLiteral("/ur1 plik.txt");
                const QString f2 = h2b2bSrc + QStringLiteral("/.ur1_ukryty");
                const QString d1 = h2b2bSrc + QStringLiteral("/ur1 katalog zażółć");
                const QString d2 = h2b2bSrc + QStringLiteral("/ur1_dir2");
                verify(QDir().mkpath(d1 + QStringLiteral("/nested/deep")) && QDir().mkpath(d2), "H2B2B-UR1: dirs");
                const QString df1 = d1 + QStringLiteral("/nested/deep/inner.bin");
                const QString df2 = d2 + QStringLiteral("/d2.txt");
                verify(writeFileBytes(f1, "UR1_F1")
                           && writeFileBytes(f2, "UR1_F2")
                           && writeFileBytes(df1, "UR1_DEEP_DATA")
                           && writeFileBytes(df2, "UR1_D2"),
                       "H2B2B-UR1: files");
                const quint64 inoF1 = inodeOf(f1);
                const quint64 inoF2 = inodeOf(f2);
                const quint64 inoD1 = inodeOf(d1);
                const quint64 inoD2 = inodeOf(d2);
                const quint64 inoDf1 = inodeOf(df1);
                const QByteArray shaDf1 = sha256File(df1);

                LocalMoveHistory hist;
                auto *job = new LocalBatchMoveJob(
                    {QUrl::fromLocalFile(f1), QUrl::fromLocalFile(d1),
                     QUrl::fromLocalFile(f2), QUrl::fromLocalFile(d2)},
                    QUrl::fromLocalFile(h2b2bDst));
                job->setAllowDirectories(true);
                hist.recordWhenCompleted(job, 1);
                QEventLoop loop;
                QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
                job->start();
                loop.exec();
                verify(job->error() == KJob::NoError
                           && job->outcome() == BatchMoveOutcome::FullSuccess
                           && hist.canUndo() && !hist.canRedo(),
                       "H2B2B-UR1: mixed batch Move succeeds and records compound Undo");
                delete job;

                const QString dstF1 = h2b2bDst + QStringLiteral("/ur1 plik.txt");
                const QString dstF2 = h2b2bDst + QStringLiteral("/.ur1_ukryty");
                const QString dstD1 = h2b2bDst + QStringLiteral("/ur1 katalog zażółć");
                const QString dstD2 = h2b2bDst + QStringLiteral("/ur1_dir2");
                const QString dstDf1 = dstD1 + QStringLiteral("/nested/deep/inner.bin");
                verify(inodeOf(dstF1) == inoF1 && inodeOf(dstF2) == inoF2
                           && inodeOf(dstD1) == inoD1 && inodeOf(dstD2) == inoD2
                           && inodeOf(dstDf1) == inoDf1
                           && sha256File(dstDf1) == shaDf1,
                       "H2B2B-UR1: Move preserved inodes and SHA-256");

                // Compound Undo (Ctrl+Z)
                KJob *undoJob = hist.undo();
                verify(undoJob != nullptr, "H2B2B-UR1: undo job created");
                {
                    QEventLoop uLoop;
                    QObject::connect(undoJob, &KJob::result, &uLoop, &QEventLoop::quit);
                    undoJob->exec();
                }
                verify(undoJob->error() == KJob::NoError
                           && !hist.canUndo() && hist.canRedo()
                           && !QFileInfo::exists(dstF1) && !QFileInfo::exists(dstD1)
                           && inodeOf(f1) == inoF1 && inodeOf(f2) == inoF2
                           && inodeOf(d1) == inoD1 && inodeOf(d2) == inoD2
                           && inodeOf(df1) == inoDf1
                           && sha256File(df1) == shaDf1,
                       "H2B2B-UR1: compound Undo restored all 4 items with preserved inodes and SHA-256");
                delete undoJob;

                // Compound Redo (Ctrl+Y)
                KJob *redoJob = hist.redo();
                verify(redoJob != nullptr, "H2B2B-UR1: redo job created");
                {
                    QEventLoop rLoop;
                    QObject::connect(redoJob, &KJob::result, &rLoop, &QEventLoop::quit);
                    redoJob->exec();
                }
                verify(redoJob->error() == KJob::NoError
                           && hist.canUndo() && !hist.canRedo()
                           && !QFileInfo::exists(f1) && !QFileInfo::exists(d1)
                           && inodeOf(dstF1) == inoF1 && inodeOf(dstF2) == inoF2
                           && inodeOf(dstD1) == inoD1 && inodeOf(dstD2) == inoD2
                           && inodeOf(dstDf1) == inoDf1
                           && sha256File(dstDf1) == shaDf1,
                       "H2B2B-UR1: compound Redo re-moved all 4 items with preserved inodes and SHA-256");
                delete redoJob;
            }

            // UR2: Collision at original source blocks Undo in Preflight before any mutation
            {
                const QString d1 = h2b2bSrc + QStringLiteral("/ur2_d1");
                const QString d2 = h2b2bSrc + QStringLiteral("/ur2_d2");
                verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2B-UR2: dirs");
                LocalMoveHistory hist;
                auto *job = new LocalBatchMoveJob(
                    {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                    QUrl::fromLocalFile(h2b2bDst));
                job->setAllowDirectories(true);
                hist.recordWhenCompleted(job, 1);
                QEventLoop loop;
                QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
                job->start();
                loop.exec();
                verify(job->error() == KJob::NoError && hist.canUndo(), "H2B2B-UR2: initial Move ok");
                delete job;

                verify(QDir().mkpath(d1), "H2B2B-UR2: create collision at source d1");
                KJob *undoJob = hist.undo();
                verify(undoJob != nullptr, "H2B2B-UR2: undo job created");
                undoJob->exec();
                verify(undoJob->error() != KJob::NoError
                           && isDirPresent(h2b2bDst + QStringLiteral("/ur2_d1"))
                           && isDirPresent(h2b2bDst + QStringLiteral("/ur2_d2"))
                           && !isDirPresent(d2),
                       "H2B2B-UR2: Undo preflight collision blocks entire Undo before moving d2");
                delete undoJob;
            }

            // UR3: Directory modified between Move and Undo -> blocked in Preflight
            {
                const QString d1 = h2b2bSrc + QStringLiteral("/ur3_d1");
                const QString d2 = h2b2bSrc + QStringLiteral("/ur3_d2");
                verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2B-UR3: dirs");
                LocalMoveHistory hist;
                auto *job = new LocalBatchMoveJob(
                    {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                    QUrl::fromLocalFile(h2b2bDst));
                job->setAllowDirectories(true);
                hist.recordWhenCompleted(job, 1);
                QEventLoop loop;
                QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
                job->start();
                loop.exec();
                verify(job->error() == KJob::NoError && hist.canUndo(), "H2B2B-UR3: initial Move ok");
                delete job;

                verify(writeFileBytes(h2b2bDst + QStringLiteral("/ur3_d1/tamper.txt"), "X"),
                       "H2B2B-UR3: modify moved dir");
                KJob *undoJob = hist.undo();
                verify(undoJob != nullptr, "H2B2B-UR3: undo job created");
                undoJob->exec();
                verify(undoJob->error() != KJob::NoError
                           && isDirPresent(h2b2bDst + QStringLiteral("/ur3_d1"))
                           && isDirPresent(h2b2bDst + QStringLiteral("/ur3_d2"))
                           && !isDirPresent(d2),
                       "H2B2B-UR3: modified directory blocks Undo in Preflight before moving d2");
                delete undoJob;
            }

            // UR4: Partial Undo (step 0 d2 succeeds, step 1 d1 collides mid-Undo) -> preserves both, fences history
            {
                const QString prevFile = h2b2bSrc + QStringLiteral("/ur4_prev.txt");
                verify(writeFileBytes(prevFile, "PREV"), "H2B2B-UR4: prevFile");
                LocalMoveHistory hist;
                auto *prevJob = new LocalFileMoveJob(
                    QUrl::fromLocalFile(prevFile),
                    QUrl::fromLocalFile(h2b2bDst + QStringLiteral("/ur4_prev.txt")));
                hist.recordWhenCompleted(prevJob, 1);
                prevJob->exec();
                verify(prevJob->error() == KJob::NoError && hist.canUndo(), "H2B2B-UR4: prev move ok");
                delete prevJob;

                const QString d1 = h2b2bSrc + QStringLiteral("/ur4_d1");
                const QString d2 = h2b2bSrc + QStringLiteral("/ur4_d2");
                verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2B-UR4: dirs");
                auto *job = new LocalBatchMoveJob(
                    {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                    QUrl::fromLocalFile(h2b2bDst));
                job->setAllowDirectories(true);
                hist.recordWhenCompleted(job, 2);
                QEventLoop loop;
                QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
                job->start();
                loop.exec();
                verify(job->error() == KJob::NoError && hist.canUndo(), "H2B2B-UR4: batch move ok");
                delete job;

                // Undo runs in reverse: step 0 = d2, step 1 = d1. Inject collision at d1 right before step 1!
                simulateBatchMoveBeforeStep() = [&](int step) {
                    if (step == 1) {
                        QDir().mkpath(d1);
                    }
                };
                KJob *undoJob = hist.undo();
                verify(undoJob != nullptr, "H2B2B-UR4: undo job created");
                undoJob->exec();
                resetBatchTestHooks();

                verify(undoJob->error() != KJob::NoError
                           && isDirPresent(d2)
                           && isDirPresent(h2b2bDst + QStringLiteral("/ur4_d1"))
                           && !hist.canUndo()
                           && !hist.canRedo(),
                       "H2B2B-UR4: partial Undo preserves d2 at src and d1 at dst, blocks Redo & fences prevFile Undo");
                delete undoJob;
            }

            // UR5: Partial failure during Move (fsync error at step 1) -> no Undo record, Retry blocked
            {
                const QString d1 = h2b2bSrc + QStringLiteral("/ur5_d1");
                const QString d2 = h2b2bSrc + QStringLiteral("/ur5_d2");
                verify(QDir().mkpath(d1) && QDir().mkpath(d2), "H2B2B-UR5: dirs");
                LocalMoveHistory hist;
                TransferQueueController q;
                simulateBatchMoveDestinationFsyncErrorAtStep() = {1, EIO};
                TransferTaskDescriptor desc;
                desc.title = QStringLiteral("H2B2B-UR5");
                desc.type = TransferOperationType::Move;
                desc.backend = TransferBackendKind::LocalNative;
                desc.sources = {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)};
                desc.destination = QUrl::fromLocalFile(h2b2bDst);
                desc.startMode = TransferJobStartMode::ControllerStarts;
                desc.jobFactory = [&]() -> KJob * {
                    auto *j = new LocalBatchMoveJob(
                        {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                        QUrl::fromLocalFile(h2b2bDst), &q);
                    j->setAllowDirectories(true);
                    hist.recordWhenCompleted(j, 1);
                    return j;
                };
                const auto enq = q.enqueue(std::move(desc));
                verify(waitUntil([&] {
                    const auto s = q.taskSnapshot(enq.id);
                    return s.has_value() && s->state == TransferTaskState::Failed;
                }), "H2B2B-UR5: partial fsync failure fails task");
                resetBatchTestHooks();
                verify(!hist.canUndo() && !q.checkRetryEligibility(enq.id).eligible,
                       "H2B2B-UR5: partial Move creates no Undo record and blocks Retry");
            }

            // UR6: Non-Btrfs FS refused by MoveSafetyPolicy::evaluate() for mixed batch
            {
                const QString d1 = h2b2bSrc + QStringLiteral("/ur6_d1");
                const QString f1 = h2b2bSrc + QStringLiteral("/ur6_f1.txt");
                verify(QDir().mkpath(d1) && writeFileBytes(f1, "U6"), "H2B2B-UR6: fixtures");
                simulateBatchMoveUnsupportedFsMagic() = EXT4_SUPER_MAGIC;
                const auto evalExt4 = MoveSafetyPolicy::evaluate(
                    {QUrl::fromLocalFile(f1), QUrl::fromLocalFile(d1)},
                    QUrl::fromLocalFile(h2b2bDst),
                    true);
                resetBatchTestHooks();
                verify(!evalExt4.allowed
                           && evalExt4.decision == MovePolicyDecision::RefusedLocalContainment,
                       "H2B2B-UR6: non-Btrfs mixed batch move refused fail-closed by MoveSafetyPolicy");
            }
        }
    }

    // Reconstructed H.2C.1A: Simulated ext4 batch gating & fail-closed checks
    {
        const QString eRoot = btrfsRoot.filePath(QStringLiteral("h2c_ext4"));
        const QString eSrc = eRoot + QStringLiteral("/src");
        const QString eDst = eRoot + QStringLiteral("/dst");
        verify(QDir().mkpath(eSrc) && QDir().mkpath(eDst), "H2C.1A: setup dirs");

        // 1. Files-only batch on simulated ext4 blocked by default, allowed with setAllowExt4(true)
        {
            const QString f1 = eSrc + QStringLiteral("/ef1.txt");
            const QString f2 = eSrc + QStringLiteral("/ef2.txt");
            verify(writeFileBytes(f1, "EF1") && writeFileBytes(f2, "EF2"), "H2C.1A-1: setup");
            simulateBatchMoveExt4Filesystem() = true;

            LocalBatchMoveJob blockedJob({QUrl::fromLocalFile(f1), QUrl::fromLocalFile(f2)},
                                         QUrl::fromLocalFile(eDst));
            blockedJob.setAutoDelete(false);
            verify(!blockedJob.exec() && blockedJob.error() != KJob::NoError
                       && blockedJob.batchResult().outcome == BatchMoveOutcome::FullRefusalBeforeExecution,
                   "H2C.1A-1: ext4 batch blocked without setAllowExt4(true)");

            // GUI policy remains locked on ext4
            const auto guiEval = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(f1), QUrl::fromLocalFile(f2)},
                QUrl::fromLocalFile(eDst),
                true);
            verify(!guiEval.allowed && guiEval.decision == MovePolicyDecision::RefusedLocalContainment,
                   "H2C.1A-1: MoveSafetyPolicy remains locked on ext4");

            LocalBatchMoveJob allowedJob({QUrl::fromLocalFile(f1), QUrl::fromLocalFile(f2)},
                                         QUrl::fromLocalFile(eDst));
            allowedJob.setAllowExt4(true);
            allowedJob.setAutoDelete(false);
            verify(allowedJob.exec() && allowedJob.error() == KJob::NoError
                       && allowedJob.batchResult().outcome == BatchMoveOutcome::FullSuccess
                       && allowedJob.verifiedFsProvenance() == LocalBtrfsMoveCommon::VerifiedMoveFsProvenance::Ext4,
                   "H2C.1A-1: ext4 files batch succeeds with setAllowExt4(true)");
            resetBatchTestHooks();
        }

        // 2. Mixed batch on simulated ext4 with setAllowDirectories(true) + setAllowExt4(true)
        {
            const QString mf = eSrc + QStringLiteral("/em_file.txt");
            const QString md = eSrc + QStringLiteral("/em_dir");
            verify(writeFileBytes(mf, "EMF") && QDir().mkpath(md)
                       && writeFileBytes(md + QStringLiteral("/inner.txt"), "EMI"),
                   "H2C.1A-2: setup");
            simulateBatchMoveExt4Filesystem() = true;
            LocalBatchMoveJob mixedJob({QUrl::fromLocalFile(mf), QUrl::fromLocalFile(md)},
                                       QUrl::fromLocalFile(eDst));
            mixedJob.setAllowDirectories(true);
            mixedJob.setAllowExt4(true);
            mixedJob.setAutoDelete(false);
            verify(mixedJob.exec() && mixedJob.error() == KJob::NoError
                       && mixedJob.batchResult().outcome == BatchMoveOutcome::FullSuccess,
                   "H2C.1A-2: ext4 mixed batch succeeds with explicit opt-in");
            resetBatchTestHooks();
        }

        // 3. Missing unique mnt_id / FS_IOC_GETFLAGS error / casefold / encrypt / partial failure on ext4
        {
            const QString g1 = eSrc + QStringLiteral("/g1.txt");
            const QString gd = eSrc + QStringLiteral("/gd");
            verify(writeFileBytes(g1, "G1") && QDir().mkpath(gd), "H2C.1A-3: setup");

            simulateBatchMoveExt4Filesystem() = true;
            simulateBatchMoveExt4StatxMntIdUniqueMissing() = true;
            LocalBatchMoveJob noUniq({QUrl::fromLocalFile(g1), QUrl::fromLocalFile(gd)},
                                     QUrl::fromLocalFile(eDst));
            noUniq.setAllowDirectories(true);
            noUniq.setAllowExt4(true);
            noUniq.setAutoDelete(false);
            verify(!noUniq.exec() && noUniq.error() != KJob::NoError,
                   "H2C.1A-3a: missing STATX_MNT_ID_UNIQUE rejected fail-closed");
            resetBatchTestHooks();

            simulateBatchMoveExt4Filesystem() = true;
            simulateBatchMoveExt4FlagsError() = true;
            LocalBatchMoveJob flagsFail({QUrl::fromLocalFile(g1), QUrl::fromLocalFile(gd)},
                                        QUrl::fromLocalFile(eDst));
            flagsFail.setAllowDirectories(true);
            flagsFail.setAllowExt4(true);
            flagsFail.setAutoDelete(false);
            verify(!flagsFail.exec() && flagsFail.error() != KJob::NoError,
                   "H2C.1A-3b: FS_IOC_GETFLAGS failure rejected fail-closed");
            resetBatchTestHooks();

            simulateBatchMoveExt4Filesystem() = true;
            simulateBatchMoveExt4CasefoldAllDirs() = true;
            LocalBatchMoveJob cfFail({QUrl::fromLocalFile(g1), QUrl::fromLocalFile(gd)},
                                     QUrl::fromLocalFile(eDst));
            cfFail.setAllowDirectories(true);
            cfFail.setAllowExt4(true);
            cfFail.setAutoDelete(false);
            verify(!cfFail.exec() && cfFail.error() != KJob::NoError,
                   "H2C.1A-3c: ext4 casefold rejected fail-closed");
            resetBatchTestHooks();

            simulateBatchMoveExt4Filesystem() = true;
            simulateBatchMoveExt4Encrypt() = true;
            LocalBatchMoveJob encFail({QUrl::fromLocalFile(g1), QUrl::fromLocalFile(gd)},
                                      QUrl::fromLocalFile(eDst));
            encFail.setAllowDirectories(true);
            encFail.setAllowExt4(true);
            encFail.setAutoDelete(false);
            verify(!encFail.exec() && encFail.error() != KJob::NoError,
                   "H2C.1A-3d: ext4 FS_ENCRYPT_FL rejected fail-closed");
            resetBatchTestHooks();

            simulateBatchMoveExt4Filesystem() = true;
            simulateBatchMoveDestinationFsyncErrorAtStep() = {1, EIO};
            LocalMoveHistory partHist;
            partHist.setAllowExt4History(true);
            LocalBatchMoveJob partJob({QUrl::fromLocalFile(g1), QUrl::fromLocalFile(gd)},
                                      QUrl::fromLocalFile(eDst));
            partJob.setAllowDirectories(true);
            partJob.setAllowExt4(true);
            partJob.setAutoDelete(false);
            verify(partHist.recordWhenCompleted(&partJob, 550), "H2C.1A-3e: attach");
            verify(!partJob.exec() && partJob.error() != KJob::NoError
                       && partJob.property("thispcBatchPartiallyCompleted").toBool()
                       && partHist.count() == 0,
                   "H2C.1A-3e: partial ext4 batch creates no reversible history");
            resetBatchTestHooks();
        }
    }

    // Reconstructed H.2C Provenance (T4-batch, T5-batch, T6-batch, T10)
    {
        const QString pRoot = btrfsRoot.filePath(QStringLiteral("h2c_prov"));
        const QString pSrc = pRoot + QStringLiteral("/src");
        const QString pDst = pRoot + QStringLiteral("/dst");
        verify(QDir().mkpath(pSrc) && QDir().mkpath(pDst), "T4-T6: setup dirs");

        // T4-batch: authorized Btrfs batch move records Btrfs provenance
        {
            const QString f0 = pSrc + QStringLiteral("/t4_0.txt");
            const QString f1 = pSrc + QStringLiteral("/t4_1.txt");
            verify(writeFileBytes(f0, "T4_0") && writeFileBytes(f1, "T4_1"), "T4-batch: setup");
            LocalMoveHistory hist;
            hist.setAllowExt4History(false);
            LocalBatchMoveJob job({QUrl::fromLocalFile(f0), QUrl::fromLocalFile(f1)},
                                  QUrl::fromLocalFile(pDst));
            job.setAutoDelete(false);
            verify(hist.recordWhenCompleted(&job, 701), "T4-batch: attach");
            verify(job.exec()
                       && job.verifiedFsProvenance() == LocalBtrfsMoveCommon::VerifiedMoveFsProvenance::Btrfs
                       && hist.count() == 1 && hist.undoSerial() == 701,
                   "T4-batch: authorized Btrfs batch move recorded with Btrfs provenance");
        }

        // T5-batch: simulated Ext4 blocked when allowExt4History=false, recorded as Ext4 when opt-in=true
        {
            const QString f0 = pSrc + QStringLiteral("/t5_0.txt");
            const QString f1 = pSrc + QStringLiteral("/t5_1.txt");
            verify(writeFileBytes(f0, "T5_0") && writeFileBytes(f1, "T5_1"), "T5-batch: setup");

            simulateBatchMoveExt4Filesystem() = true;
            LocalMoveHistory histNoOptIn;
            histNoOptIn.setAllowExt4History(false);
            LocalBatchMoveJob blocked({QUrl::fromLocalFile(f0), QUrl::fromLocalFile(f1)},
                                      QUrl::fromLocalFile(pDst));
            blocked.setAllowExt4(true);
            blocked.setAutoDelete(false);
            verify(histNoOptIn.recordWhenCompleted(&blocked, 702), "T5-batch: attach no opt-in");
            verify(blocked.exec() && histNoOptIn.count() == 0,
                   "T5-batch: Ext4 batch move NOT recorded when allowExt4History=false");

            const QString back0 = pDst + QStringLiteral("/t5_0.txt");
            const QString back1 = pDst + QStringLiteral("/t5_1.txt");
            const QString dst2 = pSrc + QStringLiteral("/t5_back");
            verify(QDir().mkpath(dst2), "T5-batch: back dir");
            LocalMoveHistory histOptIn;
            histOptIn.setAllowExt4History(true);
            LocalBatchMoveJob allowed({QUrl::fromLocalFile(back0), QUrl::fromLocalFile(back1)},
                                      QUrl::fromLocalFile(dst2));
            allowed.setAllowExt4(true);
            allowed.setAutoDelete(false);
            verify(histOptIn.recordWhenCompleted(&allowed, 703), "T5-batch: attach opt-in");
            verify(allowed.exec() && histOptIn.count() == 1 && histOptIn.undoSerial() == 703,
                   "T5-batch: Ext4 batch move recorded as verified Ext4 when allowExt4History=true");

            histOptIn.setAllowExt4History(false);
            KJob *undoJob = histOptIn.undo();
            auto *batchUndo = qobject_cast<LocalBatchMoveJob *>(undoJob);
            verify(batchUndo != nullptr && !batchUndo->allowExt4(),
                   "T5-batch: recorded Ext4 batch does NOT masquerade as Btrfs");
            verify(!undoJob->exec() && undoJob->error() != KJob::NoError,
                   "T5-batch: Undo blocked when opt-in disabled");
            delete undoJob;
            resetBatchTestHooks();
        }

        // T6-batch: unknown/tmpfs provenance rejected fail-closed
        {
            const QString f0 = pSrc + QStringLiteral("/t6_0.txt");
            const QString f1 = pSrc + QStringLiteral("/t6_1.txt");
            verify(writeFileBytes(f0, "T6_0") && writeFileBytes(f1, "T6_1"), "T6-batch: setup");
            simulateBatchMoveUnsupportedFsMagic() = 0x01021994UL;
            LocalMoveHistory hist;
            hist.setAllowExt4History(true);
            LocalBatchMoveJob job({QUrl::fromLocalFile(f0), QUrl::fromLocalFile(f1)},
                                  QUrl::fromLocalFile(pDst));
            job.setAutoDelete(false);
            verify(hist.recordWhenCompleted(&job, 704), "T6-batch: attach");
            verify(!job.exec() && job.error() != KJob::NoError && hist.count() == 0,
                   "T6-batch: unknown/tmpfs provenance rejected fail-closed");
            resetBatchTestHooks();
        }

        // T10: post-rename verify failure / durability failure yields no Undo entry & fences prior history
        {
            const QString s0 = pSrc + QStringLiteral("/t10_seed0.txt");
            const QString s1 = pSrc + QStringLiteral("/t10_seed1.txt");
            const QString f0 = pSrc + QStringLiteral("/t10_0.txt");
            const QString f1 = pSrc + QStringLiteral("/t10_1.txt");
            verify(writeFileBytes(s0, "S0") && writeFileBytes(s1, "S1")
                       && writeFileBytes(f0, "T10_0") && writeFileBytes(f1, "T10_1"),
                   "T10: setup");
            LocalMoveHistory hist;
            LocalBatchMoveJob seed({QUrl::fromLocalFile(s0), QUrl::fromLocalFile(s1)},
                                   QUrl::fromLocalFile(pDst));
            seed.setAutoDelete(false);
            verify(hist.recordWhenCompleted(&seed, 800) && seed.exec() && hist.canUndo(),
                   "T10: seed history ok");

            simulateBatchMovePostRenameVerifyFailAtStep() = 0;
            LocalBatchMoveJob failJob({QUrl::fromLocalFile(f0), QUrl::fromLocalFile(f1)},
                                      QUrl::fromLocalFile(pDst));
            failJob.setAutoDelete(false);
            verify(hist.recordWhenCompleted(&failJob, 801), "T10: attach");
            verify(!failJob.exec() && failJob.error() != KJob::NoError
                       && !hist.canUndo() && hist.count() == 1,
                   "T10: post-rename verify fails, adds no Undo entry and fences prior history");
            resetBatchTestHooks();

            // T10-durability: destination fsync failure on step 1 -> no Undo entry & fences prior history
            const QString d0 = pSrc + QStringLiteral("/t10_d0.txt");
            const QString d1 = pSrc + QStringLiteral("/t10_d1.txt");
            const QString ds0 = pSrc + QStringLiteral("/t10_ds0.txt");
            const QString ds1 = pSrc + QStringLiteral("/t10_ds1.txt");
            verify(writeFileBytes(ds0, "DS0") && writeFileBytes(ds1, "DS1")
                       && writeFileBytes(d0, "D0") && writeFileBytes(d1, "D1"),
                   "T10-durability: setup");
            LocalMoveHistory durHist;
            LocalBatchMoveJob durSeed({QUrl::fromLocalFile(ds0), QUrl::fromLocalFile(ds1)},
                                      QUrl::fromLocalFile(pDst));
            durSeed.setAutoDelete(false);
            verify(durHist.recordWhenCompleted(&durSeed, 802) && durSeed.exec() && durHist.canUndo(),
                   "T10-durability: seed history ok");
            simulateBatchMoveDestinationFsyncErrorAtStep() = {1, EIO};
            LocalBatchMoveJob durFail({QUrl::fromLocalFile(d0), QUrl::fromLocalFile(d1)},
                                      QUrl::fromLocalFile(pDst));
            durFail.setAutoDelete(false);
            verify(durHist.recordWhenCompleted(&durFail, 803), "T10-durability: attach");
            verify(!durFail.exec() && durFail.error() != KJob::NoError
                       && durFail.property("thispcBatchPartiallyCompleted").toBool()
                       && !durHist.canUndo() && durHist.count() == 1,
                   "T10-durability: fsync failure adds no Undo entry and fences prior history");
            resetBatchTestHooks();
        }
    }

    // Reconstructed U5C.0 & U5C.1: Batch strict directory snapshot opt-in, hashing, mutation/inode checks
    {
        const QString u5Root = btrfsRoot.filePath(QStringLiteral("u5c_batch"));
        const QString u5Src = u5Root + QStringLiteral("/src");
        const QString u5Dst = u5Root + QStringLiteral("/dst");
        verify(QDir().mkpath(u5Src) && QDir().mkpath(u5Dst), "U5C: setup dirs");

        // U5C.0: Without history attachment, captureStrictDirectorySnapshot() stays false and 0 files hashed
        {
            const QString dA = u5Src + QStringLiteral("/fast_a");
            const QString dB = u5Src + QStringLiteral("/fast_b");
            verify(QDir().mkpath(dA) && QDir().mkpath(dB)
                       && writeFileBytes(dA + QStringLiteral("/fa.txt"), "FAST_A")
                       && writeFileBytes(dB + QStringLiteral("/fb.txt"), "FAST_B"),
                   "U5C.0: setup");
            batchMoveSnapshotFilesHashedCount().store(0, std::memory_order_relaxed);
            LocalBatchMoveJob fastJob({QUrl::fromLocalFile(dA), QUrl::fromLocalFile(dB)},
                                      QUrl::fromLocalFile(u5Dst));
            fastJob.setAllowDirectories(true);
            fastJob.setAutoDelete(false);
            verify(!fastJob.captureStrictDirectorySnapshot(), "U5C.0: default strict snapshot false");
            verify(fastJob.exec() && fastJob.error() == KJob::NoError
                       && batchMoveSnapshotFilesHashedCount().load(std::memory_order_relaxed) == 0,
                   "U5C.0: non-history batch move skips strict file hashing");
        }

        // U5C.1: History-backed Batch Move hashes files, records strict snapshots, detects same-size mutation & inode swap
        {
            const QString sA = u5Src + QStringLiteral("/strict_a");
            const QString sB = u5Src + QStringLiteral("/strict_b");
            verify(QDir().mkpath(sA) && QDir().mkpath(sB)
                       && writeFileBytes(sA + QStringLiteral("/sa.txt"), "SA_DATA_01")
                       && writeFileBytes(sB + QStringLiteral("/sb.txt"), "SB_DATA_01"),
                   "U5C.1-strict: setup");

            batchMoveSnapshotFilesHashedCount().store(0, std::memory_order_relaxed);
            LocalMoveHistory sHist;
            LocalBatchMoveJob sJob({QUrl::fromLocalFile(sA), QUrl::fromLocalFile(sB)},
                                   QUrl::fromLocalFile(u5Dst));
            sJob.setAllowDirectories(true);
            sJob.setAutoDelete(false);
            verify(sHist.recordWhenCompleted(&sJob, 952) && sJob.captureStrictDirectorySnapshot(),
                   "U5C.1-strict: opt-in enabled");
            verify(sJob.exec()
                       && sJob.batchResult().outcome == BatchMoveOutcome::FullSuccess
                       && batchMoveSnapshotFilesHashedCount().load(std::memory_order_relaxed) == 2
                       && sJob.batchResult().items.at(0).movedDirSnapshot.strictContentDigest
                       && sJob.batchResult().items.at(1).movedDirSnapshot.strictContentDigest
                       && sHist.count() == 1 && sHist.canUndo(),
                   "U5C.1-strict: history-backed Batch Move hashed 2 files and recorded strict snapshots");

            // Same-size mutation inside moved directory -> Undo must refuse fail-closed without moving
            const QString movedFile = u5Dst + QStringLiteral("/strict_a/sa.txt");
            verify(writeFileBytes(movedFile, "SA_DATA_99"), "U5C.1-mutate: same-size edit");
            KJob *mutUndo = sHist.undo();
            verify(mutUndo != nullptr && !mutUndo->exec() && mutUndo->error() != KJob::NoError,
                   "U5C.1-mutate: Undo refused on same-size SHA-256 mismatch");
            delete mutUndo;
            verify(!QFileInfo::exists(sA) && QFileInfo::exists(u5Dst + QStringLiteral("/strict_a")),
                   "U5C.1-mutate: destination preserved on refusal");
        }

        // U5C.1: Inode swap with identical bytes inside moved directory -> Undo refused fail-closed
        {
            const QString iA = u5Src + QStringLiteral("/inode_a");
            const QString iB = u5Src + QStringLiteral("/inode_b");
            verify(QDir().mkpath(iA) && QDir().mkpath(iB)
                       && writeFileBytes(iA + QStringLiteral("/ia.txt"), "IDENTICAL_BYTES")
                       && writeFileBytes(iB + QStringLiteral("/ib.txt"), "IB_BYTES"),
                   "U5C.1-inode: setup");
            LocalMoveHistory iHist;
            LocalBatchMoveJob iJob({QUrl::fromLocalFile(iA), QUrl::fromLocalFile(iB)},
                                   QUrl::fromLocalFile(u5Dst));
            iJob.setAllowDirectories(true);
            iJob.setAutoDelete(false);
            verify(iHist.recordWhenCompleted(&iJob, 954) && iJob.exec() && iHist.canUndo(),
                   "U5C.1-inode: move ok");
            const QString movedIa = u5Dst + QStringLiteral("/inode_a/ia.txt");
            const QString cloneIa = u5Dst + QStringLiteral("/inode_a/ia.txt.clone");
            verify(writeFileBytes(cloneIa, "IDENTICAL_BYTES")
                       && ::rename(QFile::encodeName(cloneIa).constData(),
                                   QFile::encodeName(movedIa).constData()) == 0,
                   "U5C.1-inode: replace file with new inode and identical bytes");
            KJob *inodeUndo = iHist.undo();
            verify(inodeUndo != nullptr && !inodeUndo->exec() && inodeUndo->error() != KJob::NoError,
                   "U5C.1-inode: Undo refused on inode swap with identical bytes");
            delete inodeUndo;
            verify(!QFileInfo::exists(iA) && QFileInfo::exists(u5Dst + QStringLiteral("/inode_a")),
                   "U5C.1-inode: destination preserved on refusal");
        }

        // U5C.1: Undo collision -> refused without overwrite
        {
            const QString cA = u5Src + QStringLiteral("/col_a.txt");
            const QString cB = u5Src + QStringLiteral("/col_b.txt");
            verify(writeFileBytes(cA, "CA") && writeFileBytes(cB, "CB"), "U5C.1-col: setup");
            LocalMoveHistory cHist;
            LocalBatchMoveJob cJob({QUrl::fromLocalFile(cA), QUrl::fromLocalFile(cB)},
                                   QUrl::fromLocalFile(u5Dst));
            cJob.setAutoDelete(false);
            verify(cHist.recordWhenCompleted(&cJob, 953) && cJob.exec() && cHist.canUndo(),
                   "U5C.1-col: move ok");
            verify(writeFileBytes(cA, "COLLISION"), "U5C.1-col: create blocker at source");
            KJob *colUndo = cHist.undo();
            verify(colUndo != nullptr && !colUndo->exec() && colUndo->error() != KJob::NoError,
                   "U5C.1-col: Undo refused on destination collision");
            delete colUndo;
            verify(QFileInfo::exists(u5Dst + QStringLiteral("/col_a.txt")),
                   "U5C.1-col: moved file preserved without overwrite");
        }

        // U5C.1-file-strict: Regular-file batch detects same-size mutation, size change, and inode swap
        {
            const QString r1 = u5Src + QStringLiteral("/rf_1.txt");
            const QString r2 = u5Src + QStringLiteral("/rf_2.txt");
            verify(writeFileBytes(r1, "RF_DATA_01") && writeFileBytes(r2, "RF_DATA_02"),
                   "U5C.1-file: setup");
            LocalMoveHistory rHist;
            LocalBatchMoveJob rJob({QUrl::fromLocalFile(r1), QUrl::fromLocalFile(r2)},
                                   QUrl::fromLocalFile(u5Dst));
            rJob.setAutoDelete(false);
            verify(rHist.recordWhenCompleted(&rJob, 955) && rJob.exec() && rHist.canUndo(),
                   "U5C.1-file: move ok");
            const QString movedR1 = u5Dst + QStringLiteral("/rf_1.txt");
            // Same-size mutation
            verify(writeFileBytes(movedR1, "RF_DATA_99"), "U5C.1-file: same-size edit");
            KJob *uMut = rHist.undo();
            verify(uMut != nullptr && !uMut->exec() && uMut->error() != KJob::NoError,
                   "U5C.1-file: Undo refused on same-size byte mutation");
            delete uMut;
            // Size change
            verify(writeFileBytes(movedR1, "RF_DATA_LONGER"), "U5C.1-file: size change");
            KJob *uSize = rHist.undo();
            verify(uSize != nullptr && !uSize->exec() && uSize->error() != KJob::NoError,
                   "U5C.1-file: Undo refused on size change");
            delete uSize;
            // Inode swap with original bytes
            const QString movedR2 = u5Dst + QStringLiteral("/rf_2.txt");
            const QString cloneR2 = u5Dst + QStringLiteral("/rf_2.txt.clone");
            verify(writeFileBytes(cloneR2, "RF_DATA_02")
                       && ::rename(QFile::encodeName(cloneR2).constData(),
                                   QFile::encodeName(movedR2).constData()) == 0,
                   "U5C.1-file: inode swap on second file");
            KJob *uInode = rHist.undo();
            verify(uInode != nullptr && !uInode->exec() && uInode->error() != KJob::NoError,
                   "U5C.1-file: Undo refused on inode swap");
            delete uInode;
            verify(!QFileInfo::exists(r1) && !QFileInfo::exists(r2),
                   "U5C.1-file: sources untouched after strict refusals");
        }
    }

    // H.2C-U6.3: Batch & Mixed Batch history authorization hardening
    {
        auto checkBatchMoveApi = []<typename H>(H *) {
            return requires(H &h) {
                h.recordCompletedBatchMove(QList<QPair<QUrl, QUrl>>{}, QList<LocalRegularFileIdentity>{}, 0);
            };
        };
        auto checkMixedBatchMoveApi = []<typename H>(H *) {
            return requires(H &h) {
                h.recordCompletedMixedBatchMove(
                    QList<QPair<QUrl, QUrl>>{}, QList<BatchMoveExpectedItem>{}, false, 0);
            };
        };
        constexpr bool hasPublicRecordCompletedBatchMove =
            checkBatchMoveApi(static_cast<LocalMoveHistory *>(nullptr));
        constexpr bool hasPublicRecordCompletedMixedBatchMove =
            checkMixedBatchMoveApi(static_cast<LocalMoveHistory *>(nullptr));
        verify(!hasPublicRecordCompletedBatchMove && !hasPublicRecordCompletedMixedBatchMove,
               "U6.3-1: public recordCompletedBatchMove/recordCompletedMixedBatchMove removed");

        const QString u63Root = btrfsRoot.filePath(QStringLiteral("u63_auth"));
        const QString u63Src = u63Root + QStringLiteral("/src");
        const QString u63Dst = u63Root + QStringLiteral("/dst");
        verify(QDir().mkpath(u63Src) && QDir().mkpath(u63Dst), "U6.3: setup dirs");

        // Late attachment after start() (even with pre-enabled strict snapshot) & after finish()
        {
            const QString a1 = u63Src + QStringLiteral("/a1.txt");
            const QString a2 = u63Src + QStringLiteral("/a2.txt");
            const QString b1 = u63Src + QStringLiteral("/b1.txt");
            const QString b2 = u63Src + QStringLiteral("/b2.txt");
            verify(writeFileBytes(a1, "A1") && writeFileBytes(a2, "A2")
                       && writeFileBytes(b1, "B1") && writeFileBytes(b2, "B2"),
                   "U6.3-2: setup files");

            LocalMoveHistory hist;
            LocalBatchMoveJob seed(
                {QUrl::fromLocalFile(a1), QUrl::fromLocalFile(a2)},
                QUrl::fromLocalFile(u63Dst));
            seed.setAutoDelete(false);
            verify(hist.recordWhenCompleted(&seed, 1001), "U6.3-2: seed attach");
            verify(seed.exec() && hist.count() == 1 && hist.canUndo(), "U6.3-2: seed move ok");

            LocalBatchMoveJob lateJob(
                {QUrl::fromLocalFile(b1), QUrl::fromLocalFile(b2)},
                QUrl::fromLocalFile(u63Dst));
            lateJob.setAutoDelete(false);
            verify(lateJob.setCaptureStrictDirectorySnapshot(true), "U6.3-2: pre-enable snapshot");
            lateJob.start();
            verify(lateJob.hasStarted(), "U6.3-2: lateJob started");
            verify(!hist.recordWhenCompleted(&lateJob, 1002),
                   "U6.3-2: late recordWhenCompleted after start() rejected");
            verify(!hist.canUndo(), "U6.3-2: late attachment fences prior history");
            QEventLoop loop;
            QObject::connect(&lateJob, &KJob::result, &loop, &QEventLoop::quit);
            if (!lateJob.hasFinished()) {
                loop.exec();
            }
            verify(!hist.recordWhenCompleted(&lateJob, 1003),
                   "U6.3-2: recordWhenCompleted after finish() rejected");
            verify(hist.count() == 1 && !hist.canUndo(),
                   "U6.3-2: finished job did not create a second record");
        }

        // Double attachment (same history & second history) + duplicate result emission guard
        {
            const QString c1 = u63Src + QStringLiteral("/c1.txt");
            const QString c2 = u63Src + QStringLiteral("/c2.txt");
            verify(writeFileBytes(c1, "C1") && writeFileBytes(c2, "C2"), "U6.3-3: setup");

            LocalMoveHistory h1;
            LocalMoveHistory h2;
            LocalBatchMoveJob job(
                {QUrl::fromLocalFile(c1), QUrl::fromLocalFile(c2)},
                QUrl::fromLocalFile(u63Dst));
            job.setAutoDelete(false);
            verify(h1.recordWhenCompleted(&job, 1010), "U6.3-3: first attach succeeds");
            verify(!h2.recordWhenCompleted(&job, 1011), "U6.3-3: second history attach refused");
            verify(!h1.recordWhenCompleted(&job, 1012), "U6.3-3: duplicate same history attach refused");
            verify(job.exec() && h1.count() == 1 && h2.count() == 0,
                   "U6.3-3: only first authorized history records");

            QMetaObject::invokeMethod(&job, "result", Q_ARG(KJob *, &job));
            QCoreApplication::processEvents();
            verify(h1.count() == 1, "U6.3-3: duplicate result signal produces no duplicate record");
        }

        // Lifetime safety: job destroyed before queued delivery / history destroyed before job completes
        {
            const QString d1 = u63Src + QStringLiteral("/d1.txt");
            const QString d2 = u63Src + QStringLiteral("/d2.txt");
            verify(writeFileBytes(d1, "D1") && writeFileBytes(d2, "D2"), "U6.3-4: setup");

            {
                LocalMoveHistory hist;
                auto *job = new LocalBatchMoveJob(
                    {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                    QUrl::fromLocalFile(u63Dst));
                job->setAutoDelete(false);
                verify(hist.recordWhenCompleted(job, 1020), "U6.3-4a: attach");
                delete job;
                QCoreApplication::processEvents();
                verify(hist.count() == 0, "U6.3-4a: deleting job before start leaves no record");
            }

            {
                auto *hist = new LocalMoveHistory;
                LocalBatchMoveJob job(
                    {QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                    QUrl::fromLocalFile(u63Dst));
                job.setAutoDelete(false);
                verify(hist->recordWhenCompleted(&job, 1021), "U6.3-4b: attach");
                delete hist;
                verify(job.exec() && job.error() == KJob::NoError,
                       "U6.3-4b: job finishes cleanly after history destroyed (no dangling callback)");
            }
        }

        // Authorized Mixed Batch Move -> Undo -> Redo round-trip
        {
            const QString mFileSrc = u63Src + QStringLiteral("/m_file.txt");
            const QString mDir = u63Src + QStringLiteral("/m_dir");
            const QString mBackDst = u63Src + QStringLiteral("/m_back");
            verify(QDir().mkpath(mDir) && QDir().mkpath(mBackDst)
                       && writeFileBytes(mFileSrc, "MFILE")
                       && writeFileBytes(mDir + QStringLiteral("/inner.txt"), "INNER_M"),
                   "U6.3-5: setup mixed");

            LocalMoveHistory mHist;
            LocalBatchMoveJob mJob(
                {QUrl::fromLocalFile(mFileSrc), QUrl::fromLocalFile(mDir)},
                QUrl::fromLocalFile(mBackDst));
            mJob.setAllowDirectories(true);
            mJob.setAutoDelete(false);
            verify(mHist.recordWhenCompleted(&mJob, 1030), "U6.3-5: attach mixed");
            verify(mJob.exec() && mJob.error() == KJob::NoError && mHist.count() == 1 && mHist.canUndo(),
                   "U6.3-5: mixed batch move recorded");

            KJob *undoJob = mHist.undo();
            verify(undoJob != nullptr && undoJob->exec() && undoJob->error() == KJob::NoError,
                   "U6.3-5: mixed batch undo succeeded");
            delete undoJob;
            verify(QFileInfo::exists(mFileSrc) && QFileInfo::exists(mDir + QStringLiteral("/inner.txt"))
                       && mHist.canRedo(),
                   "U6.3-5: sources restored and redo available");

            KJob *redoJob = mHist.redo();
            verify(redoJob != nullptr && redoJob->exec() && redoJob->error() == KJob::NoError,
                   "U6.3-5: mixed batch redo succeeded");
            delete redoJob;
            verify(QFileInfo::exists(mBackDst + QStringLiteral("/m_file.txt"))
                       && QFileInfo::exists(mBackDst + QStringLiteral("/m_dir/inner.txt")),
                   "U6.3-5: destinations restored after redo");
        }

        // Legal files-only Batch Move -> Undo -> Redo, and unauthorized/unattached Undo/Redo refused
        {
            LocalMoveHistory emptyHist;
            verify(!emptyHist.canUndo() && !emptyHist.canRedo()
                       && emptyHist.undo() == nullptr && emptyHist.redo() == nullptr,
                   "U6.3-6: unattached history cannot Undo or Redo");

            const QString f1 = u63Src + QStringLiteral("/f_only_1.txt");
            const QString f2 = u63Src + QStringLiteral("/f_only_2.txt");
            const QString fDst = u63Dst + QStringLiteral("/files_only");
            verify(QDir().mkpath(fDst)
                       && writeFileBytes(f1, "FO1") && writeFileBytes(f2, "FO2"),
                   "U6.3-6: setup files-only batch");
            LocalMoveHistory fHist;
            LocalBatchMoveJob fJob({QUrl::fromLocalFile(f1), QUrl::fromLocalFile(f2)},
                                   QUrl::fromLocalFile(fDst));
            fJob.setAutoDelete(false);
            verify(fHist.recordWhenCompleted(&fJob, 1031) && fJob.exec() && fJob.error() == KJob::NoError
                       && fHist.count() == 1 && fHist.canUndo(),
                   "U6.3-6: files-only batch move recorded");
            KJob *fUndo = fHist.undo();
            verify(fUndo != nullptr && fUndo->exec() && fUndo->error() == KJob::NoError
                       && QFileInfo::exists(f1) && QFileInfo::exists(f2) && fHist.canRedo(),
                   "U6.3-6: files-only batch undo succeeded");
            delete fUndo;
            KJob *fRedo = fHist.redo();
            verify(fRedo != nullptr && fRedo->exec() && fRedo->error() == KJob::NoError
                       && QFileInfo::exists(fDst + QStringLiteral("/f_only_1.txt"))
                       && QFileInfo::exists(fDst + QStringLiteral("/f_only_2.txt")),
                   "U6.3-6: files-only batch redo succeeded");
            delete fRedo;
        }

        // Cursor, serials (undoSerial/redoSerial), fenceIrreversible(), and Redo collision
        {
            const QString s1a = u63Src + QStringLiteral("/cur_1a.txt");
            const QString s1b = u63Src + QStringLiteral("/cur_1b.txt");
            const QString s2a = u63Src + QStringLiteral("/cur_2a.txt");
            const QString s2b = u63Src + QStringLiteral("/cur_2b.txt");
            const QString cDst = u63Dst + QStringLiteral("/cursor");
            verify(QDir().mkpath(cDst)
                       && writeFileBytes(s1a, "1A") && writeFileBytes(s1b, "1B")
                       && writeFileBytes(s2a, "2A") && writeFileBytes(s2b, "2B"),
                   "U6.3-7: setup cursor/serials");

            LocalMoveHistory cHist;
            LocalBatchMoveJob j1({QUrl::fromLocalFile(s1a), QUrl::fromLocalFile(s1b)},
                                 QUrl::fromLocalFile(cDst));
            j1.setAutoDelete(false);
            verify(cHist.recordWhenCompleted(&j1, 1101) && j1.exec(), "U6.3-7: batch 1 ok");

            LocalBatchMoveJob j2({QUrl::fromLocalFile(s2a), QUrl::fromLocalFile(s2b)},
                                 QUrl::fromLocalFile(cDst));
            j2.setAutoDelete(false);
            verify(cHist.recordWhenCompleted(&j2, 1102) && j2.exec(), "U6.3-7: batch 2 ok");
            verify(cHist.count() == 2 && cHist.undoSerial() == 1102 && !cHist.canRedo(),
                   "U6.3-7: serials track top of stack");

            KJob *u2 = cHist.undo();
            verify(u2 != nullptr && u2->exec() && cHist.canUndo() && cHist.canRedo()
                       && cHist.undoSerial() == 1101 && cHist.redoSerial() == 1102,
                   "U6.3-7: undo moves cursor to 1101/1102");
            delete u2;

            // Redo collision check
            const QString moved2a = cDst + QStringLiteral("/cur_2a.txt");
            verify(writeFileBytes(moved2a, "REDO_BLOCKER"), "U6.3-7: create redo collision");
            KJob *r2Fail = cHist.redo();
            verify(r2Fail != nullptr && !r2Fail->exec() && r2Fail->error() != KJob::NoError
                       && cHist.canUndo() && cHist.canRedo(),
                   "U6.3-7: Redo refused on destination collision");
            delete r2Fail;

            // fenceIrreversible() via failed partial batch clears Redo tail and fences prior Undo
            const QString f1 = u63Src + QStringLiteral("/cur_f1.txt");
            const QString f2 = u63Src + QStringLiteral("/cur_f2.txt");
            verify(writeFileBytes(f1, "F1") && writeFileBytes(f2, "F2"), "U6.3-7: setup fence");
            simulateBatchMoveDestinationFsyncErrorAtStep() = {1, EIO};
            LocalBatchMoveJob jFail({QUrl::fromLocalFile(f1), QUrl::fromLocalFile(f2)},
                                    QUrl::fromLocalFile(cDst));
            jFail.setAutoDelete(false);
            verify(cHist.recordWhenCompleted(&jFail, 1103), "U6.3-7: attach fence job");
            verify(!jFail.exec() && !cHist.canRedo() && !cHist.canUndo()
                       && cHist.undo() == nullptr && cHist.redo() == nullptr,
                   "U6.3-7: fenceIrreversible clears Redo tail and blocks Undo past fence");
            resetBatchTestHooks();
        }
    }

    resetBatchTestHooks();
    qInfo("PASS: %d LocalBatchMoveJob assertions", checks);
    return 0;
}
