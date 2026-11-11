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

        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(d2)},
                                           QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg17a: multi-directory batch move refused fail-closed");
        verify(!MoveSafetyPolicy::evaluate({QUrl::fromLocalFile(d1), QUrl::fromLocalFile(f1)},
                                           QUrl::fromLocalFile(dstRoot), true).allowed,
               "Neg17b: mixed file + directory batch move refused fail-closed");
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
    }

    qInfo("PASS: %d LocalDirectoryMoveJob/history assertions", checks);
    return 0;
}
