/*
 * Safe asynchronous classification & merge planning for FOLDER-MERGE-01 (Stage 2A).
 * Uses POSIX lstat() without following symlinks or opening FIFOs/devices.
 * Note: Planning-time lstat() does NOT protect against TOCTOU races between
 * planning and execution; execution-time identity & fd checks belong to Stage 2B.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "localfileoverwrite.h"
#include "localtransferplan.h"

#include <KJob>

#include <QAtomicInteger>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>
#include <QUrl>

#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

enum class LocalMergeObjectKind {
    Missing,
    RegularFile,
    Directory,
    SymbolicLink,
    Special,
    Unreadable,
};

enum class LocalMergeEntryClassification {
    CreateNew,                      // A: destination missing, safe to create
    MergeDirectoryCandidate,        // B: source dir + existing destination dir
    FileConflict,                   // C: source regular file + distinct destination regular file
    TypeConflictFileOverDir,        // D: source file/symlink vs destination directory
    TypeConflictDirOverFile,        // E: source directory vs destination regular file
    DestinationSymlinkConflict,     // F: destination is a symbolic link (via lstat)
    BrokenSymlink,                  // G: broken symbolic link (source or destination)
    UnsupportedSpecial,             // H: FIFO, socket, char/block device, or unsupported entry
    CaseInsensitiveCollision,       // I: differing-case sibling or case-insensitive FS match
    SameObject,                     // J: source and destination have identical (st_dev, st_ino)
    DescendantOfSource,             // K: destination is inside source (or via symlink/alias)
    AmbiguousOrUnreadableMetadata,  // L: lstat/readdir/metadata unavailable (fail-closed)
};

enum class LocalMergeCaseStatus {
    None,
    ExactNameMatch,
    ActualCaseInsensitiveCollision,
    SuspectedCaseInsensitiveSibling,
    DuplicatePlannedDestination,
    UnknownFilesystemSemantics,
};

enum class LocalMergePendingDecision {
    NoneNeeded,
    AwaitUserFolderDecision,
    AwaitUserFileDecision,
    AwaitUserAmbiguousDecision,
    RefusedBlocked,
};

enum class LocalMergeApprovedAction {
    Unapproved,     // Default for all entries: NEVER defaults to Overwrite
    Create,
    MergeDirectory,
    Overwrite,      // Only set explicitly by user/caller, never by planner
    Rename,
    Skip,
    Abort,
};

enum class LocalMergeApplyPolicy {
    None,
    SkipAllFileConflicts,
    AutoRenameFileConflicts,
    OverwriteAllFileConflicts,
    MergeAllFolders,
    SkipAllFolders,
};

struct LocalMergeDecisionBinding
{
    bool bound = false;
    LocalMergeEntryClassification expectedClassification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
    LocalMergeApplyPolicy policyScope = LocalMergeApplyPolicy::None;
    struct stat expectedSourceStat {};
    bool expectedSourceValid = false;
    struct stat expectedDestinationStat {};
    bool expectedDestinationValid = false;
    struct stat expectedParentStat {};
    bool expectedParentValid = false;
    QString targetFileName;
    QString reason;
};

enum class LocalMergePlanError {
    None,
    Cancelled,
    NonLocalUrl,
    SourceMissing,
    DestinationInvalid,
    BlockedSelfOrDescendant,
    UnreadableMetadata,
};

struct LocalMergePlanEntry
{
    LocalMergeObjectKind sourceKind = LocalMergeObjectKind::Missing;
    LocalMergeObjectKind destinationKind = LocalMergeObjectKind::Missing;
    LocalMergeEntryClassification classification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
    LocalMergeCaseStatus caseStatus = LocalMergeCaseStatus::None;
    LocalMergePendingDecision pendingDecision = LocalMergePendingDecision::RefusedBlocked;
    LocalMergeApprovedAction approvedAction = LocalMergeApprovedAction::Unapproved;

    QString sourcePath;
    QString sourceRootPath;
    QString destinationPath;
    QString relativePath;
    QString caseCollisionSiblingName;

    bool sourceIsBrokenSymlink = false;
    bool destinationIsBrokenSymlink = false;
    QByteArray symbolicLinkContents;
    QString symbolicLinkTarget;

    struct stat sourceStat {};
    bool sourceStatValid = false;
    struct stat destinationStat {};
    bool destinationStatValid = false;

    qulonglong size = 0;
    qulonglong destinationSize = 0;
    QString diagnostic;
    LocalMergeDecisionBinding decisionBinding;
};

struct LocalMergePlanStats
{
    qsizetype newFileCount = 0;
    qsizetype newDirectoryCount = 0;
    qsizetype mergeDirectoryCandidateCount = 0;
    qsizetype fileConflictCount = 0;
    qsizetype typeConflictCount = 0;
    qsizetype symlinkAndUnsupportedCount = 0;
    qsizetype ambiguousNameCount = 0;
    qsizetype sameOrDescendantBlockedCount = 0;
    qsizetype unreadableMetadataCount = 0;
    qulonglong bytesToCopy = 0;
};

struct LocalMergePlan
{
    QList<LocalMergePlanEntry> entries;
    LocalMergePlanStats stats;
    QString destinationRootPath;
    struct stat destinationRootStat {};
    bool destinationRootStatValid = false;
    bool hasBlockingErrors = false;
    QString blockingReason;
};

namespace LocalMergeDetail {

inline LocalMergeObjectKind classifyFromLstat(
    const QString &path,
    struct stat *outStat,
    bool *outStatValid,
    bool *outBrokenSymlink,
    int *outErrno,
    bool *outSymlinkTargetUnreadable = nullptr)
{
    if (outStatValid) *outStatValid = false;
    if (outBrokenSymlink) *outBrokenSymlink = false;
    if (outErrno) *outErrno = 0;
    if (outSymlinkTargetUnreadable) *outSymlinkTargetUnreadable = false;

    struct stat st {};
    if (!localLstat(path, &st)) {
        const int err = errno;
        if (outErrno) *outErrno = err;
        if (err == ENOENT) {
            return LocalMergeObjectKind::Missing;
        }
        return LocalMergeObjectKind::Unreadable;
    }
    if (outStat) *outStat = st;
    if (outStatValid) *outStatValid = true;

    if (S_ISLNK(st.st_mode)) {
        struct stat targetSt {};
        if (::stat(QFile::encodeName(path).constData(), &targetSt) != 0) {
            const int targetErr = errno;
            if (targetErr == ENOENT || targetErr == ENOTDIR) {
                if (outBrokenSymlink) *outBrokenSymlink = true;
            } else {
                if (outSymlinkTargetUnreadable) *outSymlinkTargetUnreadable = true;
            }
        }
        return LocalMergeObjectKind::SymbolicLink;
    }
    if (S_ISDIR(st.st_mode)) {
        return LocalMergeObjectKind::Directory;
    }
    if (S_ISREG(st.st_mode)) {
        return LocalMergeObjectKind::RegularFile;
    }
    return LocalMergeObjectKind::Special;
}

inline bool sameInode(const struct stat &a, bool aValid, const struct stat &b, bool bValid)
{
    return aValid && bValid && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

inline bool isDestinationDescendantOfSource(
    const struct stat &sourceStat,
    bool sourceStatValid,
    const QString &sourcePath,
    const QString &destinationDirectoryPath)
{
    if (!sourceStatValid) {
        return false;
    }
    const QString cleanSource = QFileInfo(sourcePath).canonicalFilePath();
    const QString cleanDest = QFileInfo(destinationDirectoryPath).canonicalFilePath();
    if (!cleanSource.isEmpty() && !cleanDest.isEmpty()) {
        if (cleanDest == cleanSource || cleanDest.startsWith(cleanSource + QLatin1Char('/'))) {
            return true;
        }
    }

    // Walk destination ancestors with lstat() to detect alias/symlink/bind-like containment
    QString current = QDir::cleanPath(destinationDirectoryPath);
    while (!current.isEmpty()) {
        struct stat curSt {};
        if (localLstat(current, &curSt)
            && curSt.st_dev == sourceStat.st_dev
            && curSt.st_ino == sourceStat.st_ino) {
            return true;
        }
        const QString parent = QFileInfo(current).path();
        if (parent == current || parent.isEmpty()) {
            break;
        }
        current = parent;
    }
    return false;
}

}  // namespace LocalMergeDetail

class LocalMergePlanWorker final : public QObject
{
    Q_OBJECT

public:
    LocalMergePlanWorker(
        QList<QUrl> sources,
        QUrl destinationDirectory,
        int entryDelayMilliseconds = 0)
        : m_sources(std::move(sources))
        , m_destinationDirectory(std::move(destinationDirectory))
        , m_entryDelayMilliseconds(qMax(0, entryDelayMilliseconds))
    {
    }

    void requestCancel()
    {
        m_cancelled.storeRelease(true);
    }

public Q_SLOTS:
    void run()
    {
        if (!m_destinationDirectory.isLocalFile()) {
            fail(LocalMergePlanError::NonLocalUrl,
                 tr("The destination is not a local directory."));
            return;
        }
        const QString destRoot = QDir::cleanPath(m_destinationDirectory.toLocalFile());
        struct stat destRootStat {};
        bool destRootValid = false;
        int destErr = 0;
        const LocalMergeObjectKind destRootKind = LocalMergeDetail::classifyFromLstat(
            destRoot, &destRootStat, &destRootValid, nullptr, &destErr);
        bool destIsUsableDir = (destRootKind == LocalMergeObjectKind::Directory);
        if (destRootKind == LocalMergeObjectKind::SymbolicLink) {
            struct stat targetSt {};
            destIsUsableDir = (::stat(QFile::encodeName(destRoot).constData(), &targetSt) == 0
                               && S_ISDIR(targetSt.st_mode));
        }
        if (!destIsUsableDir) {
            fail(LocalMergePlanError::DestinationInvalid,
                 tr("The destination directory does not exist or is not a directory."));
            return;
        }

        LocalMergePlan plan;
        plan.destinationRootPath = destRoot;
        if (destRootKind == LocalMergeObjectKind::Directory && destRootValid) {
            plan.destinationRootStat = destRootStat;
            plan.destinationRootStatValid = true;
        }
        for (const QUrl &sourceUrl : std::as_const(m_sources)) {
            if (cancelled()) {
                fail(LocalMergePlanError::Cancelled, {});
                return;
            }
            if (!sourceUrl.isLocalFile()) {
                fail(LocalMergePlanError::NonLocalUrl,
                     tr("A source is not local."));
                return;
            }
            const QString srcPath = QDir::cleanPath(sourceUrl.toLocalFile());
            struct stat srcRootStat {};
            bool srcRootValid = false;
            int srcErr = 0;
            const LocalMergeObjectKind srcRootKind = LocalMergeDetail::classifyFromLstat(
                srcPath, &srcRootStat, &srcRootValid, nullptr, &srcErr);
            if (srcRootKind == LocalMergeObjectKind::Missing) {
                fail(LocalMergePlanError::SourceMissing,
                     tr("A source item does not exist: %1").arg(srcPath));
                return;
            }

            const QString rootName = QFileInfo(srcPath).fileName();
            if (rootName.isEmpty()) {
                fail(LocalMergePlanError::SourceMissing,
                     tr("Invalid source path: %1").arg(srcPath));
                return;
            }

            const QString sourceRoot = QDir::cleanPath(QFileInfo(srcPath).path());
            if (srcRootKind == LocalMergeObjectKind::Directory
                && LocalMergeDetail::isDestinationDescendantOfSource(
                    srcRootStat, srcRootValid, srcPath, destRoot)) {
                LocalMergePlanEntry blocked;
                blocked.sourceKind = srcRootKind;
                blocked.destinationKind = destRootKind;
                blocked.classification = LocalMergeEntryClassification::DescendantOfSource;
                blocked.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
                blocked.approvedAction = LocalMergeApprovedAction::Unapproved;
                blocked.sourcePath = srcPath;
                blocked.sourceRootPath = sourceRoot;
                blocked.destinationPath = QDir(destRoot).filePath(rootName);
                blocked.relativePath = rootName;
                blocked.sourceStat = srcRootStat;
                blocked.sourceStatValid = srcRootValid;
                blocked.diagnostic = tr("Cannot copy a directory into itself or its descendant.");
                plan.entries.push_back(blocked);
                ++plan.stats.sameOrDescendantBlockedCount;
                plan.hasBlockingErrors = true;
                plan.blockingReason = blocked.diagnostic;
                continue;
            }

            if (!appendEntry(srcPath, rootName, sourceRoot, destRoot, plan)) {
                if (cancelled()) {
                    fail(LocalMergePlanError::Cancelled, {});
                    return;
                }
            }
        }

        Q_EMIT completed(plan);
    }

Q_SIGNALS:
    void entryPlanned(qsizetype count, qulonglong bytes);
    void completed(const LocalMergePlan &plan);
    void failed(LocalMergePlanError error, const QString &message);

private:
    struct DirSiblingIndex {
        bool readable = false;
        QHash<QString, QList<QString>> foldedToNames;
    };

    DirSiblingIndex readDirectorySiblings(const QString &dirPath)
    {
        const QString clean = QDir::cleanPath(dirPath);
        auto it = m_destDirCache.constFind(clean);
        if (it != m_destDirCache.constEnd()) {
            return it.value();
        }
        DirSiblingIndex idx;
        DIR *stream = ::opendir(QFile::encodeName(clean).constData());
        if (!stream) {
            m_destDirCache.insert(clean, idx);
            return idx;
        }
        errno = 0;
        while (const auto *child = ::readdir(stream)) {
            const QByteArray rawName(child->d_name);
            if (rawName == "." || rawName == "..") continue;
            const QString name = QFile::decodeName(rawName);
            idx.foldedToNames[name.toCaseFolded()].append(name);
        }
        idx.readable = (errno == 0);
        ::closedir(stream);
        m_destDirCache.insert(clean, idx);
        return idx;
    }

    bool appendEntry(
        const QString &sourcePath,
        const QString &relativePath,
        const QString &sourceRoot,
        const QString &destinationRoot,
        LocalMergePlan &plan)
    {
        if (cancelled()) {
            return false;
        }

        LocalMergePlanEntry entry;
        entry.sourcePath = sourcePath;
        entry.sourceRootPath = sourceRoot;
        entry.relativePath = relativePath;
        entry.destinationPath = QDir(destinationRoot).filePath(relativePath);

        int srcErr = 0;
        bool srcSymlinkTargetUnreadable = false;
        entry.sourceKind = LocalMergeDetail::classifyFromLstat(
            entry.sourcePath, &entry.sourceStat, &entry.sourceStatValid,
            &entry.sourceIsBrokenSymlink, &srcErr, &srcSymlinkTargetUnreadable);

        int dstErr = 0;
        bool dstSymlinkTargetUnreadable = false;
        entry.destinationKind = LocalMergeDetail::classifyFromLstat(
            entry.destinationPath, &entry.destinationStat, &entry.destinationStatValid,
            &entry.destinationIsBrokenSymlink, &dstErr, &dstSymlinkTargetUnreadable);

        if (entry.sourceKind == LocalMergeObjectKind::RegularFile && entry.sourceStatValid) {
            entry.size = static_cast<qulonglong>(entry.sourceStat.st_size);
        }
        if (entry.destinationKind == LocalMergeObjectKind::RegularFile && entry.destinationStatValid) {
            entry.destinationSize = static_cast<qulonglong>(entry.destinationStat.st_size);
        }
        if (entry.sourceKind == LocalMergeObjectKind::SymbolicLink) {
            entry.symbolicLinkContents = readLocalLink(entry.sourcePath);
            entry.symbolicLinkTarget = QFile::decodeName(entry.symbolicLinkContents);
        }

        const QString cleanDestPath = QDir::cleanPath(entry.destinationPath);
        const QString foldedDestKey = cleanDestPath.toCaseFolded();
        const QString destParent = QFileInfo(cleanDestPath).path();
        const QString targetName = QFileInfo(cleanDestPath).fileName();

        // Check duplicate planned destinations in the same batch (exact or differing case)
        if (m_plannedExactDestinations.contains(cleanDestPath)) {
            entry.classification = LocalMergeEntryClassification::CaseInsensitiveCollision;
            entry.caseStatus = LocalMergeCaseStatus::DuplicatePlannedDestination;
            entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
            entry.diagnostic = tr("Duplicate destination path in batch: %1").arg(cleanDestPath);
            ++plan.stats.ambiguousNameCount;
            plan.hasBlockingErrors = true;
            plan.entries.push_back(entry);
            return true;
        }
        if (m_plannedFoldedDestinations.contains(foldedDestKey)) {
            entry.classification = LocalMergeEntryClassification::CaseInsensitiveCollision;
            entry.caseStatus = LocalMergeCaseStatus::DuplicatePlannedDestination;
            entry.pendingDecision = LocalMergePendingDecision::AwaitUserAmbiguousDecision;
            entry.diagnostic = tr("Batch contains differing-case destinations for: %1").arg(cleanDestPath);
            ++plan.stats.ambiguousNameCount;
            plan.entries.push_back(entry);
            return true;
        }
        m_plannedExactDestinations.insert(cleanDestPath);
        m_plannedFoldedDestinations.insert(foldedDestKey);

        auto detectExistingCaseStatus = [&](const QString &name) -> std::pair<LocalMergeCaseStatus, QString> {
            const DirSiblingIndex siblings = readDirectorySiblings(destParent);
            if (!siblings.readable) {
                return {LocalMergeCaseStatus::UnknownFilesystemSemantics, {}};
            }
            const QStringList matches = siblings.foldedToNames.value(name.toCaseFolded());
            if (matches.contains(name)) {
                return {LocalMergeCaseStatus::ExactNameMatch, {}};
            }
            for (const QString &sib : matches) {
                if (sib != name) {
                    return {LocalMergeCaseStatus::ActualCaseInsensitiveCollision, sib};
                }
            }
            return {LocalMergeCaseStatus::ExactNameMatch, {}};
        };

        auto checkMissingDestCaseCollision = [&]() -> bool {
            struct stat parentSt {};
            bool parentValid = false;
            const LocalMergeObjectKind parentKind = LocalMergeDetail::classifyFromLstat(
                destParent, &parentSt, &parentValid, nullptr, nullptr);
            if (parentKind != LocalMergeObjectKind::Directory) {
                return false;
            }
            const DirSiblingIndex siblings = readDirectorySiblings(destParent);
            if (!siblings.readable) {
                entry.classification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
                entry.caseStatus = LocalMergeCaseStatus::UnknownFilesystemSemantics;
                entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
                entry.diagnostic = tr("Destination parent directory could not be read.");
                ++plan.stats.ambiguousNameCount;
                ++plan.stats.unreadableMetadataCount;
                plan.hasBlockingErrors = true;
                return true;
            }
            const QStringList matches = siblings.foldedToNames.value(targetName.toCaseFolded());
            for (const QString &existingSibling : matches) {
                if (existingSibling != targetName) {
                    entry.classification = LocalMergeEntryClassification::CaseInsensitiveCollision;
                    entry.caseStatus = LocalMergeCaseStatus::SuspectedCaseInsensitiveSibling;
                    entry.caseCollisionSiblingName = existingSibling;
                    entry.pendingDecision = LocalMergePendingDecision::AwaitUserAmbiguousDecision;
                    entry.diagnostic = tr("Destination has a differing-case sibling: %1").arg(existingSibling);
                    ++plan.stats.ambiguousNameCount;
                    if (entry.sourceKind == LocalMergeObjectKind::RegularFile) {
                        plan.stats.bytesToCopy += entry.size;
                    }
                    return true;
                }
            }
            return false;
        };

        // Classify entry state A-L
        if (entry.sourceKind == LocalMergeObjectKind::Unreadable
            || entry.destinationKind == LocalMergeObjectKind::Unreadable) {
            entry.classification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
            entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
            entry.diagnostic = tr("Metadata unreadable for source or destination.");
            ++plan.stats.unreadableMetadataCount;
            plan.hasBlockingErrors = true;
        } else if (entry.sourceKind == LocalMergeObjectKind::Special
                   || entry.destinationKind == LocalMergeObjectKind::Special) {
            entry.classification = LocalMergeEntryClassification::UnsupportedSpecial;
            entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
            entry.diagnostic = tr("Special file (FIFO/socket/device) is not supported.");
            ++plan.stats.symlinkAndUnsupportedCount;
            plan.hasBlockingErrors = true;
        } else if (entry.destinationKind != LocalMergeObjectKind::Missing
                   && LocalMergeDetail::sameInode(
                       entry.sourceStat, entry.sourceStatValid,
                       entry.destinationStat, entry.destinationStatValid)) {
            entry.classification = LocalMergeEntryClassification::SameObject;
            entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
            entry.diagnostic = tr("Source and destination refer to the same filesystem object.");
            ++plan.stats.sameOrDescendantBlockedCount;
        } else if (entry.destinationKind == LocalMergeObjectKind::SymbolicLink) {
            if (dstSymlinkTargetUnreadable) {
                entry.classification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
                entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
                entry.diagnostic = tr("Destination symbolic link target metadata is unreadable.");
                ++plan.stats.unreadableMetadataCount;
                plan.hasBlockingErrors = true;
            } else {
                entry.classification = entry.destinationIsBrokenSymlink
                    ? LocalMergeEntryClassification::BrokenSymlink
                    : LocalMergeEntryClassification::DestinationSymlinkConflict;
                entry.pendingDecision = LocalMergePendingDecision::AwaitUserAmbiguousDecision;
                entry.diagnostic = entry.destinationIsBrokenSymlink
                    ? tr("Destination is a broken symbolic link.")
                    : tr("Destination is an existing symbolic link.");
                ++plan.stats.symlinkAndUnsupportedCount;
            }
        } else if (entry.sourceKind == LocalMergeObjectKind::SymbolicLink
                   && entry.destinationKind == LocalMergeObjectKind::Missing) {
            if (srcSymlinkTargetUnreadable) {
                entry.classification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
                entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
                entry.diagnostic = tr("Source symbolic link target metadata is unreadable.");
                ++plan.stats.unreadableMetadataCount;
                plan.hasBlockingErrors = true;
            } else if (checkMissingDestCaseCollision()) {
                // Classified as CaseInsensitiveCollision / AmbiguousOrUnreadableMetadata
            } else if (entry.sourceIsBrokenSymlink) {
                entry.classification = LocalMergeEntryClassification::BrokenSymlink;
                entry.pendingDecision = LocalMergePendingDecision::AwaitUserAmbiguousDecision;
                entry.diagnostic = tr("Source is a broken symbolic link.");
                ++plan.stats.symlinkAndUnsupportedCount;
            } else {
                entry.classification = LocalMergeEntryClassification::CreateNew;
                entry.pendingDecision = LocalMergePendingDecision::NoneNeeded;
                ++plan.stats.symlinkAndUnsupportedCount;
            }
        } else if (entry.sourceKind == LocalMergeObjectKind::Directory
                   && entry.destinationKind == LocalMergeObjectKind::Directory) {
            entry.classification = LocalMergeEntryClassification::MergeDirectoryCandidate;
            const auto cs = detectExistingCaseStatus(targetName);
            entry.caseStatus = cs.first;
            entry.caseCollisionSiblingName = cs.second;
            entry.pendingDecision = LocalMergePendingDecision::AwaitUserFolderDecision;
            ++plan.stats.mergeDirectoryCandidateCount;
            if (entry.caseStatus == LocalMergeCaseStatus::ActualCaseInsensitiveCollision) {
                ++plan.stats.ambiguousNameCount;
            }
        } else if (entry.sourceKind == LocalMergeObjectKind::RegularFile
                   && entry.destinationKind == LocalMergeObjectKind::RegularFile) {
            const auto cs = detectExistingCaseStatus(targetName);
            entry.caseStatus = cs.first;
            entry.caseCollisionSiblingName = cs.second;
            if (entry.caseStatus == LocalMergeCaseStatus::ActualCaseInsensitiveCollision) {
                entry.classification = LocalMergeEntryClassification::CaseInsensitiveCollision;
                entry.pendingDecision = LocalMergePendingDecision::AwaitUserAmbiguousDecision;
                entry.diagnostic = tr("Case-insensitive collision with existing file: %1").arg(cs.second);
                ++plan.stats.ambiguousNameCount;
            } else {
                entry.classification = LocalMergeEntryClassification::FileConflict;
                entry.pendingDecision = LocalMergePendingDecision::AwaitUserFileDecision;
            }
            ++plan.stats.fileConflictCount;
            plan.stats.bytesToCopy += entry.size;
        } else if ((entry.sourceKind == LocalMergeObjectKind::RegularFile
                    || entry.sourceKind == LocalMergeObjectKind::SymbolicLink)
                   && entry.destinationKind == LocalMergeObjectKind::Directory) {
            entry.classification = LocalMergeEntryClassification::TypeConflictFileOverDir;
            entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
            entry.diagnostic = tr("Cannot overwrite a directory with a file or symlink.");
            ++plan.stats.typeConflictCount;
        } else if (entry.sourceKind == LocalMergeObjectKind::Directory
                   && entry.destinationKind == LocalMergeObjectKind::RegularFile) {
            entry.classification = LocalMergeEntryClassification::TypeConflictDirOverFile;
            entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
            entry.diagnostic = tr("Cannot merge a directory onto an existing regular file.");
            ++plan.stats.typeConflictCount;
        } else if (entry.destinationKind == LocalMergeObjectKind::Missing) {
            if (!checkMissingDestCaseCollision()) {
                entry.classification = LocalMergeEntryClassification::CreateNew;
                entry.pendingDecision = LocalMergePendingDecision::NoneNeeded;
                if (entry.sourceKind == LocalMergeObjectKind::RegularFile) {
                    ++plan.stats.newFileCount;
                    plan.stats.bytesToCopy += entry.size;
                } else if (entry.sourceKind == LocalMergeObjectKind::Directory) {
                    ++plan.stats.newDirectoryCount;
                }
            }
        }

        plan.entries.push_back(entry);
        Q_EMIT entryPlanned(plan.entries.size(), plan.stats.bytesToCopy);

        if (m_entryDelayMilliseconds > 0) {
            QThread::msleep(static_cast<unsigned long>(m_entryDelayMilliseconds));
        }

        // Never follow symlinks recursively; only recurse into real Directory sources
        if (entry.sourceKind != LocalMergeObjectKind::Directory) {
            return true;
        }
        if (entry.classification == LocalMergeEntryClassification::SameObject
            || entry.classification == LocalMergeEntryClassification::DescendantOfSource
            || (entry.destinationKind != LocalMergeObjectKind::Directory
                && entry.destinationKind != LocalMergeObjectKind::Missing)) {
            return true;
        }

        DIR *stream = ::opendir(QFile::encodeName(entry.sourcePath).constData());
        if (!stream) {
            LocalMergePlanEntry errEntry;
            errEntry.sourceKind = LocalMergeObjectKind::Unreadable;
            errEntry.classification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
            errEntry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
            errEntry.sourcePath = entry.sourcePath;
            errEntry.sourceRootPath = sourceRoot;
            errEntry.destinationPath = entry.destinationPath;
            errEntry.relativePath = entry.relativePath;
            errEntry.diagnostic = tr("Could not read source directory: %1").arg(entry.sourcePath);
            plan.entries.push_back(errEntry);
            ++plan.stats.unreadableMetadataCount;
            plan.hasBlockingErrors = true;
            return true;
        }

        QStringList names;
        errno = 0;
        while (const auto *child = ::readdir(stream)) {
            const QByteArray rawName(child->d_name);
            if (rawName != "." && rawName != "..") {
                names.push_back(QFile::decodeName(rawName));
            }
            if (cancelled()) break;
        }
        const int readErr = errno;
        ::closedir(stream);
        if (cancelled()) {
            return false;
        }
        if (readErr != 0) {
            ++plan.stats.unreadableMetadataCount;
            plan.hasBlockingErrors = true;
            return true;
        }

        names.sort();
        const QDir srcDir(entry.sourcePath);
        for (const QString &name : std::as_const(names)) {
            if (!appendEntry(
                    srcDir.filePath(name),
                    relativePath + QLatin1Char('/') + name,
                    sourceRoot,
                    destinationRoot,
                    plan)) {
                return false;
            }
        }
        return true;
    }

    bool cancelled() const
    {
        return m_cancelled.loadAcquire();
    }

    void fail(LocalMergePlanError error, const QString &message)
    {
        Q_EMIT failed(error, message);
    }

    QList<QUrl> m_sources;
    QUrl m_destinationDirectory;
    int m_entryDelayMilliseconds = 0;
    QAtomicInteger<bool> m_cancelled = false;
    QSet<QString> m_plannedExactDestinations;
    QSet<QString> m_plannedFoldedDestinations;
    QHash<QString, DirSiblingIndex> m_destDirCache;
};

class LocalMergePlanJob final : public KJob
{
    Q_OBJECT

public:
    LocalMergePlanJob(
        QList<QUrl> sources,
        const QUrl &destinationDirectory,
        QObject *parent = nullptr,
        int entryDelayMilliseconds = 0)
        : KJob(parent)
        , m_worker(new LocalMergePlanWorker(
              std::move(sources),
              destinationDirectory,
              entryDelayMilliseconds))
    {
        qRegisterMetaType<LocalMergePlan>();
        qRegisterMetaType<LocalMergePlanError>();
        setCapabilities(KJob::Killable);
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::started,
                m_worker, &LocalMergePlanWorker::run);
        connect(m_worker, &LocalMergePlanWorker::entryPlanned,
                this, [this](qsizetype count, qulonglong bytes) {
                    m_plannedEntryCount = count;
                    setProcessedAmount(KJob::Bytes, bytes);
                });
        connect(m_worker, &LocalMergePlanWorker::completed,
                this, [this](const LocalMergePlan &plan) {
                    m_plan = plan;
                    setTotalAmount(KJob::Bytes, plan.stats.bytesToCopy);
                    setProcessedAmount(KJob::Bytes, plan.stats.bytesToCopy);
                    finish(LocalMergePlanError::None, {});
                });
        connect(m_worker, &LocalMergePlanWorker::failed,
                this, &LocalMergePlanJob::finish);
        connect(&m_thread, &QThread::finished,
                m_worker, &QObject::deleteLater);
    }

    ~LocalMergePlanJob() override
    {
        if (m_worker) {
            m_worker->requestCancel();
        }
        m_thread.quit();
        m_thread.wait();
    }

    void start() override
    {
        if (m_started) return;
        m_started = true;
        m_thread.start();
    }

    void requestCancelAsync()
    {
        if (m_worker) {
            m_worker->requestCancel();
        }
    }
    bool hasFinished() const { return m_finished; }

    const LocalMergePlan &plan() const { return m_plan; }
    LocalMergePlanError planError() const { return m_planError; }
    qsizetype plannedEntryCount() const { return m_plannedEntryCount; }

protected:
    bool doKill() override
    {
        if (!m_thread.isRunning()) return false;
        m_finished = true;
        m_planError = LocalMergePlanError::Cancelled;
        if (m_worker) {
            m_worker->requestCancel();
        }
        return true;
    }

private:
    void finish(LocalMergePlanError error, const QString &message)
    {
        if (m_finished) return;
        m_finished = true;
        m_planError = error;
        m_thread.quit();
        m_thread.wait();
        m_worker = nullptr;
        if (error == LocalMergePlanError::Cancelled) {
            setError(KJob::KilledJobError);
            setErrorText(tr("Merge planning cancelled."));
        } else if (error != LocalMergePlanError::None) {
            setError(KJob::UserDefinedError);
            setErrorText(message);
        } else {
            setPercent(100);
        }
        emitResult();
    }

    QThread m_thread;
    LocalMergePlanWorker *m_worker = nullptr;
    LocalMergePlan m_plan;
    LocalMergePlanError m_planError = LocalMergePlanError::None;
    qsizetype m_plannedEntryCount = 0;
    bool m_started = false;
    bool m_finished = false;
};
