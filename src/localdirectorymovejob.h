/*
 * Safe atomic same-volume Btrfs single-directory move (H.2A).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localbtrfsmovecommon.h"
#include "localfileoverwrite.h"

#include <KIO/Job>
#include <KJob>

#include <QAtomicInteger>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QLocale>
#include <QMetaObject>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <linux/stat.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>
#include <vector>

enum class LocalDirectoryMoveError {
    None,
    Cancelled,
    InvalidInput,
    UnsupportedFilesystem,
    UnsupportedKernelFeature,
    MountOrSubvolumeBoundary,
    ContainmentViolation,
    CycleDetected,
    DestinationExists,
    AtomicMove,
    PostRenameVerification,
    Durability,
};

enum class LocalDirectoryMoveOutcome {
    NotPublished,
    PublishedUnconfirmedDurability,
    PublishedAndDurable,
};

Q_DECLARE_METATYPE(LocalDirectoryMoveError)
Q_DECLARE_METATYPE(LocalDirectoryMoveOutcome)

struct LocalBtrfsDirectoryQualification {
    bool ok = false;
    LocalDirectoryMoveError error = LocalDirectoryMoveError::InvalidInput;
    int systemError = 0;
    QString reason;
    LocalDirectoryIdentity sourceIdentity;
    LocalDirectoryIdentity srcParentIdentity;
    LocalDirectoryIdentity dstParentIdentity;
    bool sameParent = false;
    quint64 scannedBytes = 0;
    qsizetype scannedRegularFiles = 0;
    qsizetype scannedSubdirectories = 0;
};

#ifdef THISPC_TEST_HARNESS
inline unsigned long &simulateDirMoveUnsupportedFsMagic()
{
    static unsigned long magic = 0;
    return magic;
}

inline bool &simulateDirMoveRenameNoReplaceUnsupported()
{
    static bool unsupported = false;
    return unsupported;
}

inline bool &simulateDirMoveExdev()
{
    static bool exdev = false;
    return exdev;
}

inline bool &simulateDirMoveOpenat2Unsupported()
{
    static bool unsupported = false;
    return unsupported;
}

inline bool &simulateDirMoveStatxMntIdUnsupported()
{
    static bool unsupported = false;
    return unsupported;
}

inline bool &simulateDirMoveBindMountMismatch()
{
    static bool mismatch = false;
    return mismatch;
}

inline bool &simulateDirMoveSubvolumeBoundary()
{
    static bool subvol = false;
    return subvol;
}

inline int &simulateDirMoveDestinationFsyncError()
{
    static int err = 0;
    return err;
}

inline int &simulateDirMoveSourceFsyncError()
{
    static int err = 0;
    return err;
}

inline std::function<void()> &simulateDirMoveBeforeRename()
{
    static std::function<void()> hook;
    return hook;
}

inline std::function<void()> &simulateDirMoveBeforeDstFsync()
{
    static std::function<void()> hook;
    return hook;
}
#endif

namespace LocalDirectoryMoveDetail {

using LocalBtrfsMoveCommon::ScopedFd;
using LocalBtrfsMoveCommon::trMsg;
using LocalBtrfsMoveCommon::splitCleanLocalPath;
using LocalBtrfsMoveCommon::isMountPointInMountInfo;

inline LocalBtrfsMoveCommon::TestFaultHooks currentDirMoveFaultHooks()
{
    LocalBtrfsMoveCommon::TestFaultHooks hooks;
#ifdef THISPC_TEST_HARNESS
    hooks.openat2Unsupported = simulateDirMoveOpenat2Unsupported();
    hooks.unsupportedFsMagic = simulateDirMoveUnsupportedFsMagic();
    hooks.statxMntIdUnsupported = simulateDirMoveStatxMntIdUnsupported();
#endif
    return hooks;
}

inline int openDirectoryNoSymlinks(const QString &absDirPath, int *errOut)
{
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentDirMoveFaultHooks();
    return LocalBtrfsMoveCommon::openDirectoryNoSymlinks(absDirPath, errOut, &hooks);
}

inline int openChildDirectoryNoCrossing(int parentFd, const char *leafName, int *errOut)
{
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentDirMoveFaultHooks();
    return LocalBtrfsMoveCommon::openChildDirectoryNoCrossing(parentFd, leafName, errOut, &hooks);
}

inline bool queryFdMetadata(
    int fd,
    struct statx *stxOut,
    struct stat *stOut,
    unsigned long *fsMagicOut,
    int *errOut)
{
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentDirMoveFaultHooks();
    return LocalBtrfsMoveCommon::queryFdMetadata(fd, stxOut, stOut, fsMagicOut, errOut, &hooks);
}

inline bool dstParentHasAncestor(
    int dstParentFd,
    const LocalDirectoryIdentity &dstParentId,
    const LocalDirectoryIdentity &forbiddenAncestor)
{
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentDirMoveFaultHooks();
    return LocalBtrfsMoveCommon::dstParentHasAncestor(dstParentFd, dstParentId, forbiddenAncestor, &hooks);
}

inline QString unsupportedFsMessage()
{
    return trMsg(
        "Przenoszenie katalogów jest obecnie dozwolone wyłącznie w obrębie tego samego woluminu Btrfs. Możesz skopiować katalog zamiast go przenosić.",
        "Directory moves are currently permitted only within the same Btrfs volume. You can copy the directory instead of moving it.");
}

struct OpenedMoveContext {
    ScopedFd srcParentFd;
    ScopedFd srcDirFd;
    ScopedFd dstParentFd;
    QString srcParentPath;
    QString srcLeaf;
    QString dstParentPath;
    QString dstLeaf;
    QString cleanSrc;
    QString cleanDst;
};

inline bool qualifyAndOpenSameVolumeBtrfs(
    const QString &sourcePath,
    const QString &destinationPath,
    const LocalDirectoryIdentity *expectedSourceIdentity,
    bool checkDestinationAbsence,
    LocalBtrfsDirectoryQualification *outQual,
    OpenedMoveContext *outCtx)
{
    LocalBtrfsDirectoryQualification localQual;
    auto fail = [&](LocalDirectoryMoveError err, int sysErr, const QString &reason) {
        localQual.ok = false;
        localQual.error = err;
        localQual.systemError = sysErr;
        localQual.reason = reason;
        if (outQual) *outQual = localQual;
        return false;
    };

    QString srcParentPath, srcLeaf, dstParentPath, dstLeaf;
    if (!splitCleanLocalPath(sourcePath, &srcParentPath, &srcLeaf)
        || !splitCleanLocalPath(destinationPath, &dstParentPath, &dstLeaf)) {
        return fail(LocalDirectoryMoveError::InvalidInput, EINVAL,
                    trMsg("Nieprawidłowa ścieżka katalogu źródłowego lub docelowego.",
                          "Invalid source or destination directory path."));
    }

    const QString cleanSrc = QDir::cleanPath(sourcePath);
    const QString cleanDst = QDir::cleanPath(destinationPath);
    if (cleanSrc == cleanDst) {
        return fail(LocalDirectoryMoveError::CycleDetected, EINVAL,
                    trMsg("Nie można przenieść folderu do niego samego lub do jego podfolderu.",
                          "A folder cannot be moved into itself or one of its subfolders."));
    }

    int err = 0;
    ScopedFd srcParentFd(openDirectoryNoSymlinks(srcParentPath, &err));
    if (!srcParentFd.isValid()) {
        if (err == ENOSYS) {
            return fail(LocalDirectoryMoveError::UnsupportedKernelFeature, err,
                        trMsg("Jądro nie udostępnia wymaganego bezpiecznego mechanizmu openat2.",
                              "The kernel does not support required safe openat2 lookup."));
        }
        return fail(err == ELOOP ? LocalDirectoryMoveError::ContainmentViolation : LocalDirectoryMoveError::InvalidInput,
                    err,
                    trMsg("Nie można bezpiecznie otworzyć katalogu nadrzędnego źródła (możliwe dowiązanie symboliczne lub brak dostępu).",
                          "Could not safely open source parent directory (possible symlink or inaccessible path)."));
    }

    ScopedFd dstParentFd(openDirectoryNoSymlinks(dstParentPath, &err));
    if (!dstParentFd.isValid()) {
        if (err == ENOSYS) {
            return fail(LocalDirectoryMoveError::UnsupportedKernelFeature, err,
                        trMsg("Jądro nie udostępnia wymaganego bezpiecznego mechanizmu openat2.",
                              "The kernel does not support required safe openat2 lookup."));
        }
        return fail(err == ELOOP ? LocalDirectoryMoveError::ContainmentViolation : LocalDirectoryMoveError::InvalidInput,
                    err,
                    trMsg("Nie można bezpiecznie otworzyć katalogu docelowego (możliwe dowiązanie symboliczne lub brak miejsca docelowego).",
                          "Could not safely open destination parent directory (possible symlink or missing destination)."));
    }

    const QByteArray srcLeafBytes = QFile::encodeName(srcLeaf);
    ScopedFd srcDirFd(openChildDirectoryNoCrossing(srcParentFd.fd, srcLeafBytes.constData(), &err));
    if (!srcDirFd.isValid()) {
        if (err == ENOSYS) {
            return fail(LocalDirectoryMoveError::UnsupportedKernelFeature, err,
                        trMsg("Jądro nie udostępnia wymaganego bezpiecznego mechanizmu openat2.",
                              "The kernel does not support required safe openat2 lookup."));
        }
        if (err == EXDEV) {
            return fail(LocalDirectoryMoveError::MountOrSubvolumeBoundary, EXDEV,
                        trMsg("Przenoszenie punktu montowania lub granicy podwoluminu jest zablokowane ze względów bezpieczeństwa.",
                              "Moving a mount point or subvolume boundary is blocked for safety."));
        }
        return fail(LocalDirectoryMoveError::ContainmentViolation, err,
                    moveContainmentErrorMessage());
    }

    struct statx spStx {}, srcStx {}, dpStx {};
    struct stat spSt {}, srcSt {}, dpSt {};
    unsigned long spMagic = 0, srcMagic = 0, dpMagic = 0;
    if (!queryFdMetadata(srcParentFd.fd, &spStx, &spSt, &spMagic, &err)
        || !queryFdMetadata(srcDirFd.fd, &srcStx, &srcSt, &srcMagic, &err)
        || !queryFdMetadata(dstParentFd.fd, &dpStx, &dpSt, &dpMagic, &err)) {
        return fail(LocalDirectoryMoveError::InvalidInput, err,
                    trMsg("Nie udało się odczytać metadanych katalogu przez deskryptor.",
                          "Could not inspect directory descriptor metadata."));
    }

    if (spMagic != BTRFS_SUPER_MAGIC || srcMagic != BTRFS_SUPER_MAGIC || dpMagic != BTRFS_SUPER_MAGIC) {
        return fail(LocalDirectoryMoveError::UnsupportedFilesystem, EXDEV, unsupportedFsMessage());
    }

    if (!(spStx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE))
        || !(srcStx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE))
        || !(dpStx.stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE))) {
        return fail(LocalDirectoryMoveError::UnsupportedKernelFeature, EOPNOTSUPP,
                    trMsg("Jądro nie zwróciło identyfikatora montowania statx(STATX_MNT_ID).",
                          "Kernel did not provide statx mount identity (STATX_MNT_ID)."));
    }

#ifdef THISPC_TEST_HARNESS
    if (simulateDirMoveBindMountMismatch()) {
        dpStx.stx_mnt_id += 1;
    }
    if (simulateDirMoveSubvolumeBoundary()) {
        srcStx.stx_ino = BTRFS_FIRST_FREE_OBJECTID;
    }
#endif

    localQual.srcParentIdentity = LocalDirectoryIdentity::fromStat(spStx, spSt);
    localQual.sourceIdentity = LocalDirectoryIdentity::fromStat(srcStx, srcSt);
    localQual.dstParentIdentity = LocalDirectoryIdentity::fromStat(dpStx, dpSt);

    if (!localQual.srcParentIdentity.valid
        || !localQual.sourceIdentity.valid
        || !localQual.dstParentIdentity.valid) {
        return fail(LocalDirectoryMoveError::ContainmentViolation, EINVAL,
                    moveContainmentErrorMessage());
    }

    if (localQual.sourceIdentity.ino == BTRFS_FIRST_FREE_OBJECTID) {
        return fail(LocalDirectoryMoveError::MountOrSubvolumeBoundary, EXDEV,
                    trMsg("Przenoszenie katalogu głównego podwoluminu lub woluminu Btrfs jest zablokowane.",
                          "Moving a Btrfs volume or subvolume root directory is blocked."));
    }

    if (!localQual.sourceIdentity.sameMountScope(localQual.srcParentIdentity)
        || !localQual.srcParentIdentity.sameMountScope(localQual.dstParentIdentity)) {
        return fail(LocalDirectoryMoveError::MountOrSubvolumeBoundary, EXDEV,
                    trMsg("Źródło i cel znajdują się na różnych montowaniach, bind-mountach lub podwoluminach Btrfs.",
                          "Source and destination span different mounts, bind mounts, or Btrfs subvolumes."));
    }

    if (isMountPointInMountInfo(cleanSrc)) {
        return fail(LocalDirectoryMoveError::MountOrSubvolumeBoundary, EXDEV,
                    trMsg("Przenoszenie punktu montowania jest zablokowane ze względów bezpieczeństwa.",
                          "Moving a mount point is blocked for safety."));
    }

    if (expectedSourceIdentity && expectedSourceIdentity->valid
        && !localQual.sourceIdentity.sameObject(*expectedSourceIdentity)) {
        return fail(LocalDirectoryMoveError::ContainmentViolation, EBADF,
                    trMsg("Tożsamość katalogu uległa zmianie od momentu operacji — operacja została zablokowana.",
                          "Directory identity changed since the original operation — blocked for safety."));
    }

    localQual.sameParent = localQual.srcParentIdentity.sameObject(localQual.dstParentIdentity);
    if (localQual.sameParent && srcLeaf == dstLeaf) {
        return fail(LocalDirectoryMoveError::CycleDetected, EINVAL,
                    trMsg("Źródło i cel wskazują na ten sam obiekt.",
                          "Source and destination refer to the same directory."));
    }

    if (dstParentHasAncestor(dstParentFd.fd, localQual.dstParentIdentity, localQual.sourceIdentity)) {
        return fail(LocalDirectoryMoveError::CycleDetected, EINVAL,
                    trMsg("Nie można przenieść folderu do niego samego lub do jednego z jego podfolderów.",
                          "A folder cannot be moved into itself or one of its subfolders."));
    }

    if (checkDestinationAbsence) {
        const QByteArray dstLeafBytes = QFile::encodeName(dstLeaf);
        struct stat dstExistSt {};
        if (::fstatat(dstParentFd.fd, dstLeafBytes.constData(), &dstExistSt, AT_SYMLINK_NOFOLLOW) == 0) {
            return fail(LocalDirectoryMoveError::DestinationExists, EEXIST,
                        trMsg("Element docelowy już istnieje. Przenoszenie katalogów z nadpisywaniem jest niedostępne.",
                              "Destination already exists. Directory move with overwrite is unavailable."));
        }
        const int statErr = errno;
        if (statErr != ENOENT) {
            return fail(LocalDirectoryMoveError::InvalidInput, statErr,
                        trMsg("Nie można zweryfikować dostępności ścieżki docelowej.",
                              "Could not verify destination availability."));
        }
    }

    localQual.ok = true;
    localQual.error = LocalDirectoryMoveError::None;
    localQual.systemError = 0;
    if (outQual) *outQual = localQual;
    if (outCtx) {
        outCtx->srcParentFd = std::move(srcParentFd);
        outCtx->srcDirFd = std::move(srcDirFd);
        outCtx->dstParentFd = std::move(dstParentFd);
        outCtx->srcParentPath = srcParentPath;
        outCtx->srcLeaf = srcLeaf;
        outCtx->dstParentPath = dstParentPath;
        outCtx->dstLeaf = dstLeaf;
        outCtx->cleanSrc = cleanSrc;
        outCtx->cleanDst = cleanDst;
    }
    return true;
}

inline bool qualifyFastForPolicy(
    const QString &sourceDirPath,
    const QString &destParentDirPath,
    LocalBtrfsDirectoryQualification *outQual = nullptr)
{
    QString cleanSrcParent, srcLeaf;
    if (!splitCleanLocalPath(sourceDirPath, &cleanSrcParent, &srcLeaf)) {
        if (outQual) {
            outQual->ok = false;
            outQual->error = LocalDirectoryMoveError::InvalidInput;
            outQual->reason = moveContainmentErrorMessage();
        }
        return false;
    }
    const QString cleanDestDir = QDir::cleanPath(destParentDirPath);
    const QString hypotheticalTarget = (cleanDestDir == QLatin1String("/"))
        ? QString(QLatin1Char('/') + srcLeaf)
        : QString(cleanDestDir + QLatin1Char('/') + srcLeaf);
    return qualifyAndOpenSameVolumeBtrfs(
        sourceDirPath, hypotheticalTarget, nullptr, false, outQual, nullptr);
}

inline bool inspectDirectoryTreeContainment(
    int rootDirFd,
    const LocalDirectoryIdentity &rootIdentity,
    const std::atomic<bool> &cancelled,
    LocalBtrfsDirectoryQualification *qual,
    LocalDirectoryMoveError *errOut,
    QString *reasonOut)
{
    LocalDirectoryTreeSnapshot snapshot;
    LocalBtrfsMoveCommon::ContainmentFailureKind failureKind = LocalBtrfsMoveCommon::ContainmentFailureKind::None;
    const LocalBtrfsMoveCommon::TestFaultHooks hooks = currentDirMoveFaultHooks();
    if (!LocalBtrfsMoveCommon::inspectDirectoryTreeContainment(
            rootDirFd, rootIdentity, cancelled, &snapshot, nullptr, &failureKind, reasonOut, &hooks)) {
        if (errOut) {
            switch (failureKind) {
            case LocalBtrfsMoveCommon::ContainmentFailureKind::Cancelled:
                *errOut = LocalDirectoryMoveError::Cancelled;
                break;
            case LocalBtrfsMoveCommon::ContainmentFailureKind::InvalidInput:
                *errOut = LocalDirectoryMoveError::InvalidInput;
                break;
            case LocalBtrfsMoveCommon::ContainmentFailureKind::UnsupportedKernelFeature:
                *errOut = LocalDirectoryMoveError::UnsupportedKernelFeature;
                break;
            case LocalBtrfsMoveCommon::ContainmentFailureKind::UnsupportedFilesystem:
                *errOut = LocalDirectoryMoveError::UnsupportedFilesystem;
                break;
            case LocalBtrfsMoveCommon::ContainmentFailureKind::MountOrSubvolumeBoundary:
                *errOut = LocalDirectoryMoveError::MountOrSubvolumeBoundary;
                break;
            case LocalBtrfsMoveCommon::ContainmentFailureKind::ContainmentViolation:
            case LocalBtrfsMoveCommon::ContainmentFailureKind::None:
                *errOut = LocalDirectoryMoveError::ContainmentViolation;
                break;
            }
        }
        return false;
    }

    if (qual) {
        qual->scannedBytes = snapshot.scannedBytes;
        qual->scannedRegularFiles = snapshot.scannedRegularFiles;
        qual->scannedSubdirectories = snapshot.scannedSubdirectories;
    }
    if (errOut) *errOut = LocalDirectoryMoveError::None;
    return true;
}

} // namespace LocalDirectoryMoveDetail

class LocalDirectoryMoveJob final : public KIO::Job
{
    Q_OBJECT

public:
    LocalDirectoryMoveJob(
        const QUrl &source,
        const QUrl &destination,
        QObject *parent = nullptr,
        LocalDirectoryIdentity expectedSourceIdentity = {})
        : KIO::Job()
        , m_source(source)
        , m_destination(destination)
        , m_expectedSourceIdentity(expectedSourceIdentity)
    {
        setParent(parent);
        setProperty("thispcSourceUrls", QVariant::fromValue(QList<QUrl>{source}));
        setProperty("thispcDestinationUrl", destination);
        setProperty("thispcOverwritesDestination", false);
        setProperty("thispcIsDirectoryMove", true);
        qRegisterMetaType<LocalDirectoryMoveError>();
        qRegisterMetaType<LocalDirectoryMoveOutcome>();
        setCapabilities(KJob::Killable);
    }

    ~LocalDirectoryMoveJob() override
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
        if (!m_source.isLocalFile() || !m_destination.isLocalFile()) {
            QTimer::singleShot(0, this, [this] {
                finish(LocalDirectoryMoveError::InvalidInput,
                       LocalDirectoryMoveDetail::trMsg(
                           "Przenoszenie katalogu wymaga lokalnych adresów źródła i celu.",
                           "Directory move requires local source and destination URLs."));
            });
            return;
        }

        const QString srcPath = m_source.toLocalFile();
        const QString dstPath = m_destination.toLocalFile();
        const LocalDirectoryIdentity expectedId = m_expectedSourceIdentity;

        m_thread = QThread::create([this, srcPath, dstPath, expectedId] {
            executeWorker(srcPath, dstPath, expectedId);
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

    QUrl sourceUrl() const { return m_source; }
    QUrl destinationUrl() const { return m_destination; }
    LocalDirectoryMoveError moveError() const { return m_moveError; }
    LocalDirectoryMoveOutcome outcome() const { return m_outcome; }
    int systemError() const { return m_systemError; }
    bool destinationPublished() const { return m_destinationPublished; }
    bool directoryDurabilityConfirmed() const { return m_directoryDurabilityConfirmed; }
    LocalDirectoryIdentity movedIdentity() const { return m_movedIdentity; }
    bool isCancelRequested() const { return m_cancelled.load(std::memory_order_acquire); }

protected:
    bool doKill() override
    {
        if (m_finished || m_destinationPublishedAtomic.load(std::memory_order_acquire)) {
            return false;
        }
        m_cancelled.store(true, std::memory_order_release);
        if (m_thread && QThread::currentThread() != m_thread) {
            m_thread->wait();
        }
        if (m_destinationPublishedAtomic.load(std::memory_order_acquire)) {
            return false;
        }
        m_finished = true;
        m_moveError = LocalDirectoryMoveError::Cancelled;
        setProperty("thispcMoveError", QVariant::fromValue(m_moveError));
        return true;
    }

private:
    struct WorkerResult {
        LocalDirectoryMoveError error = LocalDirectoryMoveError::InvalidInput;
        LocalDirectoryMoveOutcome outcome = LocalDirectoryMoveOutcome::NotPublished;
        int sysErrno = 0;
        bool destinationPublished = false;
        bool durabilityConfirmed = false;
        LocalDirectoryIdentity movedIdentity;
        quint64 scannedBytes = 0;
        QString message;
    };

    void executeWorker(
        const QString &srcPath,
        const QString &dstPath,
        const LocalDirectoryIdentity &expectedIdentity)
    {
        WorkerResult res;
        auto finishWorker = [&] {
            QMetaObject::invokeMethod(this, [this, res] {
                applyWorkerResult(res);
            }, Qt::QueuedConnection);
        };

        if (m_cancelled.load(std::memory_order_acquire)) {
            res.error = LocalDirectoryMoveError::Cancelled;
            res.message = LocalDirectoryMoveDetail::trMsg("Operacja anulowana.", "Operation cancelled.");
            finishWorker();
            return;
        }

        LocalBtrfsDirectoryQualification qual;
        LocalDirectoryMoveDetail::OpenedMoveContext ctx;
        if (!LocalDirectoryMoveDetail::qualifyAndOpenSameVolumeBtrfs(
                srcPath,
                dstPath,
                expectedIdentity.valid ? &expectedIdentity : nullptr,
                true,
                &qual,
                &ctx)) {
            res.error = qual.error;
            res.sysErrno = qual.systemError;
            res.message = qual.reason;
            finishWorker();
            return;
        }

        LocalDirectoryMoveError inspectErr = LocalDirectoryMoveError::None;
        QString inspectMsg;
        if (!LocalDirectoryMoveDetail::inspectDirectoryTreeContainment(
                ctx.srcDirFd.fd, qual.sourceIdentity, m_cancelled, &qual, &inspectErr, &inspectMsg)) {
            res.error = inspectErr;
            res.message = inspectMsg;
            finishWorker();
            return;
        }
        res.scannedBytes = qual.scannedBytes;

        if (m_cancelled.load(std::memory_order_acquire)) {
            res.error = LocalDirectoryMoveError::Cancelled;
            res.message = LocalDirectoryMoveDetail::trMsg("Operacja anulowana.", "Operation cancelled.");
            finishWorker();
            return;
        }

#ifdef THISPC_TEST_HARNESS
        if (simulateDirMoveBeforeRename()) {
            simulateDirMoveBeforeRename()();
        }
#endif

        if (m_cancelled.load(std::memory_order_acquire)) {
            res.error = LocalDirectoryMoveError::Cancelled;
            res.message = LocalDirectoryMoveDetail::trMsg("Operacja anulowana.", "Operation cancelled.");
            finishWorker();
            return;
        }

        // Re-verify source directory entry inside srcParentFd still points to exact qualified inode
        const QByteArray srcLeafBytes = QFile::encodeName(ctx.srcLeaf);
        const QByteArray dstLeafBytes = QFile::encodeName(ctx.dstLeaf);
        struct statx recheckSrcStx {};
        struct stat recheckSrcSt {};
        const unsigned int recheckMask =
            STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE | STATX_SUBVOL;
        if (::syscall(SYS_statx, ctx.srcParentFd.fd, srcLeafBytes.constData(),
                      AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, recheckMask, &recheckSrcStx) != 0
            || ::fstatat(ctx.srcParentFd.fd, srcLeafBytes.constData(), &recheckSrcSt, AT_SYMLINK_NOFOLLOW) != 0) {
            res.error = LocalDirectoryMoveError::ContainmentViolation;
            res.sysErrno = errno ? errno : ENOENT;
            res.message = LocalDirectoryMoveDetail::trMsg(
                "Katalog źródłowy zniknął lub zmienił się przed przeniesieniem.",
                "Source directory disappeared or changed before rename.");
            finishWorker();
            return;
        }
        const LocalDirectoryIdentity currentEntryId = LocalDirectoryIdentity::fromStat(recheckSrcStx, recheckSrcSt);
        if (!currentEntryId.valid || !currentEntryId.sameObject(qual.sourceIdentity)) {
            res.error = LocalDirectoryMoveError::ContainmentViolation;
            res.sysErrno = EBADF;
            res.message = LocalDirectoryMoveDetail::trMsg(
                "Katalog źródłowy został podmieniony przed wykonaniem operacji — przerwano bez zmian.",
                "Source directory was replaced before rename — aborted fail-closed.");
            finishWorker();
            return;
        }

        // Atomic SYS_renameat2(..., RENAME_NOREPLACE)
        long renameRes = -1;
        int renameErr = 0;

#ifdef THISPC_TEST_HARNESS
        if (simulateDirMoveExdev()) {
            renameRes = -1;
            renameErr = EXDEV;
        } else if (simulateDirMoveRenameNoReplaceUnsupported()) {
            renameRes = -1;
            renameErr = EOPNOTSUPP;
        } else
#endif
        {
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
            renameRes = ::syscall(
                SYS_renameat2,
                ctx.srcParentFd.fd, srcLeafBytes.constData(),
                ctx.dstParentFd.fd, dstLeafBytes.constData(),
                RENAME_NOREPLACE);
            renameErr = errno;
#else
            renameRes = -1;
            renameErr = ENOSYS;
#endif
        }

        if (renameRes != 0) {
            res.sysErrno = renameErr;
            if (renameErr == EEXIST || renameErr == ENOTEMPTY || renameErr == EISDIR || renameErr == EBUSY) {
                res.error = LocalDirectoryMoveError::DestinationExists;
                res.message = LocalDirectoryMoveDetail::trMsg(
                    "Element docelowy już istnieje. Przenoszenie katalogów z nadpisywaniem jest niedostępne.",
                    "Destination already exists. Directory move with overwrite is unavailable.");
            } else if (renameErr == EXDEV) {
                res.error = LocalDirectoryMoveError::MountOrSubvolumeBoundary;
                res.message = LocalDirectoryMoveDetail::trMsg(
                    "Nie można przenieść atomowo przez granicę woluminu lub podwoluminu Btrfs (EXDEV).",
                    "Cannot atomically move across volume or Btrfs subvolume boundary (EXDEV).");
            } else if (renameErr == ENOSYS || renameErr == EOPNOTSUPP || renameErr == EINVAL) {
                res.error = LocalDirectoryMoveError::UnsupportedKernelFeature;
                res.message = LocalDirectoryMoveDetail::trMsg(
                    "System plików lub jądro nie obsługuje bezpiecznego atomowego renameat2(RENAME_NOREPLACE).",
                    "Filesystem or kernel does not support safe atomic renameat2(RENAME_NOREPLACE).");
            } else {
                res.error = LocalDirectoryMoveError::AtomicMove;
                res.message = LocalDirectoryMoveDetail::trMsg(
                    "Nie udało się atomowo przenieść katalogu.",
                    "Failed to move directory atomically.");
            }
            finishWorker();
            return;
        }

        // CRITICAL POINT OF NO RETURN: renameat2 succeeded.
        // Destination is published under dstName and old srcName is gone.
        // NEVER unlink destination and NEVER rollback from this point forward!
        m_destinationPublishedAtomic.store(true, std::memory_order_release);
        res.destinationPublished = true;
        res.outcome = LocalDirectoryMoveOutcome::PublishedUnconfirmedDurability;
        res.movedIdentity = qual.sourceIdentity;

        // Post-rename identity verification on destination entry and srcDirFd
        struct statx postDstStx {};
        struct stat postDstSt {};
        if (::syscall(SYS_statx, ctx.dstParentFd.fd, dstLeafBytes.constData(),
                      AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, recheckMask, &postDstStx) != 0
            || ::fstatat(ctx.dstParentFd.fd, dstLeafBytes.constData(), &postDstSt, AT_SYMLINK_NOFOLLOW) != 0) {
            res.error = LocalDirectoryMoveError::PostRenameVerification;
            res.sysErrno = errno ? errno : EIO;
            res.message = LocalDirectoryMoveDetail::trMsg(
                "Katalog został przeniesiony do lokalizacji docelowej, ale weryfikacja po przeniesieniu nie powiodła się (%1). Katalog docelowy został zachowany bez cofania.",
                "Directory was moved to destination, but post-rename verification failed (%1). Destination was preserved without rollback.")
                .arg(res.sysErrno);
            finishWorker();
            return;
        }

        const LocalDirectoryIdentity postDstId = LocalDirectoryIdentity::fromStat(postDstStx, postDstSt);
        if (!postDstId.valid || postDstId.ino != qual.sourceIdentity.ino
            || postDstId.devMajor != qual.sourceIdentity.devMajor
            || postDstId.devMinor != qual.sourceIdentity.devMinor) {
            res.error = LocalDirectoryMoveError::PostRenameVerification;
            res.sysErrno = EIO;
            res.message = LocalDirectoryMoveDetail::trMsg(
                "Wykryto niezgodność tożsamości katalogu po przeniesieniu — zachowano stan bez automatycznego cofania.",
                "Post-rename directory identity mismatch detected — preserved without automatic rollback.");
            finishWorker();
            return;
        }
        res.movedIdentity = postDstId;

#ifdef THISPC_TEST_HARNESS
        if (simulateDirMoveBeforeDstFsync()) {
            simulateDirMoveBeforeDstFsync()();
        }
#endif

        // Durability Gate: fsync destination parent (and source parent when distinct)
        int dstSyncRes = ::fsync(ctx.dstParentFd.fd);
        int dstSyncErr = (dstSyncRes == 0) ? 0 : (errno ? errno : EIO);
#ifdef THISPC_TEST_HARNESS
        if (simulateDirMoveDestinationFsyncError() != 0) {
            dstSyncRes = -1;
            dstSyncErr = simulateDirMoveDestinationFsyncError();
        }
#endif
        if (dstSyncRes != 0) {
            res.error = LocalDirectoryMoveError::Durability;
            res.sysErrno = dstSyncErr;
            res.durabilityConfirmed = false;
            res.message = LocalDirectoryMoveDetail::trMsg(
                "Katalog został przeniesiony i jest widoczny w miejscu docelowym, lecz synchronizacja katalogu docelowego (fsync) zwróciła błąd (%1). Stara nazwa nie istnieje; brak automatycznego cofania i ponawiania.",
                "Directory was moved to destination, but destination directory synchronization (fsync) failed (%1). Old name no longer exists; no automatic rollback or retry.")
                .arg(dstSyncErr);
            finishWorker();
            return;
        }

        if (!qual.sameParent) {
            int srcSyncRes = ::fsync(ctx.srcParentFd.fd);
            int srcSyncErr = (srcSyncRes == 0) ? 0 : (errno ? errno : EIO);
#ifdef THISPC_TEST_HARNESS
            if (simulateDirMoveSourceFsyncError() != 0) {
                srcSyncRes = -1;
                srcSyncErr = simulateDirMoveSourceFsyncError();
            }
#endif
            if (srcSyncRes != 0) {
                res.error = LocalDirectoryMoveError::Durability;
                res.sysErrno = srcSyncErr;
                res.durabilityConfirmed = false;
                res.message = LocalDirectoryMoveDetail::trMsg(
                    "Katalog został przeniesiony i jest widoczny w miejscu docelowym, lecz synchronizacja katalogu źródłowego (fsync) zwróciła błąd (%1). Stara nazwa nie istnieje; trwałość usunięcia starego wpisu nie jest potwierdzona.",
                    "Directory was moved to destination, but source parent synchronization (fsync) failed (%1). Old name no longer exists; durability of old entry removal is unconfirmed.")
                    .arg(srcSyncErr);
                finishWorker();
                return;
            }
        }

        res.durabilityConfirmed = true;
        res.destinationPublished = true;
        res.outcome = LocalDirectoryMoveOutcome::PublishedAndDurable;
        res.error = LocalDirectoryMoveError::None;
        res.sysErrno = 0;
        res.message.clear();
        finishWorker();
    }

    void applyWorkerResult(const WorkerResult &res)
    {
        if (m_finished) return;
        if (res.scannedBytes > 0) {
            setTotalAmount(KJob::Bytes, res.scannedBytes);
            setProcessedAmount(KJob::Bytes, res.destinationPublished ? res.scannedBytes : 0);
        }
        m_destinationPublished = res.destinationPublished;
        m_directoryDurabilityConfirmed = res.durabilityConfirmed;
        m_outcome = res.outcome;
        m_movedIdentity = res.movedIdentity;
        m_systemError = res.sysErrno;
        finish(res.error, res.message);
    }

    void finish(LocalDirectoryMoveError error, const QString &message)
    {
        if (m_finished) return;
        m_finished = true;
        m_moveError = error;
        setProperty("thispcMoveError", QVariant::fromValue(error));
        setProperty("thispcDestinationPublished", m_destinationPublished);
        setProperty("thispcDirectoryDurabilityConfirmed", m_directoryDurabilityConfirmed);
        setProperty("thispcDirectoryMoveOutcome", QVariant::fromValue(m_outcome));
        if (m_destinationPublished && (error != LocalDirectoryMoveError::None || !m_directoryDurabilityConfirmed)) {
            setProperty("thispcPartialMoveOutcome", true);
        }
        if (m_systemError != 0) {
            setProperty("thispcSystemError", m_systemError);
        }
        if (error == LocalDirectoryMoveError::Cancelled) {
            setError(KJob::KilledJobError);
        } else if (error != LocalDirectoryMoveError::None) {
            setError(KJob::UserDefinedError);
            setErrorText(message);
        }
        emitResult();
    }

    QUrl m_source;
    QUrl m_destination;
    LocalDirectoryIdentity m_expectedSourceIdentity;
    QThread *m_thread = nullptr;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_destinationPublishedAtomic{false};
    bool m_started = false;
    bool m_finished = false;
    bool m_destinationPublished = false;
    bool m_directoryDurabilityConfirmed = false;
    LocalDirectoryMoveOutcome m_outcome = LocalDirectoryMoveOutcome::NotPublished;
    LocalDirectoryIdentity m_movedIdentity;
    LocalDirectoryMoveError m_moveError = LocalDirectoryMoveError::None;
    int m_systemError = 0;
};
