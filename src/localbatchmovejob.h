/*
 * Safe atomic same-volume Btrfs batch regular-file move (Stage 5.3.2H.2B.1).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localbtrfsmovecommon.h"
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
    CycleDetected,
    DestinationExists,
    AtomicMove,
    PostRenameVerification,
    Durability,
};

enum class BatchMoveItemKind {
    RegularFile,
    Directory,
    Unknown,
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
Q_DECLARE_METATYPE(BatchMoveItemKind)
Q_DECLARE_METATYPE(BatchMoveItemState)
Q_DECLARE_METATYPE(BatchMoveOutcome)

struct BatchMoveItemPlan {
    int index = 0;
    BatchMoveItemKind kind = BatchMoveItemKind::RegularFile;
    QUrl sourceUrl;
    QUrl destinationUrl;
    QString srcParentPath;
    QString srcLeaf;
    QString dstParentPath;
    QString dstLeaf;
    QString cleanSrcPath;
    QString cleanDstPath;
    LocalRegularFileIdentity sourceIdentity;
    LocalDirectoryIdentity dirIdentity;
    LocalDirectoryTreeSnapshot dirSnapshot;
    LocalDirectoryIdentity srcParentIdentity;
    LocalDirectoryIdentity dstParentIdentity;
    quint64 size = 0;
};

struct BatchMoveItemResult {
    int index = 0;
    BatchMoveItemKind kind = BatchMoveItemKind::Unknown;
    QUrl sourceUrl;
    QUrl destinationUrl;
    BatchMoveItemState state = BatchMoveItemState::Pending;
    BatchMoveError error = BatchMoveError::None;
    int systemError = 0;
    bool destinationPublished = false;
    bool durabilityConfirmed = false;
    LocalRegularFileIdentity movedIdentity;
    LocalDirectoryIdentity movedDirIdentity;
    LocalDirectoryTreeSnapshot movedDirSnapshot;
    QString errorText;
};

struct BatchMoveExpectedItem {
    BatchMoveItemKind kind = BatchMoveItemKind::RegularFile;
    LocalRegularFileIdentity fileIdentity;
    LocalDirectoryIdentity dirIdentity;
    LocalDirectoryTreeSnapshot dirSnapshot;
};

struct BatchMovePreflight {
    bool ok = false;
    BatchMoveError error = BatchMoveError::InvalidInput;
    int systemError = 0;
    int failedItemIndex = -1; // -1 for global/batch-wide errors without a single culprit item
    QString reason;
    LocalDirectoryIdentity anchorScope;
    LocalBtrfsMoveCommon::VerifiedMoveFsProvenance verifiedFsProvenance =
        LocalBtrfsMoveCommon::VerifiedMoveFsProvenance::Unknown;
    QList<BatchMoveItemPlan> items;
    QList<BatchMoveItemKind> observedKinds;
    quint64 totalBytes = 0;
};

struct BatchMoveResult {
    BatchMoveOutcome outcome = BatchMoveOutcome::FullRefusalBeforeExecution;
    BatchMoveError error = BatchMoveError::InvalidInput;
    int systemError = 0;
    int failedItemIndex = -1; // -1 for global/batch-wide errors without a single culprit item
    QString reason;
    int totalCount = 0;
    int publishedCount = 0;
    int durableCount = 0;
    int failedCount = 0;
    int skippedCount = 0;
    bool partialMoveOutcome = false;
    LocalBtrfsMoveCommon::VerifiedMoveFsProvenance verifiedFsProvenance =
        LocalBtrfsMoveCommon::VerifiedMoveFsProvenance::Unknown;
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

inline bool &simulateBatchMoveExt4Filesystem()
{
    static bool flag = false;
    return flag;
}

inline bool &simulateBatchMoveExt4StatxMntIdUniqueMissing()
{
    static bool flag = false;
    return flag;
}

inline bool &simulateBatchMoveExt4FlagsError()
{
    static bool flag = false;
    return flag;
}

inline bool &simulateBatchMoveExt4CasefoldAllDirs()
{
    static bool flag = false;
    return flag;
}

inline quint64 &simulateBatchMoveExt4CasefoldInode()
{
    static quint64 ino = 0;
    return ino;
}

inline bool &simulateBatchMoveExt4Encrypt()
{
    static bool flag = false;
    return flag;
}

inline std::atomic<int> &batchMoveSnapshotFilesHashedCount()
{
    static std::atomic<int> count{0};
    return count;
}
#endif

namespace LocalBatchMoveDetail {

using LocalBtrfsMoveCommon::ScopedFd;
using LocalBtrfsMoveCommon::trMsg;
using LocalBtrfsMoveCommon::splitCleanLocalPath;
using LocalBtrfsMoveCommon::isMountPointInMountInfo;

inline LocalBtrfsMoveCommon::TestFaultHooks currentBatchMoveFaultHooks()
{
    LocalBtrfsMoveCommon::TestFaultHooks hooks;
#ifdef THISPC_TEST_HARNESS
    hooks.openat2Unsupported = simulateBatchMoveOpenat2Unsupported();
    hooks.unsupportedFsMagic = simulateBatchMoveUnsupportedFsMagic();
    hooks.statxMntIdUnsupported = simulateBatchMoveStatxMntIdUnsupported();
    hooks.simulateExt4Filesystem = simulateBatchMoveExt4Filesystem();
    hooks.simulateExt4StatxMntIdUniqueMissing = simulateBatchMoveExt4StatxMntIdUniqueMissing();
    hooks.simulateExt4FlagsError = simulateBatchMoveExt4FlagsError();
    hooks.simulateExt4CasefoldAllDirs = simulateBatchMoveExt4CasefoldAllDirs();
    hooks.simulateExt4CasefoldInode = simulateBatchMoveExt4CasefoldInode();
    hooks.simulateExt4Encrypt = simulateBatchMoveExt4Encrypt();
    hooks.snapshotFilesHashedCounter = &batchMoveSnapshotFilesHashedCount();
#endif
    return hooks;
}

inline int openBatchDirNoSymlinks(const QString &absDirPath, int *errOut)
{
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentBatchMoveFaultHooks();
    return LocalBtrfsMoveCommon::openDirectoryNoSymlinks(absDirPath, errOut, &hooks);
}

inline int openBatchChildPathNoCrossing(int parentFd, const char *leafName, int *errOut)
{
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentBatchMoveFaultHooks();
    return LocalBtrfsMoveCommon::openChildPathNoCrossing(parentFd, leafName, errOut, &hooks);
}

inline int openBatchChildDirNoCrossing(int parentFd, const char *leafName, int *errOut)
{
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentBatchMoveFaultHooks();
    return LocalBtrfsMoveCommon::openChildDirectoryNoCrossing(parentFd, leafName, errOut, &hooks);
}

inline bool queryBatchFdMetadata(
    int fd,
    struct statx *stxOut,
    struct stat *stOut,
    unsigned long *fsMagicOut,
    int *errOut)
{
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentBatchMoveFaultHooks();
    return LocalBtrfsMoveCommon::queryFdMetadata(fd, stxOut, stOut, fsMagicOut, errOut, &hooks);
}

inline QString unsupportedBatchFsMessage(
    const LocalBtrfsMoveCommon::FilesystemMoveQualificationProfile &profile = {})
{
    if (profile.fsProfile == LocalBtrfsMoveCommon::LocalMoveFsProfile::Ext4Only) {
        return trMsg(
            "Zbiorcze przenoszenie na tym etapie wymaga tego samego woluminu ext4 bez nadpisywania. Użyj operacji Kopiuj.",
            "Batch moving on this profile requires the same ext4 volume without overwriting. Use Copy instead.");
    }
    return trMsg(
        "Zbiorcze przenoszenie plików (Batch Move) jest obecnie dozwolone wyłącznie w obrębie tego samego woluminu Btrfs bez nadpisywania. Użyj operacji Kopiuj.",
        "Batch file moving is currently permitted only within the same Btrfs volume without overwriting. Use Copy instead.");
}

inline BatchMoveError mapContainmentFailureToBatchError(
    LocalBtrfsMoveCommon::ContainmentFailureKind kind)
{
    switch (kind) {
    case LocalBtrfsMoveCommon::ContainmentFailureKind::Cancelled:
        return BatchMoveError::Cancelled;
    case LocalBtrfsMoveCommon::ContainmentFailureKind::InvalidInput:
        return BatchMoveError::InvalidInput;
    case LocalBtrfsMoveCommon::ContainmentFailureKind::UnsupportedKernelFeature:
        return BatchMoveError::UnsupportedKernelFeature;
    case LocalBtrfsMoveCommon::ContainmentFailureKind::UnsupportedFilesystem:
        return BatchMoveError::UnsupportedFilesystem;
    case LocalBtrfsMoveCommon::ContainmentFailureKind::MountOrSubvolumeBoundary:
        return BatchMoveError::MountOrSubvolumeBoundary;
    case LocalBtrfsMoveCommon::ContainmentFailureKind::ContainmentViolation:
    case LocalBtrfsMoveCommon::ContainmentFailureKind::None:
        return BatchMoveError::ContainmentViolation;
    }
    return BatchMoveError::ContainmentViolation;
}

struct OpenedBatchParent {
    ScopedFd fd;
    LocalDirectoryIdentity identity;
};

struct OpenedBatchExecutionContext {
    QHash<QString, std::shared_ptr<OpenedBatchParent>> parents;
    QHash<int, std::shared_ptr<ScopedFd>> dirFds;
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
    const QList<BatchMoveExpectedItem> *expectedItems,
    bool strictHistoryCheck,
    BatchMovePreflight *outPreflight,
    OpenedBatchExecutionContext *outCtx,
    bool allowDirectories = false,
    bool deepInspectDirectories = false,
    const std::atomic<bool> *cancelledPtr = nullptr,
    const LocalBtrfsMoveCommon::FilesystemMoveQualificationProfile &profile = {},
    bool captureStrictDirectorySnapshot = false)
{
    BatchMovePreflight pf;
    pf.observedKinds.fill(BatchMoveItemKind::Unknown, pairs.size());
    auto failWithIndex = [&](int itemIndex, BatchMoveError err, int sysErr, const QString &reason) {
        pf.ok = false;
        pf.error = err;
        pf.systemError = sysErr;
        pf.failedItemIndex = itemIndex;
        pf.reason = reason;
        if (outPreflight) *outPreflight = pf;
        return false;
    };
    auto failGlobal = [&](BatchMoveError err, int sysErr, const QString &reason) {
        return failWithIndex(-1, err, sysErr, reason);
    };
    auto failItem = [&](int itemIndex, BatchMoveError err, int sysErr, const QString &reason) {
        return failWithIndex(itemIndex, err, sysErr, reason);
    };

    if (pairs.size() < 2) {
        return failGlobal(BatchMoveError::InvalidInput, EINVAL,
                          trMsg("Zbiorcze przenoszenie wymaga co najmniej dwóch kwalifikowanych elementów.",
                                "Batch move requires at least two qualified items."));
    }
    if (expectedItems && expectedItems->size() != pairs.size()) {
        return failGlobal(BatchMoveError::InvalidInput, EINVAL,
                          moveContainmentErrorMessage());
    }

    const std::atomic<bool> defaultCancelled{false};
    const std::atomic<bool> &cancelled = cancelledPtr ? *cancelledPtr : defaultCancelled;
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentBatchMoveFaultHooks();

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
        LocalBtrfsMoveCommon::ContainmentFailureKind parentFail =
            LocalBtrfsMoveCommon::ContainmentFailureKind::None;
        QString parentFailReason;
        if (!LocalBtrfsMoveCommon::validateQualifiedMountAndFs(
                stx, magic, profile, &parentFail, &parentFailReason)) {
            *errOut = mapContainmentFailureToBatchError(parentFail);
            *sysErrOut = (parentFail == LocalBtrfsMoveCommon::ContainmentFailureKind::UnsupportedKernelFeature)
                ? EOPNOTSUPP
                : EXDEV;
            *msgOut = parentFailReason;
            return false;
        }
        const LocalDirectoryIdentity dirId = LocalDirectoryIdentity::fromStat(stx, st);
        if (!dirId.valid) {
            *errOut = BatchMoveError::ContainmentViolation;
            *sysErrOut = EINVAL;
            *msgOut = moveContainmentErrorMessage();
            return false;
        }
        if (profile.checkExt4DirectoryFlags()) {
            LocalBtrfsMoveCommon::ContainmentFailureKind flagFail =
                LocalBtrfsMoveCommon::ContainmentFailureKind::None;
            QString flagReason;
            if (!LocalBtrfsMoveCommon::verifyExt4DirectorySafetyFlags(
                    fd.fd, dirId.ino, &flagFail, &flagReason, &hooks)) {
                *errOut = mapContainmentFailureToBatchError(flagFail);
                *sysErrOut = EOPNOTSUPP;
                *msgOut = flagReason;
                return false;
            }
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
    QList<QSet<QPair<quint64, quint64>>> perItemVisitedObjects;
    perItemVisitedObjects.resize(pairs.size());
    QList<std::shared_ptr<OpenedBatchParent>> uniqueDstParents;
    QSet<QString> seenDstParentPaths;
    QList<BatchMoveItemPlan> plans;
    plans.reserve(pairs.size());

    // First pass: path validation, intra-batch path uniqueness, parent qualification, and child inspection
    for (int i = 0; i < pairs.size(); ++i) {
        if (cancelled.load(std::memory_order_acquire)) {
            return failGlobal(BatchMoveError::Cancelled, ECANCELED,
                              trMsg("Operacja anulowana.", "Operation cancelled."));
        }

        const QUrl &srcUrl = pairs.at(i).first;
        const QUrl &dstUrl = pairs.at(i).second;
        if (!srcUrl.isValid() || !dstUrl.isValid() || !srcUrl.isLocalFile() || !dstUrl.isLocalFile()) {
            return failItem(i, BatchMoveError::InvalidInput, EINVAL, moveContainmentErrorMessage());
        }

        const QString rawSrc = srcUrl.toLocalFile();
        const QString rawDst = dstUrl.toLocalFile();
        QString srcParent, srcLeaf, dstParent, dstLeaf;
        if (!splitCleanLocalPath(rawSrc, &srcParent, &srcLeaf)
            || !splitCleanLocalPath(rawDst, &dstParent, &dstLeaf)) {
            return failItem(i, BatchMoveError::InvalidInput, EINVAL, moveContainmentErrorMessage());
        }
        const QString cleanSrc = QDir::cleanPath(rawSrc);
        const QString cleanDst = QDir::cleanPath(rawDst);
        if (cleanSrc == cleanDst) {
            return failItem(i, BatchMoveError::CollisionInBatch, EINVAL,
                            trMsg("Źródło i cel wskazują na ten sam element.",
                                  "Source and destination refer to the same item."));
        }
        if (seenCleanSources.contains(cleanSrc)) {
            return failItem(i, BatchMoveError::CollisionInBatch, EINVAL,
                            trMsg("Wykryto zduplikowane źródło na liście przenoszenia.",
                                  "Duplicate source entry detected in batch move list."));
        }
        if (seenCleanTargets.contains(cleanDst)) {
            return failItem(i, BatchMoveError::CollisionInBatch, EEXIST,
                            trMsg("Co najmniej dwa wybrane elementy mają tę samą nazwę docelową — przenoszenie bez nadpisywania zablokowane.",
                                  "Two or more selected items map to the same destination name — batch move blocked to prevent overwrite."));
        }
        seenCleanSources.insert(cleanSrc);
        seenCleanTargets.insert(cleanDst);

        if (isMountPointInMountInfo(cleanSrc)) {
            return failItem(i, BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                            trMsg("Przenoszenie punktu montowania jest zablokowane ze względów bezpieczeństwa.",
                                  "Moving a mount point is blocked for safety."));
        }

        std::shared_ptr<OpenedBatchParent> srcParentEntry;
        std::shared_ptr<OpenedBatchParent> dstParentEntry;
        BatchMoveError pErr = BatchMoveError::InvalidInput;
        int pSysErr = 0;
        QString pMsg;
        if (!getOrOpenParent(dstParent, &dstParentEntry, &pErr, &pSysErr, &pMsg)) {
            return failItem(i, pErr, pSysErr, pMsg);
        }
        if (!getOrOpenParent(srcParent, &srcParentEntry, &pErr, &pSysErr, &pMsg)) {
            return failItem(i, pErr, pSysErr, pMsg);
        }

        if (!seenDstParentPaths.contains(dstParent)) {
            seenDstParentPaths.insert(dstParent);
            uniqueDstParents.append(dstParentEntry);
        }

        if (!pf.anchorScope.valid) {
            pf.anchorScope = dstParentEntry->identity;
        }
        if (!dstParentEntry->identity.sameMountScope(pf.anchorScope)
            || !srcParentEntry->identity.sameMountScope(pf.anchorScope)) {
            return failItem(i, BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                            trMsg("Wszystkie źródła i cele partii muszą znajdować się na tym samym montowaniu i podwoluminie Btrfs.",
                                  "All batch sources and destinations must reside on the same Btrfs mount and subvolume."));
        }

        if (srcParentEntry->identity.sameObject(dstParentEntry->identity) && srcLeaf == dstLeaf) {
            return failItem(i, BatchMoveError::CollisionInBatch, EINVAL,
                            trMsg("Źródło i cel wskazują na ten sam element.",
                                  "Source and destination refer to the same item."));
        }

        // Open source entry strictly non-crossing & non-dereferencing via O_PATH
        const QByteArray srcLeafBytes = QFile::encodeName(srcLeaf);
        int childErr = 0;
        ScopedFd childFd(openBatchChildPathNoCrossing(srcParentEntry->fd.fd, srcLeafBytes.constData(), &childErr));
        if (!childFd.isValid()) {
            if (childErr == ENOSYS) {
                return failItem(i, BatchMoveError::UnsupportedKernelFeature, childErr,
                                trMsg("Jądro nie udostępnia wymaganego bezpiecznego mechanizmu openat2.",
                                      "The kernel does not support required safe openat2 lookup."));
            }
            if (childErr == EXDEV) {
                return failItem(i, BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                                trMsg("Źródło przekracza granicę montowania lub podwoluminu Btrfs.",
                                      "Source crosses a mount or Btrfs subvolume boundary."));
            }
            return failItem(i, BatchMoveError::ContainmentViolation, childErr, moveContainmentErrorMessage());
        }

        struct statx childStx {};
        struct stat childSt {};
        unsigned long childMagic = 0;
        if (!queryBatchFdMetadata(childFd.fd, &childStx, &childSt, &childMagic, &childErr)) {
            return failItem(i, BatchMoveError::ContainmentViolation, childErr, moveContainmentErrorMessage());
        }
        LocalBtrfsMoveCommon::ContainmentFailureKind childFail =
            LocalBtrfsMoveCommon::ContainmentFailureKind::None;
        QString childFailReason;
        if (!LocalBtrfsMoveCommon::validateQualifiedMountAndFs(
                childStx, childMagic, profile, &childFail, &childFailReason)) {
            const BatchMoveError mappedErr = mapContainmentFailureToBatchError(childFail);
            return failItem(i, mappedErr,
                            mappedErr == BatchMoveError::UnsupportedKernelFeature ? EOPNOTSUPP : EXDEV,
                            childFailReason);
        }
        if (!(childStx.stx_mask & STATX_TYPE)) {
            return failItem(i, BatchMoveError::ContainmentViolation, EINVAL, moveContainmentErrorMessage());
        }
        const LocalBtrfsMoveCommon::VerifiedMoveFsProvenance itemProvenance =
            LocalBtrfsMoveCommon::provenanceFromFsMagic(childMagic);
        if (itemProvenance == LocalBtrfsMoveCommon::VerifiedMoveFsProvenance::Unknown
            || (pf.verifiedFsProvenance != LocalBtrfsMoveCommon::VerifiedMoveFsProvenance::Unknown
                && pf.verifiedFsProvenance != itemProvenance)) {
            return failItem(i, BatchMoveError::UnsupportedFilesystem, EXDEV,
                            trMsg("Nie udało się jednoznacznie zweryfikować pochodzenia systemu plików.",
                                  "Could not unambiguously verify filesystem provenance."));
        }
        pf.verifiedFsProvenance = itemProvenance;

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
        itemPlan.srcParentIdentity = srcParentEntry->identity;
        itemPlan.dstParentIdentity = dstParentEntry->identity;

        if (S_ISREG(childStx.stx_mode)) {
            pf.observedKinds[i] = BatchMoveItemKind::RegularFile;
            const LocalRegularFileIdentity fileId = LocalRegularFileIdentity::fromStat(childStx, childSt);
            if (!fileId.valid) {
                return failItem(i, BatchMoveError::ContainmentViolation, EINVAL, moveContainmentErrorMessage());
            }
            if (fileId.nlink > 1) {
                return failItem(i, BatchMoveError::ContainmentViolation, EMLINK, moveContainmentErrorMessage());
            }
            if (!fileId.sameMountScope(pf.anchorScope)) {
                return failItem(i, BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                                trMsg("Jeden z plików znajduje się poza kwalifikowanym montowaniem lub podwoluminem Btrfs.",
                                      "One of the files resides outside the qualified Btrfs mount or subvolume."));
            }

            const QPair<quint64, quint64> objKey(fileId.stDev, fileId.ino);
            if (seenSourceObjects.contains(objKey)) {
                return failItem(i, BatchMoveError::CollisionInBatch, EINVAL,
                                trMsg("Wykryto dwa wpisy wskazujące ten sam obiekt (alias lub hardlink) w jednej partii.",
                                      "Two entries pointing to the same file object detected in one batch."));
            }
            seenSourceObjects.insert(objKey);
            perItemVisitedObjects[i].insert(objKey);

            if (expectedItems) {
                const BatchMoveExpectedItem &exp = expectedItems->at(i);
                const bool idMatch = (exp.kind == BatchMoveItemKind::RegularFile)
                    && (strictHistoryCheck
                            ? fileId.strictlyUnchangedForHistory(exp.fileIdentity)
                            : fileId.sameObject(exp.fileIdentity));
                if (!idMatch) {
                    return failItem(i, BatchMoveError::ContainmentViolation, EBADF,
                                    trMsg("Zawartość lub tożsamość co najmniej jednego pliku uległa zmianie — cofnięcie/ponowienie partii zostało zablokowane.",
                                          "Content or identity of at least one file changed — batch Undo/Redo blocked for safety."));
                }
            }

            itemPlan.kind = BatchMoveItemKind::RegularFile;
            itemPlan.sourceIdentity = fileId;
            itemPlan.size = fileId.size;
            pf.totalBytes += fileId.size;
        } else if (S_ISDIR(childStx.stx_mode)) {
            pf.observedKinds[i] = BatchMoveItemKind::Directory;
            if (!allowDirectories) {
                return failItem(i, BatchMoveError::ContainmentViolation, EINVAL, moveContainmentErrorMessage());
            }
            if (expectedItems && expectedItems->at(i).kind != BatchMoveItemKind::Directory) {
                return failItem(i, BatchMoveError::ContainmentViolation, EBADF,
                                trMsg("Typ co najmniej jednego elementu uległ zmianie — cofnięcie/ponowienie partii zostało zablokowane.",
                                      "Type of at least one item changed — batch Undo/Redo blocked for safety."));
            }

            int dirErr = 0;
            ScopedFd dirFd(openBatchChildDirNoCrossing(srcParentEntry->fd.fd, srcLeafBytes.constData(), &dirErr));
            if (!dirFd.isValid()) {
                if (dirErr == ENOSYS) {
                    return failItem(i, BatchMoveError::UnsupportedKernelFeature, dirErr,
                                    trMsg("Jądro nie udostępnia wymaganego bezpiecznego mechanizmu openat2.",
                                          "The kernel does not support required safe openat2 lookup."));
                }
                if (dirErr == EXDEV) {
                    return failItem(i, BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                                    trMsg("Katalog przekracza granicę montowania lub podwoluminu Btrfs.",
                                          "Directory crosses a mount or Btrfs subvolume boundary."));
                }
                return failItem(i, BatchMoveError::ContainmentViolation, dirErr, moveContainmentErrorMessage());
            }

            struct statx dirStx {};
            struct stat dirSt {};
            unsigned long dirMagic = 0;
            if (!queryBatchFdMetadata(dirFd.fd, &dirStx, &dirSt, &dirMagic, &dirErr)) {
                return failItem(i, BatchMoveError::ContainmentViolation, dirErr, moveContainmentErrorMessage());
            }
            LocalBtrfsMoveCommon::ContainmentFailureKind dirFail =
                LocalBtrfsMoveCommon::ContainmentFailureKind::None;
            QString dirFailReason;
            if (!LocalBtrfsMoveCommon::validateQualifiedMountAndFs(
                    dirStx, dirMagic, profile, &dirFail, &dirFailReason)) {
                const BatchMoveError mappedErr = mapContainmentFailureToBatchError(dirFail);
                return failItem(i, mappedErr,
                                mappedErr == BatchMoveError::UnsupportedKernelFeature ? EOPNOTSUPP : EXDEV,
                                dirFailReason);
            }

            const LocalDirectoryIdentity dirId = LocalDirectoryIdentity::fromStat(dirStx, dirSt);
            if (!dirId.valid) {
                return failItem(i, BatchMoveError::ContainmentViolation, EINVAL, moveContainmentErrorMessage());
            }
            if (profile.checkBtrfsSubvolRootInode() && dirId.ino == BTRFS_FIRST_FREE_OBJECTID) {
                return failItem(i, BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                                trMsg("Przenoszenie katalogu głównego podwoluminu lub woluminu Btrfs jest zablokowane.",
                                      "Moving a Btrfs volume or subvolume root directory is blocked."));
            }
            if (profile.checkExt4DirectoryFlags()) {
                LocalBtrfsMoveCommon::ContainmentFailureKind flagFail =
                    LocalBtrfsMoveCommon::ContainmentFailureKind::None;
                QString flagReason;
                if (!LocalBtrfsMoveCommon::verifyExt4DirectorySafetyFlags(
                        dirFd.fd, dirId.ino, &flagFail, &flagReason, &hooks)) {
                    const BatchMoveError mappedErr = mapContainmentFailureToBatchError(flagFail);
                    return failItem(i, mappedErr, EOPNOTSUPP, flagReason);
                }
            }
            if (!dirId.sameMountScope(pf.anchorScope)) {
                return failItem(i, BatchMoveError::MountOrSubvolumeBoundary, EXDEV,
                                trMsg("Jeden z katalogów znajduje się poza kwalifikowanym montowaniem lub podwoluminem.",
                                      "One of the directories resides outside the qualified mount or subvolume."));
            }

            const QPair<quint64, quint64> objKey(dirId.stDev, dirId.ino);
            if (seenSourceObjects.contains(objKey)) {
                return failItem(i, BatchMoveError::CollisionInBatch, EINVAL,
                                trMsg("Wykryto dwa wpisy wskazujące ten sam katalog w jednej partii.",
                                      "Two entries pointing to the same directory object detected in one batch."));
            }
            seenSourceObjects.insert(objKey);
            perItemVisitedObjects[i].insert(objKey);

            LocalDirectoryTreeSnapshot snap;
            if (deepInspectDirectories) {
                LocalBtrfsMoveCommon::ContainmentFailureKind failureKind =
                    LocalBtrfsMoveCommon::ContainmentFailureKind::None;
                QString inspectReason;
                if (!LocalBtrfsMoveCommon::inspectDirectoryTreeContainment(
                        dirFd.fd, dirId, cancelled, &snap, &perItemVisitedObjects[i],
                        &failureKind, &inspectReason, &hooks, profile,
                        strictHistoryCheck, strictHistoryCheck || captureStrictDirectorySnapshot)) {
                    const BatchMoveError mappedErr = mapContainmentFailureToBatchError(failureKind);
                    if (mappedErr == BatchMoveError::Cancelled) {
                        return failGlobal(mappedErr, ECANCELED, inspectReason);
                    }
                    return failItem(i, mappedErr, EINVAL, inspectReason);
                }
                pf.totalBytes += snap.scannedBytes;
                localCtx.dirFds.insert(i, std::make_shared<ScopedFd>(std::move(dirFd)));
            }

            if (expectedItems) {
                const BatchMoveExpectedItem &exp = expectedItems->at(i);
                const bool dirIdMatch = strictHistoryCheck
                    ? dirId.strictlyUnchangedForHistory(exp.dirIdentity)
                    : dirId.sameObject(exp.dirIdentity);
                const bool snapMatch = strictHistoryCheck
                    ? (deepInspectDirectories && snap.matches(exp.dirSnapshot, true))
                    : (!deepInspectDirectories || !exp.dirSnapshot.valid || snap.matches(exp.dirSnapshot));
                if (!dirIdMatch || !snapMatch) {
                    return failItem(i, BatchMoveError::ContainmentViolation, EBADF,
                                    trMsg("Zawartość lub tożsamość co najmniej jednego katalogu uległa zmianie — cofnięcie/ponowienie partii zostało zablokowane.",
                                          "Content or identity of at least one directory changed — batch Undo/Redo blocked for safety."));
                }
            }

            itemPlan.kind = BatchMoveItemKind::Directory;
            itemPlan.dirIdentity = dirId;
            itemPlan.dirSnapshot = snap;
            itemPlan.size = snap.scannedBytes;
        } else {
            return failItem(i, BatchMoveError::ContainmentViolation, EINVAL, moveContainmentErrorMessage());
        }

        // Check destination path absence (non-dereferencing)
        const QByteArray dstLeafBytes = QFile::encodeName(dstLeaf);
        struct stat dstExistSt {};
        if (::fstatat(dstParentEntry->fd.fd, dstLeafBytes.constData(), &dstExistSt, AT_SYMLINK_NOFOLLOW) == 0) {
            return failItem(i, BatchMoveError::DestinationExists, EEXIST,
                            trMsg("Element docelowy „%1” już istnieje. Zbiorcze przenoszenie z nadpisywaniem jest niedostępne.",
                                  "Destination item \"%1\" already exists. Batch move with overwrite is unavailable.")
                                .arg(dstLeaf));
        }
        const int dstStatErr = errno;
        if (dstStatErr != ENOENT) {
            return failItem(i, BatchMoveError::InvalidInput, dstStatErr,
                            trMsg("Nie można zweryfikować dostępności ścieżki docelowej.",
                                  "Could not verify destination availability."));
        }

        plans.append(itemPlan);
    }

    // Second pass: disallow ancestor-descendant source overlaps, target-inside-source, and target==source
    for (int i = 0; i < plans.size(); ++i) {
        if (seenCleanTargets.contains(plans.at(i).cleanSrcPath)) {
            return failItem(i, BatchMoveError::CollisionInBatch, EINVAL,
                            trMsg("Ścieżka źródłowa pokrywa się ze ścieżką docelową innego elementu partii.",
                                  "A source path overlaps with another item's destination path within the batch."));
        }
        for (int j = i + 1; j < plans.size(); ++j) {
            if (LocalBtrfsMoveCommon::pathsOverlap(plans.at(i).cleanSrcPath, plans.at(j).cleanSrcPath)) {
                return failItem(j, BatchMoveError::CollisionInBatch, EINVAL,
                                trMsg("Wykryto zagnieżdżone źródła w jednej partii — operacja zablokowana.",
                                      "Nested source items detected in one batch — blocked for safety."));
            }
        }
        if (plans.at(i).kind == BatchMoveItemKind::Directory) {
            for (int j = 0; j < plans.size(); ++j) {
                if (LocalBtrfsMoveCommon::pathsOverlap(plans.at(i).cleanSrcPath, plans.at(j).cleanDstPath)) {
                    return failItem(i, BatchMoveError::CycleDetected, EINVAL,
                                    trMsg("Nie można przenieść folderu do niego samego lub do jednego z jego podfolderów.",
                                          "A folder cannot be moved into itself or one of its subfolders."));
                }
            }
            for (const auto &dstP : uniqueDstParents) {
                if (LocalBtrfsMoveCommon::dstParentHasAncestor(
                        dstP->fd.fd, dstP->identity, plans.at(i).dirIdentity, &hooks)) {
                    return failItem(i, BatchMoveError::CycleDetected, EINVAL,
                                    trMsg("Nie można przenieść folderu do niego samego lub do jednego z jego podfolderów.",
                                          "A folder cannot be moved into itself or one of its subfolders."));
                }
            }
        }
    }

    // Cross-subtree object overlap check (under same anchorScope verified above)
    if (deepInspectDirectories) {
        QSet<QPair<quint64, quint64>> mergedVisited;
        for (int i = 0; i < plans.size(); ++i) {
            for (const auto &obj : std::as_const(perItemVisitedObjects.at(i))) {
                if (mergedVisited.contains(obj)) {
                    return failItem(i, BatchMoveError::CollisionInBatch, EINVAL,
                                    trMsg("Wykryto nakładające się obiekty lub zagnieżdżone poddrzewa w jednej partii.",
                                          "Overlapping objects or nested subtrees detected within one batch."));
                }
            }
            mergedVisited.unite(perItemVisitedObjects.at(i));
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
    return runPreflight(pairs, nullptr, false, outPreflight, nullptr, false, false, nullptr);
}

inline bool qualifyFastForMixedBatchPolicy(
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
    return runPreflight(pairs, nullptr, false, outPreflight, nullptr, true, false, nullptr);
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
        qRegisterMetaType<BatchMoveItemKind>();
        qRegisterMetaType<BatchMoveItemState>();
        qRegisterMetaType<BatchMoveOutcome>();
        setCapabilities(KJob::Killable);
    }

    bool allowDirectories() const { return m_allowDirectories; }
    void setAllowDirectories(bool allow) { m_allowDirectories = allow; }
    bool allowExt4() const { return m_allowExt4; }
    void setAllowExt4(bool allow) { m_allowExt4 = allow; }
    bool captureStrictDirectorySnapshot() const { return m_captureStrictDirectorySnapshot; }
    bool setCaptureStrictDirectorySnapshot(bool enabled)
    {
        if (m_started || m_finished) {
            return false;
        }
        m_captureStrictDirectorySnapshot = enabled;
        return true;
    }
    bool hasStarted() const { return m_started; }
    bool hasFinished() const { return m_finished; }
    bool claimHistoryAttachment()
    {
        if (m_started || m_finished || m_historyAttachmentClaimed) {
            return false;
        }
        m_historyAttachmentClaimed = true;
        m_captureStrictDirectorySnapshot = true;
        return true;
    }
    LocalBtrfsMoveCommon::VerifiedMoveFsProvenance verifiedFsProvenance() const
    {
        return m_result.verifiedFsProvenance;
    }
    void setExpectedItems(const QList<BatchMoveExpectedItem> &items) { m_expectedItems = items; }

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
                res.batch.failedItemIndex = -1;
                res.batch.failedCount = 0;
                res.batch.skippedCount = res.batch.totalCount;
                res.batch.items.resize(m_pairs.size());
                for (int i = 0; i < m_pairs.size(); ++i) {
                    res.batch.items[i].index = i;
                    res.batch.items[i].kind = BatchMoveItemKind::Unknown;
                    res.batch.items[i].sourceUrl = m_pairs.at(i).first;
                    res.batch.items[i].destinationUrl = m_pairs.at(i).second;
                    res.batch.items[i].state = BatchMoveItemState::SkippedAfterAbort;
                    res.batch.items[i].error = BatchMoveError::InvalidInput;
                }
                res.batch.reason = LocalBatchMoveDetail::trMsg(
                    "Zbiorcze przenoszenie wymaga co najmniej dwóch kwalifikowanych plików.",
                    "Batch move requires at least two qualified files.");
                applyWorkerResult(res);
            });
            return;
        }

        const QList<QPair<QUrl, QUrl>> pairsCopy = m_pairs;
        QList<BatchMoveExpectedItem> expectedCopy = m_expectedItems;
        if (expectedCopy.isEmpty() && !m_expectedIdentities.isEmpty()) {
            expectedCopy.reserve(m_expectedIdentities.size());
            for (const auto &fid : std::as_const(m_expectedIdentities)) {
                BatchMoveExpectedItem item;
                item.kind = BatchMoveItemKind::RegularFile;
                item.fileIdentity = fid;
                expectedCopy.append(item);
            }
        }
        const bool strictCopy = m_strictHistoryCheck;
        const bool allowDirsCopy = m_allowDirectories;
        const bool allowExt4Copy = m_allowExt4;
        const bool captureStrictCopy = m_captureStrictDirectorySnapshot;

        m_thread = QThread::create([this, pairsCopy, expectedCopy, strictCopy, allowDirsCopy, allowExt4Copy, captureStrictCopy] {
            executeWorker(pairsCopy, expectedCopy, strictCopy, allowDirsCopy, allowExt4Copy, captureStrictCopy);
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
    int failedItemIndex() const { return m_result.failedItemIndex; }
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
        const QList<BatchMoveExpectedItem> &expectedItems,
        bool strictHistoryCheck,
        bool allowDirectories,
        bool allowExt4,
        bool captureStrictDirectorySnapshot)
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

        const LocalBtrfsMoveCommon::FilesystemMoveQualificationProfile profile{
            allowExt4 ? LocalBtrfsMoveCommon::LocalMoveFsProfile::Ext4Only
                      : LocalBtrfsMoveCommon::LocalMoveFsProfile::BtrfsOnly};

        BatchMovePreflight pf;
        LocalBatchMoveDetail::OpenedBatchExecutionContext ctx;
        const QList<BatchMoveExpectedItem> *expectedPtr =
            expectedItems.isEmpty() ? nullptr : &expectedItems;
        if (!LocalBatchMoveDetail::runPreflight(
                pairs, expectedPtr, strictHistoryCheck, &pf, &ctx,
                allowDirectories, true, &m_cancelled, profile, captureStrictDirectorySnapshot)) {
            res.batch.outcome = BatchMoveOutcome::FullRefusalBeforeExecution;
            res.batch.error = pf.error;
            res.batch.systemError = pf.systemError;
            res.batch.failedItemIndex = pf.failedItemIndex;
            res.batch.reason = pf.reason;
            for (int i = 0; i < res.batch.items.size(); ++i) {
                if (i < pf.observedKinds.size()) {
                    res.batch.items[i].kind = pf.observedKinds.at(i);
                }
            }
            if (pf.error == BatchMoveError::Cancelled
                || pf.failedItemIndex < 0
                || pf.failedItemIndex >= res.batch.items.size()) {
                res.batch.failedCount = 0;
                res.batch.skippedCount = res.batch.totalCount;
                for (int i = 0; i < res.batch.items.size(); ++i) {
                    res.batch.items[i].state = BatchMoveItemState::SkippedAfterAbort;
                    res.batch.items[i].error = pf.error;
                }
            } else {
                res.batch.failedCount = 1;
                res.batch.skippedCount = qMax(0, res.batch.totalCount - 1);
                for (int i = 0; i < res.batch.items.size(); ++i) {
                    if (i == pf.failedItemIndex) {
                        res.batch.items[i].state = BatchMoveItemState::FailedBeforePublish;
                        res.batch.items[i].error = pf.error;
                        res.batch.items[i].systemError = pf.systemError;
                        res.batch.items[i].errorText = pf.reason;
                    } else {
                        res.batch.items[i].state = BatchMoveItemState::SkippedAfterAbort;
                        res.batch.items[i].error = pf.error;
                    }
                }
            }
            publishAndReturn();
            return;
        }

        for (int i = 0; i < pf.items.size(); ++i) {
            res.batch.items[i].kind = pf.items.at(i).kind;
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

            // Step revalidation: verify source and parent metadata + tree containment (for dirs) before renameat2
            const unsigned int statxMask =
                STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE | STATX_SUBVOL;
            LocalRegularFileIdentity currentSrcId;
            LocalDirectoryIdentity currentDirId;
            LocalDirectoryTreeSnapshot currentDirSnap;
            if (itemPlan.kind == BatchMoveItemKind::RegularFile) {
                struct statx preSrcStx {};
                struct stat preSrcSt {};
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
                        "Plik źródłowy „%1” zniknął lub zmienił się przed przeniesieniem — przerwano bez zmian pozostałych elementów.",
                        "Source file \"%1\" disappeared or changed before move — stopped remaining items.")
                        .arg(itemPlan.srcLeaf);
                    itemRes.errorText = abortReason;
                    for (int j = i + 1; j < totalCount; ++j) {
                        res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    }
                    break;
                }

                const LocalBtrfsMoveCommon::TestFaultHooks hooks = LocalBatchMoveDetail::currentBatchMoveFaultHooks();
                if (hooks.simulateExt4Filesystem) {
                    preSrcStx.stx_mask &= ~STATX_SUBVOL;
                    preSrcStx.stx_subvol = 0;
                    if (hooks.simulateExt4StatxMntIdUniqueMissing) {
                        preSrcStx.stx_mask &= ~STATX_MNT_ID_UNIQUE;
                    }
                }
                currentSrcId = LocalRegularFileIdentity::fromStat(preSrcStx, preSrcSt);
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
            } else {
                const LocalBtrfsMoveCommon::TestFaultHooks hooks = LocalBatchMoveDetail::currentBatchMoveFaultHooks();
                struct statx spStx {}, dpStx {};
                struct stat spSt {}, dpSt {};
                unsigned long spMagic = 0, dpMagic = 0;
                int metaErr = 0;
                LocalBtrfsMoveCommon::ContainmentFailureKind parentRevalFail =
                    LocalBtrfsMoveCommon::ContainmentFailureKind::None;
                QString parentRevalReason;
                if (!LocalBatchMoveDetail::queryBatchFdMetadata(srcParentFd, &spStx, &spSt, &spMagic, &metaErr)
                    || !LocalBatchMoveDetail::queryBatchFdMetadata(dstParentFd, &dpStx, &dpSt, &dpMagic, &metaErr)
                    || !LocalBtrfsMoveCommon::validateQualifiedMountAndFs(
                        spStx, spMagic, profile, &parentRevalFail, &parentRevalReason)
                    || !LocalBtrfsMoveCommon::validateQualifiedMountAndFs(
                        dpStx, dpMagic, profile, &parentRevalFail, &parentRevalReason)) {
                    abortedByError = true;
                    itemRes.state = BatchMoveItemState::FailedBeforePublish;
                    itemRes.error = LocalBatchMoveDetail::mapContainmentFailureToBatchError(parentRevalFail);
                    if (itemRes.error == BatchMoveError::ContainmentViolation) {
                        itemRes.error = BatchMoveError::MountOrSubvolumeBoundary;
                    }
                    itemRes.systemError = metaErr ? metaErr : EXDEV;
                    abortError = itemRes.error;
                    abortSysErr = itemRes.systemError;
                    abortReason = parentRevalReason.isEmpty()
                        ? LocalBatchMoveDetail::trMsg(
                              "Katalog nadrzędny stracił kwalifikację systemu plików przed przeniesieniem „%1” — zatrzymano partię.",
                              "Parent directory lost filesystem qualification before moving \"%1\" — batch stopped.")
                              .arg(itemPlan.srcLeaf)
                        : parentRevalReason;
                    itemRes.errorText = abortReason;
                    for (int j = i + 1; j < totalCount; ++j) {
                        res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    }
                    break;
                }
                const LocalDirectoryIdentity reSpId = LocalDirectoryIdentity::fromStat(spStx, spSt);
                const LocalDirectoryIdentity reDpId = LocalDirectoryIdentity::fromStat(dpStx, dpSt);
                if (!reSpId.sameObject(itemPlan.srcParentIdentity)
                    || !reDpId.sameObject(itemPlan.dstParentIdentity)
                    || !reSpId.sameMountScope(pf.anchorScope)
                    || !reDpId.sameMountScope(pf.anchorScope)) {
                    abortedByError = true;
                    itemRes.state = BatchMoveItemState::FailedBeforePublish;
                    itemRes.error = BatchMoveError::MountOrSubvolumeBoundary;
                    itemRes.systemError = EXDEV;
                    abortError = itemRes.error;
                    abortSysErr = EXDEV;
                    abortReason = LocalBatchMoveDetail::trMsg(
                        "Zmienił się zakres montowania lub podwoluminu dla „%1” — zatrzymano partię.",
                        "Mount or subvolume scope changed for \"%1\" — batch stopped.")
                        .arg(itemPlan.srcLeaf);
                    itemRes.errorText = abortReason;
                    for (int j = i + 1; j < totalCount; ++j) {
                        res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    }
                    break;
                }
                if (profile.checkExt4DirectoryFlags()) {
                    LocalBtrfsMoveCommon::ContainmentFailureKind parentFlagFail =
                        LocalBtrfsMoveCommon::ContainmentFailureKind::None;
                    QString parentFlagReason;
                    if (!LocalBtrfsMoveCommon::verifyExt4DirectorySafetyFlags(
                            srcParentFd, reSpId.ino, &parentFlagFail, &parentFlagReason, &hooks)
                        || !LocalBtrfsMoveCommon::verifyExt4DirectorySafetyFlags(
                            dstParentFd, reDpId.ino, &parentFlagFail, &parentFlagReason, &hooks)) {
                        abortedByError = true;
                        itemRes.state = BatchMoveItemState::FailedBeforePublish;
                        itemRes.error = LocalBatchMoveDetail::mapContainmentFailureToBatchError(parentFlagFail);
                        itemRes.systemError = EOPNOTSUPP;
                        abortError = itemRes.error;
                        abortSysErr = EOPNOTSUPP;
                        abortReason = parentFlagReason;
                        itemRes.errorText = abortReason;
                        for (int j = i + 1; j < totalCount; ++j) {
                            res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                        }
                        break;
                    }
                }

                int reopenErr = 0;
                LocalBtrfsMoveCommon::ScopedFd curDirFd(
                    LocalBatchMoveDetail::openBatchChildDirNoCrossing(srcParentFd, srcLeafBytes.constData(), &reopenErr));
                struct statx curDirStx {};
                struct stat curDirSt {};
                unsigned long curDirMagic = 0;
                LocalBtrfsMoveCommon::ContainmentFailureKind curDirFail =
                    LocalBtrfsMoveCommon::ContainmentFailureKind::None;
                QString curDirReason;
                if (!curDirFd.isValid()
                    || !LocalBatchMoveDetail::queryBatchFdMetadata(curDirFd.fd, &curDirStx, &curDirSt, &curDirMagic, &reopenErr)
                    || !LocalBtrfsMoveCommon::validateQualifiedMountAndFs(
                        curDirStx, curDirMagic, profile, &curDirFail, &curDirReason)) {
                    abortedByError = true;
                    itemRes.state = BatchMoveItemState::FailedBeforePublish;
                    if (curDirFail != LocalBtrfsMoveCommon::ContainmentFailureKind::None) {
                        itemRes.error = LocalBatchMoveDetail::mapContainmentFailureToBatchError(curDirFail);
                    } else {
                        itemRes.error = (reopenErr == EXDEV)
                            ? BatchMoveError::MountOrSubvolumeBoundary
                            : BatchMoveError::ContainmentViolation;
                    }
                    itemRes.systemError = reopenErr ? reopenErr : ENOENT;
                    abortError = itemRes.error;
                    abortSysErr = itemRes.systemError;
                    abortReason = curDirReason.isEmpty()
                        ? LocalBatchMoveDetail::trMsg(
                              "Katalog źródłowy „%1” zniknął lub zmienił się przed przeniesieniem — przerwano bez zmian pozostałych elementów.",
                              "Source directory \"%1\" disappeared or changed before move — stopped remaining items.")
                              .arg(itemPlan.srcLeaf)
                        : curDirReason;
                    itemRes.errorText = abortReason;
                    for (int j = i + 1; j < totalCount; ++j) {
                        res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    }
                    break;
                }

                currentDirId = LocalDirectoryIdentity::fromStat(curDirStx, curDirSt);
                if (!currentDirId.valid
                    || !currentDirId.sameObject(itemPlan.dirIdentity)
                    || !currentDirId.sameMountScope(pf.anchorScope)
                    || (profile.checkBtrfsSubvolRootInode() && currentDirId.ino == BTRFS_FIRST_FREE_OBJECTID)
                    || LocalBtrfsMoveCommon::isMountPointInMountInfo(itemPlan.cleanSrcPath)) {
                    abortedByError = true;
                    itemRes.state = BatchMoveItemState::FailedBeforePublish;
                    itemRes.error = BatchMoveError::ContainmentViolation;
                    itemRes.systemError = EBADF;
                    abortError = itemRes.error;
                    abortSysErr = EBADF;
                    abortReason = LocalBatchMoveDetail::trMsg(
                        "Katalog źródłowy „%1” został podmieniony przed przeniesieniem — zatrzymano partię.",
                        "Source directory \"%1\" was replaced before move — batch stopped.")
                        .arg(itemPlan.srcLeaf);
                    itemRes.errorText = abortReason;
                    for (int j = i + 1; j < totalCount; ++j) {
                        res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    }
                    break;
                }
                if (profile.checkExt4DirectoryFlags()) {
                    LocalBtrfsMoveCommon::ContainmentFailureKind curFlagFail =
                        LocalBtrfsMoveCommon::ContainmentFailureKind::None;
                    QString curFlagReason;
                    if (!LocalBtrfsMoveCommon::verifyExt4DirectorySafetyFlags(
                            curDirFd.fd, currentDirId.ino, &curFlagFail, &curFlagReason, &hooks)) {
                        abortedByError = true;
                        itemRes.state = BatchMoveItemState::FailedBeforePublish;
                        itemRes.error = LocalBatchMoveDetail::mapContainmentFailureToBatchError(curFlagFail);
                        itemRes.systemError = EOPNOTSUPP;
                        abortError = itemRes.error;
                        abortSysErr = EOPNOTSUPP;
                        abortReason = curFlagReason;
                        itemRes.errorText = abortReason;
                        for (int j = i + 1; j < totalCount; ++j) {
                            res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                        }
                        break;
                    }
                }

                if (LocalBtrfsMoveCommon::dstParentHasAncestor(dstParentFd, reDpId, currentDirId, &hooks)) {
                    abortedByError = true;
                    itemRes.state = BatchMoveItemState::FailedBeforePublish;
                    itemRes.error = BatchMoveError::CycleDetected;
                    itemRes.systemError = EINVAL;
                    abortError = itemRes.error;
                    abortSysErr = EINVAL;
                    abortReason = LocalBatchMoveDetail::trMsg(
                        "Wykryto niedozwolony cykl katalogów przed przeniesieniem „%1” — zatrzymano partię.",
                        "Directory cycle detected before moving \"%1\" — batch stopped.")
                        .arg(itemPlan.srcLeaf);
                    itemRes.errorText = abortReason;
                    for (int j = i + 1; j < totalCount; ++j) {
                        res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    }
                    break;
                }

                LocalBtrfsMoveCommon::ContainmentFailureKind reinspectFail =
                    LocalBtrfsMoveCommon::ContainmentFailureKind::None;
                QString reinspectReason;
                const bool wantStrictDigest = captureStrictDirectorySnapshot || strictHistoryCheck;
                if (!LocalBtrfsMoveCommon::inspectDirectoryTreeContainment(
                        curDirFd.fd, currentDirId, m_cancelled, &currentDirSnap, nullptr,
                        &reinspectFail, &reinspectReason, &hooks, profile,
                        wantStrictDigest, wantStrictDigest)) {
                    if (reinspectFail == LocalBtrfsMoveCommon::ContainmentFailureKind::Cancelled) {
                        cancelledDuringExecution = true;
                        for (int j = i; j < totalCount; ++j) {
                            res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                            res.batch.items[j].error = BatchMoveError::Cancelled;
                        }
                        break;
                    }
                    abortedByError = true;
                    itemRes.state = BatchMoveItemState::FailedBeforePublish;
                    itemRes.error = LocalBatchMoveDetail::mapContainmentFailureToBatchError(reinspectFail);
                    itemRes.systemError = EINVAL;
                    abortError = itemRes.error;
                    abortSysErr = EINVAL;
                    abortReason = reinspectReason;
                    itemRes.errorText = abortReason;
                    for (int j = i + 1; j < totalCount; ++j) {
                        res.batch.items[j].state = BatchMoveItemState::SkippedAfterAbort;
                    }
                    break;
                }
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
            bool identityMatch = false;
            if (!verifyFail) {
#ifdef THISPC_TEST_HARNESS
                if (simulateBatchMoveExt4Filesystem()) {
                    postDstStx.stx_mask &= ~STATX_SUBVOL;
                    postDstStx.stx_subvol = 0;
                    if (simulateBatchMoveExt4StatxMntIdUniqueMissing()) {
                        postDstStx.stx_mask &= ~STATX_MNT_ID_UNIQUE;
                    }
                }
#endif
                if (itemPlan.kind == BatchMoveItemKind::RegularFile) {
                    const LocalRegularFileIdentity postDstId = LocalRegularFileIdentity::fromStat(postDstStx, postDstSt);
                    identityMatch = postDstId.sameObject(currentSrcId);
                    if (identityMatch) {
                        itemRes.movedIdentity = postDstId;
                    }
                } else {
                    const LocalDirectoryIdentity postDirId = LocalDirectoryIdentity::fromStat(postDstStx, postDstSt);
                    identityMatch = postDirId.sameObject(currentDirId);
                    if (identityMatch) {
                        itemRes.movedDirIdentity = postDirId;
                        itemRes.movedDirSnapshot = currentDirSnap;
                    }
                }
            }
            if (verifyFail || !identityMatch) {
                res.batch.partialMoveOutcome = true;
                itemRes.state = BatchMoveItemState::FailedDurability;
                itemRes.error = BatchMoveError::PostRenameVerification;
                itemRes.systemError = verifyFail ? verifyErr : EIO;
                abortError = BatchMoveError::PostRenameVerification;
                abortSysErr = itemRes.systemError;
                abortReason = LocalBatchMoveDetail::trMsg(
                    "Część partii (%1 z %2) została opublikowana w celu, lecz weryfikacja po przeniesieniu elementu „%3” nie powiodła się (%4). Już przeniesione elementy zachowano bez automatycznego cofania i bez ponawiania.",
                    "Part of the batch (%1 of %2) was published to destination, but post-rename verification of \"%3\" failed (%4). Published items were preserved without automatic rollback or retry.")
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
                if (res.batch.failedItemIndex < 0) {
                    res.batch.failedItemIndex = it.index;
                }
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
            res.batch.verifiedFsProvenance = pf.verifiedFsProvenance;
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
        setProperty("thispcBatchFailedItemIndex", res.batch.failedItemIndex);
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
    QList<BatchMoveExpectedItem> m_expectedItems;
    bool m_strictHistoryCheck = false;
    bool m_captureStrictDirectorySnapshot = false;
    bool m_allowDirectories = false;
    bool m_allowExt4 = false;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_workerResultReady{false};
    WorkerResult m_workerResult;
    BatchMoveResult m_result;
    bool m_started = false;
    bool m_finished = false;
    bool m_historyAttachmentClaimed = false;
};
