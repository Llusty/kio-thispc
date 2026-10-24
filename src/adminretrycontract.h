/*
 * Admin retry contract and data models for ThisPC 0.43.0.
 *
 * Defines structural error categories, retry requests, eligibility evaluation,
 * fail-closed partial state models, and identity snapshots without performing
 * any elevated operations.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localfileoverwrite.h"

#include <KIO/Global>
#include <KProtocolInfo>
#include <KJob>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QUrl>

#include <cerrno>
#include <climits>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>


enum class AdminOperationBackend {
    Unknown,
    NativeSingleFileCopy,
    NativeSingleFileMove,
    NativeBatchTransfer,
    KioCopyJob,
    KioStoredTransferJob,
    KioMkdirJob,
    KioDeleteJob,
    KioTrashJob,
    KioChmodJob
};

enum class AdminErrorCategory {
    None,
    PermissionDenied,
    WriteAccessDenied,
    DiskFull,
    ReadOnlyFilesystem,
    PathNotFound,
    Collision,
    UserCancelled,
    AuthorizationCancelledOrDenied,
    MountLoss,
    UnsupportedBackendOrScheme,
    UnverifiableOrUnknown
};

inline const char *adminErrorCategoryToString(AdminErrorCategory cat)
{
    switch (cat) {
    case AdminErrorCategory::None:
        return "None";
    case AdminErrorCategory::PermissionDenied:
        return "PermissionDenied";
    case AdminErrorCategory::WriteAccessDenied:
        return "WriteAccessDenied";
    case AdminErrorCategory::DiskFull:
        return "DiskFull";
    case AdminErrorCategory::ReadOnlyFilesystem:
        return "ReadOnlyFilesystem";
    case AdminErrorCategory::PathNotFound:
        return "PathNotFound";
    case AdminErrorCategory::Collision:
        return "Collision";
    case AdminErrorCategory::UserCancelled:
        return "UserCancelled";
    case AdminErrorCategory::AuthorizationCancelledOrDenied:
        return "AuthorizationCancelledOrDenied";
    case AdminErrorCategory::MountLoss:
        return "MountLoss";
    case AdminErrorCategory::UnsupportedBackendOrScheme:
        return "UnsupportedBackendOrScheme";
    case AdminErrorCategory::UnverifiableOrUnknown:
        return "UnverifiableOrUnknown";
    }
    return "Unknown";
}

struct AdminOperationError {
    AdminErrorCategory category = AdminErrorCategory::None;
    int kioErrorCode = 0;
    int posixErrno = 0;
    int nativeTransferError = 0;
    QString rawErrorString;

    bool isPermissionError() const
    {
        return category == AdminErrorCategory::PermissionDenied
            || category == AdminErrorCategory::WriteAccessDenied;
    }
};

enum class AdminPartialCompletionState {
    NotStarted,
    InProgressUnfinished,
    PartialFileCreated,
    PartiallyCompleted,
    UnknownState
};

enum class AdminTransferFailureState {
    None,
    ReadFailedBeforeDataTransfer,
    PartialDestinationCreated,
    UnknownCompletionState
};

inline const char *adminTransferFailureStateToString(AdminTransferFailureState state)
{
    switch (state) {
    case AdminTransferFailureState::None:
        return "None";
    case AdminTransferFailureState::ReadFailedBeforeDataTransfer:
        return "ReadFailedBeforeDataTransfer";
    case AdminTransferFailureState::PartialDestinationCreated:
        return "PartialDestinationCreated";
    case AdminTransferFailureState::UnknownCompletionState:
        return "UnknownCompletionState";
    }
    return "Unknown";
}

enum class AdminArtifactRejectionReason {
    None,
    TargetExistedBeforeStart,       // 1. Plik docelowy istniał przed uruchomieniem zadania
    UnverifiedTaskProvenance,       // 2. Brak dowodu utworzenia przez dokładnie to zadanie
    UnknownArtifactIdentity,        // 3. Nieznana pierwotna tożsamość st_dev / st_ino
    ArtifactIdentityChanged,        // 4. Tożsamość st_dev / st_ino zmieniła się przed usunięciem
    ParentDirectoryChanged,         // 5. Katalog nadrzędny został podmieniony lub usunięty
    TargetIsSymlinkOrHardlink,      // 6. Symlink, broken symlink, hardlink lub niebezpieczna ścieżka
    TaskWriteNotConfirmedStopped,   // 7. Zadanie nie potwierdziło jednoznacznie zakończenia zapisów
    TargetUsedByAnotherTask,        // 8. Inne zadanie korzysta z tego samego celu
    NonZeroPartialData,             // 9a. Plik o niezerowej długości
    AmbiguousOrPartialState,        // 9b. Niejednoznaczny lub częściowy stan (.part / partialState)
    ForeignOrRootOwner,             // 9c. Plik należy do root lub innego użytkownika
    NoSafeDeletePermission          // 9d. Brak uprawnień zwykłego użytkownika do bezpiecznego usunięcia
};

inline const char *adminArtifactRejectionReasonToString(AdminArtifactRejectionReason reason)
{
    switch (reason) {
    case AdminArtifactRejectionReason::None:
        return "None";
    case AdminArtifactRejectionReason::TargetExistedBeforeStart:
        return "TargetExistedBeforeStart";
    case AdminArtifactRejectionReason::UnverifiedTaskProvenance:
        return "UnverifiedTaskProvenance";
    case AdminArtifactRejectionReason::UnknownArtifactIdentity:
        return "UnknownArtifactIdentity";
    case AdminArtifactRejectionReason::ArtifactIdentityChanged:
        return "ArtifactIdentityChanged";
    case AdminArtifactRejectionReason::ParentDirectoryChanged:
        return "ParentDirectoryChanged";
    case AdminArtifactRejectionReason::TargetIsSymlinkOrHardlink:
        return "TargetIsSymlinkOrHardlink";
    case AdminArtifactRejectionReason::TaskWriteNotConfirmedStopped:
        return "TaskWriteNotConfirmedStopped";
    case AdminArtifactRejectionReason::TargetUsedByAnotherTask:
        return "TargetUsedByAnotherTask";
    case AdminArtifactRejectionReason::NonZeroPartialData:
        return "NonZeroPartialData";
    case AdminArtifactRejectionReason::AmbiguousOrPartialState:
        return "AmbiguousOrPartialState";
    case AdminArtifactRejectionReason::ForeignOrRootOwner:
        return "ForeignOrRootOwner";
    case AdminArtifactRejectionReason::NoSafeDeletePermission:
        return "NoSafeDeletePermission";
    }
    return "Unknown";
}

struct AdminArtifactOwnershipAudit {
    bool artifactExists = false;
    bool artifactPreserved = false;
    QString preservedArtifactPath;

    // 9-point ownership and safety verification:
    bool check1_notExistedBefore = true;
    bool check2_createdByThisTask = false;
    bool check3_knownIdentity = false;
    bool check4_identityUnchanged = false;
    bool check5_parentDirUnchanged = true;
    bool check6_noSymlinkOrHardlink = true;
    bool check7_writesConfirmedStopped = false;
    bool check8_noOtherTaskUsingTarget = true;
    bool check9_noPartialOrAmbiguousState = true;
    bool check9_ownedByCurrentUser = true;
    bool check9_hasSafeDeletePermission = true;

    AdminArtifactRejectionReason primaryReason = AdminArtifactRejectionReason::None;

    bool allChecksPassed() const
    {
        return check1_notExistedBefore
            && check2_createdByThisTask
            && check3_knownIdentity
            && check4_identityUnchanged
            && check5_parentDirUnchanged
            && check6_noSymlinkOrHardlink
            && check7_writesConfirmedStopped
            && check8_noOtherTaskUsingTarget
            && check9_noPartialOrAmbiguousState
            && check9_ownedByCurrentUser
            && check9_hasSafeDeletePermission
            && primaryReason == AdminArtifactRejectionReason::None;
    }
};

struct AdminLocalIdentity {
    struct stat statBuf {};
    bool exists = false;
    bool isDirectory = false;
    bool isRegular = false;
    bool isSymLink = false;
    bool isBrokenSymLink = false;
    QString symlinkTarget;

    static AdminLocalIdentity read(const QString &path)
    {
        AdminLocalIdentity id;
        if (path.isEmpty()) {
            return id;
        }

        const QByteArray encoded = QFile::encodeName(path);
        struct stat lsb {};
        if (::lstat(encoded.constData(), &lsb) == 0) {
            id.exists = true;
            id.statBuf = lsb;
            id.isDirectory = S_ISDIR(lsb.st_mode);
            id.isRegular = S_ISREG(lsb.st_mode);
            id.isSymLink = S_ISLNK(lsb.st_mode);

            if (id.isSymLink) {
                char buf[PATH_MAX];
                const ssize_t len = ::readlink(encoded.constData(), buf, sizeof(buf) - 1);
                if (len > 0) {
                    buf[len] = '\0';
                    id.symlinkTarget = QFile::decodeName(buf);
                }
                struct stat sb {};
                if (::stat(encoded.constData(), &sb) != 0 && errno == ENOENT) {
                    id.isBrokenSymLink = true;
                }
            }
        }
        return id;
    }

    bool sameFile(const AdminLocalIdentity &other) const
    {
        return exists && other.exists
            && statBuf.st_dev == other.statBuf.st_dev
            && statBuf.st_ino == other.statBuf.st_ino;
    }

    bool matches(const QString &path) const
    {
        if (!exists) {
            return !read(path).exists;
        }
        const auto other = read(path);
        if (!other.exists) {
            return false;
        }
        if (isSymLink != other.isSymLink || isDirectory != other.isDirectory || isRegular != other.isRegular) {
            return false;
        }
        if (isSymLink) {
            return symlinkTarget == other.symlinkTarget && isBrokenSymLink == other.isBrokenSymLink;
        }
        return statBuf.st_dev == other.statBuf.st_dev
            && statBuf.st_ino == other.statBuf.st_ino
            && statBuf.st_size == other.statBuf.st_size
            && statBuf.st_mode == other.statBuf.st_mode
            && statBuf.st_mtim.tv_sec == other.statBuf.st_mtim.tv_sec
            && statBuf.st_mtim.tv_nsec == other.statBuf.st_mtim.tv_nsec;
    }
};

struct AdminRetryRequest {
    FileOperationType operationType = FileOperationType::Custom;
    AdminOperationBackend backend = AdminOperationBackend::Unknown;
    QList<QUrl> sourceUrls;
    QUrl destinationDirectory;
    QList<QUrl> destinationUrls;
    QString requestedName;
    QByteArray fileContents;
    AdminOperationError error;
    bool wasCancelled = false;
    AdminPartialCompletionState partialState = AdminPartialCompletionState::UnknownState;
    bool overwriteApproved = false;
    bool alreadyRetriedAsAdmin = false;
    bool clearClipboardOnSuccess = false;
    bool isGuiRequest = false;

    QList<AdminLocalIdentity> sourceIdentities;
    AdminLocalIdentity destinationDirectoryIdentity;
    AdminLocalIdentity targetItemIdentity;

    FileOperationContext originalContext;
};

enum class AdminRetryEligibility {
    Eligible,
    Ineligible,
    RequiresInspection,
    UnsupportedBackend
};

enum class AdminRetryIneligibilityReason {
    None,
    UserCancelled,
    AlreadyRetriedAsAdmin,
    NonPermissionError,
    DiskFull,
    ReadOnlyFilesystem,
    TargetCollision,
    SourceMissing,
    NonLocalScheme,
    TrashProtocolUnsupported,
    ArchiveOperationUnsupported,
    UnverifiablePartialState,
    SourceIdentityChanged,
    DestinationIdentityChanged,
    UnsafeSymlinkTraversal,
    TargetInsideSourceDirectory,
    MissingAdminWorker,
    CrossFilesystemMove,
    DirectoryMoveUnsupported,
    SameParentRequired,
    BatchOperationRequiresInspection,
    DeletionRequiresInspection,
    OperationDisallowedInGui,
    Unknown
};

struct AdminRetryQualification {
    AdminRetryEligibility eligibility = AdminRetryEligibility::Ineligible;
    AdminRetryIneligibilityReason reason = AdminRetryIneligibilityReason::None;
    QString message;
    bool isReversible = false;

    bool canAttemptElevation() const
    {
        return eligibility == AdminRetryEligibility::Eligible;
    }
};

inline bool isGuiAdminRetryAllowed(FileOperationType type)
{
    switch (type) {
    case FileOperationType::Copy:
    case FileOperationType::CreateFile:
    case FileOperationType::CreateFolder:
        return true;
    default:
        return false;
    }
}

inline bool isLocalFilesystemReadOnly(const QString &localPath)
{
    if (localPath.isEmpty()) {
        return false;
    }
    struct statvfs fs {};
    if (::statvfs(QFile::encodeName(localPath).constData(), &fs) == 0) {
        return (fs.f_flag & ST_RDONLY) != 0;
    }
    return false;
}

inline bool isDestinationInsideSource(const QString &targetPath, const QString &sourcePath)
{
    const QString cleanDir = QDir::cleanPath(sourcePath);
    const QString cleanTarget = QDir::cleanPath(targetPath);
    if (cleanDir.isEmpty() || cleanTarget.isEmpty()) {
        return false;
    }
    if (cleanTarget == cleanDir) {
        return true;
    }
    return cleanTarget.startsWith(cleanDir + QLatin1Char('/'));
}

inline bool hasSymlinkInPathComponents(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || clean == QLatin1String("/")) {
        return false;
    }
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString current;
    for (const QString &part : parts) {
        current += QLatin1Char('/') + part;
        const QByteArray encoded = QFile::encodeName(current);
        struct stat sb {};
        if (::lstat(encoded.constData(), &sb) == 0) {
            if (S_ISLNK(sb.st_mode)) {
                return true;
            }
        }
    }
    return false;
}

inline AdminOperationError classifyOperationError(
    KJob *job,
    int explicitErrno = 0,
    const QString &destinationDirectoryPath = QString(),
    const QString &targetFilePath = QString(),
    const QString &partFilePath = QString())
{
    AdminOperationError opError;
    if (!job) {
        if (explicitErrno != 0) {
            opError.posixErrno = explicitErrno;
            if (explicitErrno == EACCES || explicitErrno == EPERM) {
                opError.category = AdminErrorCategory::PermissionDenied;
            } else if (explicitErrno == EROFS) {
                opError.category = AdminErrorCategory::ReadOnlyFilesystem;
            } else if (explicitErrno == ENOSPC || explicitErrno == EDQUOT) {
                opError.category = AdminErrorCategory::DiskFull;
            } else if (explicitErrno == ENOENT) {
                opError.category = AdminErrorCategory::PathNotFound;
            } else if (explicitErrno == EEXIST) {
                opError.category = AdminErrorCategory::Collision;
            } else {
                opError.category = AdminErrorCategory::UnverifiableOrUnknown;
            }
        }
        return opError;
    }

    opError.kioErrorCode = job->error();
    opError.rawErrorString = job->errorString();
    opError.posixErrno = explicitErrno != 0 ? explicitErrno : job->property("thispcSystemError").toInt();

    if (opError.kioErrorCode == 0 && opError.posixErrno == 0) {
        opError.category = AdminErrorCategory::None;
        return opError;
    }

    const bool isPrivileged = job->property("thispcPrivilegedOperation").toBool();

    const bool userCancelled = job->error() == KJob::KilledJobError
        || job->error() == KIO::ERR_USER_CANCELED;

    if (userCancelled) {
        const QString errStr = (job->errorText() + QLatin1Char(' ') + job->errorString()).toLower();
        if (errStr.contains(QStringLiteral("polkit"))
            || errStr.contains(QStringLiteral("policykit"))
            || errStr.contains(QStringLiteral("auth"))) {
            opError.category = AdminErrorCategory::AuthorizationCancelledOrDenied;
        } else {
            opError.category = AdminErrorCategory::UserCancelled;
        }
        return opError;
    }

    if (opError.posixErrno != 0) {
        if (opError.posixErrno == EACCES || opError.posixErrno == EPERM) {
            opError.category = isPrivileged
                ? AdminErrorCategory::AuthorizationCancelledOrDenied
                : AdminErrorCategory::PermissionDenied;
            return opError;
        }
        if (opError.posixErrno == EROFS) {
            opError.category = AdminErrorCategory::ReadOnlyFilesystem;
            return opError;
        }
        if (opError.posixErrno == ENOSPC || opError.posixErrno == EDQUOT) {
            opError.category = AdminErrorCategory::DiskFull;
            return opError;
        }
        if (opError.posixErrno == ENOENT) {
            opError.category = AdminErrorCategory::PathNotFound;
            return opError;
        }
        if (opError.posixErrno == EEXIST) {
            opError.category = AdminErrorCategory::Collision;
            return opError;
        }
    }

    switch (job->error()) {
    case KIO::ERR_ACCESS_DENIED:
    case KIO::ERR_CANNOT_ENTER_DIRECTORY:
    case KIO::ERR_CANNOT_CHMOD:
    case KIO::ERR_CANNOT_DELETE_ORIGINAL:
        if (isPrivileged) {
            // Under kio-admin, D-Bus AccessDenied (caused by Polkit cancellation, wrong password,
            // or authorization denial) is mapped to KIO::ERR_ACCESS_DENIED.
            // Because KIO does not distinguish cancellation from denial in this path,
            // we classify it unambiguously as AuthorizationCancelledOrDenied.
            opError.category = AdminErrorCategory::AuthorizationCancelledOrDenied;
        } else {
            opError.category = AdminErrorCategory::PermissionDenied;
        }
        break;

    case KIO::ERR_WRITE_ACCESS_DENIED:
    case KIO::ERR_CANNOT_WRITE:
    case KIO::ERR_CANNOT_MKDIR:
    case KIO::ERR_CANNOT_RMDIR:
    case KIO::ERR_CANNOT_RENAME:
    case KIO::ERR_CANNOT_DELETE:
        if (isPrivileged) {
            opError.category = AdminErrorCategory::AuthorizationCancelledOrDenied;
        } else {
            opError.category = AdminErrorCategory::WriteAccessDenied;
        }
        break;

    case KIO::ERR_DISK_FULL:
        opError.category = AdminErrorCategory::DiskFull;
        break;

    case KIO::ERR_FILE_ALREADY_EXIST:
    case KIO::ERR_DIR_ALREADY_EXIST:
        opError.category = AdminErrorCategory::Collision;
        break;

    case KIO::ERR_DOES_NOT_EXIST:
        opError.category = AdminErrorCategory::PathNotFound;
        break;

    case KIO::ERR_CANNOT_MOUNT:
    case KIO::ERR_CANNOT_UNMOUNT:
        opError.category = AdminErrorCategory::MountLoss;
        break;

    case KIO::ERR_UNSUPPORTED_PROTOCOL:
        opError.category = AdminErrorCategory::UnsupportedBackendOrScheme;
        break;

    case KIO::ERR_CANNOT_READ:
        if (opError.posixErrno == EACCES || opError.posixErrno == EPERM) {
            opError.category = isPrivileged
                ? AdminErrorCategory::AuthorizationCancelledOrDenied
                : AdminErrorCategory::PermissionDenied;
        } else {
            opError.category = AdminErrorCategory::UnverifiableOrUnknown;
        }
        break;

    case KIO::ERR_CANNOT_TRUNCATE: {
        // Do NOT classify globally as PermissionDenied.
        // Truncate failure can be caused by EROFS, ENOSPC, EDQUOT, filesystem limits, or lack of write permissions.
        // We only classify as WriteAccessDenied if there is structural evidence:
        // 1. Destination directory exists, is a directory, not a symlink, and has no symlinks in path components.
        // 2. Destination filesystem is not read-only.
        // 3. Destination filesystem is not out of space.
        // 4. Target file and .part file do not already exist on disk (no pre-existing collision or unmanaged partial state).
        // 5. Current process cannot write to destination directory (access(W_OK) yields EACCES or EPERM).
        QString dirPath = destinationDirectoryPath;
        if (dirPath.isEmpty() && !targetFilePath.isEmpty()) {
            dirPath = QFileInfo(targetFilePath).absolutePath();
        }
        if (dirPath.isEmpty() && job && !job->errorText().isEmpty()) {
            dirPath = QFileInfo(job->errorText()).absolutePath();
        }

        if (!dirPath.isEmpty()) {
            const AdminLocalIdentity dirId = AdminLocalIdentity::read(dirPath);
            if (dirId.exists && dirId.isDirectory && !dirId.isSymLink && !dirId.isBrokenSymLink && !hasSymlinkInPathComponents(dirPath)) {
                if (isLocalFilesystemReadOnly(dirPath)) {
                    opError.category = AdminErrorCategory::ReadOnlyFilesystem;
                    return opError;
                }

                struct statvfs fs {};
                if (::statvfs(QFile::encodeName(dirPath).constData(), &fs) == 0 && fs.f_bavail == 0) {
                    opError.category = AdminErrorCategory::DiskFull;
                    return opError;
                }

                if (!targetFilePath.isEmpty() && AdminLocalIdentity::read(targetFilePath).exists) {
                    opError.category = AdminErrorCategory::Collision;
                    return opError;
                }
                const QString partPath = !partFilePath.isEmpty()
                    ? partFilePath
                    : (!targetFilePath.isEmpty() ? targetFilePath + QLatin1String(".part") : (job ? job->errorText() : QString()));
                if (!partPath.isEmpty() && AdminLocalIdentity::read(partPath).exists) {
                    opError.category = AdminErrorCategory::Collision;
                    return opError;
                }

                errno = 0;
                if (::access(QFile::encodeName(dirPath).constData(), W_OK) != 0) {
                    const int err = errno;
                    if (err == EACCES || err == EPERM) {
                        opError.posixErrno = err;
                        opError.category = isPrivileged
                            ? AdminErrorCategory::AuthorizationCancelledOrDenied
                            : AdminErrorCategory::WriteAccessDenied;
                        return opError;
                    }
                }
            }
        }

        opError.category = AdminErrorCategory::UnverifiableOrUnknown;
        break;
    }

    default:
        opError.category = opError.posixErrno != 0
            ? (opError.posixErrno == EACCES || opError.posixErrno == EPERM
                   ? (isPrivileged ? AdminErrorCategory::AuthorizationCancelledOrDenied : AdminErrorCategory::PermissionDenied)
                   : AdminErrorCategory::UnverifiableOrUnknown)
            : AdminErrorCategory::UnverifiableOrUnknown;
        break;
    }

    return opError;
}

class AdminRetryPolicy
{
public:
    static AdminRetryQualification evaluate(
        const AdminRetryRequest &request,
        bool isKioAdminAvailable = true)
    {
        AdminRetryQualification q;
        q.isReversible = false;

        if (request.wasCancelled || request.error.category == AdminErrorCategory::UserCancelled) {
            q.eligibility = AdminRetryEligibility::Ineligible;
            q.reason = AdminRetryIneligibilityReason::UserCancelled;
            q.message = trLocal(
                "Operacja została anulowana przez użytkownika.",
                "Operation was cancelled by the user.");
            return q;
        }

        if (request.isGuiRequest && !isGuiAdminRetryAllowed(request.operationType)) {
            q.eligibility = AdminRetryEligibility::Ineligible;
            q.reason = AdminRetryIneligibilityReason::OperationDisallowedInGui;
            q.message = trLocal(
                "Ta operacja nie jest dozwolona do ponowienia w trybie graficznym administratora.",
                "This operation is not permitted for GUI administrator retry.");
            return q;
        }

        if (request.alreadyRetriedAsAdmin) {
            q.eligibility = AdminRetryEligibility::Ineligible;
            q.reason = AdminRetryIneligibilityReason::AlreadyRetriedAsAdmin;
            q.message = trLocal(
                "Operacja była już ponawiana jako administrator (blokada pętli ponowień).",
                "Operation was already retried as administrator (retry loop prevention).");
            return q;
        }

        if (!request.error.isPermissionError()) {
            q.eligibility = AdminRetryEligibility::Ineligible;
            if (request.error.category == AdminErrorCategory::DiskFull) {
                q.reason = AdminRetryIneligibilityReason::DiskFull;
                q.message = trLocal(
                    "Brak miejsca na dysku. Uprawnienia administratora nie rozwiążą problemu.",
                    "Disk is full. Administrator privileges cannot resolve this error.");
            } else if (request.error.category == AdminErrorCategory::ReadOnlyFilesystem) {
                q.reason = AdminRetryIneligibilityReason::ReadOnlyFilesystem;
                q.message = trLocal(
                    "Docelowy system plików jest zamontowany w trybie tylko do odczytu.",
                    "Target filesystem is mounted read-only.");
            } else if (request.error.category == AdminErrorCategory::Collision) {
                q.reason = AdminRetryIneligibilityReason::TargetCollision;
                q.message = trLocal(
                    "Element docelowy o tej nazwie już istnieje.",
                    "A destination item with this name already exists.");
            } else if (request.error.category == AdminErrorCategory::PathNotFound) {
                q.reason = AdminRetryIneligibilityReason::SourceMissing;
                q.message = trLocal(
                    "Element źródłowy lub ścieżka docelowa nie istnieje.",
                    "Source item or destination path does not exist.");
            } else {
                q.reason = AdminRetryIneligibilityReason::NonPermissionError;
                q.message = trLocal(
                    "Błąd operacji nie wynika z braku uprawnień.",
                    "The operation failure was not caused by permission denial.");
            }
            return q;
        }

        if (!isKioAdminAvailable) {
            q.eligibility = AdminRetryEligibility::Ineligible;
            q.reason = AdminRetryIneligibilityReason::MissingAdminWorker;
            q.message = trLocal(
                "Brak protokołu KIO admin:// w systemie (wymagany pakiet kio-admin).",
                "KIO admin:// protocol is missing from the system (kio-admin package required).");
            return q;
        }

        if (request.operationType == FileOperationType::Trash
            || request.backend == AdminOperationBackend::KioTrashJob
            || (request.destinationDirectory.isValid() && request.destinationDirectory.scheme() == QStringLiteral("trash"))) {
            q.eligibility = AdminRetryEligibility::UnsupportedBackend;
            q.reason = AdminRetryIneligibilityReason::TrashProtocolUnsupported;
            q.message = trLocal(
                "Kosz (trash:/) nie obsługuje operacji z uprawnieniami administratora.",
                "Trash protocol (trash:/) does not support administrator operations.");
            return q;
        }

        auto isLocalOrAdmin = [](const QUrl &url) {
            if (!url.isValid()) return false;
            return url.isLocalFile() || isAdminUrl(url);
        };

        for (const QUrl &src : request.sourceUrls) {
            if (!isLocalOrAdmin(src)) {
                q.eligibility = AdminRetryEligibility::UnsupportedBackend;
                q.reason = AdminRetryIneligibilityReason::NonLocalScheme;
                q.message = trLocal(
                    "Operacje na zasobach zdalnych lub urządzeniach MTP nie obsługują uprawnień lokalnego administratora.",
                    "Remote locations and MTP devices do not support local administrator privileges.");
                return q;
            }
        }

        if (request.destinationDirectory.isValid() && !isLocalOrAdmin(request.destinationDirectory)) {
            q.eligibility = AdminRetryEligibility::UnsupportedBackend;
            q.reason = AdminRetryIneligibilityReason::NonLocalScheme;
            q.message = trLocal(
                "Katalog docelowy nie jest lokalnym systemem plików.",
                "Destination directory is not a local filesystem.");
            return q;
        }

        const QString destLocalPath = localPathForFileOrAdmin(request.destinationDirectory);
        if (!destLocalPath.isEmpty() && isLocalFilesystemReadOnly(destLocalPath)) {
            q.eligibility = AdminRetryEligibility::Ineligible;
            q.reason = AdminRetryIneligibilityReason::ReadOnlyFilesystem;
            q.message = trLocal(
                "Docelowy system plików jest zamontowany w trybie tylko do odczytu (read-only).",
                "Destination filesystem is mounted in read-only mode.");
            return q;
        }

        if (request.partialState == AdminPartialCompletionState::UnknownState
            || request.partialState == AdminPartialCompletionState::PartialFileCreated) {
            q.eligibility = AdminRetryEligibility::Ineligible;
            q.reason = AdminRetryIneligibilityReason::UnverifiablePartialState;
            q.message = trLocal(
                "Stan częściowego wykonania operacji nie kwalifikuje się do bezpiecznego ponowienia (zasada fail-closed).",
                "Partial execution state of the operation is not eligible for safe retry (fail-closed policy).");
            return q;
        }

        if (request.partialState == AdminPartialCompletionState::PartiallyCompleted) {
            q.eligibility = AdminRetryEligibility::RequiresInspection;
            q.reason = AdminRetryIneligibilityReason::UnverifiablePartialState;
            q.message = trLocal(
                "Operacja została częściowo wykonana; wymaga inspekcji przed ponowieniem.",
                "Operation was partially completed; inspection required before retry.");
            return q;
        }

        if (!request.sourceIdentities.isEmpty()) {
            for (int i = 0; i < request.sourceUrls.size() && i < request.sourceIdentities.size(); ++i) {
                const QString path = localPathForFileOrAdmin(request.sourceUrls.at(i));
                if (!path.isEmpty() && !request.sourceIdentities.at(i).matches(path)) {
                    q.eligibility = AdminRetryEligibility::Ineligible;
                    q.reason = AdminRetryIneligibilityReason::SourceIdentityChanged;
                    q.message = trLocal(
                        "Plik źródłowy uległ zmianie po wystąpieniu błędu (wykryto TOCTOU).",
                        "Source file changed after failure occurred (TOCTOU detected).");
                    return q;
                }
            }
        }

        if (request.destinationDirectoryIdentity.exists && !destLocalPath.isEmpty()) {
            if (!request.destinationDirectoryIdentity.matches(destLocalPath)) {
                q.eligibility = AdminRetryEligibility::Ineligible;
                q.reason = AdminRetryIneligibilityReason::DestinationIdentityChanged;
                q.message = trLocal(
                    "Katalog docelowy uległ zmianie (wykryto TOCTOU).",
                    "Destination directory changed (TOCTOU detected).");
                return q;
            }
        }

        if (request.targetItemIdentity.exists && !destLocalPath.isEmpty()) {
            const QString targetPath = !request.requestedName.isEmpty()
                ? QDir(destLocalPath).filePath(request.requestedName)
                : destLocalPath;
            if (!request.targetItemIdentity.matches(targetPath)) {
                q.eligibility = AdminRetryEligibility::Ineligible;
                q.reason = AdminRetryIneligibilityReason::DestinationIdentityChanged;
                q.message = trLocal(
                    "Element docelowy uległ zmianie (wykryto TOCTOU).",
                    "Destination item changed (TOCTOU detected).");
                return q;
            }
        }

        if (!destLocalPath.isEmpty()) {
            for (const QUrl &srcUrl : request.sourceUrls) {
                const QString srcPath = localPathForFileOrAdmin(srcUrl);
                if (!srcPath.isEmpty() && isDestinationInsideSource(destLocalPath, srcPath)) {
                    q.eligibility = AdminRetryEligibility::Ineligible;
                    q.reason = AdminRetryIneligibilityReason::TargetInsideSourceDirectory;
                    q.message = trLocal(
                        "Katalog docelowy znajduje się wewnątrz folderu źródłowego.",
                        "Destination directory is inside the source folder.");
                    return q;
                }
            }
        }

        for (const QUrl &srcUrl : request.sourceUrls) {
            const QString srcPath = localPathForFileOrAdmin(srcUrl);
            if (!srcPath.isEmpty()) {
                const AdminLocalIdentity id = AdminLocalIdentity::read(srcPath);
                if (id.isBrokenSymLink) {
                    q.eligibility = AdminRetryEligibility::Ineligible;
                    q.reason = AdminRetryIneligibilityReason::UnsafeSymlinkTraversal;
                    q.message = trLocal(
                        "Wykryto uszkodzone dowiązanie symboliczne (broken symlink).",
                        "Broken symbolic link detected.");
                    return q;
                }
            }
        }

        if (request.sourceUrls.size() > 1) {
            q.eligibility = AdminRetryEligibility::RequiresInspection;
            q.reason = AdminRetryIneligibilityReason::BatchOperationRequiresInspection;
            q.message = trLocal(
                "Operacja wieloplikowa wymaga weryfikacji postępu i inspekcji (Stage 5).",
                "Batch operation requires progress verification and inspection (Stage 5).");
            return q;
        }

        if (request.operationType == FileOperationType::Delete) {
            q.eligibility = AdminRetryEligibility::RequiresInspection;
            q.reason = AdminRetryIneligibilityReason::DeletionRequiresInspection;
            q.message = trLocal(
                "Trwałe usuwanie z prawami administratora wymaga dodatkowego potwierdzenia (Stage 5).",
                "Permanent deletion as administrator requires explicit confirmation (Stage 5).");
            return q;
        }

        if (request.operationType == FileOperationType::Rename && !request.sourceUrls.isEmpty() && !destLocalPath.isEmpty()) {
            const QString srcPath = localPathForFileOrAdmin(request.sourceUrls.first());
            if (!srcPath.isEmpty()) {
                const QString srcParent = QDir::cleanPath(QFileInfo(srcPath).absolutePath());
                const QString destParent = QDir::cleanPath(destLocalPath);
                if (srcParent != destParent) {
                    q.eligibility = AdminRetryEligibility::Ineligible;
                    q.reason = AdminRetryIneligibilityReason::SameParentRequired;
                    q.message = trLocal(
                        "Zmiana nazwy musi odbywać się w tym samym katalogu nadrzędnym.",
                        "Rename operation must stay within the same parent directory.");
                    return q;
                }
            }
        }

        if (request.operationType == FileOperationType::Move && !request.sourceUrls.isEmpty() && !destLocalPath.isEmpty()) {
            const QString srcPath = localPathForFileOrAdmin(request.sourceUrls.first());
            if (!srcPath.isEmpty()) {
                const AdminLocalIdentity srcId = !request.sourceIdentities.isEmpty()
                    ? request.sourceIdentities.first()
                    : AdminLocalIdentity::read(srcPath);
                if (srcId.isDirectory) {
                    q.eligibility = AdminRetryEligibility::Ineligible;
                    q.reason = AdminRetryIneligibilityReason::DirectoryMoveUnsupported;
                    q.message = trLocal(
                        "Przenoszenie katalogu między różnymi lokalizacjami jest poza zakresem Stage 3.",
                        "Directory move across locations is out of scope in Stage 3.");
                    return q;
                }
                struct stat srcStat {}, destStat {};
                if (::stat(QFile::encodeName(srcPath).constData(), &srcStat) == 0
                    && ::stat(QFile::encodeName(destLocalPath).constData(), &destStat) == 0) {
                    if (srcStat.st_dev != destStat.st_dev) {
                        q.eligibility = AdminRetryEligibility::Ineligible;
                        q.reason = AdminRetryIneligibilityReason::CrossFilesystemMove;
                        q.message = trLocal(
                            "Przenoszenie między różnymi systemami plików jest niedozwolone w Stage 3 (EXDEV).",
                            "Cross-filesystem move is not allowed in Stage 3 (EXDEV).");
                        return q;
                    }
                }
            }
        }

        q.eligibility = AdminRetryEligibility::Eligible;
        q.reason = AdminRetryIneligibilityReason::None;
        q.message = trLocal(
            "Operacja kwalifikuje się do bezpiecznego ponowienia z uprawnieniami administratora.",
            "Operation is eligible for safe retry with administrator privileges.");
        return q;
    }
};
