/*
 * Safe local merge execution engine for FOLDER-MERGE-01 (Stage 2B.1 + 2B.2A + 2B.2B).
 * - Stage 2B.1: ADDITIVE-ONLY execution.
 * - Stage 2B.2A: Descriptor-anchored file/dir operations closing TOCTOU windows,
 *   plus safe Skip, Rename, and strictly-scoped ApplyToAll (SkipAllFileConflicts).
 * - Stage 2B.2B: Safe Overwrite via O_TMPFILE/anchored staging + atomic
 *   renameat2(RENAME_EXCHANGE) + post-exchange displaced-inode verification +
 *   durability/recovery contract (fail-closed on NTFS/VFAT/unsupported FS).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "localfilecopyjob.h"
#include "localmergeplan.h"
#include "localmergetransaction.h"

#include <KIO/Job>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <linux/fs.h>
#include <memory>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

#ifndef BTRFS_SUPER_MAGIC
#define BTRFS_SUPER_MAGIC 0x9123683e
#endif

#ifndef EXT4_SUPER_MAGIC
#define EXT4_SUPER_MAGIC 0xef53
#endif

#ifndef XFS_SB_MAGIC
#define XFS_SB_MAGIC 0x58465342
#endif

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#ifndef NTFS3_SUPER_MAGIC
#define NTFS3_SUPER_MAGIC 0x7366746e
#endif

#ifndef NTFS_SB_MAGIC
#define NTFS_SB_MAGIC 0x5346544e
#endif

enum class LocalMergeExecutionOutcome {
    NotStarted,
    Succeeded,
    SucceededWithSkippedConflicts,
    CancelledNoChanges,
    CancelledPartialSuccess,
    FailedNoChanges,
    FailedPartialSuccess,
    FailedUncertainDurability,
    NeedsReplan,
};

struct LocalMergeSkippedEntry
{
    QString relativePath;
    QString reason;
    LocalMergeEntryClassification classification = LocalMergeEntryClassification::FileConflict;
    struct stat sourceStat {};
    bool sourceStatValid = false;
    struct stat destinationStat {};
    bool destinationStatValid = false;
};

struct LocalMergeReplacedFileRecord
{
    QString relativePath;
    struct stat previousStat {};
    bool previousStatValid = false;
    struct stat newStat {};
    bool newStatValid = false;
    QByteArray newSha256;
    bool durabilityConfirmed = false;
    bool backupPreserved = false;
    QString backupFileName;
    QString recoveryPath;
    QString transactionId;
    QString transactionPhase;
    QString previousFileState;
    QString newFileState;
    bool manualInterventionRequired = false;
    QString note;
};

struct LocalMergeUndoContract
{
    bool reversible = false;
    bool requiresIrreversibleSerial = false;
    bool hasOverwrites = false;
    bool hasPreservedBackups = false;
    QStringList reversibleCreatedFiles;
    QStringList reversibleCreatedDirectories;
    QStringList existingDirectoriesNeverDelete;
    QStringList irreversibleOverwrittenFiles;
    QStringList preservedBackupPaths;
    QString blockReason;
};

struct LocalMergeAnchoredDirectory
{
    QString relativePath;
    QString sourcePath;
    QString destinationPath;
    QString sourceRootPath;
    struct stat expectedSourceStat {};
    bool expectedSourceValid = false;
    struct stat expectedDestStat {};
    bool expectedDestValid = false;
};

struct LocalMergeExecutionReport
{
    LocalMergeExecutionOutcome outcome = LocalMergeExecutionOutcome::NotStarted;
    QStringList filesCreated;
    QStringList filesRenamed;
    QStringList filesOverwritten;
    QStringList symlinksCreated;
    QStringList directoriesCreated;
    QStringList existingDirectoriesUsed;
    QList<LocalMergeSkippedEntry> skippedEntries;
    QList<LocalMergeReplacedFileRecord> replacedFiles;
    QStringList conflictsSkipped;
    QStringList objectsBlocked;
    QStringList failedOperations;
    QStringList entriesNeedingReplan;
    QStringList skippedSubtreeEntries;
    qsizetype skippedSubtreeCount = 0;
    qulonglong bytesProcessed = 0;
    QString lastPublishedEntry;
    LocalMergeUndoContract undoContract;
    bool completedFully = false;
    bool partialSuccess = false;
    bool cancelled = false;
    bool uncertainDurability = false;
    bool concurrentModificationDetected = false;
    bool manualInterventionRequired = false;
    bool retryAllowed = false;
};

namespace LocalMergeExecDetail {

struct UniqueFd {
    int fd = -1;
    UniqueFd() = default;
    explicit UniqueFd(int f) : fd(f) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&other) noexcept : fd(other.fd) { other.fd = -1; }
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    void reset(int newFd = -1)
    {
        if (fd >= 0) ::close(fd);
        fd = newFd;
    }
    int dupFd() const { return fd >= 0 ? ::dup(fd) : -1; }
    bool valid() const { return fd >= 0; }
};

inline UniqueFd openDirectoryNoSymlinks(const QString &absDirPath, int *outErrno = nullptr)
{
    if (outErrno) *outErrno = 0;
    const QString clean = QDir::cleanPath(absDirPath);
    if (clean.isEmpty() || !QDir::isAbsolutePath(clean)) {
        if (outErrno) *outErrno = EINVAL;
        return {};
    }

    UniqueFd cur(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!cur.valid()) {
        if (outErrno) *outErrno = errno;
        return {};
    }
    if (clean == QLatin1String("/")) {
        return cur;
    }

    const QStringList parts = clean.mid(1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QByteArray encoded = QFile::encodeName(part);
        UniqueFd next(::openat(cur.fd, encoded.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next.valid()) {
            if (outErrno) *outErrno = errno;
            return {};
        }
        cur = std::move(next);
    }
    return cur;
}

inline bool verifyFdIdentity(int fd, const struct stat &expected, bool expectedValid)
{
    if (fd < 0 || !expectedValid) return false;
    struct stat actual {};
    if (::fstat(fd, &actual) != 0) return false;
    return actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino;
}

inline bool statIdentityExactMatch(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev
        && a.st_ino == b.st_ino
        && a.st_mode == b.st_mode
        && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
}

// SEC-046-04: Never preserve SUID/SGID/sticky on newly copied regular files.
// Ordinary owner/group/other rwx bits (including executable bits such as 0755) are preserved.
inline mode_t sanitizeRegularFileMode(mode_t sourceMode)
{
    return sourceMode & static_cast<mode_t>(0777);
}

// SEC-046-04: Strip SUID/SGID (and sticky bit by explicit user-copy policy) from
// newly created destination directories; preserve ordinary owner/group/other rwx bits.
inline mode_t sanitizeDirectoryMode(mode_t sourceMode)
{
    return sourceMode & static_cast<mode_t>(0777);
}

// SEC-046-04: Always create newly populated directories restrictively (owner-only rwx 0700)
// so private source trees are never exposed as 0755 during copy and read-only (0500)
// source directories remain writable by the owner until postorder permission finalization.
inline mode_t initialDirectoryCreationMode()
{
    return static_cast<mode_t>(0700);
}

inline bool isNonPosixPermissionFs(int fd)
{
    if (fd < 0) return false;
    struct statfs sfs {};
    if (::fstatfs(fd, &sfs) != 0) return false;
    const unsigned long ftype = static_cast<unsigned long>(sfs.f_type);
    return ftype == MSDOS_SUPER_MAGIC
        || ftype == FUSE_SUPER_MAGIC
        || ftype == NTFS3_SUPER_MAGIC
        || ftype == NTFS_SB_MAGIC;
}

inline bool isSafeOverwriteFilesystem(int dirFd, bool simulateUnsupportedFs, QString *outReason = nullptr)
{
    if (simulateUnsupportedFs) {
        if (outReason) *outReason = QStringLiteral("Simulated unsupported filesystem (NTFS/VFAT/FUSE) for atomic Overwrite.");
        return false;
    }
    if (dirFd < 0) {
        if (outReason) *outReason = QStringLiteral("Invalid destination directory descriptor.");
        return false;
    }
    struct statfs sfs {};
    if (::fstatfs(dirFd, &sfs) != 0) {
        if (outReason) *outReason = QStringLiteral("Could not query destination filesystem type.");
        return false;
    }
    const unsigned long ftype = static_cast<unsigned long>(sfs.f_type);
    if (ftype == MSDOS_SUPER_MAGIC) {
        if (outReason) *outReason = QStringLiteral("VFAT/FAT32 does not support safe atomic Overwrite.");
        return false;
    }
    if (ftype == FUSE_SUPER_MAGIC || ftype == NTFS3_SUPER_MAGIC || ftype == NTFS_SB_MAGIC) {
        if (outReason) *outReason = QStringLiteral("NTFS/FUSE Overwrite is blocked until NTFS-specific durability/exchange guarantees are verified.");
        return false;
    }
    if (ftype == BTRFS_SUPER_MAGIC || ftype == EXT4_SUPER_MAGIC
        || ftype == XFS_SB_MAGIC || ftype == TMPFS_MAGIC) {
        return true;
    }
    if (outReason) *outReason = QStringLiteral("Destination filesystem is not on the verified atomic-exchange allowlist.");
    return false;
}

inline qulonglong nextStagingSequence()
{
    static std::atomic<qulonglong> seq{1};
    return seq.fetch_add(1, std::memory_order_relaxed);
}

inline bool directoryHasCaseCollision(int dirFd, const QString &candidateName, QString *outCollidingName = nullptr)
{
    if (dirFd < 0) return true;
    const int duped = ::dup(dirFd);
    if (duped < 0) return true;
    DIR *stream = ::fdopendir(duped);
    if (!stream) {
        ::close(duped);
        return true;
    }
    ::rewinddir(stream);
    const QString foldedCandidate = candidateName.toCaseFolded();
    bool collision = false;
    errno = 0;
    while (const auto *ent = ::readdir(stream)) {
        const QByteArray raw(ent->d_name);
        if (raw == "." || raw == "..") continue;
        const QString existing = QFile::decodeName(raw);
        if (existing == candidateName || existing.toCaseFolded() == foldedCandidate) {
            if (outCollidingName) *outCollidingName = existing;
            collision = true;
            break;
        }
    }
    const int readErr = errno;
    ::closedir(stream);
    return collision || readErr != 0;
}

inline bool isSafeSingleFileNameComponent(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return false;
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.contains(QLatin1Char('\0'))) {
        return false;
    }
    return true;
}

}  // namespace LocalMergeExecDetail

// Descriptor-anchored, TOCTOU-hardened single-file copy/overwrite worker.
// All syscalls operate on verified srcParentFd and destParentFd directory fds.
class LocalAnchoredFileCopyWorker final : public QObject
{
    Q_OBJECT

public:
    enum class Mode {
        CreateNoOverwrite,
        OverwriteExisting,
    };

    enum class FaultPhase {
        AfterJournalPrepared,
        AfterStagingCreated,
        AfterStagingDurable,
        AfterExchangeIntentDurable,
        BeforeExchange,
        AfterExchange,
        AfterUnexpectedInodeDetected,
        BeforeRollback,
        AfterRollback,
        BeforeBackupRename,
        AfterPreviousFileSecured,
        AfterDirectoryDurable,
        BeforeBackupUnlink,
    };

    struct FaultConfig {
        bool simulateFsyncEio = false;
        bool simulateOTmpfileUnsupported = false;
        bool simulateRenameExchangeUnsupported = false;
        bool simulateUnsupportedFs = false;
        std::function<void(const QString &relPath, int destParentFd)> prePublishHook;
        std::function<void(FaultPhase phase, const QString &relPath, int destParentFd, const QString &partName, const QString &backupName)> phaseHook;
        std::function<bool()> verifyChainHook;
    };

    LocalAnchoredFileCopyWorker(
        Mode mode,
        int srcParentFd,
        QString srcName,
        struct stat expectedSrcStat,
        int destParentFd,
        QString partName,
        QString destFinalName,
        struct stat expectedDestStat,
        QString relativePath,
        qsizetype chunkSize,
        int chunkDelayMilliseconds,
        FaultConfig faults)
        : m_mode(mode)
        , m_srcParentFd(srcParentFd)
        , m_srcName(std::move(srcName))
        , m_expectedSrcStat(expectedSrcStat)
        , m_destParentFd(destParentFd)
        , m_partName(std::move(partName))
        , m_destFinalName(std::move(destFinalName))
        , m_expectedDestStat(expectedDestStat)
        , m_relativePath(std::move(relativePath))
        , m_chunkSize(qMax<qsizetype>(4096, chunkSize))
        , m_chunkDelayMilliseconds(qMax(0, chunkDelayMilliseconds))
        , m_faults(std::move(faults))
    {
    }

    ~LocalAnchoredFileCopyWorker() override
    {
        if (m_srcParentFd >= 0) ::close(m_srcParentFd);
        if (m_destParentFd >= 0) ::close(m_destParentFd);
    }

    void requestCancel() { m_cancelled.storeRelease(true); }
    bool destinationPublished() const { return m_destinationPublished; }
    bool directoryDurabilityConfirmed() const { return m_directoryDurabilityConfirmed; }
    bool backupPreserved() const { return m_backupPreserved; }
    bool concurrentModificationDetected() const { return m_concurrentModificationDetected; }
    bool manualInterventionRequired() const { return m_manualInterventionRequired; }
    QString displacedBackupName() const { return m_displacedBackupName; }
    QString transactionId() const { return m_transactionId; }
    QString transactionPhase() const { return m_transactionPhase; }
    struct stat previousDestStat() const { return m_previousDestStat; }
    struct stat publishedDestStat() const { return m_publishedDestStat; }
    QByteArray checksum() const { return m_checksum; }

public Q_SLOTS:
    void run()
    {
        if (m_faults.verifyChainHook && !m_faults.verifyChainHook()) {
            Q_EMIT failed(KIO::ERR_DOES_NOT_EXIST, tr("Directory tree moved or replaced outside approved merge tree."));
            return;
        }

        const QByteArray srcEnc = QFile::encodeName(m_srcName);
        const QByteArray partEnc = QFile::encodeName(m_partName);
        const QByteArray dstEnc = QFile::encodeName(m_destFinalName);

        LocalMergeExecDetail::UniqueFd srcFd(
            ::openat(m_srcParentFd, srcEnc.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (!srcFd.valid()) {
            Q_EMIT failed(KIO::ERR_CANNOT_OPEN_FOR_READING, tr("Could not open anchored source file."));
            return;
        }
        struct stat actualSrc {};
        if (::fstat(srcFd.fd, &actualSrc) != 0
            || !S_ISREG(actualSrc.st_mode)
            || !LocalMergeExecDetail::statIdentityExactMatch(actualSrc, m_expectedSrcStat)) {
            Q_EMIT failed(KIO::ERR_DOES_NOT_EXIST, tr("Source file identity changed before copy."));
            return;
        }

        if (m_mode == Mode::CreateNoOverwrite) {
            struct stat dstChk {};
            if (::fstatat(m_destParentFd, dstEnc.constData(), &dstChk, AT_SYMLINK_NOFOLLOW) == 0
                || errno != ENOENT) {
                Q_EMIT failed(KIO::ERR_FILE_ALREADY_EXIST, tr("The destination already exists."));
                return;
            }
        } else {
            QString fsReason;
            if (!LocalMergeExecDetail::isSafeOverwriteFilesystem(m_destParentFd, m_faults.simulateUnsupportedFs, &fsReason)) {
                Q_EMIT failed(KIO::ERR_UNSUPPORTED_ACTION, fsReason);
                return;
            }
            struct stat dstChk {};
            if (::fstatat(m_destParentFd, dstEnc.constData(), &dstChk, AT_SYMLINK_NOFOLLOW) != 0
                || !S_ISREG(dstChk.st_mode)
                || !LocalMergeExecDetail::statIdentityExactMatch(dstChk, m_expectedDestStat)) {
                Q_EMIT failed(KIO::ERR_FILE_ALREADY_EXIST,
                              tr("Destination file changed, became a symlink, or does not match bound identity."));
                return;
            }
        }

        // Stage into O_TMPFILE when supported (no O_EXCL so linkat can materialize it),
        // otherwise fall back to descriptor-anchored O_CREAT|O_EXCL named staging.
        // For OverwriteExisting, isolate staging & journal inside an owner-only 0700 .thispc-tx.* directory.
        LocalMergeExecDetail::UniqueFd txDirFd;
        QString txDirName;
        QByteArray txDirEnc;
        QString journalName;
        LocalMergeTxJournalRecord txRec;
        int stageParentFd = m_destParentFd;
        QByteArray stageEnc = partEnc;

        if (m_mode == Mode::OverwriteExisting) {
            m_transactionId = LocalMergeTxDetail::generateTxId();
            if (m_transactionId.isEmpty() || !LocalMergeTxValidation::isValidTxId(m_transactionId)) {
                Q_EMIT failed(KIO::ERR_CANNOT_MKDIR, tr("Could not generate secure transaction identifier."));
                return;
            }
            txDirName = QStringLiteral(".thispc-tx.%1").arg(m_transactionId);
            txDirEnc = QFile::encodeName(txDirName);
            journalName = QStringLiteral(".thispc-merge-tx.%1").arg(m_transactionId);

            if (::mkdirat(m_destParentFd, txDirEnc.constData(), 0700) != 0) {
                Q_EMIT failed(KIO::ERR_CANNOT_MKDIR, tr("Could not create isolated 0700 transaction directory."));
                return;
            }
            txDirFd.reset(::openat(m_destParentFd, txDirEnc.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat txDirSt {};
            if (!txDirFd.valid()
                || ::fstat(txDirFd.fd, &txDirSt) != 0
                || !S_ISDIR(txDirSt.st_mode)
                || txDirSt.st_uid != ::geteuid()
                || (txDirSt.st_mode & 0777) != 0700) {
                txDirFd.reset();
                ::unlinkat(m_destParentFd, txDirEnc.constData(), AT_REMOVEDIR);
                Q_EMIT failed(KIO::ERR_CANNOT_MKDIR, tr("Transaction directory failed 0700 ownership/mode verification."));
                return;
            }
            ::fsync(m_destParentFd);

            struct stat parentSt {};
            ::fstat(m_destParentFd, &parentSt);

            txRec.txId = m_transactionId;
            txRec.phase = LocalMergeTxPhase::Prepared;
            txRec.approvedAction = QStringLiteral("Overwrite");
            txRec.sourcePath = m_relativePath;
            txRec.destinationPath = m_destFinalName;
            txRec.destFileName = m_destFinalName;
            txRec.stageFileName = QStringLiteral("stage");
            txRec.replacedBackupName = QStringLiteral(".thispc-replaced.%1.%2").arg(m_destFinalName, m_transactionId);
            txRec.parentStat = LocalMergeTxStatSnapshot::fromStat(parentSt);
            txRec.sourceStat = LocalMergeTxStatSnapshot::fromStat(actualSrc);
            txRec.expectedDestStat = LocalMergeTxStatSnapshot::fromStat(m_expectedDestStat);

            if (!LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec)) {
                txDirFd.reset();
                ::unlinkat(m_destParentFd, txDirEnc.constData(), AT_REMOVEDIR);
                Q_EMIT failed(KIO::ERR_CANNOT_WRITE, tr("Could not write durable transaction journal."));
                return;
            }
            m_transactionPhase = localMergeTxPhaseName(txRec.phase);
            if (m_faults.phaseHook) {
                m_faults.phaseHook(FaultPhase::AfterJournalPrepared, m_relativePath, m_destParentFd, txDirName, journalName);
            }

            stageParentFd = txDirFd.fd;
            stageEnc = QByteArray("stage");
        }

        bool usedOTmpfile = false;
        LocalMergeExecDetail::UniqueFd stageFd;
#ifdef O_TMPFILE
        if (!m_faults.simulateOTmpfileUnsupported) {
            stageFd.reset(::openat(stageParentFd, ".", O_RDWR | O_TMPFILE | O_CLOEXEC, 0600));
            if (stageFd.valid()) {
                usedOTmpfile = true;
            }
        }
#endif
        if (!stageFd.valid()) {
            stageFd.reset(::openat(stageParentFd, stageEnc.constData(),
                                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            if (!stageFd.valid()) {
                const int err = errno;
                if (txDirFd.valid()) {
                    ::unlinkat(txDirFd.fd, QFile::encodeName(journalName).constData(), 0);
                    txDirFd.reset();
                    ::unlinkat(m_destParentFd, txDirEnc.constData(), AT_REMOVEDIR);
                }
                Q_EMIT failed(err == EEXIST ? KIO::ERR_FILE_ALREADY_EXIST : KIO::ERR_CANNOT_OPEN_FOR_WRITING,
                              tr("Could not create anchored staging file."));
                return;
            }
        }

        struct stat stageStat {};
        if (::fstat(stageFd.fd, &stageStat) != 0 || !S_ISREG(stageStat.st_mode)) {
            if (!usedOTmpfile) {
                stageFd.reset();
                ::unlinkat(stageParentFd, stageEnc.constData(), 0);
            }
            if (txDirFd.valid()) {
                ::unlinkat(txDirFd.fd, QFile::encodeName(journalName).constData(), 0);
                txDirFd.reset();
                ::unlinkat(m_destParentFd, txDirEnc.constData(), AT_REMOVEDIR);
            }
            Q_EMIT failed(KIO::ERR_CANNOT_OPEN_FOR_WRITING, tr("Could not stat anchored staging file."));
            return;
        }

        if (m_mode == Mode::OverwriteExisting) {
            txRec.phase = LocalMergeTxPhase::StagingCreated;
            txRec.stageStat = LocalMergeTxStatSnapshot::fromStat(stageStat);
            LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec);
            m_transactionPhase = localMergeTxPhaseName(txRec.phase);
            if (m_faults.phaseHook) {
                m_faults.phaseHook(FaultPhase::AfterStagingCreated, m_relativePath, m_destParentFd, txDirName, journalName);
            }
        }

        auto cleanupNamedStagingIfOwned = [&](int parentDirFd, const QByteArray &nameEnc, const struct stat &expectedSt) {
            struct stat cur {};
            if (::fstatat(parentDirFd, nameEnc.constData(), &cur, AT_SYMLINK_NOFOLLOW) == 0
                && cur.st_dev == expectedSt.st_dev && cur.st_ino == expectedSt.st_ino) {
                ::unlinkat(parentDirFd, nameEnc.constData(), 0);
            }
        };

        auto cleanupTxDirPreExchange = [&]() {
            if (txDirFd.valid()) {
                ::unlinkat(txDirFd.fd, QFile::encodeName(journalName).constData(), 0);
                txDirFd.reset();
                ::unlinkat(m_destParentFd, txDirEnc.constData(), AT_REMOVEDIR);
            }
        };

        auto cleanupStage = [&]() {
            stageFd.reset();
            if (!usedOTmpfile) {
                cleanupNamedStagingIfOwned(stageParentFd, stageEnc, stageStat);
            }
            cleanupTxDirPreExchange();
        };

        QByteArray buf(m_chunkSize, Qt::Uninitialized);
        QCryptographicHash hasher(QCryptographicHash::Sha256);
        qulonglong processed = 0;

        while (true) {
            if (m_cancelled.loadAcquire()) {
                cleanupStage();
                Q_EMIT cancelled();
                return;
            }
            const ssize_t n = ::read(srcFd.fd, buf.data(), static_cast<size_t>(buf.size()));
            if (n < 0) {
                if (errno == EINTR) continue;
                cleanupStage();
                Q_EMIT failed(KIO::ERR_CANNOT_READ, tr("Error reading anchored source file."));
                return;
            }
            if (n == 0) break;

            hasher.addData(QByteArrayView(buf.constData(), n));
            ssize_t off = 0;
            while (off < n) {
                const ssize_t w = ::write(stageFd.fd, buf.constData() + off, static_cast<size_t>(n - off));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    const int werr = errno;
                    cleanupStage();
                    Q_EMIT failed((werr == ENOSPC || werr == EDQUOT) ? KIO::ERR_DISK_FULL : KIO::ERR_CANNOT_WRITE,
                                  tr("Error writing anchored staging file."));
                    return;
                }
                off += w;
            }
            processed += static_cast<qulonglong>(n);
            Q_EMIT progress(processed);
            if (m_chunkDelayMilliseconds > 0) {
                QThread::msleep(static_cast<unsigned long>(m_chunkDelayMilliseconds));
            }
        }

        // Verify source was not modified in-place while streaming
        struct stat postReadSrc {};
        if (::fstat(srcFd.fd, &postReadSrc) != 0
            || !LocalMergeExecDetail::statIdentityExactMatch(postReadSrc, actualSrc)) {
            cleanupStage();
            Q_EMIT failed(KIO::ERR_CANNOT_READ, tr("Source file was modified during copy."));
            return;
        }

        m_checksum = hasher.result();

        if (m_cancelled.loadAcquire()) {
            cleanupStage();
            Q_EMIT cancelled();
            return;
        }

        const mode_t safeFileMode = LocalMergeExecDetail::sanitizeRegularFileMode(actualSrc.st_mode);
        if (::fchmod(stageFd.fd, safeFileMode) != 0) {
            const int chmodErr = errno;
            if (!LocalMergeExecDetail::isNonPosixPermissionFs(m_destParentFd)
                && chmodErr != ENOTSUP && chmodErr != EOPNOTSUPP && chmodErr != ENOSYS) {
                cleanupStage();
                Q_EMIT failed(KIO::ERR_CANNOT_CHMOD, tr("Could not apply safe permissions to staging file."));
                return;
            }
        }
        const struct timespec times[2] = {actualSrc.st_atim, actualSrc.st_mtim};
        ::futimens(stageFd.fd, times);

        if (::fsync(stageFd.fd) != 0) {
            cleanupStage();
            Q_EMIT failed(KIO::ERR_CANNOT_WRITE, tr("Could not sync staging file to disk."));
            return;
        }
        ::fstat(stageFd.fd, &stageStat);

        if (m_faults.prePublishHook) {
            m_faults.prePublishHook(m_relativePath, m_destParentFd);
        }

        if (m_cancelled.loadAcquire()) {
            cleanupStage();
            Q_EMIT cancelled();
            return;
        }

        if (m_faults.verifyChainHook && !m_faults.verifyChainHook()) {
            cleanupStage();
            Q_EMIT failed(KIO::ERR_DOES_NOT_EXIST, tr("Directory tree moved or replaced before publication."));
            return;
        }

        auto linkFdIntoDir = [&](int fd, int targetDirFd, const QByteArray &targetEnc, int *outErrno) -> bool {
            int res = ::linkat(fd, "", targetDirFd, targetEnc.constData(), AT_EMPTY_PATH);
            int err = errno;
            if (res != 0 && (err == ENOENT || err == EPERM || err == EINVAL)) {
                const QByteArray procFd = "/proc/self/fd/" + QByteArray::number(fd);
                res = ::linkat(AT_FDCWD, procFd.constData(), targetDirFd, targetEnc.constData(), AT_SYMLINK_FOLLOW);
                err = errno;
            }
            if (outErrno) *outErrno = err;
            return res == 0;
        };

        if (m_mode == Mode::CreateNoOverwrite) {
            int linkErr = 0;
            if (linkFdIntoDir(stageFd.fd, m_destParentFd, dstEnc, &linkErr)) {
                m_destinationPublished = true;
                m_publishedDestStat = stageStat;
                stageFd.reset();
                if (!usedOTmpfile) {
                    cleanupNamedStagingIfOwned(m_destParentFd, partEnc, stageStat);
                }
            } else if (linkErr == EEXIST || linkErr == EBUSY || linkErr == ENOTEMPTY || linkErr == EISDIR) {
                cleanupStage();
                Q_EMIT failed(KIO::ERR_FILE_ALREADY_EXIST, tr("The destination already exists."));
                return;
            } else {
                struct statfs sfs {};
                const bool isVfat = (::fstatfs(m_destParentFd, &sfs) == 0
                                     && static_cast<unsigned long>(sfs.f_type) == MSDOS_SUPER_MAGIC);
                if (!isVfat || usedOTmpfile) {
                    cleanupStage();
                    Q_EMIT failed(KIO::ERR_CANNOT_RENAME, tr("Atomic no-replace publication failed."));
                    return;
                }
                stageFd.reset();
                if (::syscall(SYS_renameat2, m_destParentFd, partEnc.constData(),
                              m_destParentFd, dstEnc.constData(), RENAME_NOREPLACE) != 0) {
                    const int rerr = errno;
                    cleanupNamedStagingIfOwned(m_destParentFd, partEnc, stageStat);
                    Q_EMIT failed(rerr == EEXIST ? KIO::ERR_FILE_ALREADY_EXIST : KIO::ERR_CANNOT_RENAME,
                                  tr("Atomic renameat2(RENAME_NOREPLACE) failed."));
                    return;
                }
                m_destinationPublished = true;
                m_publishedDestStat = stageStat;
            }

            const int syncRes = m_faults.simulateFsyncEio ? -1 : ::fsync(m_destParentFd);
            if (syncRes != 0) {
                m_directoryDurabilityConfirmed = false;
                Q_EMIT failed(KIO::ERR_CANNOT_WRITE,
                              tr("File published, but destination directory fsync failed (uncertain durability)."));
                return;
            }
            m_directoryDurabilityConfirmed = true;
            Q_EMIT succeeded();
            return;
        }

        // Mode::OverwriteExisting inside 0700 txDirFd:
        // If O_TMPFILE was used, materialize stageFd inside txDirFd as "stage" before RENAME_EXCHANGE.
        if (usedOTmpfile) {
            int linkErr = 0;
            if (!linkFdIntoDir(stageFd.fd, txDirFd.fd, stageEnc, &linkErr)) {
                stageFd.reset();
                cleanupTxDirPreExchange();
                Q_EMIT failed(linkErr == EEXIST ? KIO::ERR_FILE_ALREADY_EXIST : KIO::ERR_CANNOT_RENAME,
                              tr("Could not materialize O_TMPFILE staging entry for exchange."));
                return;
            }
            usedOTmpfile = false;
        }
        stageFd.reset();

        txRec.phase = LocalMergeTxPhase::StagingDurable;
        txRec.newSha256Hex = QString::fromLatin1(m_checksum.toHex());
        txRec.stageStat = LocalMergeTxStatSnapshot::fromStat(stageStat);
        if (!LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec)) {
            cleanupNamedStagingIfOwned(txDirFd.fd, stageEnc, stageStat);
            cleanupTxDirPreExchange();
            Q_EMIT failed(KIO::ERR_CANNOT_WRITE, tr("Could not persist StagingDurable journal."));
            return;
        }
        m_transactionPhase = localMergeTxPhaseName(txRec.phase);
        if (m_faults.phaseHook) {
            m_faults.phaseHook(FaultPhase::AfterStagingDurable, m_relativePath, m_destParentFd, txDirName, journalName);
        }

        // Pre-exchange target identity verification
        struct stat preExchDst {};
        if (::fstatat(m_destParentFd, dstEnc.constData(), &preExchDst, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISREG(preExchDst.st_mode)
            || !LocalMergeExecDetail::statIdentityExactMatch(preExchDst, m_expectedDestStat)) {
            cleanupNamedStagingIfOwned(txDirFd.fd, stageEnc, stageStat);
            cleanupTxDirPreExchange();
            Q_EMIT failed(KIO::ERR_FILE_ALREADY_EXIST,
                          tr("Destination file identity changed before overwrite exchange."));
            return;
        }

        const QString backupName = txRec.replacedBackupName;
        const QByteArray backupEnc = QFile::encodeName(backupName);
        const QString relStageInDest = txDirName + QLatin1Char('/') + QStringLiteral("stage");
        const QString relBackupInDest = txDirName + QLatin1Char('/') + backupName;

        // Persist ExchangeIntentDurable BEFORE calling RENAME_EXCHANGE
        txRec.phase = LocalMergeTxPhase::ExchangeIntentDurable;
        if (!LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec)) {
            cleanupNamedStagingIfOwned(txDirFd.fd, stageEnc, stageStat);
            cleanupTxDirPreExchange();
            Q_EMIT failed(KIO::ERR_CANNOT_WRITE, tr("Could not persist ExchangeIntentDurable journal."));
            return;
        }
        m_transactionPhase = localMergeTxPhaseName(txRec.phase);
        if (m_faults.phaseHook) {
            m_faults.phaseHook(FaultPhase::AfterExchangeIntentDurable, m_relativePath, m_destParentFd, txDirName, journalName);
        }

        auto renameStageInTxToBackup = [&]() -> QPair<QString, QByteArray> {
            if (::syscall(SYS_renameat2, txDirFd.fd, stageEnc.constData(),
                          txDirFd.fd, backupEnc.constData(), RENAME_NOREPLACE) == 0) {
                ::fsync(txDirFd.fd);
                return {backupName, backupEnc};
            }
            const QString retryBackup = QStringLiteral(".thispc-replaced.%1.%2.%3")
                                            .arg(m_destFinalName)
                                            .arg(static_cast<qulonglong>(::getpid()))
                                            .arg(LocalMergeExecDetail::nextStagingSequence());
            const QByteArray retryEnc = QFile::encodeName(retryBackup);
            if (::syscall(SYS_renameat2, txDirFd.fd, stageEnc.constData(),
                          txDirFd.fd, retryEnc.constData(), RENAME_NOREPLACE) == 0) {
                ::fsync(txDirFd.fd);
                return {retryBackup, retryEnc};
            }
            return {QStringLiteral("stage"), stageEnc};
        };

        if (m_faults.phaseHook) {
            m_faults.phaseHook(FaultPhase::BeforeExchange, m_relativePath, m_destParentFd, relStageInDest, relBackupInDest);
        }

        if (m_faults.simulateRenameExchangeUnsupported) {
            cleanupNamedStagingIfOwned(txDirFd.fd, stageEnc, stageStat);
            cleanupTxDirPreExchange();
            Q_EMIT failed(KIO::ERR_UNSUPPORTED_ACTION,
                          tr("Atomic renameat2(RENAME_EXCHANGE) is not supported."));
            return;
        }

        if (::syscall(SYS_renameat2, txDirFd.fd, stageEnc.constData(),
                      m_destParentFd, dstEnc.constData(), RENAME_EXCHANGE) != 0) {
            cleanupNamedStagingIfOwned(txDirFd.fd, stageEnc, stageStat);
            cleanupTxDirPreExchange();
            Q_EMIT failed(KIO::ERR_CANNOT_RENAME,
                          tr("Atomic renameat2(RENAME_EXCHANGE) failed; destination left untouched."));
            return;
        }

        if (m_faults.phaseHook) {
            m_faults.phaseHook(FaultPhase::AfterExchange, m_relativePath, m_destParentFd, relStageInDest, relBackupInDest);
        }

        // Post-exchange verification: inspect what RENAME_EXCHANGE actually displaced into txDirFd/stage.
        struct stat displacedStat {};
        if (::fstatat(txDirFd.fd, stageEnc.constData(), &displacedStat, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISREG(displacedStat.st_mode)
            || !LocalMergeExecDetail::statIdentityExactMatch(displacedStat, m_expectedDestStat)) {
            m_concurrentModificationDetected = true;
            if (m_faults.phaseHook) {
                m_faults.phaseHook(FaultPhase::AfterUnexpectedInodeDetected, m_relativePath, m_destParentFd, relStageInDest, relBackupInDest);
            }
            struct stat curPartNow {};
            struct stat curDstNow {};
            const bool partStable = (::fstatat(txDirFd.fd, stageEnc.constData(), &curPartNow, AT_SYMLINK_NOFOLLOW) == 0
                                     && curPartNow.st_dev == displacedStat.st_dev
                                     && curPartNow.st_ino == displacedStat.st_ino);
            const bool dstStable = (::fstatat(m_destParentFd, dstEnc.constData(), &curDstNow, AT_SYMLINK_NOFOLLOW) == 0
                                    && curDstNow.st_dev == stageStat.st_dev
                                    && curDstNow.st_ino == stageStat.st_ino);
            if (partStable && dstStable) {
                if (m_faults.phaseHook) {
                    m_faults.phaseHook(FaultPhase::BeforeRollback, m_relativePath, m_destParentFd, relStageInDest, relBackupInDest);
                }
                if (::syscall(SYS_renameat2, txDirFd.fd, stageEnc.constData(),
                              m_destParentFd, dstEnc.constData(), RENAME_EXCHANGE) == 0) {
                    if (m_faults.phaseHook) {
                        m_faults.phaseHook(FaultPhase::AfterRollback, m_relativePath, m_destParentFd, relStageInDest, relBackupInDest);
                    }
                    struct stat postRollbackPart {};
                    if (::fstatat(txDirFd.fd, stageEnc.constData(), &postRollbackPart, AT_SYMLINK_NOFOLLOW) == 0
                        && postRollbackPart.st_dev == stageStat.st_dev
                        && postRollbackPart.st_ino == stageStat.st_ino) {
                        cleanupNamedStagingIfOwned(txDirFd.fd, stageEnc, stageStat);
                        cleanupTxDirPreExchange();
                        Q_EMIT failed(KIO::ERR_FILE_ALREADY_EXIST,
                                      tr("Concurrent destination modification detected during exchange; rolled back cleanly."));
                        return;
                    }
                    const auto preserved = renameStageInTxToBackup();
                    txRec.phase = LocalMergeTxPhase::UncertainDurability;
                    txRec.replacedBackupName = preserved.first;
                    LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec);
                    m_transactionPhase = localMergeTxPhaseName(txRec.phase);
                    m_destinationPublished = true;
                    m_directoryDurabilityConfirmed = false;
                    m_backupPreserved = true;
                    m_manualInterventionRequired = true;
                    m_displacedBackupName = txDirName + QLatin1Char('/') + preserved.first;
                    m_previousDestStat = postRollbackPart;
                    m_publishedDestStat = stageStat;
                    Q_EMIT failed(KIO::ERR_CANNOT_WRITE,
                                  tr("Concurrent modification during rollback; displaced object preserved at %1.")
                                      .arg(m_displacedBackupName));
                    return;
                }
            }
            const auto preserved = renameStageInTxToBackup();
            txRec.phase = LocalMergeTxPhase::UncertainDurability;
            txRec.replacedBackupName = preserved.first;
            LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec);
            m_transactionPhase = localMergeTxPhaseName(txRec.phase);
            m_destinationPublished = true;
            m_directoryDurabilityConfirmed = false;
            m_backupPreserved = true;
            m_manualInterventionRequired = true;
            m_displacedBackupName = txDirName + QLatin1Char('/') + preserved.first;
            m_previousDestStat = displacedStat;
            m_publishedDestStat = stageStat;
            Q_EMIT failed(KIO::ERR_CANNOT_WRITE,
                          tr("Concurrent modification detected during exchange; displaced object preserved at %1.")
                              .arg(m_displacedBackupName));
            return;
        }

        m_destinationPublished = true;
        m_previousDestStat = displacedStat;
        m_publishedDestStat = stageStat;
        txRec.phase = LocalMergeTxPhase::ExchangePerformed;
        LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec);
        m_transactionPhase = localMergeTxPhaseName(txRec.phase);

        if (m_faults.phaseHook) {
            m_faults.phaseHook(FaultPhase::BeforeBackupRename, m_relativePath, m_destParentFd, relStageInDest, relBackupInDest);
        }

        // Move displaced old file to .thispc-replaced.* inside 0700 txDirFd before directory fsync
        const auto renamedBackup = renameStageInTxToBackup();
        const QString activeBackupName = renamedBackup.first;
        const QByteArray activeBackupEnc = renamedBackup.second;
        m_displacedBackupName = txDirName + QLatin1Char('/') + activeBackupName;

        txRec.phase = LocalMergeTxPhase::PreviousFileSecured;
        txRec.replacedBackupName = activeBackupName;
        LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec);
        m_transactionPhase = localMergeTxPhaseName(txRec.phase);
        if (m_faults.phaseHook) {
            m_faults.phaseHook(FaultPhase::AfterPreviousFileSecured, m_relativePath, m_destParentFd, relStageInDest, m_displacedBackupName);
        }

        const int syncRes = m_faults.simulateFsyncEio ? -1 : ::fsync(m_destParentFd);
        if (syncRes != 0) {
            txRec.phase = LocalMergeTxPhase::UncertainDurability;
            txRec.durabilityConfirmed = false;
            LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec);
            m_transactionPhase = localMergeTxPhaseName(txRec.phase);
            m_directoryDurabilityConfirmed = false;
            m_backupPreserved = true;
            m_manualInterventionRequired = true;
            Q_EMIT failed(KIO::ERR_CANNOT_WRITE,
                          tr("Overwrite published, but destination directory fsync failed; previous file preserved at %1.")
                              .arg(m_displacedBackupName));
            return;
        }

        m_directoryDurabilityConfirmed = true;
        txRec.phase = LocalMergeTxPhase::DirectoryDurable;
        txRec.durabilityConfirmed = true;
        LocalMergeTxDetail::writeJournalAtomically(txDirFd.fd, journalName, txRec);
        m_transactionPhase = localMergeTxPhaseName(txRec.phase);
        if (m_faults.phaseHook) {
            m_faults.phaseHook(FaultPhase::AfterDirectoryDurable, m_relativePath, m_destParentFd, relStageInDest, m_displacedBackupName);
            m_faults.phaseHook(FaultPhase::BeforeBackupUnlink, m_relativePath, m_destParentFd, relStageInDest, m_displacedBackupName);
        }

        // SAFE-CLEANUP-01: Inside 0700 txDirFd, openat(O_NOFOLLOW) + fstat verify before unlinkat()
        bool backupUnlinked = false;
        LocalMergeExecDetail::UniqueFd bkFd(
            ::openat(txDirFd.fd, activeBackupEnc.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        struct stat bkChk {};
        if (bkFd.valid()
            && ::fstat(bkFd.fd, &bkChk) == 0
            && S_ISREG(bkChk.st_mode)
            && bkChk.st_dev == m_expectedDestStat.st_dev
            && bkChk.st_ino == m_expectedDestStat.st_ino) {
            bkFd.reset();
            if (::unlinkat(txDirFd.fd, activeBackupEnc.constData(), 0) == 0) {
                backupUnlinked = true;
            }
        }

        if (backupUnlinked) {
            m_backupPreserved = false;
            ::unlinkat(txDirFd.fd, QFile::encodeName(journalName).constData(), 0);
            txDirFd.reset();
            ::unlinkat(m_destParentFd, txDirEnc.constData(), AT_REMOVEDIR);
            ::fsync(m_destParentFd);
            m_transactionPhase = localMergeTxPhaseName(LocalMergeTxPhase::Completed);
        } else {
            // Fail-closed: preserve unknown/unverified object inside 0700 txDirFd
            m_backupPreserved = true;
            m_manualInterventionRequired = true;
        }

        Q_EMIT succeeded();
    }

Q_SIGNALS:
    void progress(qulonglong bytes);
    void succeeded();
    void cancelled();
    void failed(int errorCode, const QString &errorText);

private:
    Mode m_mode = Mode::CreateNoOverwrite;
    int m_srcParentFd = -1;
    QString m_srcName;
    struct stat m_expectedSrcStat {};
    int m_destParentFd = -1;
    QString m_partName;
    QString m_destFinalName;
    struct stat m_expectedDestStat {};
    QString m_relativePath;
    qsizetype m_chunkSize = 256 * 1024;
    int m_chunkDelayMilliseconds = 0;
    FaultConfig m_faults;
    QAtomicInteger<bool> m_cancelled = false;
    bool m_destinationPublished = false;
    bool m_directoryDurabilityConfirmed = false;
    bool m_backupPreserved = false;
    bool m_concurrentModificationDetected = false;
    bool m_manualInterventionRequired = false;
    QString m_displacedBackupName;
    QString m_transactionId;
    QString m_transactionPhase;
    struct stat m_previousDestStat {};
    struct stat m_publishedDestStat {};
    QByteArray m_checksum;
};

class LocalAnchoredFileCopyJob final : public KJob
{
    Q_OBJECT

public:
    LocalAnchoredFileCopyJob(
        LocalAnchoredFileCopyWorker::Mode mode,
        int srcParentFd,
        QString srcName,
        struct stat expectedSrcStat,
        int destParentFd,
        QString partName,
        QString destFinalName,
        struct stat expectedDestStat,
        QString relativePath,
        QObject *parent = nullptr,
        qsizetype chunkSize = 256 * 1024,
        int chunkDelayMilliseconds = 0,
        LocalAnchoredFileCopyWorker::FaultConfig faults = {})
        : KJob(parent)
        , m_worker(new LocalAnchoredFileCopyWorker(
              mode, srcParentFd, std::move(srcName), expectedSrcStat,
              destParentFd, std::move(partName), std::move(destFinalName),
              expectedDestStat, std::move(relativePath), chunkSize, chunkDelayMilliseconds,
              std::move(faults)))
    {
        setCapabilities(KJob::Killable);
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::started, m_worker, &LocalAnchoredFileCopyWorker::run);
        connect(m_worker, &LocalAnchoredFileCopyWorker::progress, this,
                [this](qulonglong b) { setProcessedAmount(KJob::Bytes, b); });
        connect(m_worker, &LocalAnchoredFileCopyWorker::succeeded, this, [this] {
            finish(KJob::NoError, {});
        });
        connect(m_worker, &LocalAnchoredFileCopyWorker::cancelled, this, [this] {
            finish(KJob::KilledJobError, tr("Copy cancelled."));
        });
        connect(m_worker, &LocalAnchoredFileCopyWorker::failed, this, &LocalAnchoredFileCopyJob::finish);
    }

    ~LocalAnchoredFileCopyJob() override
    {
        if (m_worker) m_worker->requestCancel();
        m_thread.quit();
        m_thread.wait();
        delete m_worker;
        m_worker = nullptr;
    }

    void start() override
    {
        if (m_started) return;
        m_started = true;
        m_thread.start();
    }

    void requestCancelAsync()
    {
        if (m_worker) m_worker->requestCancel();
    }
    bool hasFinished() const { return m_finished; }
    void waitForThreadExit()
    {
        m_thread.quit();
        m_thread.wait();
    }

    bool destinationPublished() const { return m_published; }
    bool directoryDurabilityConfirmed() const { return m_durabilityConfirmed; }
    bool backupPreserved() const { return m_backupPreserved; }
    bool concurrentModificationDetected() const { return m_concurrentModificationDetected; }
    bool manualInterventionRequired() const { return m_manualInterventionRequired; }
    QString displacedBackupName() const { return m_displacedBackupName; }
    QString transactionId() const { return m_transactionId; }
    QString transactionPhase() const { return m_transactionPhase; }
    struct stat previousDestStat() const { return m_previousDestStat; }
    struct stat publishedDestStat() const { return m_publishedDestStat; }
    QByteArray checksum() const { return m_checksum; }

protected:
    bool doKill() override
    {
        if (m_finished) return false;
        if (m_worker) m_worker->requestCancel();
        return true;
    }

private:
    void finish(int err, const QString &text)
    {
        if (m_finished) return;
        m_finished = true;
        if (m_worker) {
            m_published = m_worker->destinationPublished();
            m_durabilityConfirmed = m_worker->directoryDurabilityConfirmed();
            m_backupPreserved = m_worker->backupPreserved();
            m_concurrentModificationDetected = m_worker->concurrentModificationDetected();
            m_manualInterventionRequired = m_worker->manualInterventionRequired();
            m_displacedBackupName = m_worker->displacedBackupName();
            m_transactionId = m_worker->transactionId();
            m_transactionPhase = m_worker->transactionPhase();
            m_previousDestStat = m_worker->previousDestStat();
            m_publishedDestStat = m_worker->publishedDestStat();
            m_checksum = m_worker->checksum();
        }
        m_thread.quit();
        m_thread.wait();
        delete m_worker;
        m_worker = nullptr;
        setError(err);
        setErrorText(text);
        emitResult();
    }

    QThread m_thread;
    LocalAnchoredFileCopyWorker *m_worker = nullptr;
    bool m_started = false;
    bool m_finished = false;
    bool m_published = false;
    bool m_durabilityConfirmed = false;
    bool m_backupPreserved = false;
    bool m_concurrentModificationDetected = false;
    bool m_manualInterventionRequired = false;
    QString m_displacedBackupName;
    QString m_transactionId;
    QString m_transactionPhase;
    struct stat m_previousDestStat {};
    struct stat m_publishedDestStat {};
    QByteArray m_checksum;
};

// Stage 2B.1 additive approval (never approves overwrites, type conflicts, symlinks, or ambiguous names)
inline void approveAdditiveMergePlan(LocalMergePlan &plan)
{
    for (LocalMergePlanEntry &entry : plan.entries) {
        if (entry.classification == LocalMergeEntryClassification::CreateNew
            && (entry.sourceKind == LocalMergeObjectKind::Directory
                || entry.sourceKind == LocalMergeObjectKind::RegularFile)) {
            entry.approvedAction = LocalMergeApprovedAction::Create;
        } else if (entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate) {
            entry.approvedAction = LocalMergeApprovedAction::MergeDirectory;
        } else if (entry.classification == LocalMergeEntryClassification::FileConflict) {
            entry.approvedAction = LocalMergeApprovedAction::Skip;
            entry.decisionBinding.bound = true;
            entry.decisionBinding.expectedClassification = entry.classification;
            entry.decisionBinding.expectedSourceStat = entry.sourceStat;
            entry.decisionBinding.expectedSourceValid = entry.sourceStatValid;
            entry.decisionBinding.expectedDestinationStat = entry.destinationStat;
            entry.decisionBinding.expectedDestinationValid = entry.destinationStatValid;
            entry.decisionBinding.reason = QStringLiteral("Additive mode skips existing file conflict");
        } else {
            entry.approvedAction = LocalMergeApprovedAction::Unapproved;
        }
    }
}

// Binds an explicit Skip decision to a specific FileConflict entry
inline bool approveFileSkip(
    LocalMergePlan &plan,
    const QString &relativePath,
    const QString &reason = QStringLiteral("Skipped by user"))
{
    for (LocalMergePlanEntry &entry : plan.entries) {
        if (entry.relativePath != relativePath) continue;
        if (entry.classification != LocalMergeEntryClassification::FileConflict) return false;
        entry.approvedAction = LocalMergeApprovedAction::Skip;
        entry.decisionBinding.bound = true;
        entry.decisionBinding.expectedClassification = entry.classification;
        entry.decisionBinding.policyScope = LocalMergeApplyPolicy::None;
        entry.decisionBinding.expectedSourceStat = entry.sourceStat;
        entry.decisionBinding.expectedSourceValid = entry.sourceStatValid;
        entry.decisionBinding.expectedDestinationStat = entry.destinationStat;
        entry.decisionBinding.expectedDestinationValid = entry.destinationStatValid;
        entry.decisionBinding.reason = reason;
        return true;
    }
    return false;
}

// Applies SkipAllFileConflicts ONLY to exact FileConflict entries (never type conflicts, symlinks, or ambiguous names)
inline qsizetype applySkipAllFileConflictsPolicy(
    LocalMergePlan &plan,
    const QString &reason = QStringLiteral("Skipped by SkipAllFileConflicts policy"))
{
    qsizetype count = 0;
    for (LocalMergePlanEntry &entry : plan.entries) {
        if (entry.classification == LocalMergeEntryClassification::FileConflict
            && entry.caseStatus == LocalMergeCaseStatus::ExactNameMatch
            && entry.approvedAction == LocalMergeApprovedAction::Unapproved) {
            entry.approvedAction = LocalMergeApprovedAction::Skip;
            entry.decisionBinding.bound = true;
            entry.decisionBinding.expectedClassification = entry.classification;
            entry.decisionBinding.policyScope = LocalMergeApplyPolicy::SkipAllFileConflicts;
            entry.decisionBinding.expectedSourceStat = entry.sourceStat;
            entry.decisionBinding.expectedSourceValid = entry.sourceStatValid;
            entry.decisionBinding.expectedDestinationStat = entry.destinationStat;
            entry.decisionBinding.expectedDestinationValid = entry.destinationStatValid;
            entry.decisionBinding.reason = reason;
            ++count;
        }
    }
    return count;
}

// Binds an explicit Create decision to a non-conflicting SymbolicLink (or dangling source symlink)
inline bool approveSymlinkCreate(
    LocalMergePlan &plan,
    const QString &relativePath,
    const QString &reason = QStringLiteral("Explicitly approved symbolic link copy"))
{
    for (LocalMergePlanEntry &entry : plan.entries) {
        if (entry.relativePath != relativePath) continue;
        if (entry.sourceKind != LocalMergeObjectKind::SymbolicLink
            || entry.destinationKind != LocalMergeObjectKind::Missing
            || entry.symbolicLinkContents.isEmpty()
            || !entry.sourceStatValid
            || !S_ISLNK(entry.sourceStat.st_mode)) {
            return false;
        }
        entry.approvedAction = LocalMergeApprovedAction::Create;
        entry.decisionBinding = LocalMergeDecisionBinding{};
        entry.decisionBinding.bound = true;
        entry.decisionBinding.expectedClassification = entry.classification;
        entry.decisionBinding.expectedSourceStat = entry.sourceStat;
        entry.decisionBinding.expectedSourceValid = entry.sourceStatValid;
        entry.decisionBinding.reason = reason;
        return true;
    }
    return false;
}

// Binds an explicit Overwrite decision ONLY to a specific regular-file-on-regular-file FileConflict
inline bool approveFileOverwrite(
    LocalMergePlan &plan,
    const QString &relativePath,
    const QString &reason = QStringLiteral("Explicitly approved overwrite"),
    QString *outRejectReason = nullptr,
    LocalMergeApplyPolicy scope = LocalMergeApplyPolicy::None)
{
    auto reject = [outRejectReason](const QString &r) {
        if (outRejectReason) *outRejectReason = r;
        return false;
    };

    LocalMergePlanEntry *target = nullptr;
    for (LocalMergePlanEntry &entry : plan.entries) {
        if (entry.relativePath == relativePath) {
            target = &entry;
            break;
        }
    }
    if (!target) return reject(QStringLiteral("Entry not found in plan."));
    if (target->classification != LocalMergeEntryClassification::FileConflict
        || target->sourceKind != LocalMergeObjectKind::RegularFile
        || target->destinationKind != LocalMergeObjectKind::RegularFile
        || !target->sourceStatValid || !target->destinationStatValid
        || !S_ISREG(target->sourceStat.st_mode)
        || !S_ISREG(target->destinationStat.st_mode)) {
        return reject(QStringLiteral("Overwrite is only permitted for regular-file-on-regular-file conflicts."));
    }

    const QFileInfo dstInfo(target->destinationPath);
    LocalMergeExecDetail::UniqueFd parentFd = LocalMergeExecDetail::openDirectoryNoSymlinks(dstInfo.path());
    if (!parentFd.valid()) {
        return reject(QStringLiteral("Destination parent directory is invalid or symlinked."));
    }
    struct stat parentSt {};
    if (::fstat(parentFd.fd, &parentSt) != 0) {
        return reject(QStringLiteral("Could not stat destination parent directory."));
    }
    struct stat curDst {};
    if (::fstatat(parentFd.fd, QFile::encodeName(dstInfo.fileName()).constData(), &curDst, AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISREG(curDst.st_mode)
        || !LocalMergeExecDetail::statIdentityExactMatch(curDst, target->destinationStat)) {
        return reject(QStringLiteral("Destination file changed or is no longer a regular file matching plan."));
    }

    target->approvedAction = LocalMergeApprovedAction::Overwrite;
    target->decisionBinding.bound = true;
    target->decisionBinding.expectedClassification = target->classification;
    target->decisionBinding.policyScope = scope;
    target->decisionBinding.expectedSourceStat = target->sourceStat;
    target->decisionBinding.expectedSourceValid = true;
    target->decisionBinding.expectedDestinationStat = target->destinationStat;
    target->decisionBinding.expectedDestinationValid = true;
    target->decisionBinding.expectedParentStat = parentSt;
    target->decisionBinding.expectedParentValid = true;
    target->decisionBinding.reason = reason;
    return true;
}

// Applies OverwriteAllFileConflicts ONLY to exact-name regular-file FileConflict entries in this plan
inline qsizetype applyOverwriteAllFileConflictsPolicy(
    LocalMergePlan &plan,
    const QString &reason = QStringLiteral("Approved by OverwriteAllFileConflicts policy"))
{
    qsizetype count = 0;
    for (LocalMergePlanEntry &entry : plan.entries) {
        if (entry.classification == LocalMergeEntryClassification::FileConflict
            && entry.caseStatus == LocalMergeCaseStatus::ExactNameMatch
            && entry.sourceKind == LocalMergeObjectKind::RegularFile
            && entry.destinationKind == LocalMergeObjectKind::RegularFile
            && entry.approvedAction == LocalMergeApprovedAction::Unapproved) {
            if (approveFileOverwrite(plan, entry.relativePath, reason, nullptr,
                                     LocalMergeApplyPolicy::OverwriteAllFileConflicts)) {
                ++count;
            }
        }
    }
    return count;
}

// Binds a validated single-component Rename decision to a specific FileConflict entry
inline bool approveFileRename(
    LocalMergePlan &plan,
    const QString &relativePath,
    const QString &newFileName,
    QString *outRejectReason = nullptr)
{
    auto reject = [outRejectReason](const QString &r) {
        if (outRejectReason) *outRejectReason = r;
        return false;
    };
    if (!LocalMergeExecDetail::isSafeSingleFileNameComponent(newFileName)) {
        return reject(QStringLiteral("Target filename must be a single safe component without separators or '..'."));
    }

    LocalMergePlanEntry *targetEntry = nullptr;
    for (LocalMergePlanEntry &entry : plan.entries) {
        if (entry.relativePath == relativePath) {
            targetEntry = &entry;
            break;
        }
    }
    if (!targetEntry) return reject(QStringLiteral("Entry not found in plan."));
    const bool regularCaseRename =
        targetEntry->classification == LocalMergeEntryClassification::CaseInsensitiveCollision
        && targetEntry->sourceKind == LocalMergeObjectKind::RegularFile;
    if (targetEntry->classification != LocalMergeEntryClassification::FileConflict && !regularCaseRename) {
        return reject(QStringLiteral("Rename is only permitted for regular FileConflict or CaseInsensitiveCollision entries."));
    }

    const QFileInfo dstInfo(targetEntry->destinationPath);
    const QString parentPath = dstInfo.path();
    const QString candidateDestPath = QDir::cleanPath(QDir(parentPath).filePath(newFileName));

    const QString foldedCandidate = candidateDestPath.toCaseFolded();
    for (const LocalMergePlanEntry &other : std::as_const(plan.entries)) {
        if (&other == targetEntry) continue;
        const QString otherDest = (other.approvedAction == LocalMergeApprovedAction::Rename && !other.decisionBinding.targetFileName.isEmpty())
            ? QDir::cleanPath(QDir(QFileInfo(other.destinationPath).path()).filePath(other.decisionBinding.targetFileName))
            : QDir::cleanPath(other.destinationPath);
        if (otherDest.toCaseFolded() == foldedCandidate) {
            return reject(QStringLiteral("Target filename collides with another planned entry."));
        }
    }

    LocalMergeExecDetail::UniqueFd parentFd = LocalMergeExecDetail::openDirectoryNoSymlinks(parentPath);
    if (!parentFd.valid()) {
        return reject(QStringLiteral("Destination parent directory is invalid or symlinked."));
    }
    struct stat parentSt {};
    if (::fstat(parentFd.fd, &parentSt) != 0) {
        return reject(QStringLiteral("Could not stat destination parent directory."));
    }

    struct stat existingSt {};
    if (::fstatat(parentFd.fd, QFile::encodeName(newFileName).constData(), &existingSt, AT_SYMLINK_NOFOLLOW) == 0
        || errno != ENOENT) {
        return reject(QStringLiteral("Target filename already exists (or is a symlink/broken symlink)."));
    }
    QString collidingSibling;
    if (LocalMergeExecDetail::directoryHasCaseCollision(parentFd.fd, newFileName, &collidingSibling)) {
        return reject(QStringLiteral("Target filename has a case-insensitive collision with: %1").arg(collidingSibling));
    }

    targetEntry->approvedAction = LocalMergeApprovedAction::Rename;
    targetEntry->decisionBinding.bound = true;
    targetEntry->decisionBinding.expectedClassification = targetEntry->classification;
    targetEntry->decisionBinding.policyScope = LocalMergeApplyPolicy::None;
    targetEntry->decisionBinding.expectedSourceStat = targetEntry->sourceStat;
    targetEntry->decisionBinding.expectedSourceValid = targetEntry->sourceStatValid;
    targetEntry->decisionBinding.expectedDestinationStat = targetEntry->destinationStat;
    targetEntry->decisionBinding.expectedDestinationValid = targetEntry->destinationStatValid;
    targetEntry->decisionBinding.expectedParentStat = parentSt;
    targetEntry->decisionBinding.expectedParentValid = true;
    targetEntry->decisionBinding.targetFileName = newFileName;
    return true;
}

// Stage 3B.2: Descriptor-anchored directory Rename + full descendant revalidation.
// Never trusts old decisionBindings; checks NAME_MAX/PATH_MAX, symlinks, broken symlinks,
// case collisions, and re-stats every descendant's source inode.
inline bool approveDirectoryRenameAndRevalidate(
    LocalMergePlan &plan,
    const QString &relativePath,
    const QString &newFolderName,
    QString *outRejectReason = nullptr,
    bool *outNeedsReplan = nullptr)
{
    if (outNeedsReplan) *outNeedsReplan = false;
    auto reject = [&](const QString &r, bool needsReplan = false) {
        if (outRejectReason) *outRejectReason = r;
        if (outNeedsReplan) *outNeedsReplan = needsReplan;
        return false;
    };

    if (!LocalMergeExecDetail::isSafeSingleFileNameComponent(newFolderName)) {
        return reject(QStringLiteral("Target folder name must be a single safe component without separators or '..'."));
    }
    if (QFile::encodeName(newFolderName).size() > 255) {
        return reject(QStringLiteral("Target folder name exceeds maximum component length (255 bytes)."));
    }

    qsizetype targetIdx = -1;
    for (qsizetype i = 0; i < plan.entries.size(); ++i) {
        if (plan.entries.at(i).relativePath == relativePath) {
            targetIdx = i;
            break;
        }
    }
    if (targetIdx < 0) return reject(QStringLiteral("Directory entry not found in plan."));

    LocalMergePlanEntry &target = plan.entries[targetIdx];
    if (target.classification == LocalMergeEntryClassification::SameObject) {
        return reject(QStringLiteral("Refusing Rename on SameObject directory (source and destination are the same physical folder)."));
    }
    if (target.classification != LocalMergeEntryClassification::MergeDirectoryCandidate
        || target.sourceKind != LocalMergeObjectKind::Directory
        || !target.sourceStatValid
        || !S_ISDIR(target.sourceStat.st_mode)) {
        return reject(QStringLiteral("Directory Rename is only permitted for distinct MergeDirectoryCandidate entries."));
    }

    const QFileInfo srcInfo(target.sourcePath);
    LocalMergeExecDetail::UniqueFd srcParentFd = LocalMergeExecDetail::openDirectoryNoSymlinks(srcInfo.path());
    if (!srcParentFd.valid()) {
        return reject(QStringLiteral("Source parent directory is invalid or symlinked."));
    }
    struct stat curSrcSt {};
    if (::fstatat(srcParentFd.fd, QFile::encodeName(srcInfo.fileName()).constData(), &curSrcSt, AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISDIR(curSrcSt.st_mode)
        || curSrcSt.st_dev != target.sourceStat.st_dev
        || curSrcSt.st_ino != target.sourceStat.st_ino) {
        return reject(QStringLiteral("Source directory identity changed or became non-directory."));
    }
    if (target.destinationStatValid
        && curSrcSt.st_dev == target.destinationStat.st_dev
        && curSrcSt.st_ino == target.destinationStat.st_ino) {
        return reject(QStringLiteral("Refusing Rename on SameObject directory (identical st_dev/st_ino)."));
    }

    const QFileInfo dstInfo(target.destinationPath);
    const QString dstParentAbs = dstInfo.path();
    const QString newDestDir = QDir::cleanPath(QDir(dstParentAbs).filePath(newFolderName));
    if (QFile::encodeName(newDestDir).size() >= 4096) {
        return reject(QStringLiteral("Resulting destination path exceeds PATH_MAX."));
    }

    LocalMergeExecDetail::UniqueFd dstParentFd = LocalMergeExecDetail::openDirectoryNoSymlinks(dstParentAbs);
    if (!dstParentFd.valid()) {
        return reject(QStringLiteral("Destination parent directory is invalid or symlinked."));
    }
    struct stat dstParentSt {};
    if (::fstat(dstParentFd.fd, &dstParentSt) != 0) {
        return reject(QStringLiteral("Could not stat destination parent directory."));
    }
    if (target.classification == LocalMergeEntryClassification::MergeDirectoryCandidate && target.destinationStatValid) {
        struct stat curDstSt {};
        if (::fstatat(dstParentFd.fd, QFile::encodeName(dstInfo.fileName()).constData(), &curDstSt, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISDIR(curDstSt.st_mode)
            || curDstSt.st_dev != target.destinationStat.st_dev
            || curDstSt.st_ino != target.destinationStat.st_ino) {
            return reject(QStringLiteral("Original conflicting destination directory identity changed."), true);
        }
    }

    // Check target name is genuinely free (not existing dir/file, symlink, broken symlink, or case collision)
    struct stat existingSt {};
    if (::fstatat(dstParentFd.fd, QFile::encodeName(newFolderName).constData(), &existingSt, AT_SYMLINK_NOFOLLOW) == 0
        || errno != ENOENT) {
        return reject(QStringLiteral("Renamed target folder already exists (or is a symlink/broken symlink); replan required."), true);
    }
    QString collidingSibling;
    if (LocalMergeExecDetail::directoryHasCaseCollision(dstParentFd.fd, newFolderName, &collidingSibling)) {
        return reject(QStringLiteral("Renamed target folder has a case-insensitive collision with: %1").arg(collidingSibling), true);
    }

    const qsizetype slash = relativePath.lastIndexOf(QLatin1Char('/'));
    const QString newRelPrefix = (slash > 0) ? (relativePath.left(slash + 1) + newFolderName) : newFolderName;
    const QString oldPrefixSlash = relativePath + QLatin1Char('/');
    const QString foldedNewDest = newDestDir.toCaseFolded();

    for (qsizetype i = 0; i < plan.entries.size(); ++i) {
        if (i == targetIdx) continue;
        const LocalMergePlanEntry &other = plan.entries.at(i);
        if (other.relativePath.startsWith(oldPrefixSlash)) continue;
        if (QDir::cleanPath(other.destinationPath).toCaseFolded() == foldedNewDest) {
            return reject(QStringLiteral("Renamed folder collides with another planned destination entry."), true);
        }
    }

    // Revalidate all descendants BEFORE mutating target
    struct DescendantUpdate {
        qsizetype index = -1;
        QString newRel;
        QString newDest;
        struct stat freshSrc {};
        LocalMergeObjectKind freshKind = LocalMergeObjectKind::Unreadable;
        qulonglong freshSize = 0;
    };
    QList<DescendantUpdate> updates;
    for (qsizetype i = 0; i < plan.entries.size(); ++i) {
        if (i == targetIdx) continue;
        const LocalMergePlanEntry &desc = plan.entries.at(i);
        if (!desc.relativePath.startsWith(oldPrefixSlash)) continue;

        const QString tail = desc.relativePath.mid(oldPrefixSlash.size());
        const QStringList tailParts = tail.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &comp : tailParts) {
            if (!LocalMergeExecDetail::isSafeSingleFileNameComponent(comp) || QFile::encodeName(comp).size() > 255) {
                return reject(QStringLiteral("Descendant component exceeds NAME_MAX or is unsafe: %1").arg(comp));
            }
        }
        const QString newRel = newRelPrefix + QLatin1Char('/') + tail;
        const QString newDest = QDir::cleanPath(newDestDir + QLatin1Char('/') + tail);
        if (QFile::encodeName(newDest).size() >= 4096) {
            return reject(QStringLiteral("Descendant destination path exceeds PATH_MAX: %1").arg(newDest));
        }

        struct stat dSrcSt {};
        if (::lstat(QFile::encodeName(desc.sourcePath).constData(), &dSrcSt) != 0) {
            return reject(QStringLiteral("Could not re-stat descendant source: %1").arg(desc.sourcePath));
        }
        LocalMergeObjectKind kind = LocalMergeObjectKind::Unreadable;
        if (S_ISREG(dSrcSt.st_mode)) kind = LocalMergeObjectKind::RegularFile;
        else if (S_ISDIR(dSrcSt.st_mode)) kind = LocalMergeObjectKind::Directory;
        else if (S_ISLNK(dSrcSt.st_mode)) kind = LocalMergeObjectKind::SymbolicLink;
        else kind = LocalMergeObjectKind::Special;

        if (desc.sourceStatValid
            && (dSrcSt.st_dev != desc.sourceStat.st_dev
                || dSrcSt.st_ino != desc.sourceStat.st_ino
                || (dSrcSt.st_mode & S_IFMT) != (desc.sourceStat.st_mode & S_IFMT))) {
            return reject(QStringLiteral("Descendant source inode changed before revalidation: %1").arg(desc.sourcePath));
        }
        updates.append(DescendantUpdate{i, newRel, newDest, dSrcSt, kind,
                                        kind == LocalMergeObjectKind::RegularFile ? static_cast<qulonglong>(dSrcSt.st_size) : 0ULL});
    }

    // Commit revalidated target & descendants (invalidating all old bindings/overwrites)
    target.relativePath = newRelPrefix;
    target.destinationPath = newDestDir;
    target.approvedAction = LocalMergeApprovedAction::Rename;
    target.decisionBinding = LocalMergeDecisionBinding{};
    target.decisionBinding.bound = true;
    target.decisionBinding.expectedClassification = target.classification;
    target.decisionBinding.expectedSourceStat = target.sourceStat;
    target.decisionBinding.expectedSourceValid = true;
    target.decisionBinding.expectedDestinationStat = target.destinationStat;
    target.decisionBinding.expectedDestinationValid = target.destinationStatValid;
    target.decisionBinding.expectedParentStat = dstParentSt;
    target.decisionBinding.expectedParentValid = true;
    target.decisionBinding.targetFileName = newFolderName;

    for (const DescendantUpdate &u : updates) {
        LocalMergePlanEntry &desc = plan.entries[u.index];
        desc.relativePath = u.newRel;
        desc.destinationPath = u.newDest;
        desc.destinationKind = LocalMergeObjectKind::Missing;
        desc.destinationStat = {};
        desc.destinationStatValid = false;
        desc.destinationSize = 0;
        desc.sourceStat = u.freshSrc;
        desc.sourceStatValid = true;
        desc.sourceKind = u.freshKind;
        desc.size = u.freshSize;
        desc.decisionBinding = LocalMergeDecisionBinding{};
        if (u.freshKind == LocalMergeObjectKind::Directory || u.freshKind == LocalMergeObjectKind::RegularFile) {
            desc.classification = LocalMergeEntryClassification::CreateNew;
            desc.approvedAction = LocalMergeApprovedAction::Create;
        } else if (u.freshKind == LocalMergeObjectKind::SymbolicLink) {
            const QByteArray rawLink = readLocalLink(desc.sourcePath);
            desc.symbolicLinkContents = rawLink;
            desc.symbolicLinkTarget = QFile::decodeName(rawLink);
            struct stat targetChk {};
            const bool broken = (::stat(QFile::encodeName(desc.sourcePath).constData(), &targetChk) != 0);
            desc.sourceIsBrokenSymlink = broken;
            desc.classification = broken
                ? LocalMergeEntryClassification::BrokenSymlink
                : LocalMergeEntryClassification::CreateNew;
            if (!rawLink.isEmpty()) {
                desc.approvedAction = LocalMergeApprovedAction::Create;
                desc.decisionBinding.bound = true;
                desc.decisionBinding.expectedClassification = desc.classification;
                desc.decisionBinding.expectedSourceStat = u.freshSrc;
                desc.decisionBinding.expectedSourceValid = true;
            } else {
                desc.approvedAction = LocalMergeApprovedAction::Unapproved;
            }
        } else {
            desc.classification = LocalMergeEntryClassification::UnsupportedSpecial;
            desc.approvedAction = LocalMergeApprovedAction::Unapproved;
        }
    }
    return true;
}

// Binds Skip to a conflicting directory and all of its descendants
inline bool approveDirectorySkip(
    LocalMergePlan &plan,
    const QString &relativePath,
    const QString &reason = QStringLiteral("Directory skipped by user"))
{
    bool found = false;
    const QString prefixSlash = relativePath + QLatin1Char('/');
    for (LocalMergePlanEntry &entry : plan.entries) {
        if (entry.relativePath == relativePath) {
            const bool skippableDirectory =
                entry.sourceKind == LocalMergeObjectKind::Directory
                && (entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate
                    || entry.classification == LocalMergeEntryClassification::SameObject
                    || entry.classification == LocalMergeEntryClassification::TypeConflictDirOverFile
                    || entry.classification == LocalMergeEntryClassification::DestinationSymlinkConflict
                    || entry.classification == LocalMergeEntryClassification::BrokenSymlink
                    || entry.classification == LocalMergeEntryClassification::CaseInsensitiveCollision);
            if (!skippableDirectory) {
                return false;
            }
            entry.approvedAction = LocalMergeApprovedAction::Skip;
            entry.decisionBinding = LocalMergeDecisionBinding{};
            entry.decisionBinding.bound = true;
            entry.decisionBinding.expectedClassification = entry.classification;
            entry.decisionBinding.expectedSourceStat = entry.sourceStat;
            entry.decisionBinding.expectedSourceValid = entry.sourceStatValid;
            entry.decisionBinding.expectedDestinationStat = entry.destinationStat;
            entry.decisionBinding.expectedDestinationValid = entry.destinationStatValid;
            entry.decisionBinding.reason = reason;
            found = true;
        } else if (found && entry.relativePath.startsWith(prefixSlash)) {
            entry.approvedAction = LocalMergeApprovedAction::Skip;
            entry.decisionBinding = LocalMergeDecisionBinding{};
            entry.decisionBinding.reason = QStringLiteral("Parent folder skipped");
        }
    }
    return found;
}

// Applies SkipAllFolders ONLY to exact MergeDirectoryCandidate entries (never SameObject or file conflicts)
inline qsizetype applySkipAllFoldersPolicy(
    LocalMergePlan &plan,
    const QString &reason = QStringLiteral("Skipped by SkipAllFolders policy"))
{
    QStringList toSkip;
    for (const LocalMergePlanEntry &entry : std::as_const(plan.entries)) {
        if (entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate
            && entry.caseStatus == LocalMergeCaseStatus::ExactNameMatch
            && entry.approvedAction == LocalMergeApprovedAction::Unapproved) {
            bool ancestorSkipped = false;
            for (const QString &s : toSkip) {
                if (entry.relativePath.startsWith(s + QLatin1Char('/'))) {
                    ancestorSkipped = true;
                    break;
                }
            }
            if (!ancestorSkipped) {
                toSkip.append(entry.relativePath);
            }
        }
    }
    qsizetype count = 0;
    for (const QString &rel : toSkip) {
        if (approveDirectorySkip(plan, rel, reason)) {
            ++count;
        }
    }
    return count;
}

class LocalMergeExecutionJob final : public KIO::Job
{
    Q_OBJECT

public:
    using CopyJobFactory = std::function<KJob *(
        const QUrl &source,
        const QUrl &destination,
        QObject *parent)>;
    using PrePublishHook = std::function<void(const QString &relativePath, int destParentFd)>;

    explicit LocalMergeExecutionJob(
        LocalMergePlan plan,
        QObject *parent = nullptr,
        qsizetype chunkSize = 256 * 1024,
        int chunkDelayMilliseconds = 0)
        : m_plan(std::move(plan))
        , m_chunkSize(qMax<qsizetype>(4096, chunkSize))
        , m_chunkDelayMilliseconds(qMax(0, chunkDelayMilliseconds))
    {
        setParent(parent);
        setCapabilities(KJob::Killable);
    }

    ~LocalMergeExecutionJob() override
    {
        for (auto *anchored : findChildren<LocalAnchoredFileCopyJob *>()) {
            anchored->requestCancelAsync();
            anchored->waitForThreadExit();
        }
        m_createdDirsForFinalization.clear();
        m_srcDirFds.clear();
        m_destDirFds.clear();
        m_srcRoots.clear();
    }

    using FaultPhaseHook = std::function<void(
        LocalAnchoredFileCopyWorker::FaultPhase phase,
        const QString &relativePath,
        int destParentFd,
        const QString &partName,
        const QString &backupName)>;

    using PreDirPermFinalizeHook = std::function<void(const QString &relativePath, const QString &destinationPath, int dirFd)>;
    using PreTopLevelAnchorHook = std::function<void(const QString &destRootPath, const QString &relativePath)>;

    void setCopyJobFactory(CopyJobFactory factory) { m_copyJobFactory = std::move(factory); }
    void setPrePublishHook(PrePublishHook hook) { m_prePublishHook = std::move(hook); }
    void setFaultPhaseHook(FaultPhaseHook hook) { m_faultPhaseHook = std::move(hook); }
    void setPreDirPermFinalizeHook(PreDirPermFinalizeHook hook) { m_preDirPermFinalizeHook = std::move(hook); }
    void setPreTopLevelAnchorHook(PreTopLevelAnchorHook hook) { m_preTopLevelAnchorHook = std::move(hook); }
    void setSimulateDirChmodError(bool simulate) { m_simulateDirChmodError = simulate; }
    void setSimulateFsyncEio(bool simulate) { m_simulateFsyncEio = simulate; }
    void setSimulateOTmpfileUnsupported(bool simulate) { m_simulateOTmpfileUnsupported = simulate; }
    void setSimulateRenameExchangeUnsupported(bool simulate) { m_simulateRenameExchangeUnsupported = simulate; }
    void setSimulateUnsupportedOverwriteFs(bool simulate) { m_simulateUnsupportedOverwriteFs = simulate; }
    void setPreviouslyCompletedDirectories(QList<LocalMergeAnchoredDirectory> dirs)
    {
        m_preApprovedDirectories = std::move(dirs);
    }

    // Production cancellation path: cancels active copy worker and waits for worker thread
    // exit via copyJob's finished handler before emitting LocalMergeExecutionJob::result().
    void requestSafeCancel()
    {
        if (m_finished) return;
        m_cancelRequested = true;
        m_report.cancelled = true;
        if (KJob *active = m_activeCopyJob.data()) {
            if (auto *anchored = qobject_cast<LocalAnchoredFileCopyJob *>(active)) {
                if (!anchored->hasFinished()) {
                    anchored->requestCancelAsync();
                    return;
                }
            } else {
                active->kill(KJob::Quietly);
            }
            m_activeCopyJob = nullptr;
        }
        finalizeOutcome(
            KJob::KilledJobError,
            tr("Merge cancelled."),
            hasPublishedAnything()
                ? LocalMergeExecutionOutcome::CancelledPartialSuccess
                : LocalMergeExecutionOutcome::CancelledNoChanges);
    }

    void start() override
    {
        if (m_started) return;
        m_started = true;
        QTimer::singleShot(0, this, [this] { beginExecution(); });
    }

    const LocalMergeExecutionReport &report() const { return m_report; }

protected:
    bool doKill() override
    {
        if (m_finished) return false;
        m_cancelRequested = true;
        m_report.cancelled = true;
        if (KJob *active = m_activeCopyJob.data()) {
            active->kill(KJob::Quietly);
            m_activeCopyJob = nullptr;
        }
        finalizeOutcome(
            KJob::KilledJobError,
            tr("Merge cancelled."),
            hasPublishedAnything()
                ? LocalMergeExecutionOutcome::CancelledPartialSuccess
                : LocalMergeExecutionOutcome::CancelledNoChanges);
        return true;
    }

private:
    struct AnchoredRoot
    {
        QString rootPath;
        std::shared_ptr<LocalMergeExecDetail::UniqueFd> rootFd;
        struct stat rootStat {};
    };

    bool hasPublishedAnything() const
    {
        return !m_report.filesCreated.isEmpty()
            || !m_report.filesRenamed.isEmpty()
            || !m_report.filesOverwritten.isEmpty()
            || !m_report.symlinksCreated.isEmpty()
            || !m_report.directoriesCreated.isEmpty();
    }

    static QString parentRelativePath(const QString &relPath)
    {
        const qsizetype slash = relPath.lastIndexOf(QLatin1Char('/'));
        return slash > 0 ? relPath.left(slash) : QString();
    }

    static QString stripRelativeComponents(const QString &absFilePath, const QString &relativePath)
    {
        QString dir = QDir::cleanPath(QFileInfo(absFilePath).path());
        const qsizetype depth = relativePath.count(QLatin1Char('/'));
        for (qsizetype i = 0; i < depth; ++i) {
            dir = QDir::cleanPath(QFileInfo(dir).path());
        }
        return dir;
    }

    static QString effectiveSourceRootFor(const LocalMergePlanEntry &entry)
    {
        if (!entry.sourceRootPath.isEmpty()) {
            return QDir::cleanPath(entry.sourceRootPath);
        }
        return stripRelativeComponents(entry.sourcePath, entry.relativePath);
    }

    bool verifyBindingForEntry(const LocalMergePlanEntry &entry, int destParentFd) const
    {
        if (!entry.decisionBinding.bound) return true;
        if (entry.decisionBinding.expectedClassification != entry.classification) return false;
        if (entry.decisionBinding.expectedParentValid
            && !LocalMergeExecDetail::verifyFdIdentity(
                destParentFd,
                entry.decisionBinding.expectedParentStat,
                true)) {
            return false;
        }
        return true;
    }

    bool verifySourceChainStillAnchored(const QString &absDirPath, const QString &srcRootPath) const
    {
        const QString cleanRoot = QDir::cleanPath(srcRootPath);
        const QString cleanAbsDir = QDir::cleanPath(absDirPath);
        if (cleanRoot.isEmpty() || cleanAbsDir.isEmpty()) {
            return false;
        }
        auto rootIt = m_srcRoots.constFind(cleanRoot);
        if (rootIt == m_srcRoots.constEnd() || !rootIt->rootFd || !rootIt->rootFd->valid()) {
            return false;
        }
        struct stat curRoot {};
        if (::fstat(rootIt->rootFd->fd, &curRoot) != 0
            || curRoot.st_nlink == 0
            || curRoot.st_dev != rootIt->rootStat.st_dev
            || curRoot.st_ino != rootIt->rootStat.st_ino) {
            return false;
        }
        const QString relDir = QDir(cleanRoot).relativeFilePath(cleanAbsDir);
        if (relDir == QLatin1String(".")) {
            return true;
        }
        if (relDir.isEmpty() || relDir == QLatin1String("..") || relDir.startsWith(QLatin1String("../"))) {
            return false;
        }

        int curFd = rootIt->rootFd->fd;
        LocalMergeExecDetail::UniqueFd ownedStep;
        QString absPrefix = cleanRoot;
        const QStringList parts = relDir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            absPrefix = QDir::cleanPath(QDir(absPrefix).filePath(part));
            LocalMergeExecDetail::UniqueFd next(
                ::openat(curFd, QFile::encodeName(part).constData(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!next.valid()) return false;
            struct stat st {};
            if (::fstat(next.fd, &st) != 0 || st.st_nlink == 0) return false;
            auto it = m_srcDirFds.constFind(absPrefix);
            if (it != m_srcDirFds.constEnd() && it.value() && it.value()->valid()) {
                struct stat cachedSt {};
                if (::fstat(it.value()->fd, &cachedSt) != 0
                    || cachedSt.st_dev != st.st_dev
                    || cachedSt.st_ino != st.st_ino) {
                    return false;
                }
            }
            ownedStep = std::move(next);
            curFd = ownedStep.fd;
        }
        return true;
    }

    bool verifyDestRootStillAnchored() const
    {
        if (!m_destRootFd.valid()) return false;
        struct stat curRoot {};
        if (::fstat(m_destRootFd.fd, &curRoot) != 0
            || !S_ISDIR(curRoot.st_mode)
            || curRoot.st_nlink == 0
            || curRoot.st_dev != m_destRootStat.st_dev
            || curRoot.st_ino != m_destRootStat.st_ino) {
            return false;
        }
        // SEC-046-08: Verify m_destRootPath still resolves without symlinks to the exact
        // same physical directory (st_dev, st_ino). A rename+replace leaves m_destRootFd
        // open with st_nlink > 0, so pathname identity must also be revalidated.
        LocalMergeExecDetail::UniqueFd pathFd =
            LocalMergeExecDetail::openDirectoryNoSymlinks(m_destRootPath);
        if (!pathFd.valid()) return false;
        struct stat pathSt {};
        if (::fstat(pathFd.fd, &pathSt) != 0
            || !S_ISDIR(pathSt.st_mode)
            || pathSt.st_nlink == 0
            || pathSt.st_dev != m_destRootStat.st_dev
            || pathSt.st_ino != m_destRootStat.st_ino) {
            return false;
        }
        return true;
    }

    bool verifyDirectoryChainStillAnchored(const QString &relDir, bool sourceSide) const
    {
        if (sourceSide) {
            // Destination calls use sourceSide=false; source calls should use verifySourceChainStillAnchored().
            return false;
        }
        if (!verifyDestRootStillAnchored()) return false;
        if (relDir.isEmpty()) return true;

        int curFd = m_destRootFd.fd;
        LocalMergeExecDetail::UniqueFd ownedStep;
        QString prefix;
        const QStringList parts = relDir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            prefix = prefix.isEmpty() ? part : (prefix + QLatin1Char('/') + part);
            LocalMergeExecDetail::UniqueFd next(
                ::openat(curFd, QFile::encodeName(part).constData(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!next.valid()) return false;
            struct stat st {};
            if (::fstat(next.fd, &st) != 0 || st.st_nlink == 0) return false;
            auto it = m_destDirFds.constFind(prefix);
            if (it != m_destDirFds.constEnd() && it.value() && it.value()->valid()) {
                struct stat cachedSt {};
                if (::fstat(it.value()->fd, &cachedSt) != 0
                    || cachedSt.st_dev != st.st_dev
                    || cachedSt.st_ino != st.st_ino) {
                    return false;
                }
            }
            ownedStep = std::move(next);
            curFd = ownedStep.fd;
        }
        return true;
    }

    void populateUndoContract()
    {
        LocalMergeUndoContract &uc = m_report.undoContract;
        uc = LocalMergeUndoContract{};
        uc.reversibleCreatedFiles = m_report.filesCreated + m_report.symlinksCreated;
        uc.reversibleCreatedDirectories = m_report.directoriesCreated;
        uc.existingDirectoriesNeverDelete = m_report.existingDirectoriesUsed;
        uc.irreversibleOverwrittenFiles = m_report.filesOverwritten;
        uc.hasOverwrites = !m_report.filesOverwritten.isEmpty();

        for (const auto &rec : std::as_const(m_report.replacedFiles)) {
            if (rec.backupPreserved && !rec.recoveryPath.isEmpty()) {
                uc.preservedBackupPaths.append(rec.recoveryPath);
            }
        }
        uc.hasPreservedBackups = !uc.preservedBackupPaths.isEmpty();

        if (m_report.uncertainDurability) {
            uc.reversible = false;
            uc.requiresIrreversibleSerial = true;
            uc.blockReason = QStringLiteral("Merge ended with uncertain durability; automatic Undo is blocked.");
        } else if (uc.hasOverwrites) {
            uc.reversible = false;
            uc.requiresIrreversibleSerial = true;
            uc.blockReason = QStringLiteral("Merge overwrote existing files; requires irreversible serial unless backups are retained.");
        } else if (hasPublishedAnything()) {
            uc.reversible = !m_report.partialSuccess;
            uc.requiresIrreversibleSerial = m_report.partialSuccess;
            if (m_report.partialSuccess) {
                uc.blockReason = QStringLiteral("Partial merge failure; requires explicit recovery before Undo.");
            }
        } else {
            uc.reversible = false;
            uc.requiresIrreversibleSerial = false;
        }
    }

    void beginExecution()
    {
        if (m_finished || m_cancelRequested) return;

        if (m_plan.entries.isEmpty()) {
            finalizeFailure(KJob::UserDefinedError, tr("Merge plan is empty."));
            return;
        }
        if (m_plan.hasBlockingErrors) {
            finalizeFailure(KJob::UserDefinedError,
                            m_plan.blockingReason.isEmpty()
                                ? tr("Merge plan contains blocking errors.")
                                : m_plan.blockingReason);
            return;
        }

        m_destRootPath = !m_plan.destinationRootPath.isEmpty()
            ? QDir::cleanPath(m_plan.destinationRootPath)
            : stripRelativeComponents(m_plan.entries.first().destinationPath, m_plan.entries.first().relativePath);
        m_destRootFd = LocalMergeExecDetail::openDirectoryNoSymlinks(m_destRootPath);
        if (!m_destRootFd.valid()
            || ::fstat(m_destRootFd.fd, &m_destRootStat) != 0
            || !S_ISDIR(m_destRootStat.st_mode)
            || m_destRootStat.st_nlink == 0) {
            finalizeFailure(KJob::UserDefinedError, tr("Merge destination root is invalid or symlinked."));
            return;
        }
        // SEC-046-08: Reject if destination root identity changed between planning and execution.
        if (m_plan.destinationRootStatValid
            && (m_destRootStat.st_dev != m_plan.destinationRootStat.st_dev
                || m_destRootStat.st_ino != m_plan.destinationRootStat.st_ino)) {
            finalizeFailure(KJob::UserDefinedError,
                            tr("Merge destination root identity changed after planning."));
            return;
        }

        auto anchorSourceRoot = [&](const QString &rootPath) -> bool {
            const QString cleanRoot = QDir::cleanPath(rootPath);
            if (cleanRoot.isEmpty()) return false;
            if (m_srcRoots.contains(cleanRoot)) return true;
            auto fd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
                LocalMergeExecDetail::openDirectoryNoSymlinks(cleanRoot));
            struct stat st {};
            if (!fd->valid() || ::fstat(fd->fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
                return false;
            }
            AnchoredRoot ar;
            ar.rootPath = cleanRoot;
            ar.rootFd = std::move(fd);
            ar.rootStat = st;
            m_srcRoots.insert(cleanRoot, std::move(ar));
            return true;
        };

        for (const auto &entry : std::as_const(m_plan.entries)) {
            if (!anchorSourceRoot(effectiveSourceRootFor(entry))) {
                finalizeFailure(KJob::UserDefinedError, tr("Merge source root is invalid or symlinked."));
                return;
            }
        }
        for (const auto &preDir : std::as_const(m_preApprovedDirectories)) {
            const QString preSrcRoot = !preDir.sourceRootPath.isEmpty()
                ? QDir::cleanPath(preDir.sourceRootPath)
                : stripRelativeComponents(preDir.sourcePath, preDir.relativePath);
            if (!anchorSourceRoot(preSrcRoot)) {
                finalizeFailure(KJob::UserDefinedError, tr("Merge source root is invalid or symlinked."));
                return;
            }
        }

        // Re-anchor and revalidate directories already created/merged/renamed in earlier rounds (shallowest first)
        QList<LocalMergeAnchoredDirectory> sortedPreDirs = m_preApprovedDirectories;
        std::sort(sortedPreDirs.begin(), sortedPreDirs.end(),
                  [](const LocalMergeAnchoredDirectory &a, const LocalMergeAnchoredDirectory &b) {
                      const qsizetype da = a.relativePath.count(QLatin1Char('/'));
                      const qsizetype db = b.relativePath.count(QLatin1Char('/'));
                      return (da != db) ? (da < db) : (a.relativePath < b.relativePath);
                  });
        for (const auto &preDir : std::as_const(sortedPreDirs)) {
            const QString cleanSrcPath = QDir::cleanPath(preDir.sourcePath);
            const QString cleanDstPath = QDir::cleanPath(preDir.destinationPath);
            const QString preSrcRoot = !preDir.sourceRootPath.isEmpty()
                ? QDir::cleanPath(preDir.sourceRootPath)
                : stripRelativeComponents(cleanSrcPath, preDir.relativePath);
            if (!verifySourceChainStillAnchored(cleanSrcPath, preSrcRoot)
                || !verifyDirectoryChainStillAnchored(preDir.relativePath, false)) {
                finalizeFailure(KJob::UserDefinedError,
                                tr("Previously approved directory became invalid or symlinked: %1").arg(preDir.relativePath));
                return;
            }
            auto srcFd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
                LocalMergeExecDetail::openDirectoryNoSymlinks(cleanSrcPath));
            auto dstFd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
                LocalMergeExecDetail::openDirectoryNoSymlinks(cleanDstPath));
            if (!srcFd->valid()
                || !LocalMergeExecDetail::verifyFdIdentity(srcFd->fd, preDir.expectedSourceStat, preDir.expectedSourceValid)
                || !dstFd->valid()
                || !LocalMergeExecDetail::verifyFdIdentity(dstFd->fd, preDir.expectedDestStat, preDir.expectedDestValid)) {
                finalizeFailure(KJob::UserDefinedError,
                                tr("Previously approved directory identity changed during replan: %1").arg(preDir.relativePath));
                return;
            }
            m_srcDirFds.insert(cleanSrcPath, std::move(srcFd));
            m_destDirFds.insert(preDir.relativePath, std::move(dstFd));
            m_approvedDirectories.insert(preDir.relativePath);
        }

        bool anyApproved = false;
        qulonglong approvedBytes = 0;
        for (const auto &entry : std::as_const(m_plan.entries)) {
            if (entry.approvedAction == LocalMergeApprovedAction::Overwrite) {
                if (entry.classification != LocalMergeEntryClassification::FileConflict
                    || entry.sourceKind != LocalMergeObjectKind::RegularFile
                    || entry.destinationKind != LocalMergeObjectKind::RegularFile
                    || !entry.decisionBinding.bound
                    || !entry.decisionBinding.expectedDestinationValid
                    || !entry.decisionBinding.expectedParentValid) {
                    finalizeFailure(KJob::UserDefinedError,
                                    tr("Overwrite requires an explicitly bound regular-file conflict decision: %1")
                                        .arg(entry.relativePath));
                    return;
                }
                anyApproved = true;
                approvedBytes += entry.size;
                continue;
            }
            if (entry.approvedAction == LocalMergeApprovedAction::Create
                || entry.approvedAction == LocalMergeApprovedAction::MergeDirectory
                || entry.approvedAction == LocalMergeApprovedAction::Rename
                || entry.approvedAction == LocalMergeApprovedAction::Skip) {
                anyApproved = true;
                if (entry.approvedAction != LocalMergeApprovedAction::Skip
                    && entry.sourceKind == LocalMergeObjectKind::RegularFile) {
                    approvedBytes += entry.size;
                }
            }
        }
        if (!anyApproved) {
            finalizeFailure(KJob::UserDefinedError,
                            tr("Merge plan has no explicitly approved actions."));
            return;
        }

        setTotalAmount(KJob::Bytes, approvedBytes);
        scheduleNext();
    }

    void scheduleNext()
    {
        if (!m_finished && !m_cancelRequested) {
            QTimer::singleShot(0, this, [this] { processNext(); });
        }
    }

    void processNext()
    {
        if (m_finished || m_cancelRequested || m_activeCopyJob) return;

        if (m_index >= m_plan.entries.size()) {
            if (!finalizeCreatedDirectoryPermissions()) {
                return;
            }
            m_finished = true;
            m_report.completedFully = true;
            m_report.partialSuccess = false;
            m_report.retryAllowed = false;
            m_report.outcome = m_report.skippedEntries.isEmpty()
                ? LocalMergeExecutionOutcome::Succeeded
                : LocalMergeExecutionOutcome::SucceededWithSkippedConflicts;
            populateUndoContract();
            setPercent(100);
            emitResult();
            return;
        }

        const LocalMergePlanEntry entry = m_plan.entries.at(m_index++);
        const QString parentRel = parentRelativePath(entry.relativePath);
        if (!parentRel.isEmpty() && !m_approvedDirectories.contains(parentRel)) {
            bool hasSkippedAncestor = false;
            for (const QString &sk : std::as_const(m_skippedDirectories)) {
                if (parentRel == sk || parentRel.startsWith(sk + QLatin1Char('/'))) {
                    hasSkippedAncestor = true;
                    break;
                }
            }
            if (hasSkippedAncestor || entry.approvedAction == LocalMergeApprovedAction::Skip) {
                m_report.skippedSubtreeEntries.append(entry.relativePath);
                ++m_report.skippedSubtreeCount;
            } else {
                m_report.objectsBlocked.append(entry.relativePath);
            }
            scheduleNext();
            return;
        }

        if (entry.approvedAction == LocalMergeApprovedAction::Abort) {
            finalizeFailure(KJob::UserDefinedError, tr("Merge aborted by plan decision."));
            return;
        }

        // Handle FileConflict with explicit Skip, Rename, or Overwrite
        if (entry.classification == LocalMergeEntryClassification::FileConflict) {
            if (entry.approvedAction == LocalMergeApprovedAction::Skip) {
                handleSkipConflict(entry);
                return;
            }
            if (entry.approvedAction == LocalMergeApprovedAction::Rename) {
                handleRenameConflict(entry);
                return;
            }
            if (entry.approvedAction == LocalMergeApprovedAction::Overwrite) {
                handleOverwriteConflict(entry);
                return;
            }
            m_report.objectsBlocked.append(entry.relativePath);
            scheduleNext();
            return;
        }

        if (entry.approvedAction == LocalMergeApprovedAction::Overwrite) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Overwrite is refused for non-FileConflict entry: %1").arg(entry.relativePath));
            return;
        }

        if (entry.classification == LocalMergeEntryClassification::SameObject) {
            if (entry.approvedAction == LocalMergeApprovedAction::Rename) {
                m_report.failedOperations.append(entry.relativePath);
                finalizeFailure(KJob::UserDefinedError,
                                tr("Refusing unsafe Rename on SameObject directory: %1").arg(entry.relativePath));
                return;
            }
            if (entry.approvedAction == LocalMergeApprovedAction::Skip) {
                if (entry.sourceKind == LocalMergeObjectKind::Directory) {
                    handleSkipDirectory(entry);
                } else {
                    handleSkipConflict(entry);
                }
                return;
            }
            m_report.objectsBlocked.append(entry.relativePath);
            scheduleNext();
            return;
        }

        if (entry.classification != LocalMergeEntryClassification::CreateNew
            && entry.classification != LocalMergeEntryClassification::MergeDirectoryCandidate) {
            if (entry.classification == LocalMergeEntryClassification::BrokenSymlink
                && entry.sourceKind == LocalMergeObjectKind::SymbolicLink
                && entry.destinationKind == LocalMergeObjectKind::Missing
                && entry.approvedAction == LocalMergeApprovedAction::Create) {
                handleCreateSymlink(entry);
                return;
            }
            if (entry.classification == LocalMergeEntryClassification::CaseInsensitiveCollision
                && entry.sourceKind == LocalMergeObjectKind::RegularFile
                && entry.approvedAction == LocalMergeApprovedAction::Rename) {
                handleRenameConflict(entry);
                return;
            }
            if (entry.approvedAction == LocalMergeApprovedAction::Skip) {
                if (entry.sourceKind == LocalMergeObjectKind::Directory) {
                    handleSkipDirectory(entry);
                } else {
                    handleSkipConflict(entry);
                }
                return;
            }
            m_report.objectsBlocked.append(entry.relativePath);
            scheduleNext();
            return;
        }

        if (entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate) {
            if (entry.approvedAction == LocalMergeApprovedAction::Skip) {
                handleSkipDirectory(entry);
                return;
            }
            if (entry.approvedAction == LocalMergeApprovedAction::Rename) {
                handleRenameDirectory(entry);
                return;
            }
            if (entry.approvedAction != LocalMergeApprovedAction::MergeDirectory) {
                m_report.objectsBlocked.append(entry.relativePath);
                scheduleNext();
                return;
            }
            handleMergeDirectory(entry);
            return;
        }

        if (entry.approvedAction == LocalMergeApprovedAction::Skip) {
            handleSkipConflict(entry);
            return;
        }

        if (entry.approvedAction != LocalMergeApprovedAction::Create) {
            m_report.objectsBlocked.append(entry.relativePath);
            scheduleNext();
            return;
        }
        if (entry.sourceKind == LocalMergeObjectKind::SymbolicLink) {
            handleCreateSymlink(entry);
            return;
        }
        if (entry.sourceKind == LocalMergeObjectKind::Directory) {
            handleCreateDirectory(entry);
            return;
        }
        if (entry.sourceKind == LocalMergeObjectKind::RegularFile) {
            handleCreateOrRenameFile(entry, QFileInfo(entry.destinationPath).fileName(), false);
            return;
        }

        m_report.objectsBlocked.append(entry.relativePath);
        scheduleNext();
    }

    int ensureAnchoredParentFd(
        const QString &relPath,
        const QString &absPath,
        bool sourceSide,
        const QString &srcRootPath = QString())
    {
        const QString parentAbs = QDir::cleanPath(QFileInfo(absPath).path());
        if (sourceSide) {
            if (!verifySourceChainStillAnchored(parentAbs, srcRootPath)) {
                return -1;
            }
            auto it = m_srcDirFds.constFind(parentAbs);
            if (it != m_srcDirFds.constEnd() && it.value() && it.value()->valid()) {
                return it.value()->fd;
            }
            auto fd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
                LocalMergeExecDetail::openDirectoryNoSymlinks(parentAbs));
            if (!fd->valid()) return -1;
            m_srcDirFds.insert(parentAbs, fd);
            return fd->fd;
        }

        const QString parentRel = parentRelativePath(relPath);
        if (parentRel.isEmpty() && m_preTopLevelAnchorHook) {
            m_preTopLevelAnchorHook(m_destRootPath, relPath);
        }
        if (m_cancelRequested || m_finished) {
            return -1;
        }
        if (!verifyDirectoryChainStillAnchored(parentRel, false)) {
            return -1;
        }
        if (parentRel.isEmpty()) {
            // SEC-046-08: Never reopen the destination root by pathname without identity
            // verification. Return the already-anchored m_destRootFd directly after
            // verifyDirectoryChainStillAnchored("") has verified both FD and path identity.
            return m_destRootFd.valid() ? m_destRootFd.fd : -1;
        }
        auto it = m_destDirFds.constFind(parentRel);
        if (it != m_destDirFds.constEnd() && it.value() && it.value()->valid()) {
            return it.value()->fd;
        }
        // Walk nested parent from the anchored m_destRootFd using O_NOFOLLOW instead of
        // reopening parentAbs from "/" by pathname.
        int curFd = m_destRootFd.fd;
        LocalMergeExecDetail::UniqueFd ownedStep;
        const QStringList parts = parentRel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            LocalMergeExecDetail::UniqueFd next(
                ::openat(curFd, QFile::encodeName(part).constData(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!next.valid()) return -1;
            ownedStep = std::move(next);
            curFd = ownedStep.fd;
        }
        if (!ownedStep.valid()) return -1;
        auto fd = std::make_shared<LocalMergeExecDetail::UniqueFd>(std::move(ownedStep));
        m_destDirFds.insert(parentRel, fd);
        return fd->fd;
    }

    void handleCreateSymlink(const LocalMergePlanEntry &entry)
    {
        if (entry.symbolicLinkContents.isEmpty() || !entry.sourceStatValid || !S_ISLNK(entry.sourceStat.st_mode)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KIO::ERR_CANNOT_SYMLINK,
                            tr("Symbolic link entry lacks valid link metadata: %1").arg(entry.relativePath));
            return;
        }
        const QString srcRoot = effectiveSourceRootFor(entry);
        const int srcParentFd = ensureAnchoredParentFd(entry.relativePath, entry.sourcePath, true, srcRoot);
        const int dstParentFd = ensureAnchoredParentFd(entry.relativePath, entry.destinationPath, false);
        if (srcParentFd < 0 || dstParentFd < 0) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Parent directory invalid or symlinked."));
            return;
        }

        const QByteArray srcEnc = QFile::encodeName(QFileInfo(entry.sourcePath).fileName());
        struct stat curSrc {};
        if (::fstatat(srcParentFd, srcEnc.constData(), &curSrc, AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISLNK(curSrc.st_mode)
            || curSrc.st_dev != entry.sourceStat.st_dev
            || curSrc.st_ino != entry.sourceStat.st_ino) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Source symbolic link identity changed after planning: %1").arg(entry.relativePath));
            return;
        }

        QByteArray currentTarget(4096, Qt::Uninitialized);
        ssize_t linkLen = ::readlinkat(srcParentFd, srcEnc.constData(), currentTarget.data(), currentTarget.size());
        if (linkLen < 0 || linkLen >= currentTarget.size()) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KIO::ERR_CANNOT_SYMLINK,
                            tr("Could not read symbolic link target atomically: %1").arg(entry.relativePath));
            return;
        }
        currentTarget.resize(linkLen);
        if (currentTarget != entry.symbolicLinkContents) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Source symbolic link target changed after planning: %1").arg(entry.relativePath));
            return;
        }

        const QString dstName = QFileInfo(entry.destinationPath).fileName();
        const QByteArray dstEnc = QFile::encodeName(dstName);
        struct stat chk {};
        if (::fstatat(dstParentFd, dstEnc.constData(), &chk, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
            m_report.entriesNeedingReplan.append(entry.relativePath);
            finalizeOutcome(KIO::ERR_FILE_ALREADY_EXIST,
                            tr("Symbolic link destination already exists; replan required: %1").arg(entry.relativePath),
                            LocalMergeExecutionOutcome::NeedsReplan);
            return;
        }
        QString colliding;
        if (LocalMergeExecDetail::directoryHasCaseCollision(dstParentFd, dstName, &colliding)) {
            m_report.entriesNeedingReplan.append(entry.relativePath);
            finalizeOutcome(KIO::ERR_FILE_ALREADY_EXIST,
                            tr("Symbolic link destination has a differing-case collision; replan required: %1").arg(colliding),
                            LocalMergeExecutionOutcome::NeedsReplan);
            return;
        }

        if (m_prePublishHook) {
            m_prePublishHook(entry.relativePath, dstParentFd);
        }
        if (m_cancelRequested) {
            finalizeOutcome(KJob::KilledJobError,
                            tr("Merge cancelled."),
                            hasPublishedAnything()
                                ? LocalMergeExecutionOutcome::CancelledPartialSuccess
                                : LocalMergeExecutionOutcome::CancelledNoChanges);
            return;
        }
        const QString srcParentAbs = QDir::cleanPath(QFileInfo(entry.sourcePath).path());
        const QString parentRel = parentRelativePath(entry.relativePath);
        if (!verifySourceChainStillAnchored(srcParentAbs, srcRoot)
            || !verifyDirectoryChainStillAnchored(parentRel, false)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KIO::ERR_DOES_NOT_EXIST,
                            tr("Directory tree moved or replaced before symbolic link publication."));
            return;
        }

        if (::symlinkat(entry.symbolicLinkContents.constData(), dstParentFd, dstEnc.constData()) != 0) {
            const int serr = errno;
            if (serr == EEXIST) {
                m_report.entriesNeedingReplan.append(entry.relativePath);
                finalizeOutcome(KIO::ERR_FILE_ALREADY_EXIST,
                                tr("Symbolic link collided concurrently; replan required: %1").arg(entry.relativePath),
                                LocalMergeExecutionOutcome::NeedsReplan);
                return;
            }
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KIO::ERR_CANNOT_SYMLINK,
                            tr("Could not create symbolic link: %1").arg(entry.destinationPath));
            return;
        }

        const struct timespec times[2]{entry.sourceStat.st_atim, entry.sourceStat.st_mtim};
        ::utimensat(dstParentFd, dstEnc.constData(), times, AT_SYMLINK_NOFOLLOW);
        if (m_simulateFsyncEio || ::fsync(dstParentFd) != 0) {
            m_report.uncertainDurability = true;
            m_report.symlinksCreated.append(entry.relativePath);
            m_report.lastPublishedEntry = entry.relativePath;
            m_report.failedOperations.append(entry.relativePath);
            finalizeOutcome(KIO::ERR_CANNOT_WRITE,
                            tr("Symbolic link created, but destination directory fsync failed (uncertain durability)."),
                            LocalMergeExecutionOutcome::FailedUncertainDurability);
            return;
        }

        m_report.symlinksCreated.append(entry.relativePath);
        m_report.lastPublishedEntry = entry.relativePath;
        scheduleNext();
    }

    void handleSkipDirectory(const LocalMergePlanEntry &entry)
    {
        m_skippedDirectories.insert(entry.relativePath);
        LocalMergeSkippedEntry skipped;
        skipped.relativePath = entry.relativePath;
        skipped.reason = entry.decisionBinding.reason.isEmpty()
            ? QStringLiteral("Directory skipped")
            : entry.decisionBinding.reason;
        skipped.classification = entry.classification;
        skipped.sourceStat = entry.sourceStat;
        skipped.sourceStatValid = entry.sourceStatValid;
        skipped.destinationStat = entry.destinationStat;
        skipped.destinationStatValid = entry.destinationStatValid;
        m_report.skippedEntries.append(skipped);
        m_report.conflictsSkipped.append(entry.relativePath);
        ++m_report.skippedSubtreeCount;
        scheduleNext();
    }

    void handleRenameDirectory(const LocalMergePlanEntry &entry)
    {
        if (!entry.decisionBinding.bound
            || !LocalMergeExecDetail::isSafeSingleFileNameComponent(entry.decisionBinding.targetFileName)
            || QFile::encodeName(entry.decisionBinding.targetFileName).size() > 255
            || QFile::encodeName(entry.destinationPath).size() >= 4096) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Directory rename decision lacks a valid bound target name."));
            return;
        }
        const int srcParentFd = ensureAnchoredParentFd(entry.relativePath, entry.sourcePath, true, effectiveSourceRootFor(entry));
        const int dstParentFd = ensureAnchoredParentFd(entry.relativePath, entry.destinationPath, false);
        if (srcParentFd < 0 || dstParentFd < 0 || !verifyBindingForEntry(entry, dstParentFd)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Parent directory identity changed or became symlink."));
            return;
        }

        const QByteArray srcNameEnc = QFile::encodeName(QFileInfo(entry.sourcePath).fileName());
        auto srcFd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
            ::openat(srcParentFd, srcNameEnc.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!srcFd->valid() || !LocalMergeExecDetail::verifyFdIdentity(srcFd->fd, entry.sourceStat, entry.sourceStatValid)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Source directory identity changed before rename: %1").arg(entry.sourcePath));
            return;
        }

        const QString &targetFolderName = entry.decisionBinding.targetFileName;
        const QByteArray targetEnc = QFile::encodeName(targetFolderName);
        struct stat chk {};
        if (::fstatat(dstParentFd, targetEnc.constData(), &chk, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
            m_report.entriesNeedingReplan.append(entry.relativePath);
            finalizeOutcome(KIO::ERR_FILE_ALREADY_EXIST,
                            tr("Renamed target folder now exists or is a symlink; replan required: %1").arg(entry.relativePath),
                            LocalMergeExecutionOutcome::NeedsReplan);
            return;
        }
        QString colliding;
        if (LocalMergeExecDetail::directoryHasCaseCollision(dstParentFd, targetFolderName, &colliding)) {
            m_report.entriesNeedingReplan.append(entry.relativePath);
            finalizeOutcome(KIO::ERR_FILE_ALREADY_EXIST,
                            tr("Renamed target folder now has a case collision with %1; replan required.").arg(colliding),
                            LocalMergeExecutionOutcome::NeedsReplan);
            return;
        }

        if (::mkdirat(dstParentFd, targetEnc.constData(), LocalMergeExecDetail::initialDirectoryCreationMode()) != 0) {
            const int err = errno;
            if (err == EEXIST) {
                m_report.entriesNeedingReplan.append(entry.relativePath);
                finalizeOutcome(KIO::ERR_FILE_ALREADY_EXIST,
                                tr("Renamed target folder collided concurrently; replan required."),
                                LocalMergeExecutionOutcome::NeedsReplan);
                return;
            }
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure((err == ENOSPC || err == EDQUOT) ? KIO::ERR_DISK_FULL : KIO::ERR_CANNOT_MKDIR,
                            tr("Could not create renamed destination directory: %1").arg(entry.destinationPath));
            return;
        }

        auto dstFd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
            ::openat(dstParentFd, targetEnc.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat createdDstSt {};
        if (!dstFd->valid() || ::fstat(dstFd->fd, &createdDstSt) != 0 || !S_ISDIR(createdDstSt.st_mode)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Newly created renamed directory could not be anchored: %1").arg(entry.destinationPath));
            return;
        }

        struct stat verifiedSrcSt {};
        if (::fstat(srcFd->fd, &verifiedSrcSt) == 0 && S_ISDIR(verifiedSrcSt.st_mode)) {
            recordCreatedDirectoryForFinalization(
                entry.relativePath,
                entry.destinationPath,
                targetFolderName,
                dstFd,
                createdDstSt,
                verifiedSrcSt);
        }

        m_srcDirFds.insert(QDir::cleanPath(entry.sourcePath), std::move(srcFd));
        m_destDirFds.insert(entry.relativePath, std::move(dstFd));
        m_approvedDirectories.insert(entry.relativePath);
        m_report.directoriesCreated.append(entry.relativePath);
        m_report.lastPublishedEntry = entry.relativePath;
        scheduleNext();
    }

    void handleSkipConflict(const LocalMergePlanEntry &entry)
    {
        const int srcParentFd = ensureAnchoredParentFd(entry.relativePath, entry.sourcePath, true, effectiveSourceRootFor(entry));
        const int dstParentFd = ensureAnchoredParentFd(entry.relativePath, entry.destinationPath, false);
        if (srcParentFd < 0 || dstParentFd < 0) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Parent directory invalid during skip verification."));
            return;
        }
        if (entry.decisionBinding.bound) {
            struct stat curSrc {};
            struct stat curDst {};
            const bool srcOk = !entry.decisionBinding.expectedSourceValid
                || (::fstatat(srcParentFd, QFile::encodeName(QFileInfo(entry.sourcePath).fileName()).constData(),
                              &curSrc, AT_SYMLINK_NOFOLLOW) == 0
                    && curSrc.st_dev == entry.decisionBinding.expectedSourceStat.st_dev
                    && curSrc.st_ino == entry.decisionBinding.expectedSourceStat.st_ino);
            const bool dstOk = !entry.decisionBinding.expectedDestinationValid
                || (::fstatat(dstParentFd, QFile::encodeName(QFileInfo(entry.destinationPath).fileName()).constData(),
                              &curDst, AT_SYMLINK_NOFOLLOW) == 0
                    && curDst.st_dev == entry.decisionBinding.expectedDestinationStat.st_dev
                    && curDst.st_ino == entry.decisionBinding.expectedDestinationStat.st_ino);
            if (!srcOk || !dstOk) {
                m_report.failedOperations.append(entry.relativePath);
                finalizeFailure(KJob::UserDefinedError,
                                tr("Source or destination identity changed after Skip decision: %1").arg(entry.relativePath));
                return;
            }
        }

        LocalMergeSkippedEntry skipped;
        skipped.relativePath = entry.relativePath;
        skipped.reason = entry.decisionBinding.reason.isEmpty()
            ? QStringLiteral("Conflict skipped")
            : entry.decisionBinding.reason;
        skipped.classification = entry.classification;
        skipped.sourceStat = entry.sourceStat;
        skipped.sourceStatValid = entry.sourceStatValid;
        skipped.destinationStat = entry.destinationStat;
        skipped.destinationStatValid = entry.destinationStatValid;
        m_report.skippedEntries.append(skipped);
        m_report.conflictsSkipped.append(entry.relativePath);
        scheduleNext();
    }

    void handleRenameConflict(const LocalMergePlanEntry &entry)
    {
        if (!entry.decisionBinding.bound
            || !LocalMergeExecDetail::isSafeSingleFileNameComponent(entry.decisionBinding.targetFileName)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Rename decision lacks a valid bound target filename."));
            return;
        }
        const int dstParentFd = ensureAnchoredParentFd(entry.relativePath, entry.destinationPath, false);
        if (dstParentFd < 0 || !verifyBindingForEntry(entry, dstParentFd)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Destination parent identity changed after Rename decision."));
            return;
        }
        if (entry.decisionBinding.expectedDestinationValid) {
            struct stat curDst {};
            if (::fstatat(dstParentFd, QFile::encodeName(QFileInfo(entry.destinationPath).fileName()).constData(),
                          &curDst, AT_SYMLINK_NOFOLLOW) != 0
                || curDst.st_dev != entry.decisionBinding.expectedDestinationStat.st_dev
                || curDst.st_ino != entry.decisionBinding.expectedDestinationStat.st_ino) {
                m_report.failedOperations.append(entry.relativePath);
                finalizeFailure(KJob::UserDefinedError, tr("Conflicting destination changed after Rename decision."));
                return;
            }
        }
        QString colliding;
        if (LocalMergeExecDetail::directoryHasCaseCollision(dstParentFd, entry.decisionBinding.targetFileName, &colliding)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KIO::ERR_FILE_ALREADY_EXIST,
                            tr("Renamed target filename now collides with: %1").arg(colliding));
            return;
        }

        handleCreateOrRenameFile(entry, entry.decisionBinding.targetFileName, true);
    }

    void handleOverwriteConflict(const LocalMergePlanEntry &entry)
    {
        if (!entry.decisionBinding.bound || !entry.decisionBinding.expectedDestinationValid) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Overwrite decision lacks bound target identity."));
            return;
        }
        const int srcParentFd = ensureAnchoredParentFd(entry.relativePath, entry.sourcePath, true, effectiveSourceRootFor(entry));
        const int dstParentFd = ensureAnchoredParentFd(entry.relativePath, entry.destinationPath, false);
        if (srcParentFd < 0 || dstParentFd < 0 || !verifyBindingForEntry(entry, dstParentFd)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Parent directory identity changed or moved outside approved tree."));
            return;
        }

        const QString srcFileName = QFileInfo(entry.sourcePath).fileName();
        const QString dstFileName = QFileInfo(entry.destinationPath).fileName();
        const QString partName = QStringLiteral(".thispc-part.%1.%2.%3")
                                     .arg(dstFileName)
                                     .arg(static_cast<qulonglong>(::getpid()))
                                     .arg(LocalMergeExecDetail::nextStagingSequence());

        const int srcFdDup = ::dup(srcParentFd);
        const int dstFdDup = ::dup(dstParentFd);
        if (srcFdDup < 0 || dstFdDup < 0) {
            if (srcFdDup >= 0) ::close(srcFdDup);
            if (dstFdDup >= 0) ::close(dstFdDup);
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Could not duplicate directory descriptors."));
            return;
        }

        const QString srcParentAbs = QDir::cleanPath(QFileInfo(entry.sourcePath).path());
        const QString srcRoot = effectiveSourceRootFor(entry);
        const QString parentRel = parentRelativePath(entry.relativePath);
        LocalAnchoredFileCopyWorker::FaultConfig faults;
        faults.simulateFsyncEio = m_simulateFsyncEio;
        faults.simulateOTmpfileUnsupported = m_simulateOTmpfileUnsupported;
        faults.simulateRenameExchangeUnsupported = m_simulateRenameExchangeUnsupported;
        faults.simulateUnsupportedFs = m_simulateUnsupportedOverwriteFs;
        faults.prePublishHook = m_prePublishHook;
        faults.phaseHook = m_faultPhaseHook;
        faults.verifyChainHook = [this, srcParentAbs, srcRoot, parentRel]() {
            return verifySourceChainStillAnchored(srcParentAbs, srcRoot)
                && verifyDirectoryChainStillAnchored(parentRel, false);
        };

        auto *copyJob = new LocalAnchoredFileCopyJob(
            LocalAnchoredFileCopyWorker::Mode::OverwriteExisting,
            srcFdDup, srcFileName,
            entry.decisionBinding.expectedSourceValid ? entry.decisionBinding.expectedSourceStat : entry.sourceStat,
            dstFdDup, partName, dstFileName,
            entry.decisionBinding.expectedDestinationStat,
            entry.relativePath,
            this, m_chunkSize, m_chunkDelayMilliseconds, std::move(faults));

        m_activeCopyJob = copyJob;
        copyJob->setAutoDelete(false);
        connect(copyJob, &KJob::processedAmountChanged, this,
                [this](KJob *, KJob::Unit unit, qulonglong amount) {
                    if (!m_finished && unit == KJob::Bytes) {
                        setProcessedAmount(KJob::Bytes, m_report.bytesProcessed + amount);
                    }
                });
        connect(copyJob, &KJob::result, this, [this, entry](KJob *completed) {
            m_activeCopyJob = nullptr;
            auto *anchored = qobject_cast<LocalAnchoredFileCopyJob *>(completed);
            const bool published = anchored && anchored->destinationPublished();
            const bool durable = anchored && anchored->directoryDurabilityConfirmed();
            const bool backupPreserved = anchored && anchored->backupPreserved();
            const bool concurrentMod = anchored && anchored->concurrentModificationDetected();
            const bool manualIntervention = anchored && anchored->manualInterventionRequired();
            const QString backupRel = anchored ? anchored->displacedBackupName() : QString();
            const QString txId = anchored ? anchored->transactionId() : QString();
            const QString txPhase = anchored ? anchored->transactionPhase() : QString();
            struct stat emptySt {};
            const struct stat prevSt = anchored ? anchored->previousDestStat() : emptySt;
            const struct stat pubSt = anchored ? anchored->publishedDestStat() : emptySt;
            const QByteArray sha = anchored ? anchored->checksum() : QByteArray();
            completed->deleteLater();

            if (concurrentMod) {
                m_report.concurrentModificationDetected = true;
            }
            if (manualIntervention) {
                m_report.manualInterventionRequired = true;
            }
            if (published) {
                LocalMergeReplacedFileRecord rec;
                rec.relativePath = entry.relativePath;
                rec.previousStat = prevSt;
                rec.previousStatValid = true;
                rec.newStat = pubSt;
                rec.newStatValid = true;
                rec.newSha256 = sha;
                rec.durabilityConfirmed = durable;
                rec.backupPreserved = backupPreserved;
                rec.backupFileName = backupRel.isEmpty() ? QString() : QFileInfo(backupRel).fileName();
                if (backupPreserved && !backupRel.isEmpty()) {
                    rec.recoveryPath = QDir(QFileInfo(entry.destinationPath).path()).filePath(backupRel);
                }
                rec.transactionId = txId;
                rec.transactionPhase = txPhase;
                rec.previousFileState = backupPreserved
                    ? QStringLiteral("SecuredBackupPreserved")
                    : QStringLiteral("CleanedAfterDurableCommit");
                rec.newFileState = durable
                    ? QStringLiteral("PublishedDurable")
                    : QStringLiteral("PublishedUncertainDurability");
                rec.manualInterventionRequired = manualIntervention;
                rec.note = durable
                    ? QStringLiteral("Replaced atomically with durable directory fsync.")
                    : QStringLiteral("Published with uncertain durability; displaced previous file preserved for recovery.");
                m_report.replacedFiles.append(rec);
            }

            if (m_finished) return;
            if (completed->error() != KJob::NoError && published && !durable) {
                m_report.failedOperations.append(entry.relativePath);
                m_report.uncertainDurability = true;
                m_report.filesOverwritten.append(entry.relativePath);
                m_report.lastPublishedEntry = entry.relativePath;
                finalizeOutcome(completed->error(), completed->errorText(),
                                LocalMergeExecutionOutcome::FailedUncertainDurability);
                return;
            }
            if (m_cancelRequested) {
                if (published) {
                    m_report.filesOverwritten.append(entry.relativePath);
                    m_report.lastPublishedEntry = entry.relativePath;
                }
                finalizeOutcome(
                    KJob::KilledJobError,
                    tr("Merge cancelled."),
                    hasPublishedAnything()
                        ? LocalMergeExecutionOutcome::CancelledPartialSuccess
                        : LocalMergeExecutionOutcome::CancelledNoChanges);
                return;
            }
            if (completed->error() != KJob::NoError) {
                m_report.failedOperations.append(entry.relativePath);
                finalizeFailure(completed->error(), completed->errorText());
                return;
            }
            m_report.filesOverwritten.append(entry.relativePath);
            m_report.bytesProcessed += entry.size;
            m_report.lastPublishedEntry = entry.relativePath;
            setProcessedAmount(KJob::Bytes, m_report.bytesProcessed);
            scheduleNext();
        });
        copyJob->start();
    }

    void handleMergeDirectory(const LocalMergePlanEntry &entry)
    {
        const int srcParentFd = ensureAnchoredParentFd(entry.relativePath, entry.sourcePath, true, effectiveSourceRootFor(entry));
        const int dstParentFd = ensureAnchoredParentFd(entry.relativePath, entry.destinationPath, false);
        if (srcParentFd < 0 || dstParentFd < 0) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Parent directory invalid or symlinked."));
            return;
        }

        auto srcFd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
            ::openat(srcParentFd, QFile::encodeName(QFileInfo(entry.sourcePath).fileName()).constData(),
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        auto dstFd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
            ::openat(dstParentFd, QFile::encodeName(QFileInfo(entry.destinationPath).fileName()).constData(),
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));

        if (!srcFd->valid() || !LocalMergeExecDetail::verifyFdIdentity(srcFd->fd, entry.sourceStat, entry.sourceStatValid)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Source directory identity changed after planning: %1").arg(entry.sourcePath));
            return;
        }
        if (!dstFd->valid() || !LocalMergeExecDetail::verifyFdIdentity(dstFd->fd, entry.destinationStat, entry.destinationStatValid)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Destination directory identity changed or became symlink: %1").arg(entry.destinationPath));
            return;
        }

        m_srcDirFds.insert(QDir::cleanPath(entry.sourcePath), std::move(srcFd));
        m_destDirFds.insert(entry.relativePath, std::move(dstFd));
        m_approvedDirectories.insert(entry.relativePath);
        m_report.existingDirectoriesUsed.append(entry.relativePath);
        scheduleNext();
    }

    void handleCreateDirectory(const LocalMergePlanEntry &entry)
    {
        const int srcParentFd = ensureAnchoredParentFd(entry.relativePath, entry.sourcePath, true, effectiveSourceRootFor(entry));
        const int dstParentFd = ensureAnchoredParentFd(entry.relativePath, entry.destinationPath, false);
        if (srcParentFd < 0 || dstParentFd < 0) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Parent directory invalid or symlinked."));
            return;
        }

        const QByteArray srcNameEnc = QFile::encodeName(QFileInfo(entry.sourcePath).fileName());
        auto srcFd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
            ::openat(srcParentFd, srcNameEnc.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!srcFd->valid() || !LocalMergeExecDetail::verifyFdIdentity(srcFd->fd, entry.sourceStat, entry.sourceStatValid)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Source directory identity changed after planning: %1").arg(entry.sourcePath));
            return;
        }

        const QString dstLeafName = QFileInfo(entry.destinationPath).fileName();
        const QByteArray dstNameEnc = QFile::encodeName(dstLeafName);
        if (::mkdirat(dstParentFd, dstNameEnc.constData(), LocalMergeExecDetail::initialDirectoryCreationMode()) != 0) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Could not create destination directory: %1").arg(entry.destinationPath));
            return;
        }

        auto dstFd = std::make_shared<LocalMergeExecDetail::UniqueFd>(
            ::openat(dstParentFd, dstNameEnc.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat createdDstSt {};
        if (!dstFd->valid() || ::fstat(dstFd->fd, &createdDstSt) != 0 || !S_ISDIR(createdDstSt.st_mode)) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError,
                            tr("Newly created directory could not be anchored: %1").arg(entry.destinationPath));
            return;
        }

        struct stat verifiedSrcSt {};
        if (::fstat(srcFd->fd, &verifiedSrcSt) == 0 && S_ISDIR(verifiedSrcSt.st_mode)) {
            recordCreatedDirectoryForFinalization(
                entry.relativePath,
                entry.destinationPath,
                dstLeafName,
                dstFd,
                createdDstSt,
                verifiedSrcSt);
        }

        m_srcDirFds.insert(QDir::cleanPath(entry.sourcePath), std::move(srcFd));
        m_destDirFds.insert(entry.relativePath, std::move(dstFd));
        m_approvedDirectories.insert(entry.relativePath);
        m_report.directoriesCreated.append(entry.relativePath);
        m_report.lastPublishedEntry = entry.relativePath;
        scheduleNext();
    }

    void handleCreateOrRenameFile(
        const LocalMergePlanEntry &entry,
        const QString &targetFileName,
        bool isRename)
    {
        const QString srcRoot = effectiveSourceRootFor(entry);
        const int srcParentFd = ensureAnchoredParentFd(entry.relativePath, entry.sourcePath, true, srcRoot);
        const int dstParentFd = ensureAnchoredParentFd(entry.relativePath, entry.destinationPath, false);
        if (srcParentFd < 0 || dstParentFd < 0) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Parent directory invalid or symlinked."));
            return;
        }

        const QString srcFileName = QFileInfo(entry.sourcePath).fileName();
        const QString partName = QStringLiteral(".thispc-part.%1.%2.%3")
                                     .arg(targetFileName)
                                     .arg(static_cast<qulonglong>(::getpid()))
                                     .arg(LocalMergeExecDetail::nextStagingSequence());
        const QString targetRel = isRename
            ? (parentRelativePath(entry.relativePath).isEmpty()
                   ? targetFileName
                   : parentRelativePath(entry.relativePath) + QLatin1Char('/') + targetFileName)
            : entry.relativePath;

        KJob *copyJob = nullptr;
        if (m_copyJobFactory) {
            const QUrl srcUrl = QUrl::fromLocalFile(entry.sourcePath);
            const QUrl dstUrl = QUrl::fromLocalFile(
                QDir(QFileInfo(entry.destinationPath).path()).filePath(targetFileName));
            copyJob = m_copyJobFactory(srcUrl, dstUrl, this);
        } else {
            const int srcFdDup = ::dup(srcParentFd);
            const int dstFdDup = ::dup(dstParentFd);
            if (srcFdDup < 0 || dstFdDup < 0) {
                if (srcFdDup >= 0) ::close(srcFdDup);
                if (dstFdDup >= 0) ::close(dstFdDup);
                m_report.failedOperations.append(entry.relativePath);
                finalizeFailure(KJob::UserDefinedError, tr("Could not duplicate directory descriptors."));
                return;
            }
            const QString srcParentAbs = QDir::cleanPath(QFileInfo(entry.sourcePath).path());
            const QString parentRel = parentRelativePath(entry.relativePath);
            LocalAnchoredFileCopyWorker::FaultConfig faults;
            faults.simulateFsyncEio = m_simulateFsyncEio;
            faults.simulateOTmpfileUnsupported = m_simulateOTmpfileUnsupported;
            faults.prePublishHook = m_prePublishHook;
            faults.phaseHook = m_faultPhaseHook;
            faults.verifyChainHook = [this, srcParentAbs, srcRoot, parentRel]() {
                return verifySourceChainStillAnchored(srcParentAbs, srcRoot)
                    && verifyDirectoryChainStillAnchored(parentRel, false);
            };
            struct stat emptyDestStat {};
            copyJob = new LocalAnchoredFileCopyJob(
                LocalAnchoredFileCopyWorker::Mode::CreateNoOverwrite,
                srcFdDup, srcFileName,
                entry.decisionBinding.bound && entry.decisionBinding.expectedSourceValid
                    ? entry.decisionBinding.expectedSourceStat
                    : entry.sourceStat,
                dstFdDup, partName, targetFileName, emptyDestStat, entry.relativePath,
                this, m_chunkSize, m_chunkDelayMilliseconds, std::move(faults));
        }

        if (!copyJob) {
            m_report.failedOperations.append(entry.relativePath);
            finalizeFailure(KJob::UserDefinedError, tr("Could not create copy job."));
            return;
        }

        m_activeCopyJob = copyJob;
        copyJob->setAutoDelete(false);
        connect(copyJob, &KJob::processedAmountChanged, this,
                [this](KJob *, KJob::Unit unit, qulonglong amount) {
                    if (!m_finished && unit == KJob::Bytes) {
                        setProcessedAmount(KJob::Bytes, m_report.bytesProcessed + amount);
                    }
                });
        connect(copyJob, &KJob::result, this, [this, entry, targetRel, isRename](KJob *completed) {
            m_activeCopyJob = nullptr;
            auto *anchored = qobject_cast<LocalAnchoredFileCopyJob *>(completed);
            const bool published = anchored && anchored->destinationPublished();
            const bool publishedDespiteError = published && !anchored->directoryDurabilityConfirmed();
            completed->deleteLater();
            if (m_finished) return;
            if (completed->error() != KJob::NoError && publishedDespiteError) {
                m_report.failedOperations.append(entry.relativePath);
                m_report.uncertainDurability = true;
                m_report.filesCreated.append(targetRel);
                if (isRename) m_report.filesRenamed.append(targetRel);
                m_report.lastPublishedEntry = targetRel;
                finalizeOutcome(completed->error(), completed->errorText(),
                                LocalMergeExecutionOutcome::FailedUncertainDurability);
                return;
            }
            if (m_cancelRequested) {
                if (published || completed->error() == KJob::NoError) {
                    m_report.filesCreated.append(targetRel);
                    if (isRename) {
                        m_report.filesRenamed.append(targetRel);
                    }
                    m_report.lastPublishedEntry = targetRel;
                }
                finalizeOutcome(
                    KJob::KilledJobError,
                    tr("Merge cancelled."),
                    hasPublishedAnything()
                        ? LocalMergeExecutionOutcome::CancelledPartialSuccess
                        : LocalMergeExecutionOutcome::CancelledNoChanges);
                return;
            }
            if (completed->error() != KJob::NoError) {
                m_report.failedOperations.append(entry.relativePath);
                finalizeFailure(completed->error(), completed->errorText());
                return;
            }
            m_report.filesCreated.append(targetRel);
            if (isRename) {
                m_report.filesRenamed.append(targetRel);
            }
            m_report.bytesProcessed += entry.size;
            m_report.lastPublishedEntry = targetRel;
            setProcessedAmount(KJob::Bytes, m_report.bytesProcessed);
            scheduleNext();
        });
        copyJob->start();
    }

    struct CreatedDirectoryPermRecord {
        QString relativePath;
        QString destinationPath;
        QString leafName;
        std::shared_ptr<LocalMergeExecDetail::UniqueFd> dirFd;
        struct stat createdStat {};
        mode_t targetMode = 0700;
        struct timespec atim {};
        struct timespec mtim {};
        bool hasTimes = false;
    };

    void recordCreatedDirectoryForFinalization(
        const QString &relativePath,
        const QString &destinationPath,
        const QString &leafName,
        const std::shared_ptr<LocalMergeExecDetail::UniqueFd> &dirFd,
        const struct stat &createdSt,
        const struct stat &sourceSt)
    {
        CreatedDirectoryPermRecord rec;
        rec.relativePath = relativePath;
        rec.destinationPath = destinationPath;
        rec.leafName = leafName;
        rec.dirFd = dirFd;
        rec.createdStat = createdSt;
        rec.targetMode = LocalMergeExecDetail::sanitizeDirectoryMode(sourceSt.st_mode);
        rec.atim = sourceSt.st_atim;
        rec.mtim = sourceSt.st_mtim;
        rec.hasTimes = true;
        m_createdDirsForFinalization.append(std::move(rec));
    }

    // SEC-046-04: Finalize permissions on newly created directories in strict postorder
    // (reverse creation order: deepest children first, then parents) using anchored FDs.
    // Never touches pre-existing destination directories, never uses path-based chmod,
    // and fails closed if a directory was replaced (e.g., symlink swap) or fchmod fails.
    bool finalizeCreatedDirectoryPermissions()
    {
        for (qsizetype i = m_createdDirsForFinalization.size() - 1; i >= 0; --i) {
            const CreatedDirectoryPermRecord &rec = m_createdDirsForFinalization.at(i);
            if (!rec.dirFd || !rec.dirFd->valid()) {
                m_report.failedOperations.append(rec.relativePath);
                finalizeFailure(KIO::ERR_CANNOT_CHMOD,
                                tr("Anchored descriptor unavailable for directory permission finalization: %1")
                                    .arg(rec.destinationPath));
                return false;
            }

            if (m_preDirPermFinalizeHook) {
                m_preDirPermFinalizeHook(rec.relativePath, rec.destinationPath, rec.dirFd->fd);
            }

            // 1. Verify open FD identity and link count (detect unlink/replacement)
            struct stat curFdSt {};
            if (::fstat(rec.dirFd->fd, &curFdSt) != 0
                || !S_ISDIR(curFdSt.st_mode)
                || curFdSt.st_dev != rec.createdStat.st_dev
                || curFdSt.st_ino != rec.createdStat.st_ino
                || curFdSt.st_nlink == 0) {
                m_report.failedOperations.append(rec.relativePath);
                finalizeFailure(KIO::ERR_CANNOT_CHMOD,
                                tr("Destination directory identity changed or was unlinked before permission finalization: %1")
                                    .arg(rec.destinationPath));
                return false;
            }

            // 2. Verify destination parent entry still points to this exact directory (AT_SYMLINK_NOFOLLOW)
            const QString parentRel = parentRelativePath(rec.relativePath);
            int parentFd = -1;
            if (parentRel.isEmpty()) {
                if (!verifyDestRootStillAnchored()) {
                    m_report.failedOperations.append(rec.relativePath);
                    finalizeFailure(KIO::ERR_CANNOT_CHMOD,
                                    tr("Destination root identity changed before permission finalization: %1")
                                        .arg(rec.destinationPath));
                    return false;
                }
                parentFd = m_destRootFd.fd;
            } else {
                const auto pit = m_destDirFds.constFind(parentRel);
                if (pit != m_destDirFds.cend() && pit.value() && pit.value()->valid()) {
                    parentFd = pit.value()->fd;
                }
            }
            if (parentFd < 0) {
                m_report.failedOperations.append(rec.relativePath);
                finalizeFailure(KIO::ERR_CANNOT_CHMOD,
                                tr("Parent directory descriptor unavailable during permission finalization: %1")
                                    .arg(rec.destinationPath));
                return false;
            }
            struct stat pathSt {};
            const QByteArray leafEnc = QFile::encodeName(rec.leafName);
            if (::fstatat(parentFd, leafEnc.constData(), &pathSt, AT_SYMLINK_NOFOLLOW) != 0
                || !S_ISDIR(pathSt.st_mode)
                || pathSt.st_dev != rec.createdStat.st_dev
                || pathSt.st_ino != rec.createdStat.st_ino) {
                m_report.failedOperations.append(rec.relativePath);
                finalizeFailure(KIO::ERR_CANNOT_CHMOD,
                                tr("Destination directory path was replaced (possible symlink attack) before permission finalization: %1")
                                    .arg(rec.destinationPath));
                return false;
            }

            // 3. Apply sanitized mode via FD-anchored fchmod (never path-based chmod)
            if (m_simulateDirChmodError || ::fchmod(rec.dirFd->fd, rec.targetMode) != 0) {
                const int chmodErr = m_simulateDirChmodError ? EACCES : errno;
                if (m_simulateDirChmodError
                    || (!LocalMergeExecDetail::isNonPosixPermissionFs(rec.dirFd->fd)
                        && chmodErr != ENOTSUP && chmodErr != EOPNOTSUPP && chmodErr != ENOSYS)) {
                    m_report.failedOperations.append(rec.relativePath);
                    finalizeFailure(KIO::ERR_CANNOT_CHMOD,
                                    tr("Could not apply final permissions to destination directory: %1")
                                        .arg(rec.destinationPath));
                    return false;
                }
            }

            // 4. Best-effort timestamp restoration via FD-anchored futimens
            if (rec.hasTimes) {
                const struct timespec times[2] = {rec.atim, rec.mtim};
                ::futimens(rec.dirFd->fd, times);
            }
        }
        return true;
    }

    void finalizeFailure(int errorCode, const QString &errorText)
    {
        finalizeOutcome(
            errorCode,
            errorText,
            hasPublishedAnything()
                ? LocalMergeExecutionOutcome::FailedPartialSuccess
                : LocalMergeExecutionOutcome::FailedNoChanges);
    }

    void finalizeOutcome(int errorCode, const QString &errorText, LocalMergeExecutionOutcome outcome)
    {
        if (m_finished) return;
        m_finished = true;
        m_report.outcome = outcome;
        m_report.completedFully = false;
        m_report.partialSuccess = hasPublishedAnything();
        m_report.retryAllowed = !m_report.partialSuccess && !m_report.uncertainDurability;
        populateUndoContract();
        setError(errorCode);
        setErrorText(errorText);
        emitResult();
    }

    LocalMergePlan m_plan;
    qsizetype m_chunkSize = 256 * 1024;
    int m_chunkDelayMilliseconds = 0;
    CopyJobFactory m_copyJobFactory;
    PrePublishHook m_prePublishHook;
    FaultPhaseHook m_faultPhaseHook;
    PreDirPermFinalizeHook m_preDirPermFinalizeHook;
    PreTopLevelAnchorHook m_preTopLevelAnchorHook;
    bool m_simulateFsyncEio = false;
    bool m_simulateOTmpfileUnsupported = false;
    bool m_simulateRenameExchangeUnsupported = false;
    bool m_simulateUnsupportedOverwriteFs = false;
    bool m_simulateDirChmodError = false;
    QList<CreatedDirectoryPermRecord> m_createdDirsForFinalization;

    QList<LocalMergeAnchoredDirectory> m_preApprovedDirectories;
    QString m_destRootPath;
    LocalMergeExecDetail::UniqueFd m_destRootFd;
    struct stat m_destRootStat {};
    QHash<QString, AnchoredRoot> m_srcRoots;

    qsizetype m_index = 0;
    QSet<QString> m_approvedDirectories;
    QSet<QString> m_skippedDirectories;
    QHash<QString, std::shared_ptr<LocalMergeExecDetail::UniqueFd>> m_srcDirFds;
    QHash<QString, std::shared_ptr<LocalMergeExecDetail::UniqueFd>> m_destDirFds;
    QPointer<KJob> m_activeCopyJob;
    LocalMergeExecutionReport m_report;
    bool m_started = false;
    bool m_finished = false;
    bool m_cancelRequested = false;
};
