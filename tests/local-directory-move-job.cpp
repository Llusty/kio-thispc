/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localdirectorymovejob.h"
#include "localfilemovejob.h"
#include "transferqueuecontroller.h"
#include "kiostagedtransferjob.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <linux/magic.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xEF53
#endif
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

static void resetDirMoveTestHooks()
{
    simulateDirMoveUnsupportedFsMagic() = 0;
    simulateDirMoveRenameNoReplaceUnsupported() = false;
    simulateDirMoveExdev() = false;
    simulateDirMoveOpenat2Unsupported() = false;
    simulateDirMoveStatxMntIdUnsupported() = false;
    simulateDirMoveBindMountMismatch() = false;
    simulateDirMoveSubvolumeBoundary() = false;
    simulateDirMoveDestinationFsyncError() = 0;
    simulateDirMoveSourceFsyncError() = 0;
    simulateDirMoveBeforeRename() = {};
    simulateDirMoveBeforeDstFsync() = {};
    simulateDirMoveExt4Filesystem() = false;
    simulateDirMoveExt4StatxMntIdUniqueMissing() = false;
    simulateDirMoveExt4FlagsError() = false;
    simulateDirMoveExt4CasefoldAllDirs() = false;
    simulateDirMoveExt4CasefoldInode() = 0;
    simulateDirMoveExt4Encrypt() = false;
    dirMoveSnapshotFilesHashedCount().store(0, std::memory_order_relaxed);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QString btrfsBase = QString::fromLocal8Bit(qgetenv("THISPC_TEST_FILES"));
    if (btrfsBase.isEmpty()) {
        btrfsBase = QDir::homePath();
    }
    struct statfs baseSfs {};
    verify(::statfs(QFile::encodeName(btrfsBase).constData(), &baseSfs) == 0
               && static_cast<unsigned long>(baseSfs.f_type) == BTRFS_SUPER_MAGIC,
           "H.2A test workspace resides on real Btrfs volume");

    QTemporaryDir rootDir(btrfsBase + QStringLiteral("/h2a-dir-move-XXXXXX"));
    verify(rootDir.isValid(), "H.2A temporary Btrfs root created");

    const QString srcRoot = rootDir.filePath(QStringLiteral("srcRoot"));
    const QString dstRoot = rootDir.filePath(QStringLiteral("dstRoot"));
    verify(QDir().mkpath(srcRoot) && QDir().mkpath(dstRoot), "H.2A srcRoot/dstRoot created");

    // ------------------------------------------------------------------
    // Positive 1: Empty directory Btrfs -> Btrfs
    // ------------------------------------------------------------------
    {
        const QString src = srcRoot + QStringLiteral("/empty_dir");
        const QString dst = dstRoot + QStringLiteral("/empty_dir");
        verify(QDir().mkdir(src), "Pos1: empty_dir created");
        const struct stat beforeSt = lstatEntry(src);

        LocalDirectoryMoveJob job(QUrl::fromLocalFile(src), QUrl::fromLocalFile(dst));
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&] { return spy.count() == 1; }), "Pos1: empty dir job finished");
        verify(job.error() == KJob::NoError
                   && job.moveError() == LocalDirectoryMoveError::None
                   && job.outcome() == LocalDirectoryMoveOutcome::PublishedAndDurable
                   && job.destinationPublished()
                   && job.directoryDurabilityConfirmed(),
               "Pos1: empty dir moved atomically with confirmed durability");
        bool dstOk = false;
        const struct stat afterSt = lstatEntry(dst, &dstOk);
        verify(!QFileInfo::exists(src) && dstOk && S_ISDIR(afterSt.st_mode)
                   && afterSt.st_ino == beforeSt.st_ino && afterSt.st_dev == beforeSt.st_dev,
               "Pos1: old dir removed, new dir exists with identical (dev, ino)");
    }

    // ------------------------------------------------------------------
    // Positive 2 & 3: Single file + multi-file + subdirs + Unicode/spaces
    // ------------------------------------------------------------------
    {
        const QString folderName = QString::fromUtf8("katalog zażółć gęślą 📁");
        const QString src = srcRoot + QLatin1Char('/') + folderName;
        const QString dst = dstRoot + QLatin1Char('/') + folderName;
        verify(QDir().mkpath(src + QString::fromUtf8("/podkatalog 1/głęboki 2")), "Pos3: nested tree created");

        const QByteArray data1 = "single-file-payload-12345";
        const QByteArray data2 = QByteArray("unicode-zawartość-🚀\n").repeated(512);
        const QByteArray dataHidden = "hidden-dotfile";
        const QString f1 = src + QStringLiteral("/one.txt");
        const QString f2 = src + QString::fromUtf8("/podkatalog 1/głęboki 2/plik spacje ąę.bin");
        const QString f3 = src + QStringLiteral("/.hidden_cfg");
        verify(writeFileBytes(f1, data1) && writeFileBytes(f2, data2) && writeFileBytes(f3, dataHidden),
               "Pos3: child payloads created");

        const QByteArray h1 = QCryptographicHash::hash(data1, QCryptographicHash::Sha256);
        const QByteArray h2 = QCryptographicHash::hash(data2, QCryptographicHash::Sha256);
        const QByteArray h3 = QCryptographicHash::hash(dataHidden, QCryptographicHash::Sha256);
        const struct stat dirBefore = lstatEntry(src);

        LocalDirectoryMoveJob job(QUrl::fromLocalFile(src), QUrl::fromLocalFile(dst));
        job.setAutoDelete(false);
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&] { return spy.count() == 1; }), "Pos3: nested dir move finished");
        verify(job.error() == KJob::NoError && job.directoryDurabilityConfirmed(),
               "Pos3: nested dir move succeeded cleanly");
        verify(!QFileInfo::exists(src) && QFileInfo::exists(dst),
               "Pos3: source disappeared and destination appeared");

        const QString df1 = dst + QStringLiteral("/one.txt");
        const QString df2 = dst + QString::fromUtf8("/podkatalog 1/głęboki 2/plik spacje ąę.bin");
        const QString df3 = dst + QStringLiteral("/.hidden_cfg");
        verify(sha256File(df1) == h1 && sha256File(df2) == h2 && sha256File(df3) == h3,
               "Pos3: all nested regular/hidden/Unicode SHA-256 checksums intact");
        verify(lstatEntry(dst).st_ino == dirBefore.st_ino,
               "Pos3: moved directory retained exact inode");
    }

    // ------------------------------------------------------------------
    // Positive 4: Same-parent directory rename + Move + Undo + Redo + Conflict Guard on Undo
    // ------------------------------------------------------------------
    {
        const QString src = srcRoot + QStringLiteral("/hist_dir_a");
        const QString sameParentDst = srcRoot + QStringLiteral("/hist_dir_renamed");
        verify(QDir().mkdir(src), "Pos4: hist_dir_a created");
        const QByteArray payload = "history-dir-bytes";
        verify(writeFileBytes(src + QStringLiteral("/item.txt"), payload), "Pos4: item created");
        const struct stat origSt = lstatEntry(src);

        LocalMoveHistory history;
        LocalDirectoryMoveJob job(QUrl::fromLocalFile(src), QUrl::fromLocalFile(sameParentDst));
        job.setAutoDelete(false);
        verify(history.recordWhenCompleted(&job, 42), "Pos4: recordWhenCompleted attached");
        QSignalSpy spy(&job, &KJob::result);
        job.start();
        verify(waitUntil([&] { return spy.count() == 1; }), "Pos4: same-parent move finished");
        verify(job.error() == KJob::NoError && history.canUndo() && history.undoSerial() == 42,
               "Pos4: durable directory move recorded in LocalMoveHistory");

        // Undo -> moves hist_dir_renamed back to hist_dir_a
        KJob *undoJob = history.undo();
        verify(undoJob != nullptr, "Pos4: undo job created");
        QSignalSpy undoSpy(undoJob, &KJob::result);
        verify(waitUntil([&] { return undoSpy.count() == 1; }), "Pos4: undo finished");
        verify(undoJob->error() == KJob::NoError && QFileInfo::exists(src + QStringLiteral("/item.txt"))
                   && !QFileInfo::exists(sameParentDst) && lstatEntry(src).st_ino == origSt.st_ino
                   && !history.canUndo() && history.canRedo(),
               "Pos4: undo restored exact directory inode and enabled redo");
        undoJob->deleteLater();

        // Redo -> moves hist_dir_a to hist_dir_renamed
        KJob *redoJob = history.redo();
        verify(redoJob != nullptr, "Pos4: redo job created");
        QSignalSpy redoSpy(redoJob, &KJob::result);
        verify(waitUntil([&] { return redoSpy.count() == 1; }), "Pos4: redo finished");
        verify(redoJob->error() == KJob::NoError && QFileInfo::exists(sameParentDst + QStringLiteral("/item.txt"))
                   && !QFileInfo::exists(src) && lstatEntry(sameParentDst).st_ino == origSt.st_ino
                   && history.canUndo() && !history.canRedo(),
               "Pos4: redo re-applied exact directory inode and re-enabled undo");
        redoJob->deleteLater();

        // Re-occupying original source name blocks Undo without overwrite!
        verify(QDir().mkdir(src), "Pos4: occupy old source path with new foreign dir");
        verify(writeFileBytes(src + QStringLiteral("/foreign.txt"), "alien"), "Pos4: foreign content created");
        KJob *blockedUndo = history.undo();
        verify(blockedUndo != nullptr, "Pos4: undo job created for occupied-source check");
        QSignalSpy blockedSpy(blockedUndo, &KJob::result);
        verify(waitUntil([&] { return blockedSpy.count() == 1; }), "Pos4: blocked undo finished");
        verify(blockedUndo->error() != KJob::NoError
                   && QFileInfo::exists(src + QStringLiteral("/foreign.txt"))
                   && QFileInfo::exists(sameParentDst + QStringLiteral("/item.txt"))
                   && history.canUndo(),
               "Pos4: undo refused to overwrite newly created dir and preserved both");
        blockedUndo->deleteLater();
        QDir(src).removeRecursively();

        // Replacing moved destination with a different directory inode blocks Undo!
        QDir(sameParentDst).removeRecursively();
        verify(QDir().mkdir(sameParentDst), "Pos4: create impostor directory with new inode");
        verify(lstatEntry(sameParentDst).st_ino != origSt.st_ino, "Pos4: impostor has distinct inode");
        KJob *idBlockedUndo = history.undo();
        verify(idBlockedUndo != nullptr, "Pos4: identity-mismatch undo job created");
        QSignalSpy idBlockedSpy(idBlockedUndo, &KJob::result);
        verify(waitUntil([&] { return idBlockedSpy.count() == 1; }), "Pos4: identity-mismatch undo finished");
        verify(idBlockedUndo->error() != KJob::NoError && QFileInfo::exists(sameParentDst)
                   && !QFileInfo::exists(src),
               "Pos4: undo refused when destination inode mismatched recorded identity");
        idBlockedUndo->deleteLater();
    }

    // ------------------------------------------------------------------
    // Negative 1-3: Existing empty dir, non-empty dir, and regular file collisions
    // ------------------------------------------------------------------
    {
        const QString sDir = srcRoot + QStringLiteral("/col_src");
        verify(QDir().mkdir(sDir) && writeFileBytes(sDir + QStringLiteral("/s.txt"), "SRC"),
               "Neg1: collision source created");

        const QString dstEmpty = dstRoot + QStringLiteral("/col_empty");
        verify(QDir().mkdir(dstEmpty), "Neg1: empty target exists");
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(sDir), QUrl::fromLocalFile(dstEmpty));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg1: empty target finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::DestinationExists
                       && !j.destinationPublished() && QFileInfo::exists(sDir + QStringLiteral("/s.txt"))
                       && QFileInfo::exists(dstEmpty),
                   "Neg1: empty dir target rejected fail-closed without overwrite");
        }

        const QString dstNonEmpty = dstRoot + QStringLiteral("/col_nonempty");
        verify(QDir().mkdir(dstNonEmpty) && writeFileBytes(dstNonEmpty + QStringLiteral("/keep.txt"), "KEEP"),
               "Neg2: non-empty target exists");
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(sDir), QUrl::fromLocalFile(dstNonEmpty));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg2: non-empty target finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::DestinationExists
                       && sha256File(dstNonEmpty + QStringLiteral("/keep.txt"))
                              == QCryptographicHash::hash("KEEP", QCryptographicHash::Sha256)
                       && QFileInfo::exists(sDir + QStringLiteral("/s.txt")),
                   "Neg2: non-empty dir target preserved completely");
        }

        const QString dstFile = dstRoot + QStringLiteral("/col_file");
        verify(writeFileBytes(dstFile, "FILE_TARGET"), "Neg3: file target exists");
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(sDir), QUrl::fromLocalFile(dstFile));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg3: file target finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::DestinationExists
                       && sha256File(dstFile) == QCryptographicHash::hash("FILE_TARGET", QCryptographicHash::Sha256)
                       && QFileInfo::exists(sDir + QStringLiteral("/s.txt")),
                   "Neg3: existing regular file at destination preserved cleanly");
        }
    }

    // ------------------------------------------------------------------
    // Negative 4 & 5: Same directory and self/descendant cycle move
    // ------------------------------------------------------------------
    {
        const QString cyc = srcRoot + QStringLiteral("/cycle_root");
        const QString nested = cyc + QStringLiteral("/child/grandchild");
        verify(QDir().mkpath(nested) && writeFileBytes(cyc + QStringLiteral("/keep.txt"), "cyc"),
               "Neg4: cycle tree created");

        const MoveOperationClassification selfEval = MoveSafetyPolicy::evaluate(
            {QUrl::fromLocalFile(cyc)}, QUrl::fromLocalFile(dstRoot), true);
        verify(selfEval.decision == MovePolicyDecision::AllowedSingleSameVolumeBtrfsDirectory,
               "Sanity: valid move qualifies");

        const MoveOperationClassification intoSelf = MoveSafetyPolicy::evaluate(
            {QUrl::fromLocalFile(cyc)}, QUrl::fromLocalFile(cyc), true);
        const MoveOperationClassification intoDesc = MoveSafetyPolicy::evaluate(
            {QUrl::fromLocalFile(cyc)}, QUrl::fromLocalFile(nested), true);
        verify(!intoSelf.allowed && !intoDesc.allowed,
               "Neg4-5: MoveSafetyPolicy blocks moving directory into itself or descendant");

        for (const QString &badTarget : {cyc, cyc + QStringLiteral("/cycle_root"), nested + QStringLiteral("/cycle_root")}) {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(cyc), QUrl::fromLocalFile(badTarget));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg4-5: cycle job finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::CycleDetected
                       && QFileInfo::exists(cyc + QStringLiteral("/keep.txt")),
                   "Neg4-5: LocalDirectoryMoveJob rejects self/descendant cycle cleanly");
        }
    }

    // ------------------------------------------------------------------
    // Negative 6 & 7: Root "/", mount point, bind mount mismatch, subvolume boundary
    // ------------------------------------------------------------------
    {
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(QStringLiteral("/"))}, QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg6: filesystem root / move refused");
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(QStringLiteral("/home"))}, QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg6: mount/subvolume root /home move refused");

        const QString bSrc = srcRoot + QStringLiteral("/boundary_dir");
        const QString bDst = dstRoot + QStringLiteral("/boundary_dir");
        verify(QDir().mkdir(bSrc), "Neg6: boundary_dir created");

        simulateDirMoveBindMountMismatch() = true;
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(bSrc)}, QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg7a: MoveSafetyPolicy blocks bind-mount mnt_id mismatch");
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(bSrc), QUrl::fromLocalFile(bDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg7a: bind-mount job finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::MountOrSubvolumeBoundary,
                   "Neg7a: bind-mount mnt_id mismatch fails closed");
        }
        resetDirMoveTestHooks();

        simulateDirMoveSubvolumeBoundary() = true;
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(bSrc)}, QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg7b: MoveSafetyPolicy blocks subvolume boundary (inode 256)");
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(bSrc), QUrl::fromLocalFile(bDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg7b: subvolume boundary job finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::MountOrSubvolumeBoundary,
                   "Neg7b: subvolume boundary fails closed");
        }
        resetDirMoveTestHooks();
    }

    // ------------------------------------------------------------------
    // Negative 8, 9, 10: Simulated EXDEV, missing RENAME_NOREPLACE, missing openat2/STATX_MNT_ID
    // ------------------------------------------------------------------
    {
        const QString kSrc = srcRoot + QStringLiteral("/kernel_fail_dir");
        const QString kDst = dstRoot + QStringLiteral("/kernel_fail_dir");
        verify(QDir().mkdir(kSrc) && writeFileBytes(kSrc + QStringLiteral("/k.txt"), "K"),
               "Neg8: kernel_fail_dir created");

        simulateDirMoveExdev() = true;
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(kSrc), QUrl::fromLocalFile(kDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg8: EXDEV job finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::MountOrSubvolumeBoundary
                       && !j.destinationPublished() && QFileInfo::exists(kSrc + QStringLiteral("/k.txt"))
                       && !QFileInfo::exists(kDst),
                   "Neg8: EXDEV fails closed with source intact and zero rename fallback");
        }
        resetDirMoveTestHooks();

        simulateDirMoveRenameNoReplaceUnsupported() = true;
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(kSrc), QUrl::fromLocalFile(kDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg9: no RENAME_NOREPLACE finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::UnsupportedKernelFeature
                       && !j.destinationPublished() && QFileInfo::exists(kSrc + QStringLiteral("/k.txt"))
                       && !QFileInfo::exists(kDst),
                   "Neg9: unsupported RENAME_NOREPLACE fails closed with zero fallback");
        }
        resetDirMoveTestHooks();

        simulateDirMoveOpenat2Unsupported() = true;
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(kSrc)}, QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg10a: missing openat2 refused fail-closed");
        resetDirMoveTestHooks();

        simulateDirMoveStatxMntIdUnsupported() = true;
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(kSrc)}, QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg10b: missing STATX_MNT_ID refused fail-closed");
        resetDirMoveTestHooks();
    }

    // ------------------------------------------------------------------
    // Negative 11 & 12: Race hooks before renameat2 (source replaced & target created concurrently)
    // ------------------------------------------------------------------
    {
        // 11: Source replaced before rename
        const QString rSrc = srcRoot + QStringLiteral("/race_src");
        const QString rOther = srcRoot + QStringLiteral("/race_alien");
        const QString rDst = dstRoot + QStringLiteral("/race_src");
        verify(QDir().mkdir(rSrc) && QDir().mkdir(rOther)
                   && writeFileBytes(rOther + QStringLiteral("/alien.txt"), "ALIEN"),
               "Neg11: race source/alien created");

        simulateDirMoveBeforeRename() = [&] {
            const QString tmpHold = srcRoot + QStringLiteral("/race_hold");
            QDir().rename(rSrc, tmpHold);
            QDir().rename(rOther, rSrc);
            QDir().rename(tmpHold, rOther);
        };
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(rSrc), QUrl::fromLocalFile(rDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg11: swapped source finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && !j.destinationPublished()
                       && QFileInfo::exists(rSrc + QStringLiteral("/alien.txt"))
                       && !QFileInfo::exists(rDst),
                   "Neg11: substituted source detected before renameat2; alien dir untouched");
        }
        resetDirMoveTestHooks();

        // 12: Target created right before SYS_renameat2(..., RENAME_NOREPLACE)
        const QString tSrc = srcRoot + QStringLiteral("/race_tgt_src");
        const QString tDst = dstRoot + QStringLiteral("/race_tgt_src");
        verify(QDir().mkdir(tSrc) && writeFileBytes(tSrc + QStringLiteral("/src.txt"), "SRC"),
               "Neg12: target race source created");
        simulateDirMoveBeforeRename() = [&] {
            verify(writeFileBytes(tDst, "CONCURRENT_TARGET"), "Neg12: concurrent target created");
        };
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(tSrc), QUrl::fromLocalFile(tDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg12: concurrent target collision finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::DestinationExists
                       && !j.destinationPublished()
                       && QFileInfo::exists(tSrc + QStringLiteral("/src.txt"))
                       && sha256File(tDst) == QCryptographicHash::hash("CONCURRENT_TARGET", QCryptographicHash::Sha256),
                   "Neg12: RENAME_NOREPLACE atomically rejects concurrent target without overwrite");
        }
        resetDirMoveTestHooks();
    }

    // ------------------------------------------------------------------
    // Negative 13 & 14: Post-rename fsync(dstParentFd) & fsync(srcParentFd) failures
    //                   + Queue partialMoveOutcome Retry Block + NO Rollback + NO History
    // ------------------------------------------------------------------
    for (const bool dstSyncFail : {true, false}) {
        const QString fSrc = srcRoot + (dstSyncFail ? QStringLiteral("/fsync_dst_dir") : QStringLiteral("/fsync_src_dir"));
        const QString fDst = dstRoot + (dstSyncFail ? QStringLiteral("/fsync_dst_dir") : QStringLiteral("/fsync_src_dir"));
        verify(QDir().mkdir(fSrc) && writeFileBytes(fSrc + QStringLiteral("/payload.txt"), "DURABILITY_DATA"),
               "Neg13-14: fsync test dir created");
        const QByteArray payloadHash = sha256File(fSrc + QStringLiteral("/payload.txt"));

        LocalMoveHistory history;
        TransferQueueController queue;
        if (dstSyncFail) {
            simulateDirMoveDestinationFsyncError() = EIO;
        } else {
            simulateDirMoveSourceFsyncError() = EIO;
        }

        LocalDirectoryMoveJob *capturedJob = nullptr;
        TransferTaskDescriptor desc;
        desc.title = QStringLiteral("H2A fsync failure");
        desc.type = TransferOperationType::Move;
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = {QUrl::fromLocalFile(fSrc)};
        desc.destination = QUrl::fromLocalFile(dstRoot);
        desc.targetUrls = {QUrl::fromLocalFile(fDst)};
        desc.startMode = TransferJobStartMode::ControllerStarts;
        desc.jobFactory = [&]() -> KJob * {
            capturedJob = new LocalDirectoryMoveJob(QUrl::fromLocalFile(fSrc), QUrl::fromLocalFile(fDst), &queue);
            history.recordWhenCompleted(capturedJob, 100);
            return capturedJob;
        };
        const TransferEnqueueResult enq = queue.enqueue(std::move(desc));
        verify(enq.accepted(), "Neg13-14: task enqueued");
        verify(waitUntil([&] {
            const auto snap = queue.taskSnapshot(enq.id);
            return snap.has_value() && snap->state == TransferTaskState::Failed;
        }), "Neg13-14: fsync failure transitions queue task to Failed (never Completed)");

        resetDirMoveTestHooks();
        verify(QFileInfo::exists(fDst + QStringLiteral("/payload.txt"))
                   && sha256File(fDst + QStringLiteral("/payload.txt")) == payloadHash,
               "Neg13-14: destination published on disk and NEVER destructively rolled back");
        verify(!QFileInfo::exists(fSrc),
               "Neg13-14: old source entry is absent after renameat2 (honest state)");
        verify(!history.canUndo(),
               "Neg13-14: unconfirmed durability outcome NOT recorded in Undo history");

        const auto snap = queue.taskSnapshot(enq.id);
        verify(snap.has_value() && !snap->errorText.isEmpty(),
               "Neg13-14: queue preserves descriptive durability error text");

        // Retry MUST be blocked (no auto retry and manual retry refused!)
        const TransferRetryEligibility retryElig = queue.checkRetryEligibility(enq.id);
        verify(!retryElig.eligible && !queue.retryTask(enq.id).accepted(),
               "Neg13-14: TransferQueueController refuses Retry on partialMoveOutcome");
    }

    // ------------------------------------------------------------------
    // Negative 15: Unauthorized filesystems (real tmpfs /tmp + simulated VFAT/NTFS-3G/ntfs3/ext4)
    // ------------------------------------------------------------------
    {
        // Real non-Btrfs filesystem on /tmp (tmpfs)
        QTemporaryDir realTmp(QStringLiteral("/tmp/h2a-tmpfs-check-XXXXXX"));
        if (realTmp.isValid()) {
            struct statfs tmpSfs {};
            if (::statfs(QFile::encodeName(realTmp.path()).constData(), &tmpSfs) == 0
                && static_cast<unsigned long>(tmpSfs.f_type) != BTRFS_SUPER_MAGIC) {
                const QString tSrc = realTmp.filePath(QStringLiteral("src_dir"));
                const QString tDstParent = realTmp.filePath(QStringLiteral("dst_parent"));
                const QString tDst = tDstParent + QStringLiteral("/src_dir");
                verify(QDir().mkdir(tSrc) && QDir().mkdir(tDstParent), "Neg15: real tmpfs fixture created");
                verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(tSrc)}, QUrl::fromLocalFile(tDstParent), true).allowed,
                       "Neg15: real non-Btrfs (tmpfs) refused by MoveSafetyPolicy");
                LocalDirectoryMoveJob j(QUrl::fromLocalFile(tSrc), QUrl::fromLocalFile(tDst));
                j.setAutoDelete(false);
                QSignalSpy spy(&j, &KJob::result);
                j.start();
                verify(waitUntil([&] { return spy.count() == 1; }), "Neg15: real tmpfs job finishes");
                verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::UnsupportedFilesystem
                           && QFileInfo::exists(tSrc) && !QFileInfo::exists(tDst),
                       "Neg15: real non-Btrfs rejected fail-closed");
            }
        }

        const QString uSrc = srcRoot + QStringLiteral("/fs_guard_dir");
        const QString uDst = dstRoot + QStringLiteral("/fs_guard_dir");
        verify(QDir().mkdir(uSrc), "Neg15: fs_guard_dir created");
        for (const unsigned long magic : {
                 static_cast<unsigned long>(MSDOS_SUPER_MAGIC),
                 static_cast<unsigned long>(FUSE_SUPER_MAGIC),
                 static_cast<unsigned long>(NTFS3_SUPER_MAGIC),
                 static_cast<unsigned long>(EXT4_SUPER_MAGIC),
                 static_cast<unsigned long>(0x12345678UL)}) {
            simulateDirMoveUnsupportedFsMagic() = magic;
            verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(uSrc)}, QUrl::fromLocalFile(dstRoot), true).allowed,
                   "Neg15: MoveSafetyPolicy blocks simulated non-Btrfs magic");
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(uSrc), QUrl::fromLocalFile(uDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg15: simulated non-Btrfs job finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::UnsupportedFilesystem
                       && QFileInfo::exists(uSrc) && !QFileInfo::exists(uDst),
                   "Neg15: non-Btrfs magic rejected fail-closed without moving");
            resetDirMoveTestHooks();
        }
    }

    // ------------------------------------------------------------------
    // Negative 16: Directory containing unsafe objects (symlink, FIFO, Unix socket, hardlink nlink > 1)
    // ------------------------------------------------------------------
    {
        const QString extTarget = srcRoot + QStringLiteral("/external_target.txt");
        verify(writeFileBytes(extTarget, "EXT"), "Neg16: external target created");

        // 16a: Symlink inside dir
        const QString symParent = srcRoot + QStringLiteral("/contains_symlink");
        const QString symDst = dstRoot + QStringLiteral("/contains_symlink");
        verify(QDir().mkpath(symParent + QStringLiteral("/sub")), "Neg16a: dir created");
        verify(::symlink("../external_target.txt",
                         QFile::encodeName(symParent + QStringLiteral("/sub/rel.lnk")).constData()) == 0,
               "Neg16a: relative symlink inside dir created");
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(symParent), QUrl::fromLocalFile(symDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg16a: symlink-containing dir finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && QFileInfo::exists(symParent) && !QFileInfo::exists(symDst),
                   "Neg16a: directory containing symlink rejected fail-closed");
        }

        // 16b: FIFO inside dir (must never open or hang)
        const QString fifoParent = srcRoot + QStringLiteral("/contains_fifo");
        const QString fifoDst = dstRoot + QStringLiteral("/contains_fifo");
        verify(QDir().mkdir(fifoParent), "Neg16b: dir created");
        verify(::mkfifo(QFile::encodeName(fifoParent + QStringLiteral("/pipe.fifo")).constData(), 0600) == 0,
               "Neg16b: FIFO inside dir created");
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(fifoParent), QUrl::fromLocalFile(fifoDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg16b: FIFO-containing dir finishes without hang");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && QFileInfo::exists(fifoParent) && !QFileInfo::exists(fifoDst),
                   "Neg16b: directory containing FIFO rejected fail-closed");
        }

        // 16c: Unix domain socket inside dir
        const QString sockParent = srcRoot + QStringLiteral("/contains_sock");
        const QString sockDst = dstRoot + QStringLiteral("/contains_sock");
        verify(QDir().mkdir(sockParent), "Neg16c: dir created");
        const QString sockPath = sockParent + QStringLiteral("/s.sock");
        if (::mknod(QFile::encodeName(sockPath).constData(), S_IFSOCK | 0600, 0) == 0) {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(sockParent), QUrl::fromLocalFile(sockDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg16c: socket-containing dir finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && QFileInfo::exists(sockParent) && !QFileInfo::exists(sockDst),
                   "Neg16c: directory containing Unix socket rejected fail-closed");
        }

        // 16d: Hardlink (nlink > 1) inside dir
        const QString hlParent = srcRoot + QStringLiteral("/contains_hardlink");
        const QString hlDst = dstRoot + QStringLiteral("/contains_hardlink");
        verify(QDir().mkdir(hlParent), "Neg16d: dir created");
        verify(::link(QFile::encodeName(extTarget).constData(),
                      QFile::encodeName(hlParent + QStringLiteral("/hl.txt")).constData()) == 0,
               "Neg16d: hardlink inside dir created");
        {
            LocalDirectoryMoveJob j(QUrl::fromLocalFile(hlParent), QUrl::fromLocalFile(hlDst));
            j.setAutoDelete(false);
            QSignalSpy spy(&j, &KJob::result);
            j.start();
            verify(waitUntil([&] { return spy.count() == 1; }), "Neg16d: hardlink-containing dir finishes");
            verify(j.error() != KJob::NoError && j.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && QFileInfo::exists(hlParent) && !QFileInfo::exists(hlDst),
                   "Neg16d: directory containing multi-link hardlink rejected fail-closed");
        }
    }

    // ------------------------------------------------------------------
    // Negative 17 & 18: Batch, mixed, symlink standalone, pre-rename cancellation, safe retry
    // ------------------------------------------------------------------
    {
        const QString d1 = srcRoot + QStringLiteral("/batch_d1");
        const QString d2 = srcRoot + QStringLiteral("/batch_d2");
        const QString f1 = srcRoot + QStringLiteral("/batch_f1.txt");
        const QString lnkDir = srcRoot + QStringLiteral("/standalone_dir.lnk");
        verify(QDir().mkdir(d1) && QDir().mkdir(d2) && writeFileBytes(f1, "F1"), "Neg17: fixtures created");
        verify(::symlink(QFile::encodeName(d1).constData(), QFile::encodeName(lnkDir).constData()) == 0,
               "Neg17: standalone symlink to dir created");

        verify(MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                                          QUrl::fromLocalFile(dstRoot), true).decision
                   == MovePolicyDecision::AllowedBatchSameVolumeBtrfsMixed,
               "Neg17a: multi-directory batch move routes to AllowedBatchSameVolumeBtrfsMixed in H.2B.2B");
        verify(MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(f1)},
                                          QUrl::fromLocalFile(dstRoot), true).decision
                   == MovePolicyDecision::AllowedBatchSameVolumeBtrfsMixed,
               "Neg17b: mixed file + directory batch move routes to AllowedBatchSameVolumeBtrfsMixed in H.2B.2B");
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(lnkDir)},
                                           QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg17c: standalone symlink-to-dir move refused fail-closed");

        // Pre-rename cancellation (100% deterministic handshake on main thread before renameat2)
        const QString canSrc = srcRoot + QStringLiteral("/cancel_dir");
        const QString canDst = dstRoot + QStringLiteral("/cancel_dir");
        verify(QDir().mkdir(canSrc) && writeFileBytes(canSrc + QStringLiteral("/keep.txt"), "CAN"),
               "Neg18: cancel_dir created");
        LocalDirectoryMoveJob canJob(QUrl::fromLocalFile(canSrc), QUrl::fromLocalFile(canDst));
        canJob.setAutoDelete(false);
        std::atomic<bool> workerAtGate{false};
        simulateDirMoveBeforeRename() = [&] {
            workerAtGate.store(true, std::memory_order_release);
            while (!canJob.isCancelRequested()) {
                QThread::msleep(1);
            }
        };
        QSignalSpy canSpy(&canJob, &KJob::result);
        canJob.start();
        verify(waitUntil([&] { return workerAtGate.load(std::memory_order_acquire); }),
               "Neg18: worker paused at pre-rename gate");
        verify(canJob.kill(KJob::EmitResult), "Neg18: main-thread kill accepted before rename");
        verify(waitUntil([&] { return canSpy.count() == 1; }), "Neg18: cancelled dir job finishes");
        resetDirMoveTestHooks();
        verify(canJob.error() == KJob::KilledJobError
                   && canJob.moveError() == LocalDirectoryMoveError::Cancelled
                   && !canJob.destinationPublished()
                   && QFileInfo::exists(canSrc + QStringLiteral("/keep.txt"))
                   && !QFileInfo::exists(canDst),
               "Neg18: cancellation before renameat2 preserves source cleanly");

        // Neg18c: Cancellation attempted after SYS_renameat2 publishes destination is refused
        const QString postCanSrc = srcRoot + QStringLiteral("/cancel_after_rename_dir");
        const QString postCanDst = dstRoot + QStringLiteral("/cancel_after_rename_dir");
        verify(QDir().mkdir(postCanSrc) && writeFileBytes(postCanSrc + QStringLiteral("/keep.txt"), "POSTCAN"),
               "Neg18c: post-rename cancel source created");
        LocalDirectoryMoveJob postCanJob(QUrl::fromLocalFile(postCanSrc), QUrl::fromLocalFile(postCanDst));
        postCanJob.setAutoDelete(false);
        std::atomic<bool> atPostRenameGate{false};
        std::atomic<bool> releasePostRenameGate{false};
        simulateDirMoveBeforeDstFsync() = [&] {
            atPostRenameGate.store(true, std::memory_order_release);
            while (!releasePostRenameGate.load(std::memory_order_acquire)) {
                QThread::msleep(1);
            }
        };
        QSignalSpy postCanSpy(&postCanJob, &KJob::result);
        postCanJob.start();
        verify(waitUntil([&] { return atPostRenameGate.load(std::memory_order_acquire); }),
               "Neg18c: worker paused at post-rename/pre-fsync gate");
        verify(!postCanJob.kill(KJob::EmitResult),
               "Neg18c: kill refused once SYS_renameat2 published destination");
        releasePostRenameGate.store(true, std::memory_order_release);
        verify(waitUntil([&] { return postCanSpy.count() == 1; }),
               "Neg18c: post-rename dir job completes normally after kill refusal");
        resetDirMoveTestHooks();
        verify(postCanJob.error() == KJob::NoError
                   && postCanJob.destinationPublished()
                   && postCanJob.directoryDurabilityConfirmed()
                   && postCanJob.outcome() == LocalDirectoryMoveOutcome::PublishedAndDurable
                   && !QFileInfo::exists(postCanSrc)
                   && sha256File(postCanDst + QStringLiteral("/keep.txt"))
                          == QCryptographicHash::hash("POSTCAN", QCryptographicHash::Sha256),
               "Neg18c: published destination preserved and finalized accurately after refused kill");

        // Safe Retry in queue after clearing non-destructive collision
        const QString retrySrc = srcRoot + QStringLiteral("/retry_clean_dir");
        const QString retryDst = dstRoot + QStringLiteral("/retry_clean_dir");
        verify(QDir().mkdir(retrySrc) && writeFileBytes(retrySrc + QStringLiteral("/r.txt"), "RETRY"),
               "Neg18b: retry source created");
        verify(writeFileBytes(retryDst, "BLOCKER"), "Neg18b: retry collision blocker created");

        TransferQueueController qRetry;
        TransferTaskDescriptor d;
        d.title = QStringLiteral("Retry dir after collision");
        d.type = TransferOperationType::Move;
        d.backend = TransferBackendKind::LocalNative;
        d.sources = {QUrl::fromLocalFile(retrySrc)};
        d.destination = QUrl::fromLocalFile(dstRoot);
        d.targetUrls = {QUrl::fromLocalFile(retryDst)};
        d.startMode = TransferJobStartMode::ControllerStarts;
        d.jobFactory = [&]() -> KJob * {
            return new LocalDirectoryMoveJob(QUrl::fromLocalFile(retrySrc), QUrl::fromLocalFile(retryDst), &qRetry);
        };
        const auto enqR = qRetry.enqueue(std::move(d));
        verify(waitUntil([&] {
            const auto snap = qRetry.taskSnapshot(enqR.id);
            return snap.has_value() && snap->state == TransferTaskState::Failed;
        }), "Neg18b: initial collided dir move fails cleanly");
        verify(!qRetry.checkRetryEligibility(enqR.id).eligible,
               "Neg18b: Retry blocked while destination blocker still exists");
        verify(QFile::remove(retryDst), "Neg18b: remove destination blocker");
        verify(qRetry.checkRetryEligibility(enqR.id).eligible,
               "Neg18b: Retry eligible once non-published collision blocker is cleared");
        const auto retried = qRetry.retryTask(enqR.id);
        verify(retried.accepted(), "Neg18b: Retry task accepted");
        verify(waitUntil([&] {
            const auto snap = qRetry.taskSnapshot(retried.id);
            return snap.has_value() && snap->state == TransferTaskState::Completed;
        }), "Neg18b: retried dir move completes cleanly");
        verify(!QFileInfo::exists(retrySrc) && sha256File(retryDst + QStringLiteral("/r.txt"))
                   == QCryptographicHash::hash("RETRY", QCryptographicHash::Sha256),
               "Neg18b: retried dir move published with intact data");

        // =========================================================================
        // H.2C.1A: Simulated ext4 VFS tests (#ifdef THISPC_TEST_HARNESS)
        // NOTE: No real ext4 mount on host; these verify the ext4 engine rules
        // via VFS simulation hooks while keeping production GUI locked.
        // =========================================================================
        {
            const QString ext4SrcRoot = srcRoot + QStringLiteral("/sim_ext4_src");
            const QString ext4DstRoot = dstRoot + QStringLiteral("/sim_ext4_dst");
            verify(QDir().mkpath(ext4SrcRoot) && QDir().mkpath(ext4DstRoot),
                   "S-EXT4-D: test roots created");

            // S-EXT4-D1: Positive single directory move with nested subdir & hidden file
            const QString d1Src = ext4SrcRoot + QStringLiteral("/d1_tree");
            const QString d1Dst = ext4DstRoot + QStringLiteral("/d1_tree");
            verify(QDir().mkpath(d1Src + QStringLiteral("/nested"))
                       && writeFileBytes(d1Src + QStringLiteral("/nested/.dot.txt"), "EXT4_DIR_OK"),
                   "S-EXT4-D1: fixture created");
            simulateDirMoveExt4Filesystem() = true;
            LocalDirectoryMoveJob ext4Ok(QUrl::fromLocalFile(d1Src), QUrl::fromLocalFile(d1Dst));
            ext4Ok.setAllowExt4(true);
            ext4Ok.setAutoDelete(false);
            ext4Ok.exec();
            resetDirMoveTestHooks();
            verify(ext4Ok.error() == KJob::NoError
                       && ext4Ok.destinationPublished()
                       && ext4Ok.directoryDurabilityConfirmed()
                       && ext4Ok.outcome() == LocalDirectoryMoveOutcome::PublishedAndDurable
                       && !QFileInfo::exists(d1Src)
                       && sha256File(d1Dst + QStringLiteral("/nested/.dot.txt"))
                              == QCryptographicHash::hash("EXT4_DIR_OK", QCryptographicHash::Sha256),
                   "S-EXT4-D1 [SIMULATED_EXT4_VFS]: ext4 directory move succeeds when allowExt4=true");

            // S-EXT4-D2: Default allowExt4=false refuses ext4 fail-closed
            const QString d2Src = ext4SrcRoot + QStringLiteral("/d2_locked");
            const QString d2Dst = ext4DstRoot + QStringLiteral("/d2_locked");
            verify(QDir().mkdir(d2Src) && writeFileBytes(d2Src + QStringLiteral("/a.txt"), "LOCK"),
                   "S-EXT4-D2: fixture created");
            simulateDirMoveExt4Filesystem() = true;
            LocalDirectoryMoveJob ext4Locked(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d2Dst));
            ext4Locked.setAutoDelete(false);
            ext4Locked.exec();
            resetDirMoveTestHooks();
            verify(ext4Locked.error() != KJob::NoError
                       && ext4Locked.moveError() == LocalDirectoryMoveError::UnsupportedFilesystem
                       && !ext4Locked.destinationPublished()
                       && QFileInfo::exists(d2Src + QStringLiteral("/a.txt"))
                       && !QFileInfo::exists(d2Dst),
                   "S-EXT4-D2 [SIMULATED_EXT4_VFS]: ext4 refused fail-closed when allowExt4=false");

            // S-EXT4-D3: Missing STATX_MNT_ID_UNIQUE refused fail-closed
            simulateDirMoveExt4Filesystem() = true;
            simulateDirMoveExt4StatxMntIdUniqueMissing() = true;
            LocalDirectoryMoveJob ext4NoUnique(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d2Dst));
            ext4NoUnique.setAllowExt4(true);
            ext4NoUnique.setAutoDelete(false);
            ext4NoUnique.exec();
            resetDirMoveTestHooks();
            verify(ext4NoUnique.error() != KJob::NoError
                       && ext4NoUnique.moveError() == LocalDirectoryMoveError::UnsupportedKernelFeature
                       && QFileInfo::exists(d2Src + QStringLiteral("/a.txt"))
                       && !QFileInfo::exists(d2Dst),
                   "S-EXT4-D3 [SIMULATED_EXT4_VFS]: missing STATX_MNT_ID_UNIQUE refused fail-closed");

            // S-EXT4-D4: FS_IOC_GETFLAGS error refused fail-closed
            simulateDirMoveExt4Filesystem() = true;
            simulateDirMoveExt4FlagsError() = true;
            LocalDirectoryMoveJob ext4FlagsErr(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d2Dst));
            ext4FlagsErr.setAllowExt4(true);
            ext4FlagsErr.setAutoDelete(false);
            ext4FlagsErr.exec();
            resetDirMoveTestHooks();
            verify(ext4FlagsErr.error() != KJob::NoError
                       && ext4FlagsErr.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && QFileInfo::exists(d2Src + QStringLiteral("/a.txt"))
                       && !QFileInfo::exists(d2Dst),
                   "S-EXT4-D4 [SIMULATED_EXT4_VFS]: FS_IOC_GETFLAGS failure refused fail-closed");

            // S-EXT4-D5: FS_CASEFOLD_FL refused (both all-dirs and deep child inode only)
            simulateDirMoveExt4Filesystem() = true;
            simulateDirMoveExt4CasefoldAllDirs() = true;
            LocalDirectoryMoveJob ext4CfAll(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d2Dst));
            ext4CfAll.setAllowExt4(true);
            ext4CfAll.setAutoDelete(false);
            ext4CfAll.exec();
            resetDirMoveTestHooks();
            verify(ext4CfAll.error() != KJob::NoError
                       && ext4CfAll.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && QFileInfo::exists(d2Src + QStringLiteral("/a.txt")),
                   "S-EXT4-D5a [SIMULATED_EXT4_VFS]: FS_CASEFOLD_FL on dirs refused fail-closed");

            const QString d5Child = d2Src + QStringLiteral("/cf_child");
            verify(QDir().mkdir(d5Child), "S-EXT4-D5b: child dir created");
            struct stat cfChildSt {};
            verify(::lstat(QFile::encodeName(d5Child).constData(), &cfChildSt) == 0, "S-EXT4-D5b: lstat");
            simulateDirMoveExt4Filesystem() = true;
            simulateDirMoveExt4CasefoldInode() = static_cast<quint64>(cfChildSt.st_ino);
            LocalDirectoryMoveJob ext4CfChild(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d2Dst));
            ext4CfChild.setAllowExt4(true);
            ext4CfChild.setAutoDelete(false);
            ext4CfChild.exec();
            resetDirMoveTestHooks();
            verify(ext4CfChild.error() != KJob::NoError
                       && ext4CfChild.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && QFileInfo::exists(d5Child) && !QFileInfo::exists(d2Dst),
                   "S-EXT4-D5b [SIMULATED_EXT4_VFS]: deep child FS_CASEFOLD_FL refused fail-closed");

            // S-EXT4-D6: FS_ENCRYPT_FL refused fail-closed
            simulateDirMoveExt4Filesystem() = true;
            simulateDirMoveExt4Encrypt() = true;
            LocalDirectoryMoveJob ext4Enc(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d2Dst));
            ext4Enc.setAllowExt4(true);
            ext4Enc.setAutoDelete(false);
            ext4Enc.exec();
            resetDirMoveTestHooks();
            verify(ext4Enc.error() != KJob::NoError
                       && ext4Enc.moveError() == LocalDirectoryMoveError::ContainmentViolation
                       && QFileInfo::exists(d2Src + QStringLiteral("/a.txt")),
                   "S-EXT4-D6 [SIMULATED_EXT4_VFS]: FS_ENCRYPT_FL refused fail-closed");

            // S-EXT4-D7: Collision, hidden symlink, RENAME_NOREPLACE unsupported, EXDEV all refused
            const QString d7Col = ext4DstRoot + QStringLiteral("/d2_locked");
            verify(writeFileBytes(d7Col, "COL"), "S-EXT4-D7: blocker created");
            simulateDirMoveExt4Filesystem() = true;
            LocalDirectoryMoveJob ext4Col(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d7Col));
            ext4Col.setAllowExt4(true);
            ext4Col.setAutoDelete(false);
            ext4Col.exec();
            resetDirMoveTestHooks();
            verify(ext4Col.error() != KJob::NoError
                       && ext4Col.moveError() == LocalDirectoryMoveError::DestinationExists
                       && QFileInfo::exists(d2Src + QStringLiteral("/a.txt")),
                   "S-EXT4-D7a [SIMULATED_EXT4_VFS]: collision refused without overwrite");
            verify(QFile::remove(d7Col), "S-EXT4-D7: remove blocker");

            simulateDirMoveExt4Filesystem() = true;
            simulateDirMoveRenameNoReplaceUnsupported() = true;
            LocalDirectoryMoveJob ext4NoReplace(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d2Dst));
            ext4NoReplace.setAllowExt4(true);
            ext4NoReplace.setAutoDelete(false);
            ext4NoReplace.exec();
            resetDirMoveTestHooks();
            verify(ext4NoReplace.error() != KJob::NoError
                       && ext4NoReplace.moveError() == LocalDirectoryMoveError::UnsupportedKernelFeature
                       && QFileInfo::exists(d2Src + QStringLiteral("/a.txt")),
                   "S-EXT4-D7b [SIMULATED_EXT4_VFS]: RENAME_NOREPLACE unsupported refused without rename() fallback");

            simulateDirMoveExt4Filesystem() = true;
            simulateDirMoveExdev() = true;
            LocalDirectoryMoveJob ext4Exdev(QUrl::fromLocalFile(d2Src), QUrl::fromLocalFile(d2Dst));
            ext4Exdev.setAllowExt4(true);
            ext4Exdev.setAutoDelete(false);
            ext4Exdev.exec();
            resetDirMoveTestHooks();
            verify(ext4Exdev.error() != KJob::NoError
                       && ext4Exdev.moveError() == LocalDirectoryMoveError::MountOrSubvolumeBoundary
                       && QFileInfo::exists(d2Src + QStringLiteral("/a.txt")),
                   "S-EXT4-D7c [SIMULATED_EXT4_VFS]: EXDEV refused without Copy+Delete fallback");
        }
    }

    // ------------------------------------------------------------------
    // H.2C-U6.1: Single Directory Undo/Redo Strict History Hardening
    // ------------------------------------------------------------------
    {
        const QString u61Root = rootDir.filePath(QStringLiteral("u61"));
        verify(QDir().mkpath(u61Root), "U61 root created");

        const QByteArray payload34("ALPHA-34-BYTES-0123456789ABCDEFGH!");
        const QByteArray altPayload34("OMEGA-34-BYTES-0123456789ABCDEFGH!");
        verify(payload34.size() == 34 && altPayload34.size() == 34, "U61 payloads are 34 bytes");

        auto makeU61Tree = [&](const QString &dirPath, const QByteArray &innerBytes) {
            verify(QDir().mkpath(dirPath + QStringLiteral("/sub")), "U61 sub created");
            verify(writeFileBytes(dirPath + QStringLiteral("/sub/inner.txt"), innerBytes),
                   "U61 inner.txt written");
            verify(writeFileBytes(dirPath + QStringLiteral("/top.txt"), QByteArray("TOP-DATA")),
                   "U61 top.txt written");
        };

        auto readBytes = [](const QString &path) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return QByteArray();
            return f.readAll();
        };

        // U61-T1: Plain LocalDirectoryMoveJob does NOT hash contents
        {
            const QString s = u61Root + QStringLiteral("/t1_src");
            const QString d = u61Root + QStringLiteral("/t1_dst");
            makeU61Tree(s, payload34);
            resetDirMoveTestHooks();
            LocalDirectoryMoveJob job(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
            job.setAutoDelete(false);
            job.exec();
            verify(job.error() == KJob::NoError && job.destinationPublished()
                       && job.directoryDurabilityConfirmed(),
                   "U61-T1: plain LocalDirectoryMoveJob succeeds");
            verify(dirMoveSnapshotFilesHashedCount().load(std::memory_order_relaxed) == 0,
                   "U61-T1: plain LocalDirectoryMoveJob hashes 0 files");
            verify(!job.movedSnapshot().valid,
                   "U61-T1: plain LocalDirectoryMoveJob does not populate movedSnapshot");
        }

        // U61-T2: Plain LocalDirectoryMoveJob moves >512 MiB sparse file without strict snapshot limit
        {
            const QString s = u61Root + QStringLiteral("/t2_src");
            const QString d = u61Root + QStringLiteral("/t2_dst");
            verify(QDir().mkpath(s), "U61-T2 dir created");
            {
                QFile sparse(s + QStringLiteral("/sparse520m.bin"));
                verify(sparse.open(QIODevice::WriteOnly | QIODevice::Truncate), "U61-T2 sparse open");
                verify(sparse.resize(qint64(520) * 1024 * 1024), "U61-T2 sparse resize 520 MiB");
            }
            resetDirMoveTestHooks();
            LocalDirectoryMoveJob job(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
            job.setAutoDelete(false);
            job.exec();
            verify(job.error() == KJob::NoError && job.destinationPublished()
                       && QFileInfo::exists(d + QStringLiteral("/sparse520m.bin")),
                   "U61-T2: plain >512 MiB sparse directory move succeeds");
            verify(dirMoveSnapshotFilesHashedCount().load(std::memory_order_relaxed) == 0,
                   "U61-T2: plain >512 MiB move hashes 0 files");
        }

        // U61-T3: History-backed LocalDirectoryMoveJob refuses >512 MiB before renameat2
        {
            const QString s = u61Root + QStringLiteral("/t3_src");
            const QString d = u61Root + QStringLiteral("/t3_dst");
            verify(QDir().mkpath(s), "U61-T3 dir created");
            {
                QFile sparse(s + QStringLiteral("/sparse520m.bin"));
                verify(sparse.open(QIODevice::WriteOnly | QIODevice::Truncate), "U61-T3 sparse open");
                verify(sparse.resize(qint64(520) * 1024 * 1024), "U61-T3 sparse resize 520 MiB");
            }
            resetDirMoveTestHooks();
            LocalMoveHistory history;
            auto *job = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
            job->setAutoDelete(false);
            verify(history.recordWhenCompleted(job, 300), "U61-T3: recordWhenCompleted accepted");
            verify(job->captureStrictDirectorySnapshot(),
                   "U61-T3: recordWhenCompleted enabled strict snapshot opt-in");
            job->exec();
            verify(job->error() != KJob::NoError && !job->destinationPublished()
                       && QFileInfo::exists(s + QStringLiteral("/sparse520m.bin"))
                       && !QFileInfo::exists(d),
                   "U61-T3: history-backed >512 MiB directory move refused before renameat2");
            verify(!history.canUndo() && !history.canRedo(),
                   "U61-T3: refused >512 MiB move leaves history empty");
            delete job;
        }

        // U61-T4: Late opt-in after start() rejected and fences history (even prior record)
        {
            const QString sPrev = u61Root + QStringLiteral("/t4_prev_src");
            const QString dPrev = u61Root + QStringLiteral("/t4_prev_dst");
            makeU61Tree(sPrev, payload34);
            LocalMoveHistory history;
            auto *prevJob = new LocalDirectoryMoveJob(QUrl::fromLocalFile(sPrev), QUrl::fromLocalFile(dPrev));
            prevJob->setAutoDelete(false);
            verify(history.recordWhenCompleted(prevJob, 399), "U61-T4: record prior move");
            prevJob->exec();
            QCoreApplication::processEvents();
            verify(prevJob->error() == KJob::NoError && history.canUndo(), "U61-T4: prior move undoable");
            delete prevJob;

            const QString s = u61Root + QStringLiteral("/t4_src");
            const QString d = u61Root + QStringLiteral("/t4_dst");
            makeU61Tree(s, payload34);
            resetDirMoveTestHooks();
            auto *job = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
            job->setAutoDelete(false);
            QSignalSpy doneSpy(job, &KJob::result);
            job->start();
            verify(!job->setCaptureStrictDirectorySnapshot(true),
                   "U61-T4: setCaptureStrictDirectorySnapshot(true) rejected after start()");
            verify(!history.recordWhenCompleted(job, 400),
                   "U61-T4: recordWhenCompleted rejected after start()");
            verify(!history.canUndo() && !history.canRedo(),
                   "U61-T4: late registration fences history irreversible (even prior record)");
            verify(waitUntil([&] { return doneSpy.count() >= 1; }), "U61-T4: job finishes");
            verify(job->error() == KJob::NoError && QFileInfo::exists(d),
                   "U61-T4: started plain job still completes move");
            delete job;
        }

        // U61-T5: Public manual directory history recording API removed (compile-time verified)
        // + double-attachment, unconfirmed durability, and cancelled job protections
        {
            constexpr bool hasPublicManualDirRecord = []<typename H>(H *) {
                return requires(H &h, const QUrl &u, const LocalDirectoryIdentity &id) {
                    h.recordCompletedDirectoryMove(u, u, id);
                };
            }(static_cast<LocalMoveHistory *>(nullptr));
            static_assert(!hasPublicManualDirRecord,
                          "H.2C-U6.1A: public recordCompletedDirectoryMove() must not exist");
            verify(!hasPublicManualDirRecord,
                   "U61-T5a: public recordCompletedDirectoryMove() API removed");

            // U61-T5b: Double attachment on same LocalDirectoryMoveJob rejected & fences history
            {
                const QString s = u61Root + QStringLiteral("/t5b_src");
                const QString d = u61Root + QStringLiteral("/t5b_dst");
                makeU61Tree(s, payload34);
                LocalMoveHistory h1;
                LocalMoveHistory h2;
                auto *job = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
                job->setAutoDelete(false);
                verify(h1.recordWhenCompleted(job, 500), "U61-T5b: first attachment succeeds");
                verify(!h2.recordWhenCompleted(job, 501),
                       "U61-T5b: second attachment on same job refused");
                verify(!h1.recordWhenCompleted(job, 502),
                       "U61-T5b: duplicate attachment on same history refused");
                job->exec();
                QCoreApplication::processEvents();
                verify(job->error() == KJob::NoError && h1.count() == 1 && h1.undoSerial() == 500,
                       "U61-T5b: duplicate attachment on h1 does not create a second record");
                verify(h2.count() == 0 && !h2.canUndo(),
                       "U61-T5b: rejected h2 recorded nothing");
                delete job;
            }

            // U61-T5c: Unconfirmed durability (fsync error on initial Move) refuses record & fences
            {
                const QString s = u61Root + QStringLiteral("/t5c_src");
                const QString d = u61Root + QStringLiteral("/t5c_dst");
                makeU61Tree(s, payload34);
                resetDirMoveTestHooks();
                LocalMoveHistory history;
                auto *job = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
                job->setAutoDelete(false);
                verify(history.recordWhenCompleted(job, 503), "U61-T5c: attach");
                simulateDirMoveDestinationFsyncError() = EIO;
                job->exec();
                QCoreApplication::processEvents();
                resetDirMoveTestHooks();
                verify(job->error() != KJob::NoError && job->destinationPublished()
                           && !job->directoryDurabilityConfirmed()
                           && job->property("thispcPartialMoveOutcome").toBool(),
                       "U61-T5c: partial unconfirmed durability outcome");
                verify(history.count() == 0 && !history.canUndo() && !history.canRedo(),
                       "U61-T5c: unconfirmed durability records nothing and fences history");
                delete job;
            }

            // U61-T5d: Cancelled job blocks registration
            {
                const QString s = u61Root + QStringLiteral("/t5d_src");
                const QString d = u61Root + QStringLiteral("/t5d_dst");
                makeU61Tree(s, payload34);
                resetDirMoveTestHooks();
                LocalMoveHistory history;
                auto *job = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
                job->setAutoDelete(false);
                verify(history.recordWhenCompleted(job, 504), "U61-T5d: attach");
                job->kill(KJob::EmitResult);
                QCoreApplication::processEvents();
                verify(job->error() != KJob::NoError && history.count() == 0 && !history.canUndo(),
                       "U61-T5d: cancelled job not recorded");
                delete job;
            }
        }

        // U61-T6: Full Move -> Undo -> Redo cycle with strict snapshot
        {
            const QString s = u61Root + QStringLiteral("/t6_src");
            const QString d = u61Root + QStringLiteral("/t6_dst");
            makeU61Tree(s, payload34);
            struct stat origSt {};
            verify(::lstat(QFile::encodeName(s).constData(), &origSt) == 0, "U61-T6: lstat src");

            resetDirMoveTestHooks();
            LocalMoveHistory history;
            auto *job = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
            job->setAutoDelete(false);
            verify(history.recordWhenCompleted(job, 600), "U61-T6: recordWhenCompleted");
            job->exec();
            QCoreApplication::processEvents();
            verify(job->error() == KJob::NoError && job->movedSnapshot().valid
                       && job->movedSnapshot().strictContentDigest
                       && job->movedSnapshot().treeDigest.size() == 32,
                   "U61-T6: initial move captures valid 32-byte strict snapshot");
            verify(dirMoveSnapshotFilesHashedCount().load(std::memory_order_relaxed) == 2,
                   "U61-T6: initial strict move hashed both files");
            verify(history.canUndo() && !history.canRedo(), "U61-T6: history ready for Undo");
            delete job;

            KJob *undoJob = history.undo();
            verify(undoJob != nullptr, "U61-T6: undo job started");
            QSignalSpy undoSpy(undoJob, &KJob::result);
            verify(waitUntil([&] { return undoSpy.count() >= 1; }), "U61-T6: undo finishes");
            QCoreApplication::processEvents();
            verify(undoJob->error() == KJob::NoError, "U61-T6: undo succeeds");
            delete undoJob;

            struct stat afterUndoSt {};
            verify(::lstat(QFile::encodeName(s).constData(), &afterUndoSt) == 0
                       && afterUndoSt.st_ino == origSt.st_ino
                       && !QFileInfo::exists(d)
                       && readBytes(s + QStringLiteral("/sub/inner.txt")) == payload34,
                   "U61-T6: undo restored directory with original inode and contents");
            verify(!history.canUndo() && history.canRedo(), "U61-T6: history ready for Redo");

            KJob *redoJob = history.redo();
            verify(redoJob != nullptr, "U61-T6: redo job started");
            QSignalSpy redoSpy(redoJob, &KJob::result);
            verify(waitUntil([&] { return redoSpy.count() >= 1; }), "U61-T6: redo finishes");
            QCoreApplication::processEvents();
            verify(redoJob->error() == KJob::NoError, "U61-T6: redo succeeds");
            delete redoJob;

            struct stat afterRedoSt {};
            verify(::lstat(QFile::encodeName(d).constData(), &afterRedoSt) == 0
                       && afterRedoSt.st_ino == origSt.st_ino
                       && !QFileInfo::exists(s)
                       && readBytes(d + QStringLiteral("/sub/inner.txt")) == payload34,
                   "U61-T6: redo restored destination with original inode and contents");
            verify(history.canUndo() && !history.canRedo(), "U61-T6: history back at Undo-ready");
        }

        // U61-T7: Negative Undo fail-closed matrix
        auto runNegativeUndoCase = [&](const QString &name, auto mutateDst) {
            const QString s = u61Root + QStringLiteral("/") + name + QStringLiteral("_src");
            const QString d = u61Root + QStringLiteral("/") + name + QStringLiteral("_dst");
            makeU61Tree(s, payload34);
            resetDirMoveTestHooks();
            LocalMoveHistory history;
            auto *job = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
            job->setAutoDelete(false);
            verify(history.recordWhenCompleted(job, 700), "U61-T7: recordWhenCompleted");
            job->exec();
            QCoreApplication::processEvents();
            verify(job->error() == KJob::NoError && history.canUndo(), "U61-T7: initial move done");
            delete job;

            mutateDst(s, d);

            KJob *undoJob = history.undo();
            verify(undoJob != nullptr, "U61-T7: undo job created");
            QSignalSpy undoSpy(undoJob, &KJob::result);
            verify(waitUntil([&] { return undoSpy.count() >= 1; }), "U61-T7: undo finished");
            QCoreApplication::processEvents();
            verify(undoJob->error() != KJob::NoError,
                   qPrintable(QStringLiteral("U61-T7 (%1): undo refused fail-closed").arg(name)));
            verify(!undoJob->property("thispcDestinationPublished").toBool(),
                   qPrintable(QStringLiteral("U61-T7 (%1): destination not published").arg(name)));
            verify(QFileInfo::exists(d),
                   qPrintable(QStringLiteral("U61-T7 (%1): dst preserved").arg(name)));
            verify(history.canUndo() && !history.canRedo(),
                   qPrintable(QStringLiteral("U61-T7 (%1): history cursor unchanged").arg(name)));
            delete undoJob;
        };

        // U61-T7a: Same-size 34B -> 34B mutation preserving inode
        runNegativeUndoCase(QStringLiteral("t7a_samesize"), [&](const QString &, const QString &d) {
            QFile f(d + QStringLiteral("/sub/inner.txt"));
            verify(f.open(QIODevice::ReadWrite | QIODevice::Truncate), "U61-T7a: open in-place");
            verify(f.write(altPayload34) == 34 && f.flush(), "U61-T7a: write 34B");
        });

        // U61-T7b: Internal file inode replacement with identical bytes
        runNegativeUndoCase(QStringLiteral("t7b_file_ino"), [&](const QString &, const QString &d) {
            const QString p = d + QStringLiteral("/sub/inner.txt");
            verify(QFile::remove(p), "U61-T7b: remove inner.txt");
            verify(writeFileBytes(p, payload34), "U61-T7b: recreate inner.txt with same bytes");
        });

        // U61-T7c: Root directory inode replacement with identical subtree
        runNegativeUndoCase(QStringLiteral("t7c_root_ino"), [&](const QString &, const QString &d) {
            const QString aside = d + QStringLiteral("_aside");
            verify(QDir().rename(d, aside), "U61-T7c: rename dst aside");
            makeU61Tree(d, payload34);
        });

        // U61-T7d: Internal file rename
        runNegativeUndoCase(QStringLiteral("t7d_rename"), [&](const QString &, const QString &d) {
            verify(QFile::rename(d + QStringLiteral("/top.txt"),
                                 d + QStringLiteral("/top_renamed.txt")),
                   "U61-T7d: rename top.txt");
        });

        // U61-T7e: Extra file added
        runNegativeUndoCase(QStringLiteral("t7e_add"), [&](const QString &, const QString &d) {
            verify(writeFileBytes(d + QStringLiteral("/extra.txt"), QByteArray("X")),
                   "U61-T7e: add extra.txt");
        });

        // U61-T7f: Internal file deleted
        runNegativeUndoCase(QStringLiteral("t7f_delete"), [&](const QString &, const QString &d) {
            verify(QFile::remove(d + QStringLiteral("/top.txt")), "U61-T7f: remove top.txt");
        });

        // U61-T7g: File size changed
        runNegativeUndoCase(QStringLiteral("t7g_resize"), [&](const QString &, const QString &d) {
            verify(writeFileBytes(d + QStringLiteral("/top.txt"), QByteArray("TOP-DATA-EXPANDED")),
                   "U61-T7g: resize top.txt");
        });

        // U61-T7h: Collision at original src before Undo
        runNegativeUndoCase(QStringLiteral("t7h_collision"), [&](const QString &s, const QString &) {
            verify(QDir().mkpath(s), "U61-T7h: create collision dir at src");
            verify(writeFileBytes(s + QStringLiteral("/blocker.txt"), QByteArray("KEEP")),
                   "U61-T7h: write collision blocker");
        });
        verify(readBytes(u61Root + QStringLiteral("/t7h_collision_src/blocker.txt")) == QByteArray("KEEP"),
               "U61-T7h: collision blocker untouched");

        // U61-T8: Partial outcome during Undo fences history irreversible
        {
            const QString s = u61Root + QStringLiteral("/t8_src");
            const QString d = u61Root + QStringLiteral("/t8_dst");
            makeU61Tree(s, payload34);
            resetDirMoveTestHooks();
            LocalMoveHistory history;
            auto *job = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s), QUrl::fromLocalFile(d));
            job->setAutoDelete(false);
            verify(history.recordWhenCompleted(job, 800), "U61-T8: recordWhenCompleted");
            job->exec();
            QCoreApplication::processEvents();
            verify(job->error() == KJob::NoError && history.canUndo(), "U61-T8: setup move done");
            delete job;

            simulateDirMoveDestinationFsyncError() = EIO;
            KJob *undoJob = history.undo();
            verify(undoJob != nullptr, "U61-T8: undo started");
            QSignalSpy undoSpy(undoJob, &KJob::result);
            verify(waitUntil([&] { return undoSpy.count() >= 1; }), "U61-T8: undo finished");
            QCoreApplication::processEvents();
            resetDirMoveTestHooks();
            verify(undoJob->error() != KJob::NoError
                       && undoJob->property("thispcPartialMoveOutcome").toBool(),
                   "U61-T8: partial outcome flagged on fsync error");
            verify(!history.canUndo() && !history.canRedo(),
                   "U61-T8: partial outcome fences history irreversible");
            delete undoJob;
        }

        // U61-T9: Two-record stack: failing undo on #2 preserves serial/cursor and leaves #1 intact
        {
            const QString s1 = u61Root + QStringLiteral("/t9_s1");
            const QString d1 = u61Root + QStringLiteral("/t9_d1");
            const QString s2 = u61Root + QStringLiteral("/t9_s2");
            const QString d2 = u61Root + QStringLiteral("/t9_d2");
            makeU61Tree(s1, payload34);
            makeU61Tree(s2, payload34);

            resetDirMoveTestHooks();
            LocalMoveHistory history;
            auto *j1 = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s1), QUrl::fromLocalFile(d1));
            j1->setAutoDelete(false);
            verify(history.recordWhenCompleted(j1, 11), "U61-T9: record #1");
            j1->exec();
            QCoreApplication::processEvents();
            verify(j1->error() == KJob::NoError, "U61-T9: move #1 succeeds");
            delete j1;

            auto *j2 = new LocalDirectoryMoveJob(QUrl::fromLocalFile(s2), QUrl::fromLocalFile(d2));
            j2->setAutoDelete(false);
            verify(history.recordWhenCompleted(j2, 22), "U61-T9: record #2");
            j2->exec();
            QCoreApplication::processEvents();
            verify(j2->error() == KJob::NoError && history.undoSerial() == 22,
                   "U61-T9: move #2 succeeds with serial 22");
            delete j2;

            verify(writeFileBytes(d2 + QStringLiteral("/sub/inner.txt"), altPayload34),
                   "U61-T9: mutate #2");
            KJob *u2 = history.undo();
            verify(u2 != nullptr, "U61-T9: undo #2 started");
            QSignalSpy u2Spy(u2, &KJob::result);
            verify(waitUntil([&] { return u2Spy.count() >= 1; }), "U61-T9: undo #2 finishes");
            QCoreApplication::processEvents();
            verify(u2->error() != KJob::NoError && history.canUndo() && !history.canRedo()
                       && history.undoSerial() == 22,
                   "U61-T9: refused undo #2 preserves cursor and undoSerial == 22");
            delete u2;
        }
    }

    qInfo("PASS: %d LocalDirectoryMoveJob/history assertions", checks);
    return 0;
}
