/*
 * Safe atomic same-volume Btrfs batch regular-file move (Stage 5.3.2H.2B.1).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localdirectorymovejob.h"

#include <KIO/Job>
#include <KJob>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QLocale>
#include <QMetaObject>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <linux/stat.h>
#include <memory>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef STATX_MTIME
#define STATX_MTIME 0x00000040U
#endif
#ifndef STATX_CTIME
#define STATX_CTIME 0x00000080U
#endif

enum class BatchMoveError {
    None,
    Cancelled,
    InvalidInput,
    UnsupportedFilesystem,
    UnsupportedKernelFeature,
    MountOrSubvolumeBoundary,
    ContainmentViolation,
    CollisionInBatch,
    DestinationExists,
    AtomicMove,
    PostRenameVerification,
    Durability,
};

enum class BatchMoveItemState {
    Pending,
    InProgress,
    PublishedUnconfirmedDurability,
    DurableCompleted,
    FailedBeforePublish,
    FailedDurability,
    SkippedAfterAbort,
};

enum class BatchMoveOutcome {
    FullRefusalBeforeExecution,
    FullSuccess,
    PartialSuccessCleanStop,
    PartialOutcomeWithUnconfirmedDurability,
};

Q_DECLARE_METATYPE(BatchMoveError)
Q_DECLARE_METATYPE(BatchMoveItemState)
Q_DECLARE_METATYPE(BatchMoveOutcome)

struct LocalRegularFileIdentity {
    quint64 devMajor = 0;
    quint64 devMinor = 0;
    quint64 stDev = 0;
    quint64 ino = 0;
    quint64 mntId = 0;
    bool hasMntId = false;
    bool hasUniqueMntId = false;
    quint64 subvolId = 0;
    bool hasSubvol = false;
    quint64 size = 0;
    qint64 mtimeSec = 0;
    quint32 mtimeNsec = 0;
    qint64 ctimeSec = 0;
    quint32 ctimeNsec = 0;
    quint32 nlink = 0;
    bool valid = false;

    static LocalRegularFileIdentity fromStat(const struct statx &stx, const struct stat &st)
    {
        LocalRegularFileIdentity id;
        const bool hasType = (stx.stx_mask & STATX_TYPE) != 0;
        const bool hasIno = (stx.stx_mask & STATX_INO) != 0;
        const bool hasMnt = (stx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) != 0;
        const bool hasNlink = (stx.stx_mask & STATX_NLINK) != 0;
        const bool hasSize = (stx.stx_mask & STATX_SIZE) != 0;
        const bool hasMtime = (stx.stx_mask & STATX_MTIME) != 0;
        const bool hasCtime = (stx.stx_mask & STATX_CTIME) != 0;
        if (!hasType || !hasIno || !hasMnt || !hasNlink || !hasSize || !hasMtime || !hasCtime
            || !S_ISREG(stx.stx_mode)) {
            return id;
        }
        id.devMajor = stx.stx_dev_major;
        id.devMinor = stx.stx_dev_minor;
        id.stDev = static_cast<quint64>(st.st_dev);
        id.ino = static_cast<quint64>(stx.stx_ino);
        id.mntId = static_cast<quint64>(stx.stx_mnt_id);
        id.hasMntId = true;
        id.hasUniqueMntId = (stx.stx_mask & STATX_MNT_ID_UNIQUE) != 0;
        id.hasSubvol = (stx.stx_mask & STATX_SUBVOL) != 0;
        id.subvolId = id.hasSubvol ? static_cast<quint64>(stx.stx_subvol) : 0;
        id.size = static_cast<quint64>(stx.stx_size);
        id.mtimeSec = static_cast<qint64>(stx.stx_mtime.tv_sec);
        id.mtimeNsec = static_cast<quint32>(stx.stx_mtime.tv_nsec);
        id.ctimeSec = static_cast<qint64>(stx.stx_ctime.tv_sec);
        id.ctimeNsec = static_cast<quint32>(stx.stx_ctime.tv_nsec);
        id.nlink = static_cast<quint32>(stx.stx_nlink);
        id.valid = true;
        return id;
    }

    bool sameMountScope(const LocalDirectoryIdentity &dirId) const
    {
        if (!valid || !dirId.valid || !hasMntId || !dirId.hasMntId) {
            return false;
        }
        if (hasUniqueMntId != dirId.hasUniqueMntId || mntId != dirId.mntId) {
            return false;
        }
        if (devMajor != dirId.devMajor || devMinor != dirId.devMinor || stDev != dirId.stDev) {
            return false;
        }
        if (hasSubvol != dirId.hasSubvol) {
            return false;
        }
        if (hasSubvol && subvolId != dirId.subvolId) {
            return false;
        }
        return true;
    }

    bool sameObject(const LocalRegularFileIdentity &other) const
    {
        if (!valid || !other.valid) {
            return false;
        }
        if (devMajor != other.devMajor || devMinor != other.devMinor
            || stDev != other.stDev || ino != other.ino) {
            return false;
        }
        if (hasUniqueMntId == other.hasUniqueMntId && mntId != other.mntId) {
            return false;
        }
        if (hasSubvol && other.hasSubvol && subvolId != other.subvolId) {
            return false;
        }
        return true;
    }

    // Checks object identity plus nlink==1, byte size, and nanosecond mtime/ctime.
    // Note: catches in-place VFS content edits even when inode is unchanged; offline
    // raw block mutations or privileged clock spoofing outside VFS remain out of band.
    bool strictlyUnchangedForHistory(const LocalRegularFileIdentity &other) const
    {
        if (!sameObject(other)) {
            return false;
        }
        if (nlink != 1 || other.nlink != 1) {
            return false;
        }
        return size == other.size
            && mtimeSec == other.mtimeSec
            && mtimeNsec == other.mtimeNsec
            && ctimeSec == other.ctimeSec
            && ctimeNsec == other.ctimeNsec;
    }
};

struct BatchMoveItemPlan {
    int index = 0;
    QUrl sourceUrl;
    QUrl destinationUrl;
    QString srcParentPath;
    QString srcLeaf;
    QString dstParentPath;
    QString dstLeaf;
    QString cleanSrcPath;
    QString cleanDstPath;
    LocalRegularFileIdentity sourceIdentity;
    LocalDirectoryIdentity srcParentIdentity;
    LocalDirectoryIdentity dstParentIdentity;
    quint64 size = 0;
};

struct BatchMoveItemResult {
    int index = 0;
    QUrl sourceUrl;
    QUrl destinationUrl;
    BatchMoveItemState state = BatchMoveItemState::Pending;
    BatchMoveError error = BatchMoveError::None;
    int systemError = 0;
    bool destinationPublished = false;
    bool durabilityConfirmed = false;
    LocalRegularFileIdentity movedIdentity;
    QString errorText;
};

struct BatchMovePreflight {
    bool ok = false;
    BatchMoveError error = BatchMoveError::InvalidInput;
    int systemError = 0;
    QString reason;
    LocalDirectoryIdentity anchorScope;
    QList<BatchMoveItemPlan> items;
    quint64 totalBytes = 0;
};

struct BatchMoveResult {
    BatchMoveOutcome outcome = BatchMoveOutcome::FullRefusalBeforeExecution;
    BatchMoveError error = BatchMoveError::InvalidInput;
    int systemError = 0;
    QString reason;
    int totalCount = 0;
    int publishedCount = 0;
    int durableCount = 0;
    int failedCount = 0;
    int skippedCount = 0;
    bool partialMoveOutcome = false;
    QList<BatchMoveItemResult> items;
};

#ifdef THISPC_TEST_HARNESS
inline unsigned long &simulateBatchMoveUnsupportedFsMagic()
{
    static unsigned long magic = 0;
    return magic;
}

inline bool &simulateBatchMoveOpenat2Unsupported()
{
    static bool flag = false;
    return flag;
}

inline bool &simulateBatchMoveStatxMntIdUnsupported()
{
    static bool flag = false;
    return flag;
}

inline bool &simulateBatchMoveRenameNoReplaceUnsupported()
{
    static bool flag = false;
    return flag;
}

inline bool &simulateBatchMoveExdev()
{
    static bool flag = false;
    return flag;
}

inline std::function<void(int)> &simulateBatchMoveBeforeStep()
{
    static std::function<void(int)> fn;
    return fn;
}

inline std::function<void(int)> &simulateBatchMoveBeforeRename()
{
    static std::function<void(int)> fn;
    return fn;
}

inline std::function<void(int)> &simulateBatchMoveAfterRenameBeforeFsync()
{
    static std::function<void(int)> fn;
    return fn;
}

inline QPair<int, int> &simulateBatchMoveDestinationFsyncErrorAtStep()
{
    static QPair<int, int> pair{-1, 0};
    return pair;
}

inline QPair<int, int> &simulateBatchMoveSourceFsyncErrorAtStep()
{
    static QPair<int, int> pair{-1, 0};
    return pair;
}

inline int &simulateBatchMovePostRenameVerifyFailAtStep()
{
    static int step = -1;
    return step;
}
#endif

namespace LocalBatchMoveDetail {

using LocalDirectoryMoveDetail::ScopedFd;
using LocalDirectoryMoveDetail::trMsg;
using LocalDirectoryMoveDetail::splitCleanLocalPath;
using LocalDirectoryMoveDetail::isMountPointInMountInfo;

inline int batchOpenat2Syscall(
    int dirFd,
    const char *path,
    quint64 flags,
    quint64 resolve,
    int *errOut)
{
#ifdef THISPC_TEST_HARNESS
    if (simulateBatchMoveOpenat2Unsupported()) {
        if (errOut) *errOut = ENOSYS;
        return -1;
    }
#endif
#ifdef SYS_openat2
    struct open_how how {};
    how.flags = flags;
    how.mode = 0;
    how.resolve = resolve;
    const int fd = static_cast<int>(::syscall(SYS_openat2, dirFd, path, &how, sizeof(how)));
    if (fd < 0) {
        if (errOut) *errOut = errno ? errno : EIO;
        return -1;
    }
    if (errOut) *errOut = 0;
    return fd;
#else
    Q_UNUSED(dirFd);
    Q_UNUSED(path);
    Q_UNUSED(flags);
    Q_UNUSED(resolve);
    if (errOut) *errOut = ENOSYS;
    return -1;
#endif
}

inline int openBatchDirNoSymlinks(const QString &absDirPath, int *errOut)
{
    const QByteArray encoded = QFile::encodeName(absDirPath);
    return batchOpenat2Syscall(
        AT_FDCWD,
        encoded.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS,
        errOut);
}

inline int openBatchChildPathNoCrossing(int parentFd, const char *leafName, int *errOut)
{
    return batchOpenat2Syscall(
        parentFd,
        leafName,
        O_PATH | O_NOFOLLOW | O_CLOEXEC,
        RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_BENEATH | RESOLVE_NO_XDEV,
        errOut);
}

inline bool queryBatchFdMetadata(
    int fd,
    struct statx *stxOut,
    struct stat *stOut,
    unsigned long *fsMagicOut,
    int *errOut)
{
    if (fd < 0 || !stxOut || !stOut || !fsMagicOut) {
        if (errOut) *errOut = EINVAL;
        return false;
    }
    if (::fstat(fd, stOut) != 0) {
        if (errOut) *errOut = errno ? errno : EIO;
        return false;
    }
    struct statfs sfs {};
    if (::fstatfs(fd, &sfs) != 0) {
        if (errOut) *errOut = errno ? errno : EIO;
        return false;
    }
    *fsMagicOut = static_cast<unsigned long>(sfs.f_type);
#ifdef THISPC_TEST_HARNESS
    if (simulateBatchMoveUnsupportedFsMagic() != 0) {
        *fsMagicOut = simulateBatchMoveUnsupportedFsMagic();
    }
#endif

#ifdef SYS_statx
    std::memset(stxOut, 0, sizeof(*stxOut));
    const unsigned int mask =
        STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE | STATX_SUBVOL;
    if (::syscall(SYS_statx, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, mask, stxOut) != 0) {
        if (errOut) *errOut = errno ? errno : EIO;
        return false;
    }
#ifdef THISPC_TEST_HARNESS
    if (simulateBatchMoveStatxMntIdUnsupported()) {
        stxOut->stx_mask &= ~(STATX_MNT_ID | STATX_MNT_ID_UNIQUE);
        stxOut->stx_mnt_id = 0;
    }
#endif
    if (errOut) *errOut = 0;
    return true;
#else
    if (errOut) *errOut = ENOSYS;
    return false;
#endif
}

inline QString unsupportedBatchFsMessage()
{
    return trMsg(
        "Zbiorcze przenoszenie plików (Batch Move) jest obecnie dozwolone wyłącznie w obrębie tego samego woluminu Btrfs bez nadpisywania. Użyj operacji Kopiuj.",
        "Batch file moving is currently permitted only within the same Btrfs volume without overwriting. Use Copy instead.");
}

struct OpenedBatchParent {
    ScopedFd fd;
    LocalDirectoryIdentity identity;
};

struct OpenedBatchExecutionContext {
    QHash<QString, std::shared_ptr<OpenedBatchParent>> parents;
};

inline QList<QPair<QUrl, QUrl>> buildPairsForDestinationDir(
    const QList<QUrl> &sources,
    const QUrl &destinationDirectory)
{
    QList<QPair<QUrl, QUrl>> pairs;
    if (sources.isEmpty() || !destinationDirectory.isValid() || !destinationDirectory.isLocalFile()) {
        return pairs;
    }
    const QString cleanDestDir = QDir::cleanPath(destinationDirectory.toLocalFile());
    pairs.reserve(sources.size());
    for (const QUrl &src : sources) {
        if (!src.isValid() || !src.isLocalFile()) {
            pairs.clear();
            return pairs;
        }
        const QString cleanSrc = QDir::cleanPath(src.toLocalFile());
        const QString name = QFileInfo(cleanSrc).fileName();
        const QString dstPath = (cleanDestDir == QLatin1String("/"))
            ? QString(QLatin1Char('/') + name)
            : QString(cleanDestDir + QLatin1Char('/') + name);
        pairs.append({src, QUrl::fromLocalFile(dstPath)});
    }
    return pairs;
}

inline bool runPreflight(
    const QList<QPair<QUrl, QUrl>> &pairs,
    const QList<LocalRegularFileIdentity> *expectedIdentities,
    bool strictHistoryCheck,
    BatchMovePreflight *outPreflight,
    OpenedBatchExecutionContext *outCtx)
{
    BatchMovePreflight pf;
    auto fail = [&](BatchMoveError err, int sysErr, const QString &reason) {
        pf.ok = false;
        pf.error = err;
        pf.systemError = sysErr;
        pf.reason = reason;
        if (outPreflight) *outPreflight = pf;
        return false;
    };

    if (pairs.size() < 2) {
        return fail(BatchMoveError::InvalidInput, EINVAL,
                    trMsg("Zbiorcze przenoszenie wymaga co najmniej dwóch kwalifikowanych plików.",
                          "Batch move requires at least two qualified files."));
    }
    if (expectedIdentities && expectedIdentities->size() != pairs.size()) {
        return fail(BatchMoveError::InvalidInput, EINVAL,
                    moveContainmentErrorMessage());
    }

    OpenedBatchExecutionContext localCtx;
    auto getOrOpenParent = [&](const QString &dirPath,
                               std::shared_ptr<OpenedBatchParent> *outParent,
                               BatchMoveError *errOut,
                               int *sysErrOut,
                               QString *msgOut) -> bool {
        auto it = localCtx.parents.constFind(dirPath);
        if (it != localCtx.parents.constEnd()) {
            *outParent = it.value();
            return true;
        }
        int err = 0;
        ScopedFd fd(openBatchDirNoSymlinks(dirPath, &err));
        if (!fd.isValid()) {
            if (err == ENOSYS) {
                *errOut = BatchMoveError::UnsupportedKernelFeature;
                *msgOut = trMsg("Jądro nie udostępnia wymaganego bezpiecznego mechanizmu openat2.",
                                "The kernel does not support required safe openat2 lookup.");
            } else {
                *errOut = (err == ELOOP)
                    ? BatchMoveError::ContainmentViolation
                    : BatchMoveError::InvalidInput;
                *msgOut = trMsg("Nie można bezpiecznie otworzyć katalogu (możliwe dowiązanie symboliczne lub brak dostępu).",
                                "Could not safely open directory (possible symlink or inaccessible path).");
            }
            *sysErrOut = err;
            return false;
        }

        struct statx stx {};
        struct stat st {};
        unsigned long magic = 0;
        if (!queryBatchFdMetadata(fd.fd, &stx, &st, &magic, &err)) {
            *errOut = BatchMoveError::InvalidInput;
            *sysErrOut = err;
            *msgOut = trMsg("Nie udało się odczytać metadanych katalogu przez deskryptor.",
                            "Could not inspect directory descriptor metadata.");
            return false;
        }
        if (magic != BTRFS_SUPER_MAGIC) {
            *errOut = BatchMoveError::UnsupportedFilesystem;
            *sysErrOut = EXDEV;
            *msgOut = unsupportedBatchFsMessage();
            return false;
        }
        if (!(stx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE))) {
            *errOut = BatchMoveError::UnsupportedKernelFeature;
            *sysErrOut = EOPNOTSUPP;
            *msgOut = trMsg("Jądro nie zwróciło identyfikatora montowania statx(STATX_MNT_ID).",
                            "Kernel did not provide statx mount identity (STATX_MNT_ID).");
            return false;
        }
        const LocalDirectoryIdentity dirId = LocalDirectoryIdentity::fromStat(stx, st);
        if (!dirId.valid) {
            *errOut = BatchMoveError::ContainmentViolation;
            *sysErrOut = EINVAL;
            *msgOut = moveContainmentErrorMessage();
            return false;
        }

        auto entry = std::make_shared<OpenedBatchParent>();
        entry->fd = std::move(fd);
        entry->identity = dirId;
        localCtx.parents.insert(dirPath, entry);
        *outParent = entry;
        return true;
    };

    QSet<QString> seenCleanSources;
    QSet<QString> seenCleanTargets;
    QSet<QPair<quint64, quint64>> seenSourceObjects;
    QList<BatchMoveItemPlan> plans;
    plans.reserve(pairs.size());

    // First pass: path validation, intra-batch path uniqueness, parent qualification, and child inspection
    for (int i = 0; i < pairs.size(); ++i) {
        const QUrl &srcUrl = pairs.at(i).first;
        const QUrl &dstUrl = pairs.at(i).second;
        if (!srcUrl.isValid() || !dstUrl.isValid() || !srcUrl.isLocalFile() || !dstUrl.isLocalFile()) {
            return fail(BatchMoveError::InvalidInput, EINVAL, moveContainmentErrorMessage());
        }

        const QString rawSrc = srcUrl.toLocalFile();
        const QString rawDst = dstUrl.toLocalFile();
        QString srcParent, srcLeaf, dstParent, dstLeaf;
        if (!splitCleanLocalPath(rawSrc, &srcParent, &srcLeaf)
            || !splitCleanLocalPath(rawDst, &dstParent, &dstLeaf)) {
            return fail(BatchMoveError::InvalidInput, EINVAL, moveContainmentErrorMessage());
        }
        const QString cleanSrc = QDir::cleanPath(rawSrc);
        const QString cleanDst = QDir::cleanPath(rawDst);
        if (cleanSrc == cleanDst) {
            return fail(BatchMoveError::CollisionInBatch, EINVAL,
                        trMsg("Źródło i cel wskazują na ten sam plik.",
                              "Source and destination refer to the same file."));
        }
        if (seenCleanSources.contains(cleanSrc)) {
            return fail(BatchMoveError::CollisionInBatch, EINVAL,
                        trMsg("Wykryto zduplikowane źródło na liście przenoszenia.",
                              "Duplicate source entry detected in batch move list."));
        }
        if (seenCleanTargets.contains(cleanDst)) {
            return fail(BatchMoveError::CollisionInBatch, EEXIST,
                        trMsg("Co najmniej dwa wybrane pliki mają tę samą nazwę docelową — przenoszenie bez nadpisywania zablokowane.",
                              "Two or more selected files map to the same destination name — batch move blocked to prevent overwrite."));
        }
        seenCleanSources.insert(cleanSrc);
        seenCleanTargets.insert(cleanDst);

        if (isMountPointInMountInfo(cleanSrc)) {
            return fail(BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                        trMsg("Przenoszenie punktu montowania jest zablokowane ze względów bezpieczeństwa.",
                              "Moving a mount point is blocked for safety."));
        }

        std::shared_ptr<OpenedBatchParent> srcParentEntry;
        std::shared_ptr<OpenedBatchParent> dstParentEntry;
        BatchMoveError pErr = BatchMoveError::InvalidInput;
        int pSysErr = 0;
        QString pMsg;
        if (!getOrOpenParent(dstParent, &dstParentEntry, &pErr, &pSysErr, &pMsg)) {
            return fail(pErr, pSysErr, pMsg);
        }
        if (!getOrOpenParent(srcParent, &srcParentEntry, &pErr, &pSysErr, &pMsg)) {
            return fail(pErr, pSysErr, pMsg);
        }

        if (!pf.anchorScope.valid) {
            pf.anchorScope = dstParentEntry->identity;
        }
        if (!dstParentEntry->identity.sameMountScope(pf.anchorScope)
            || !srcParentEntry->identity.sameMountScope(pf.anchorScope)) {
            return fail(BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                        trMsg("Wszystkie źródła i cele partii muszą znajdować się na tym samym montowaniu i podwoluminie Btrfs.",
                              "All batch sources and destinations must reside on the same Btrfs mount and subvolume."));
        }

        if (srcParentEntry->identity.sameObject(dstParentEntry->identity) && srcLeaf == dstLeaf) {
            return fail(BatchMoveError::CollisionInBatch, EINVAL,
                        trMsg("Źródło i cel wskazują na ten sam plik.",
                              "Source and destination refer to the same file."));
        }

        // Open source file strictly non-crossing & non-dereferencing via O_PATH
        const QByteArray srcLeafBytes = QFile::encodeName(srcLeaf);
        int childErr = 0;
        ScopedFd childFd(openBatchChildPathNoCrossing(srcParentEntry->fd.fd, srcLeafBytes.constData(), &childErr));
        if (!childFd.isValid()) {
            if (childErr == ENOSYS) {
                return fail(BatchMoveError::UnsupportedKernelFeature, childErr,
                            trMsg("Jądro nie udostępnia wymaganego bezpiecznego mechanizmu openat2.",
                                  "The kernel does not support required safe openat2 lookup."));
            }
            if (childErr == EXDEV) {
                return fail(BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                            trMsg("Źródło przekracza granicę montowania lub podwoluminu Btrfs.",
                                  "Source crosses a mount or Btrfs subvolume boundary."));
            }
            return fail(BatchMoveError::ContainmentViolation, childErr, moveContainmentErrorMessage());
        }

        struct statx childStx {};
        struct stat childSt {};
        unsigned long childMagic = 0;
        if (!queryBatchFdMetadata(childFd.fd, &childStx, &childSt, &childMagic, &childErr)) {
            return fail(BatchMoveError::ContainmentViolation, childErr, moveContainmentErrorMessage());
        }
        if (childMagic != BTRFS_SUPER_MAGIC) {
            return fail(BatchMoveError::UnsupportedFilesystem, EXDEV, unsupportedBatchFsMessage());
        }
        if (!(childStx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE))) {
            return fail(BatchMoveError::UnsupportedKernelFeature, EOPNOTSUPP,
                        trMsg("Jądro nie zwróciło identyfikatora montowania statx(STATX_MNT_ID).",
                              "Kernel did not provide statx mount identity (STATX_MNT_ID)."));
        }

        // Only single-link regular files allowed
        if (!(childStx.stx_mask & STATX_TYPE) || !S_ISREG(childStx.stx_mode)) {
            return fail(BatchMoveError::ContainmentViolation, EINVAL, moveContainmentErrorMessage());
        }
        const LocalRegularFileIdentity fileId = LocalRegularFileIdentity::fromStat(childStx, childSt);
        if (!fileId.valid) {
            return fail(BatchMoveError::ContainmentViolation, EINVAL, moveContainmentErrorMessage());
        }
        if (fileId.nlink > 1) {
            return fail(BatchMoveError::ContainmentViolation, EMLINK, moveContainmentErrorMessage());
        }
        if (!fileId.sameMountScope(pf.anchorScope)) {
            return fail(BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                        trMsg("Jeden z plików znajduje się poza kwalifikowanym montowaniem lub podwoluminem Btrfs.",
                              "One of the files resides outside the qualified Btrfs mount or subvolume."));
        }

        const QPair<quint64, quint64> objKey(fileId.stDev, fileId.ino);
        if (seenSourceObjects.contains(objKey)) {
            return fail(BatchMoveError::CollisionInBatch, EINVAL,
                        trMsg("Wykryto dwa wpisy wskazujące ten sam obiekt (alias lub hardlink) w jednej partii.",
                              "Two entries pointing to the same file object detected in one batch."));
        }
        seenSourceObjects.insert(objKey);

        if (expectedIdentities) {
            const LocalRegularFileIdentity &exp = expectedIdentities->at(i);
            const bool idMatch = strictHistoryCheck
                ? fileId.strictlyUnchangedForHistory(exp)
                : fileId.sameObject(exp);
            if (!idMatch) {
                return fail(BatchMoveError::ContainmentViolation, EBADF,
                            trMsg("Zawartość lub tożsamość co najmniej jednego pliku uległa zmianie — cofnięcie/ponowienie partii zostało zablokowane.",
                                  "Content or identity of at least one file changed — batch Undo/Redo blocked for safety."));
            }
        }

        // Check destination path absence (non-dereferencing)
        const QByteArray dstLeafBytes = QFile::encodeName(dstLeaf);
        struct stat dstExistSt {};
        if (::fstatat(dstParentEntry->fd.fd, dstLeafBytes.constData(), &dstExistSt, AT_SYMLINK_NOFOLLOW) == 0) {
            return fail(BatchMoveError::DestinationExists, EEXIST,
                        trMsg("Element docelowy „%1” już istnieje. Zbiorcze przenoszenie z nadpisywaniem jest niedostępne.",
                              "Destination item \"%1\" already exists. Batch move with overwrite is unavailable.")
                            .arg(dstLeaf));
        }
        const int dstStatErr = errno;
        if (dstStatErr != ENOENT) {
            return fail(BatchMoveError::InvalidInput, dstStatErr,
                        trMsg("Nie można zweryfikować dostępności ścieżki docelowej.",
                              "Could not verify destination availability."));
        }

        BatchMoveItemPlan itemPlan;
        itemPlan.index = i;
        itemPlan.sourceUrl = srcUrl;
        itemPlan.destinationUrl = dstUrl;
        itemPlan.srcParentPath = srcParent;
        itemPlan.srcLeaf = srcLeaf;
        itemPlan.dstParentPath = dstParent;
        itemPlan.dstLeaf = dstLeaf;
        itemPlan.cleanSrcPath = cleanSrc;
        itemPlan.cleanDstPath = cleanDst;
        itemPlan.sourceIdentity = fileId;
        itemPlan.srcParentIdentity = srcParentEntry->identity;
        itemPlan.dstParentIdentity = dstParentEntry->identity;
        itemPlan.size = fileId.size;
        pf.totalBytes += fileId.size;
        plans.append(itemPlan);
    }

    // Disallow source == another item's destination (chaining/aliasing across batch steps)
    for (int i = 0; i < plans.size(); ++i) {
        if (seenCleanTargets.contains(plans.at(i).cleanSrcPath)) {
            return fail(BatchMoveError::CollisionInBatch, EINVAL,
                        trMsg("Ścieżka źródłowa pokrywa się ze ścieżką docelową innego elementu partii.",
                              "A source path overlaps with another item's destination path within the batch."));
        }
    }

    pf.ok = true;
    pf.error = BatchMoveError::None;
    pf.systemError = 0;
    pf.items = std::move(plans);
    if (outPreflight) *outPreflight = std::move(pf);
    if (outCtx) *outCtx = std::move(localCtx);
    return true;
}

inline bool qualifyFastForPolicy(
    const QList<QUrl> &sources,
    const QUrl &destinationDirectory,
    BatchMovePreflight *outPreflight = nullptr)
{
    const QList<QPair<QUrl, QUrl>> pairs = buildPairsForDestinationDir(sources, destinationDirectory);
    if (pairs.size() < 2) {
        if (outPreflight) {
            outPreflight->ok = false;
            outPreflight->error = BatchMoveError::InvalidInput;
            outPreflight->reason = moveContainmentErrorMessage();
        }
        return false;
    }
    return runPreflight(pairs, nullptr, false, outPreflight, nullptr);
}

} // namespace LocalBatchMoveDetail

class LocalBatchMoveJob final : public KIO::Job
{
    Q_OBJECT

public:
    LocalBatchMoveJob(
        const QList<QUrl> &sources,
        const QUrl &destinationDirectory,
        QObject *parent = nullptr)
        : LocalBatchMoveJob(
              LocalBatchMoveDetail::buildPairsForDestinationDir(sources, destinationDirectory),
              parent,
              {},
              false)
    {
        if (destinationDirectory.isValid()) {
            setProperty("thispcDestinationUrl", destinationDirectory);
        }
    }

    LocalBatchMoveJob(
        const QList<QPair<QUrl, QUrl>> &pairs,
        QObject *parent = nullptr,
        const QList<LocalRegularFileIdentity> &expectedIdentities = {},
        bool strictHistoryCheck = false)
        : KIO::Job()
        , m_pairs(pairs)
        , m_expectedIdentities(expectedIdentities)
        , m_strictHistoryCheck(strictHistoryCheck)
    {
        setParent(parent);
        QList<QUrl> srcUrls;
        srcUrls.reserve(pairs.size());
        for (const auto &p : pairs) {
            srcUrls.append(p.first);
        }
        setProperty("thispcSourceUrls", QVariant::fromValue(srcUrls));
        if (!pairs.isEmpty()) {
            setProperty("thispcDestinationUrl", pairs.first().second);
        }
        setProperty("thispcOverwritesDestination", false);
        setProperty("thispcIsBatchMove", true);
        qRegisterMetaType<BatchMoveError>();
        qRegisterMetaType<BatchMoveItemState>();
        qRegisterMetaType<BatchMoveOutcome>();
        setCapabilities(KJob::Killable);
    }

    ~LocalBatchMoveJob() override
    {
        m_cancelled.store(true, std::memory_order_release);
        if (m_thread) {
            m_thread->wait();
        }
    }

    void start() override
    {
        if (m_started || m_finished) return;
        m_started = true;
        if (m_pairs.size() < 2) {
            QTimer::singleShot(0, this, [this] {
                WorkerResult res;
                res.batch.totalCount = static_cast<int>(m_pairs.size());
                res.batch.outcome = BatchMoveOutcome::FullRefusalBeforeExecution;
                res.batch.error = BatchMoveError::InvalidInput;
                res.batch.reason = LocalBatchMoveDetail::trMsg(
                    "Zbiorcze przenoszenie wymaga co najmniej dwóch kwalifikowanych plików.",
                    "Batch move requires at least two qualified files.");
                applyWorkerResult(res);
            });
            return;
        }

        const QList<QPair<QUrl, QUrl>> pairsCopy = m_pairs;
        const QList<LocalRegularFileIdentity> expectedCopy = m_expectedIdentities;
        const bool strictCopy = m_strictHistoryCheck;

        m_thread = QThread::create([this, pairsCopy, expectedCopy, strictCopy] {
            executeWorker(pairsCopy, expectedCopy, strictCopy);
        });
        m_thread->setParent(this);
        connect(m_thread, &QThread::finished, this, [this] {
            if (m_thread) {
                m_thread->deleteLater();
                m_thread = nullptr;
            }
        });
        m_thread->start();
    }

    BatchMoveOutcome outcome() const { return m_result.outcome; }
    BatchMoveError batchError() const { return m_result.error; }
    int systemError() const { return m_result.systemError; }
    int totalCount() const { return m_result.totalCount; }
    int publishedCount() const { return m_result.publishedCount; }
    int durableCount() const { return m_result.durableCount; }
    int failedCount() const { return m_result.failedCount; }
    int skippedCount() const { return m_result.skippedCount; }
    bool partialMoveOutcome() const { return m_result.partialMoveOutcome; }
    const BatchMoveResult &batchResult() const { return m_result; }
    bool isCancelRequested() const { return m_cancelled.load(std::memory_order_acquire); }

protected:
    bool doKill() override
    {
        if (m_finished) {
            return false;
        }
        m_cancelled.store(true, std::memory_order_release);
        if (m_thread && QThread::currentThread() != m_thread) {
            m_thread->wait();
        }
        if (m_finished) {
            return false;
        }
        if (m_workerResultReady.load(std::memory_order_acquire)) {
            const WorkerResult res = m_workerResult;
            if (res.batch.outcome == BatchMoveOutcome::FullSuccess
                || res.batch.outcome == BatchMoveOutcome::PartialOutcomeWithUnconfirmedDurability) {
                applyWorkerResult(res);
                return false;
            }
            populatePropertiesFromResult(res);
            m_result = res.batch;
            m_finished = true;
            if (!res.batch.reason.isEmpty()) {
                setErrorText(res.batch.reason);
            }
            return true;
        }
        // Killed before thread even started
        m_result.totalCount = static_cast<int>(m_pairs.size());
        m_result.outcome = BatchMoveOutcome::FullRefusalBeforeExecution;
        m_result.error = BatchMoveError::Cancelled;
        m_result.skippedCount = m_result.totalCount;
        m_result.reason = LocalBatchMoveDetail::trMsg("Operacja anulowana.", "Operation cancelled.");
        WorkerResult synthetic;
        synthetic.batch = m_result;
        populatePropertiesFromResult(synthetic);
        m_finished = true;
        setErrorText(m_result.reason);
        return true;
    }

private:
    struct WorkerResult {
        BatchMoveResult batch;
        quint64 totalBytes = 0;
        quint64 processedBytes = 0;
    };

    void executeWorker(
        const QList<QPair<QUrl, QUrl>> &pairs,
        const QList<LocalRegularFileIdentity> &expectedIdentities,
        bool strictHistoryCheck)
    {
        WorkerResult res;
        res.batch.totalCount = static_cast<int>(pairs.size());
        res.batch.items.resize(pairs.size());
        for (int i = 0; i < pairs.size(); ++i) {
            res.batch.items[i].index = i;
            res.batch.items[i].sourceUrl = pairs.at(i).first;
            res.batch.items[i].destinationUrl = pairs.at(i).second;
            res.batch.items[i].state = BatchMoveItemState::Pending;
        }

        auto publishAndReturn = [&] {
            m_workerResult = res;
            m_workerResultReady.store(true, std::memory_order_release);
            QMetaObject::invokeMethod(this, [this, res] {
                applyWorkerResult(res);
            }, Qt::QueuedConnection);
        };

        if (m_cancelled.load(std::memory_order_acquire)) {
            res.batch.outcome = BatchMoveOutcome::FullRefusalBeforeExecution;
            res.batch.error = BatchMoveError::Cancelled;
            res.batch.skippedCount = res.batch.totalCount;
            for (auto &it : res.batch.items) {
                it.state = BatchMoveItemState::SkippedAfterAbort;
                it.error = BatchMoveError::Cancelled;
            }
            res.batch.reason = LocalBatchMoveDetail::trMsg("Operacja anulowana.", "Operation cancelled.");
            publishAndReturn();
            return;
        }

        BatchMovePreflight pf;
        LocalBatchMoveDetail::OpenedBatchExecutionContext ctx;
        const QList<LocalRegularFileIdentity> *expectedPtr =
            expectedIdentities.isEmpty() ? nullptr : &expectedIdentities;
        if (!LocalBatchMoveDetail::runPreflight(pairs, expectedPtr, strictHistoryCheck, &pf, &ctx)) {
            res.batch.outcome = BatchMoveOutcome::FullRefusalBeforeExecution;
            res.batch.error = pf.error;
            res.batch.systemError = pf.systemError;
            res.batch.reason = pf.reason;
            res.batch.failedCount = 1;
            res.batch.skippedCount = qMax(0, res.batch.totalCount - 1);
            for (int i = 0; i < res.batch.items.size(); ++i) {
                res.batch.items[i].state = (i == 0)
                    ? BatchMoveItemState::FailedBeforePublish
                    : BatchMoveItemState::SkippedAfterAbort;
                res.batch.items[i].error = pf.error;
            }
            publishAndReturn();
            return;
        }

        res.totalBytes = pf.totalBytes;
        const int totalCount = static_cast<int>(pf.items.size());
        const quint64 totalBytesSnapshot = pf.totalBytes;
        QMetaObject::invokeMethod(this, [this, totalCount, totalBytesSnapshot] {
            if (m_finished) return;
            setTotalAmount(KJob::Files, static_cast<qulonglong>(totalCount));
            setTotalAmount(KJob::Bytes, totalBytesSnapshot);
        }, Qt::QueuedConnection);

        bool abortedByError = false;
        bool cancelledDuringExecution = false;
        BatchMoveError abortError = BatchMoveError::None;
        int abortSysErr = 0;
        QString abortReason;

        for (int i = 0; i < totalCount; ++i) {
            BatchMoveItemResult &itemRes = res.batch.items[i];
            if (itemRes.state != BatchMoveItemState::Pending) {
                abortedByError = true;
                abortError = BatchMoveError::ContainmentViolation;
                abortReason = moveContainmentErrorMessage();
                break;
            }

            if (m_cancelled.load(std::memory_order_acquire)) {
                cancelledDuringExecution = true;
                itemRes.state = BatchMoveItemState::SkippedAfterAbort;
                itemRes.error = BatchMoveError::Cancelled;
                for (int j = i + 1; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    res.batch.items[j].error = BatchMoveError::Cancelled;
                }
                break;
            }

            itemRes.state = BatchMoveItemState::InProgress;

#ifdef THISPC_TEST_HARNESS
            if (simulateBatchMoveBeforeStep()) {
                simulateBatchMoveBeforeStep()(i);
            }
#endif

            if (m_cancelled.load(std::memory_order_acquire)) {
                cancelledDuringExecution = true;
                for (int j = i; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    res.batch.items[j].error = BatchMoveError::Cancelled;
                }
                break;
            }

            const BatchMoveItemPlan &itemPlan = pf.items.at(i);
            const auto srcParentIt = ctx.parents.constFind(itemPlan.srcParentPath);
            const auto dstParentIt = ctx.parents.constFind(itemPlan.dstParentPath);
            if (srcParentIt == ctx.parents.constEnd() || dstParentIt == ctx.parents.constEnd()) {
                abortedByError = true;
                itemRes.state = BatchMoveItemState::FailedBeforePublish;
                itemRes.error = BatchMoveError::InvalidInput;
                abortError = BatchMoveError::InvalidInput;
                abortReason = moveContainmentErrorMessage();
                for (int j = i + 1; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                }
                break;
            }

            const int srcParentFd = srcParentIt.value()->fd.fd;
            const int dstParentFd = dstParentIt.value()->fd.fd;
            const bool sameParent = srcParentIt.value()->identity.sameObject(dstParentIt.value()->identity);
            const QByteArray srcLeafBytes = QFile::encodeName(itemPlan.srcLeaf);
            const QByteArray dstLeafBytes = QFile::encodeName(itemPlan.dstLeaf);

            // Step revalidation: verify source identity/metadata is unchanged before renameat2
            struct statx preSrcStx {};
            struct stat preSrcSt {};
            const unsigned int statxMask =
                STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE | STATX_SUBVOL;
            if (::syscall(SYS_statx, srcParentFd, srcLeafBytes.constData(),
                          AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, statxMask, &preSrcStx) != 0
                || ::fstatat(srcParentFd, srcLeafBytes.constData(), &preSrcSt, AT_SYMLINK_NOFOLLOW) != 0) {
                const int stErr = errno ? errno : ENOENT;
                abortedByError = true;
                itemRes.state = BatchMoveItemState::FailedBeforePublish;
                itemRes.error = BatchMoveError::ContainmentViolation;
                itemRes.systemError = stErr;
                abortError = itemRes.error;
                abortSysErr = stErr;
                abortReason = LocalBatchMoveDetail::trMsg(
                    "Plik źródłowy „%1” zniknął lub zmienił się przed przeniesieniem — przerwano bez zmian pozostałych plików.",
                    "Source file \"%1\" disappeared or changed before move — stopped remaining files.")
                    .arg(itemPlan.srcLeaf);
                itemRes.errorText = abortReason;
                for (int j = i + 1; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                }
                break;
            }

            const LocalRegularFileIdentity currentSrcId = LocalRegularFileIdentity::fromStat(preSrcStx, preSrcSt);
            if (!currentSrcId.strictlyUnchangedForHistory(itemPlan.sourceIdentity)) {
                abortedByError = true;
                itemRes.state = BatchMoveItemState::FailedBeforePublish;
                itemRes.error = BatchMoveError::ContainmentViolation;
                itemRes.systemError = EBADF;
                abortError = itemRes.error;
                abortSysErr = EBADF;
                abortReason = LocalBatchMoveDetail::trMsg(
                    "Plik źródłowy „%1” został zmodyfikowany lub podmieniony po walidacji wstępnej — zatrzymano partię.",
                    "Source file \"%1\" was modified or replaced after preflight — batch stopped.")
                    .arg(itemPlan.srcLeaf);
                itemRes.errorText = abortReason;
                for (int j = i + 1; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                }
                break;
            }

            struct stat preDstExistSt {};
            if (::fstatat(dstParentFd, dstLeafBytes.constData(), &preDstExistSt, AT_SYMLINK_NOFOLLOW) == 0) {
                abortedByError = true;
                itemRes.state = BatchMoveItemState::FailedBeforePublish;
                itemRes.error = BatchMoveError::DestinationExists;
                itemRes.systemError = EEXIST;
                abortError = itemRes.error;
                abortSysErr = EEXIST;
                abortReason = LocalBatchMoveDetail::trMsg(
                    "W lokalizacji docelowej pojawił się kolidujący element „%1” — przerwano bez nadpisywania.",
                    "Conflicting destination entry \"%1\" appeared before move — stopped without overwriting.")
                    .arg(itemPlan.dstLeaf);
                itemRes.errorText = abortReason;
                for (int j = i + 1; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                }
                break;
            }

#ifdef THISPC_TEST_HARNESS
            if (simulateBatchMoveBeforeRename()) {
                simulateBatchMoveBeforeRename()(i);
            }
#endif

            if (m_cancelled.load(std::memory_order_acquire)) {
                cancelledDuringExecution = true;
                for (int j = i; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    res.batch.items[j].error = BatchMoveError::Cancelled;
                }
                break;
            }

            long renameRes = -1;
            int renameErr = 0;
#ifdef THISPC_TEST_HARNESS
            if (simulateBatchMoveExdev()) {
                renameRes = -1;
                renameErr = EXDEV;
            } else if (simulateBatchMoveRenameNoReplaceUnsupported()) {
                renameRes = -1;
                renameErr = EOPNOTSUPP;
            } else
#endif
            {
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
                renameRes = ::syscall(
                    SYS_renameat2,
                    srcParentFd, srcLeafBytes.constData(),
                    dstParentFd, dstLeafBytes.constData(),
                    RENAME_NOREPLACE);
                renameErr = errno;
#else
                renameRes = -1;
                renameErr = ENOSYS;
#endif
            }

            if (renameRes != 0) {
                abortedByError = true;
                itemRes.state = BatchMoveItemState::FailedBeforePublish;
                itemRes.systemError = renameErr;
                if (renameErr == EEXIST || renameErr == ENOTEMPTY || renameErr == EISDIR || renameErr == EBUSY) {
                    itemRes.error = BatchMoveError::DestinationExists;
                    abortReason = LocalBatchMoveDetail::trMsg(
                        "Element docelowy „%1” już istnieje — przerwano partię bez nadpisywania.",
                        "Destination \"%1\" already exists — batch stopped without overwriting.")
                        .arg(itemPlan.dstLeaf);
                } else if (renameErr == EXDEV) {
                    itemRes.error = BatchMoveError::MountOrSubvolumeBoundary;
                    abortReason = LocalBatchMoveDetail::trMsg(
                        "Przekroczono granicę woluminu lub podwoluminu Btrfs (EXDEV) dla „%1”.",
                        "Crossed volume or Btrfs subvolume boundary (EXDEV) for \"%1\".")
                        .arg(itemPlan.srcLeaf);
                } else if (renameErr == ENOSYS || renameErr == EOPNOTSUPP || renameErr == EINVAL) {
                    itemRes.error = BatchMoveError::UnsupportedKernelFeature;
                    abortReason = LocalBatchMoveDetail::trMsg(
                        "Jądro lub system plików nie obsługuje renameat2(RENAME_NOREPLACE).",
                        "Kernel or filesystem does not support atomic renameat2(RENAME_NOREPLACE).");
                } else {
                    itemRes.error = BatchMoveError::AtomicMove;
                    abortReason = LocalBatchMoveDetail::trMsg(
                        "Nie udało się atomowo przenieść pliku „%1”.",
                        "Failed to move file \"%1\" atomically.")
                        .arg(itemPlan.srcLeaf);
                }
                itemRes.errorText = abortReason;
                abortError = itemRes.error;
                abortSysErr = renameErr;
                for (int j = i + 1; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                }
                break;
            }

            // CRITICAL POINT OF NO RETURN FOR ITEM i:
            // SYS_renameat2 succeeded; dstLeaf is published and srcLeaf is gone.
            // NEVER unlink destination or roll back from here onward!
            itemRes.destinationPublished = true;
            itemRes.state = BatchMoveItemState::PublishedUnconfirmedDurability;
            ++res.batch.publishedCount;

            // Post-rename verification on destination entry
            struct statx postDstStx {};
            struct stat postDstSt {};
            bool verifyFail = false;
            int verifyErr = 0;
#ifdef THISPC_TEST_HARNESS
            if (simulateBatchMovePostRenameVerifyFailAtStep() == i) {
                verifyFail = true;
                verifyErr = EIO;
            }
#endif
            if (!verifyFail) {
                if (::syscall(SYS_statx, dstParentFd, dstLeafBytes.constData(),
                              AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, statxMask, &postDstStx) != 0
                    || ::fstatat(dstParentFd, dstLeafBytes.constData(), &postDstSt, AT_SYMLINK_NOFOLLOW) != 0) {
                    verifyFail = true;
                    verifyErr = errno ? errno : EIO;
                }
            }
            const LocalRegularFileIdentity postDstId = LocalRegularFileIdentity::fromStat(postDstStx, postDstSt);
            if (verifyFail || !postDstId.sameObject(currentSrcId)) {
                res.batch.partialMoveOutcome = true;
                itemRes.state = BatchMoveItemState::FailedDurability;
                itemRes.error = BatchMoveError::PostRenameVerification;
                itemRes.systemError = verifyFail ? verifyErr : EIO;
                abortError = BatchMoveError::PostRenameVerification;
                abortSysErr = itemRes.systemError;
                abortReason = LocalBatchMoveDetail::trMsg(
                    "Część partii (%1 z %2) została opublikowana w celu, lecz weryfikacja po przeniesieniu pliku „%3” nie powiodła się (%4). Już przeniesione pliki zachowano bez automatycznego cofania i bez ponawiania.",
                    "Part of the batch (%1 of %2) was published to destination, but post-rename verification of \"%3\" failed (%4). Published files were preserved without automatic rollback or retry.")
                    .arg(res.batch.publishedCount)
                    .arg(totalCount)
                    .arg(itemPlan.dstLeaf)
                    .arg(itemRes.systemError);
                itemRes.errorText = abortReason;
                abortedByError = true;
                for (int j = i + 1; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                }
                break;
            }
            itemRes.movedIdentity = postDstId;

#ifdef THISPC_TEST_HARNESS
            if (simulateBatchMoveAfterRenameBeforeFsync()) {
                simulateBatchMoveAfterRenameBeforeFsync()(i);
            }
#endif

            // Durability Gate for step i
            int dstSyncRes = ::fsync(dstParentFd);
            int dstSyncErr = (dstSyncRes == 0) ? 0 : (errno ? errno : EIO);
#ifdef THISPC_TEST_HARNESS
            if (simulateBatchMoveDestinationFsyncErrorAtStep().first == i
                && simulateBatchMoveDestinationFsyncErrorAtStep().second != 0) {
                dstSyncRes = -1;
                dstSyncErr = simulateBatchMoveDestinationFsyncErrorAtStep().second;
            }
#endif
            if (dstSyncRes != 0) {
                res.batch.partialMoveOutcome = true;
                itemRes.state = BatchMoveItemState::FailedDurability;
                itemRes.error = BatchMoveError::Durability;
                itemRes.systemError = dstSyncErr;
                abortError = BatchMoveError::Durability;
                abortSysErr = dstSyncErr;
                abortReason = LocalBatchMoveDetail::trMsg(
                    "Plik „%1” został już opublikowany w miejscu docelowym (%2 z %3), lecz synchronizacja katalogu docelowego (fsync) zwróciła błąd (%4). Przeniesione dane zachowano bez cofania; zatrzymano resztę partii i zablokowano Retry.",
                    "File \"%1\" was published at destination (%2 of %3), but destination directory fsync failed (%4). Moved data was preserved without rollback; remaining items stopped and Retry blocked.")
                    .arg(itemPlan.dstLeaf)
                    .arg(res.batch.publishedCount)
                    .arg(totalCount)
                    .arg(dstSyncErr);
                itemRes.errorText = abortReason;
                abortedByError = true;
                for (int j = i + 1; j < totalCount; ++j) {
                    res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                }
                break;
            }

            if (!sameParent) {
                int srcSyncRes = ::fsync(srcParentFd);
                int srcSyncErr = (srcSyncRes == 0) ? 0 : (errno ? errno : EIO);
#ifdef THISPC_TEST_HARNESS
                if (simulateBatchMoveSourceFsyncErrorAtStep().first == i
                    && simulateBatchMoveSourceFsyncErrorAtStep().second != 0) {
                    srcSyncRes = -1;
                    srcSyncErr = simulateBatchMoveSourceFsyncErrorAtStep().second;
                }
#endif
                if (srcSyncRes != 0) {
                    res.batch.partialMoveOutcome = true;
                    itemRes.state = BatchMoveItemState::FailedDurability;
                    itemRes.error = BatchMoveError::Durability;
                    itemRes.systemError = srcSyncErr;
                    abortError = BatchMoveError::Durability;
                    abortSysErr = srcSyncErr;
                    abortReason = LocalBatchMoveDetail::trMsg(
                        "Plik „%1” został już opublikowany w miejscu docelowym (%2 z %3), lecz synchronizacja katalogu źródłowego (fsync) zwróciła błąd (%4). Przeniesione dane zachowano bez cofania; zatrzymano resztę partii i zablokowano Retry.",
                        "File \"%1\" was published at destination (%2 of %3), but source directory fsync failed (%4). Moved data was preserved without rollback; remaining items stopped and Retry blocked.")
                        .arg(itemPlan.dstLeaf)
                        .arg(res.batch.publishedCount)
                        .arg(totalCount)
                        .arg(srcSyncErr);
                    itemRes.errorText = abortReason;
                    abortedByError = true;
                    for (int j = i + 1; j < totalCount; ++j) {
                        res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    }
                    break;
                }
            }

            itemRes.durabilityConfirmed = true;
            itemRes.state = BatchMoveItemState::DurableCompleted;
            itemRes.error = BatchMoveError::None;
            ++res.batch.durableCount;
            res.processedBytes += itemPlan.size;

            const int doneFiles = res.batch.durableCount;
            const quint64 doneBytes = res.processedBytes;
            QMetaObject::invokeMethod(this, [this, doneFiles, totalCount, doneBytes] {
                if (m_finished) return;
                setProcessedAmount(KJob::Files, static_cast<qulonglong>(doneFiles));
                setProcessedAmount(KJob::Bytes, doneBytes);
                emitPercent(static_cast<qulonglong>(doneFiles), static_cast<qulonglong>(totalCount));
            }, Qt::QueuedConnection);
        }

        // Summarize failed and skipped counts strictly from item states
        int failedCount = 0;
        int skippedCount = 0;
        for (const auto &it : res.batch.items) {
            if (it.state == BatchMoveItemState::FailedBeforePublish
                || it.state == BatchMoveItemState::FailedDurability) {
                ++failedCount;
            } else if (it.state == BatchMoveItemState::SkippedAfterAbort
                       || it.state == BatchMoveItemState::Pending) {
                ++skippedCount;
            }
        }
        res.batch.failedCount = failedCount;
        res.batch.skippedCount = skippedCount;

        if (res.batch.partialMoveOutcome) {
            res.batch.outcome = BatchMoveOutcome::PartialOutcomeWithUnconfirmedDurability;
            res.batch.error = abortError;
            res.batch.systemError = abortSysErr;
            res.batch.reason = abortReason;
        } else if (!abortedByError && !cancelledDuringExecution
                   && res.batch.durableCount == totalCount && res.batch.publishedCount == totalCount) {
            res.batch.outcome = BatchMoveOutcome::FullSuccess;
            res.batch.error = BatchMoveError::None;
            res.batch.systemError = 0;
            res.batch.reason.clear();
        } else if (res.batch.publishedCount > 0) {
            res.batch.outcome = BatchMoveOutcome::PartialSuccessCleanStop;
            res.batch.error = cancelledDuringExecution ? BatchMoveError::Cancelled : abortError;
            res.batch.systemError = cancelledDuringExecution ? 0 : abortSysErr;
            const int remaining = totalCount - res.batch.durableCount;
            if (cancelledDuringExecution) {
                res.batch.reason = LocalBatchMoveDetail::trMsg(
                    "Przeniesiono trwale %1 z %2 plików przed anulowaniem (pominięto: %3). Już przeniesione pliki zachowano w miejscu docelowym bez cofania.",
                    "Durably moved %1 of %2 files before cancellation (skipped: %3). Already moved files were preserved at destination without rollback.")
                    .arg(res.batch.durableCount)
                    .arg(totalCount)
                    .arg(res.batch.skippedCount);
            } else {
                res.batch.reason = LocalBatchMoveDetail::trMsg(
                    "Przeniesiono trwale %1 z %2 plików (nieprzeniesione: %3, błędy: %4, pominięte: %5). Powód zatrzymania: %6. Już przeniesione pliki zachowano w miejscu docelowym bez automatycznego cofania.",
                    "Durably moved %1 of %2 files (unmoved: %3, failed: %4, skipped: %5). Stop reason: %6. Already moved files were preserved without automatic rollback.")
                    .arg(res.batch.durableCount)
                    .arg(totalCount)
                    .arg(remaining)
                    .arg(failedCount)
                    .arg(skippedCount)
                    .arg(abortReason);
            }
        } else {
            res.batch.outcome = BatchMoveOutcome::FullRefusalBeforeExecution;
            res.batch.error = cancelledDuringExecution ? BatchMoveError::Cancelled : abortError;
            res.batch.systemError = cancelledDuringExecution ? 0 : abortSysErr;
            res.batch.reason = cancelledDuringExecution
                ? LocalBatchMoveDetail::trMsg("Operacja anulowana przed przeniesieniem jakiegokolwiek pliku.",
                                              "Operation cancelled before moving any file.")
                : abortReason;
        }

        publishAndReturn();
    }

    void populatePropertiesFromResult(const WorkerResult &res)
    {
        setProperty("thispcBatchMoveError", QVariant::fromValue(res.batch.error));
        setProperty("thispcBatchMoveOutcome", QVariant::fromValue(res.batch.outcome));
        setProperty("thispcBatchTotalCount", res.batch.totalCount);
        setProperty("thispcBatchPublishedCount", res.batch.publishedCount);
        setProperty("thispcBatchDurableCount", res.batch.durableCount);
        setProperty("thispcBatchFailedCount", res.batch.failedCount);
        setProperty("thispcBatchSkippedCount", res.batch.skippedCount);
        const bool partiallyCompleted = (res.batch.publishedCount > 0
            && res.batch.outcome != BatchMoveOutcome::FullSuccess);
        setProperty("thispcBatchPartiallyCompleted", partiallyCompleted);
        if (res.batch.partialMoveOutcome) {
            setProperty("thispcPartialMoveOutcome", true);
        }
        if (res.batch.systemError != 0) {
            setProperty("thispcSystemError", res.batch.systemError);
        }
    }

    void applyWorkerResult(const WorkerResult &res)
    {
        if (m_finished) return;
        m_finished = true;
        m_result = res.batch;
        if (res.totalBytes > 0) {
            setTotalAmount(KJob::Bytes, res.totalBytes);
            setProcessedAmount(KJob::Bytes, res.processedBytes);
        }
        setTotalAmount(KJob::Files, static_cast<qulonglong>(res.batch.totalCount));
        setProcessedAmount(KJob::Files, static_cast<qulonglong>(res.batch.durableCount));
        populatePropertiesFromResult(res);

        if (res.batch.outcome == BatchMoveOutcome::FullSuccess && res.batch.error == BatchMoveError::None) {
            emitPercent(100, 100);
        } else if (res.batch.error == BatchMoveError::Cancelled) {
            setError(KJob::KilledJobError);
            if (!res.batch.reason.isEmpty()) {
                setErrorText(res.batch.reason);
            }
        } else {
            setError(KJob::UserDefinedError);
            setErrorText(res.batch.reason);
        }
        emitResult();
    }

    QList<QPair<QUrl, QUrl>> m_pairs;
    QList<LocalRegularFileIdentity> m_expectedIdentities;
    bool m_strictHistoryCheck = false;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_workerResultReady{false};
    WorkerResult m_workerResult;
    BatchMoveResult m_result;
    bool m_started = false;
    bool m_finished = false;
};
