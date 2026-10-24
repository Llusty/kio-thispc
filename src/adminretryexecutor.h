/*
 * Admin retry executor for ThisPC 0.43.0 (Stage 2).
 *
 * Implements safe, isolated execution of single-file copy and creation operations
 * targeting or sourcing protected locations via KDE admin:// with explicit user
 * consent, minimal privilege elevation, and pre-execution safety validation.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "adminretrycontract.h"
#include "browsercommon.h"

#include <KIO/CopyJob>
#include <KIO/FileCopyJob>
#include <KIO/Job>
#include <KIO/TransferJob>
#include <KIO/StoredTransferJob>
#include <KIO/MkdirJob>
#include <KProtocolInfo>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>

#include <functional>
#include <optional>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

enum class AdminPlanType {
    Invalid,
    CopyFileToProtected,
    CopyFileFromProtected,
    CreateFileProtected,
    CreateFolderProtected,
    RenameFileProtected,
    RenameFolderProtected,
    MoveFileSameFilesystem
};

enum class AdminPlanValidationResult {
    Valid,
    MissingConsent,
    ConsentExpired,
    ConsentMismatch,
    ConsentAlreadyConsumed,
    AlreadyRetriedAsAdmin,
    MissingAdminWorker,
    UnsupportedPlanType,
    InvalidPrivilegeMapping,
    SourceMissing,
    SourceNotRegularFile,
    SourceNotDirectory,
    SourceIsDirectory,
    SourceIsSymlink,
    SourceIdentityChanged,
    SourceSymlinkInComponents,
    DestinationMissing,
    DestinationNotDirectory,
    DestinationIsSymlink,
    DestinationIdentityChanged,
    DestinationSymlinkInComponents,
    DestinationReadOnly,
    TargetCollision,
    TargetInsideSource,
    InsufficientDiskSpace,
    UnverifiablePartialState,
    CrossFilesystemMove,
    SameParentRequired,
    DirectoryMoveUnsupported
};

struct AdminValidationStatus {
    AdminPlanValidationResult code = AdminPlanValidationResult::Valid;
    QString message;

    bool isValid() const
    {
        return code == AdminPlanValidationResult::Valid;
    }
};

struct AdminExecutionPlan {
    QString planId;
    AdminPlanType planType = AdminPlanType::Invalid;
    QUrl sourceUrl;
    QUrl destinationDirectory;
    QUrl destinationUrl;
    QUrl effectiveSourceUrl;
    QUrl effectiveDestinationUrl;
    QString destinationDirectoryPath;
    QString requestedItemName;
    QByteArray fileContents;
    AdminLocalIdentity sourceIdentity;
    AdminLocalIdentity destinationDirectoryIdentity;
    AdminLocalIdentity targetItemIdentity;
    QDateTime createdAt;
    bool consumed = false;
    int retryCount = 0;
    AdminPartialCompletionState partialState = AdminPartialCompletionState::NotStarted;
    bool recordsUndo = false;
};

struct AdminUserConsent {
    QString consentId;
    QString planId;
    QUrl confirmedDestinationUrl;
    QDateTime grantedAt;
    qint64 validityDurationMs = 60000;
    bool consumed = false;

    static AdminUserConsent create(const AdminExecutionPlan &plan, qint64 validityMs = 60000)
    {
        AdminUserConsent consent;
        consent.consentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        consent.planId = plan.planId;
        consent.confirmedDestinationUrl = plan.destinationUrl.isValid() ? plan.destinationUrl : plan.effectiveDestinationUrl;
        consent.grantedAt = QDateTime::currentDateTimeUtc();
        consent.validityDurationMs = validityMs;
        consent.consumed = false;
        return consent;
    }

    bool isExpired(const QDateTime &now = QDateTime::currentDateTimeUtc()) const
    {
        if (!grantedAt.isValid()) {
            return true;
        }
        const qint64 elapsed = grantedAt.msecsTo(now);
        return elapsed < 0 || elapsed > validityDurationMs;
    }
};

#ifndef STATX_MNT_ID
#define STATX_MNT_ID 0x00001000U
#endif

inline std::optional<uint64_t> getLinuxMountId(const QString &path)
{
    if (path.isEmpty()) {
        return std::nullopt;
    }
    const QByteArray encoded = QFile::encodeName(path);
    struct statx stx {};
    if (::statx(AT_FDCWD, encoded.constData(), AT_STATX_SYNC_AS_STAT, STATX_MNT_ID, &stx) == 0) {
        if ((stx.stx_mask & STATX_MNT_ID) != 0) {
            return static_cast<uint64_t>(stx.stx_mnt_id);
        }
    }
    return std::nullopt;
}

using AdminStatvfsChecker = std::function<bool(const QString &path, struct statvfs *out)>;
using AdminMountIdChecker = std::function<std::optional<uint64_t>(const QString &path)>;

class AdminPlanValidator
{
public:
    static AdminValidationStatus validate(
        const AdminExecutionPlan &plan,
        const AdminUserConsent &consent,
        bool isKioAdminAvailable = true,
        const QDateTime &currentTime = QDateTime::currentDateTimeUtc(),
        AdminStatvfsChecker statvfsOverride = nullptr,
        AdminMountIdChecker mountIdOverride = nullptr)
    {
        AdminValidationStatus status;

        if (plan.consumed) {
            status.code = AdminPlanValidationResult::ConsentAlreadyConsumed;
            status.message = trLocal(
                "Plan wykonania został już wykorzystany.",
                "Execution plan has already been consumed.");
            return status;
        }

        if (plan.retryCount > 0) {
            status.code = AdminPlanValidationResult::AlreadyRetriedAsAdmin;
            status.message = trLocal(
                "Operacja była już ponawiana jako administrator (blokada pętli ponowień).",
                "Operation was already retried as administrator (retry loop prevention).");
            return status;
        }

        if (consent.consumed) {
            status.code = AdminPlanValidationResult::ConsentAlreadyConsumed;
            status.message = trLocal(
                "Zgoda użytkownika została już wykorzystana.",
                "User consent has already been consumed.");
            return status;
        }

        if (consent.planId.isEmpty() || consent.planId != plan.planId) {
            status.code = AdminPlanValidationResult::ConsentMismatch;
            status.message = trLocal(
                "Identyfikator planu zgody nie pasuje do operacji.",
                "Consent plan identifier does not match operation.");
            return status;
        }

        if (consent.isExpired(currentTime)) {
            status.code = AdminPlanValidationResult::ConsentExpired;
            status.message = trLocal(
                "Zgoda użytkownika wygasła.",
                "User consent has expired.");
            return status;
        }

        const QString consentDest = localPathForFileOrAdmin(consent.confirmedDestinationUrl);
        const QString planDest = localPathForFileOrAdmin(plan.destinationUrl.isValid() ? plan.destinationUrl : plan.effectiveDestinationUrl);
        if (consentDest.isEmpty() || planDest.isEmpty() || QDir::cleanPath(consentDest) != QDir::cleanPath(planDest)) {
            status.code = AdminPlanValidationResult::ConsentMismatch;
            status.message = trLocal(
                "Cel w zgodzie użytkownika różni się od celu planu.",
                "Consent destination differs from plan destination.");
            return status;
        }

        if (!isKioAdminAvailable) {
            status.code = AdminPlanValidationResult::MissingAdminWorker;
            status.message = trLocal(
                "Protokół admin:// jest niedostępny (kio-admin nie jest zainstalowany).",
                "Protocol admin:// is unavailable (kio-admin is not installed).");
            return status;
        }

        if (plan.planType == AdminPlanType::Invalid) {
            status.code = AdminPlanValidationResult::UnsupportedPlanType;
            status.message = trLocal(
                "Nieobsługiwany typ operacji w Stage 2.",
                "Unsupported operation type in Stage 2.");
            return status;
        }

        if (plan.partialState != AdminPartialCompletionState::NotStarted) {
            status.code = AdminPlanValidationResult::UnverifiablePartialState;
            status.message = trLocal(
                "Operacja w stanie częściowym lub nieznanym nie kwalifikuje się do bezpiecznego ponowienia.",
                "Partial or unknown operation state is not eligible for safe retry.");
            return status;
        }

        // Minimal privilege validation:
        if (plan.planType == AdminPlanType::CopyFileToProtected) {
            if (plan.effectiveSourceUrl.scheme() != QLatin1String("file")
                || plan.effectiveDestinationUrl.scheme() != QLatin1String("admin")) {
                status.code = AdminPlanValidationResult::InvalidPrivilegeMapping;
                status.message = trLocal(
                    "Naruszenie zasady minimalnych uprawnień dla kopiowania do katalogu chronionego.",
                    "Minimal privilege violation for copy to protected directory.");
                return status;
            }
        } else if (plan.planType == AdminPlanType::CopyFileFromProtected) {
            if (plan.effectiveSourceUrl.scheme() != QLatin1String("admin")
                || plan.effectiveDestinationUrl.scheme() != QLatin1String("file")) {
                status.code = AdminPlanValidationResult::InvalidPrivilegeMapping;
                status.message = trLocal(
                    "Naruszenie zasady minimalnych uprawnień dla kopiowania z katalogu chronionego.",
                    "Minimal privilege violation for copy from protected directory.");
                return status;
            }
        } else if (plan.planType == AdminPlanType::CreateFileProtected || plan.planType == AdminPlanType::CreateFolderProtected) {
            if (plan.effectiveDestinationUrl.scheme() != QLatin1String("admin")) {
                status.code = AdminPlanValidationResult::InvalidPrivilegeMapping;
                status.message = trLocal(
                    "Naruszenie zasady minimalnych uprawnień dla tworzenia elementu chronionego.",
                    "Minimal privilege violation for protected creation.");
                return status;
            }
        } else if (plan.planType == AdminPlanType::RenameFileProtected
            || plan.planType == AdminPlanType::RenameFolderProtected
            || plan.planType == AdminPlanType::MoveFileSameFilesystem) {
            if (plan.effectiveSourceUrl.scheme() != QLatin1String("admin")
                || plan.effectiveDestinationUrl.scheme() != QLatin1String("admin")) {
                status.code = AdminPlanValidationResult::InvalidPrivilegeMapping;
                status.message = trLocal(
                    "Operacje zmiany nazwy i przenoszenia administratora wymagają protokołu admin:// dla źródła i celu.",
                    "Administrator rename and move operations require admin:// protocol for both source and destination.");
                return status;
            }
        }

        // Destination directory validation:
        QString destDirPath = plan.destinationDirectoryPath.isEmpty()
            ? localPathForFileOrAdmin(plan.destinationDirectory)
            : plan.destinationDirectoryPath;
        if (destDirPath.isEmpty() && (plan.planType == AdminPlanType::RenameFileProtected || plan.planType == AdminPlanType::RenameFolderProtected)) {
            const QString srcP = localPathForFileOrAdmin(plan.sourceUrl.isValid() ? plan.sourceUrl : plan.effectiveSourceUrl);
            if (!srcP.isEmpty()) {
                destDirPath = QFileInfo(srcP).absolutePath();
            }
        }
        if (destDirPath.isEmpty()) {
            status.code = AdminPlanValidationResult::DestinationMissing;
            status.message = trLocal(
                "Brak ścieżki katalogu docelowego.",
                "Destination directory path is missing.");
            return status;
        }

        const AdminLocalIdentity destDirId = AdminLocalIdentity::read(destDirPath);
        if (!destDirId.exists) {
            status.code = AdminPlanValidationResult::DestinationMissing;
            status.message = trLocal(
                "Katalog docelowy nie istnieje.",
                "Destination directory does not exist.");
            return status;
        }
        if (!destDirId.isDirectory) {
            status.code = AdminPlanValidationResult::DestinationNotDirectory;
            status.message = trLocal(
                "Ścieżka docelowa nie jest katalogiem.",
                "Destination path is not a directory.");
            return status;
        }
        if (destDirId.isSymLink) {
            status.code = AdminPlanValidationResult::DestinationIsSymlink;
            status.message = trLocal(
                "Katalog docelowy jest dowiązaniem symbolicznym.",
                "Destination directory is a symlink.");
            return status;
        }
        if (plan.destinationDirectoryIdentity.exists && !plan.destinationDirectoryIdentity.matches(destDirPath)) {
            status.code = AdminPlanValidationResult::DestinationIdentityChanged;
            status.message = trLocal(
                "Tożsamość katalogu docelowego uległa zmianie (TOCTOU).",
                "Destination directory identity changed (TOCTOU).");
            return status;
        }
        if (hasSymlinkInPathComponents(destDirPath)) {
            status.code = AdminPlanValidationResult::DestinationSymlinkInComponents;
            status.message = trLocal(
                "Ścieżka docelowa zawiera dowiązanie symboliczne w komponentach ścieżki.",
                "Destination path contains a symlink in path components.");
            return status;
        }

        // Read-only filesystem check
        struct statvfs fs {};
        bool fsStatOk = false;
        if (statvfsOverride) {
            fsStatOk = statvfsOverride(destDirPath, &fs);
        } else {
            fsStatOk = (::statvfs(QFile::encodeName(destDirPath).constData(), &fs) == 0);
        }

        if (fsStatOk) {
            if ((fs.f_flag & ST_RDONLY) != 0) {
                status.code = AdminPlanValidationResult::DestinationReadOnly;
                status.message = trLocal(
                    "Docelowy system plików jest zamontowany tylko do odczytu.",
                    "Destination filesystem is mounted read-only.");
                return status;
            }
        }

        // Collision check (no auto-overwrite in Stage 2/3/4):
        const QString targetPath = !plan.requestedItemName.isEmpty()
            ? QDir(destDirPath).filePath(plan.requestedItemName)
            : localPathForFileOrAdmin(plan.effectiveDestinationUrl);
        if (!targetPath.isEmpty()) {
            const AdminLocalIdentity targetId = AdminLocalIdentity::read(targetPath);
            if (targetId.exists) {
                status.code = AdminPlanValidationResult::TargetCollision;
                status.message = trLocal(
                    "Element docelowy już istnieje (brak automatycznego nadpisywania).",
                    "Target item already exists (no automatic overwrite).");
                return status;
            }
            const QString partPath = targetPath + QLatin1String(".part");
            const AdminLocalIdentity partId = AdminLocalIdentity::read(partPath);
            if (partId.exists) {
                status.code = AdminPlanValidationResult::UnverifiablePartialState;
                status.message = trLocal(
                    "Wykryto istniejący plik częściowy (.part) w lokalizacji docelowej.",
                    "Existing partial file (.part) detected at destination.");
                return status;
            }
        }

        // Source validation for copy operations:
        if (plan.planType == AdminPlanType::CopyFileToProtected || plan.planType == AdminPlanType::CopyFileFromProtected) {
            const QString srcPath = localPathForFileOrAdmin(plan.sourceUrl);
            if (srcPath.isEmpty()) {
                status.code = AdminPlanValidationResult::SourceMissing;
                status.message = trLocal(
                    "Brak ścieżki źródłowej.",
                    "Source path is missing.");
                return status;
            }
            const AdminLocalIdentity srcId = AdminLocalIdentity::read(srcPath);
            if (!srcId.exists) {
                status.code = AdminPlanValidationResult::SourceMissing;
                status.message = trLocal(
                    "Plik źródłowy nie istnieje.",
                    "Source file does not exist.");
                return status;
            }
            if (srcId.isSymLink || srcId.isBrokenSymLink) {
                status.code = AdminPlanValidationResult::SourceIsSymlink;
                status.message = trLocal(
                    "Plik źródłowy jest dowiązaniem symbolicznym.",
                    "Source file is a symbolic link.");
                return status;
            }
            if (isDestinationInsideSource(destDirPath, srcPath)) {
                status.code = AdminPlanValidationResult::TargetInsideSource;
                status.message = trLocal(
                    "Katalog docelowy znajduje się wewnątrz folderu źródłowego.",
                    "Destination directory is inside source folder.");
                return status;
            }
            if (!srcId.isRegular) {
                status.code = AdminPlanValidationResult::SourceNotRegularFile;
                status.message = trLocal(
                    "Źródło nie jest zwykłym plikiem.",
                    "Source is not a regular file.");
                return status;
            }
            if (hasSymlinkInPathComponents(srcPath)) {
                status.code = AdminPlanValidationResult::SourceSymlinkInComponents;
                status.message = trLocal(
                    "Ścieżka źródłowa zawiera dowiązanie symboliczne w komponentach.",
                    "Source path contains a symlink in path components.");
                return status;
            }
            if (plan.sourceIdentity.exists && !plan.sourceIdentity.matches(srcPath)) {
                status.code = AdminPlanValidationResult::SourceIdentityChanged;
                status.message = trLocal(
                    "Tożsamość pliku źródłowego uległa zmianie (TOCTOU).",
                    "Source file identity changed (TOCTOU).");
                return status;
            }

            // Disk space check:
            const qint64 requiredBytes = srcId.statBuf.st_size;
            if (fsStatOk) {
                const qint64 availableBytes = static_cast<qint64>(fs.f_bavail) * static_cast<qint64>(fs.f_frsize);
                if (availableBytes < requiredBytes) {
                    status.code = AdminPlanValidationResult::InsufficientDiskSpace;
                    status.message = trLocal(
                        "Brak miejsca na dysku docelowym.",
                        "Insufficient disk space on destination.");
                    return status;
                }
            }
        }

        // Source and constraints validation for rename operations:
        if (plan.planType == AdminPlanType::RenameFileProtected || plan.planType == AdminPlanType::RenameFolderProtected) {
            const QString srcPath = localPathForFileOrAdmin(plan.sourceUrl.isValid() ? plan.sourceUrl : plan.effectiveSourceUrl);
            if (srcPath.isEmpty()) {
                status.code = AdminPlanValidationResult::SourceMissing;
                status.message = trLocal(
                    "Brak ścieżki źródłowej.",
                    "Source path is missing.");
                return status;
            }
            const AdminLocalIdentity srcId = AdminLocalIdentity::read(srcPath);
            if (!srcId.exists) {
                status.code = AdminPlanValidationResult::SourceMissing;
                status.message = trLocal(
                    "Element źródłowy nie istnieje.",
                    "Source item does not exist.");
                return status;
            }
            if (srcId.isSymLink || srcId.isBrokenSymLink) {
                status.code = AdminPlanValidationResult::SourceIsSymlink;
                status.message = trLocal(
                    "Element źródłowy jest dowiązaniem symbolicznym.",
                    "Source item is a symbolic link.");
                return status;
            }
            if (hasSymlinkInPathComponents(srcPath)) {
                status.code = AdminPlanValidationResult::SourceSymlinkInComponents;
                status.message = trLocal(
                    "Ścieżka źródłowa zawiera dowiązanie symboliczne w komponentach.",
                    "Source path contains a symlink in path components.");
                return status;
            }
            if (plan.sourceIdentity.exists && !plan.sourceIdentity.matches(srcPath)) {
                status.code = AdminPlanValidationResult::SourceIdentityChanged;
                status.message = trLocal(
                    "Tożsamość elementu źródłowego uległa zmianie (TOCTOU).",
                    "Source item identity changed (TOCTOU).");
                return status;
            }

            if (plan.planType == AdminPlanType::RenameFileProtected) {
                if (srcId.isDirectory) {
                    status.code = AdminPlanValidationResult::SourceIsDirectory;
                    status.message = trLocal(
                        "Element źródłowy jest katalogiem, oczekiwano pliku.",
                        "Source item is a directory, expected a file.");
                    return status;
                }
                if (!srcId.isRegular) {
                    status.code = AdminPlanValidationResult::SourceNotRegularFile;
                    status.message = trLocal(
                        "Źródło nie jest zwykłym plikiem.",
                        "Source is not a regular file.");
                    return status;
                }
            } else if (plan.planType == AdminPlanType::RenameFolderProtected) {
                if (!srcId.isDirectory) {
                    status.code = AdminPlanValidationResult::SourceNotDirectory;
                    status.message = trLocal(
                        "Element źródłowy nie jest katalogiem.",
                        "Source item is not a directory.");
                    return status;
                }
            }

            // Verify rename stays in same parent directory:
            const QString srcParent = QDir::cleanPath(QFileInfo(srcPath).absolutePath());
            const QString destParent = QDir::cleanPath(destDirPath);
            if (srcParent != destParent) {
                status.code = AdminPlanValidationResult::SameParentRequired;
                status.message = trLocal(
                    "Zmiana nazwy wymaga zachowania tego samego katalogu nadrzędnego.",
                    "Rename requires staying within the same parent directory.");
                return status;
            }
        }

        // Source and constraints validation for same-filesystem move:
        if (plan.planType == AdminPlanType::MoveFileSameFilesystem) {
            const QString srcPath = localPathForFileOrAdmin(plan.sourceUrl.isValid() ? plan.sourceUrl : plan.effectiveSourceUrl);
            if (srcPath.isEmpty()) {
                status.code = AdminPlanValidationResult::SourceMissing;
                status.message = trLocal(
                    "Brak ścieżki źródłowej.",
                    "Source path is missing.");
                return status;
            }
            const AdminLocalIdentity srcId = AdminLocalIdentity::read(srcPath);
            if (!srcId.exists) {
                status.code = AdminPlanValidationResult::SourceMissing;
                status.message = trLocal(
                    "Plik źródłowy nie istnieje.",
                    "Source file does not exist.");
                return status;
            }
            if (srcId.isSymLink || srcId.isBrokenSymLink) {
                status.code = AdminPlanValidationResult::SourceIsSymlink;
                status.message = trLocal(
                    "Plik źródłowy jest dowiązaniem symbolicznym.",
                    "Source file is a symbolic link.");
                return status;
            }
            if (hasSymlinkInPathComponents(srcPath)) {
                status.code = AdminPlanValidationResult::SourceSymlinkInComponents;
                status.message = trLocal(
                    "Ścieżka źródłowa zawiera dowiązanie symboliczne w komponentach.",
                    "Source path contains a symlink in path components.");
                return status;
            }
            if (srcId.isDirectory) {
                status.code = AdminPlanValidationResult::DirectoryMoveUnsupported;
                status.message = trLocal(
                    "Przenoszenie katalogów między folderami nie jest obsługiwane w Stage 3.",
                    "Moving directories across folders is not supported in Stage 3.");
                return status;
            }
            if (!srcId.isRegular) {
                status.code = AdminPlanValidationResult::SourceNotRegularFile;
                status.message = trLocal(
                    "Źródło nie jest zwykłym plikiem.",
                    "Source is not a regular file.");
                return status;
            }
            if (plan.sourceIdentity.exists && !plan.sourceIdentity.matches(srcPath)) {
                status.code = AdminPlanValidationResult::SourceIdentityChanged;
                status.message = trLocal(
                    "Tożsamość pliku źródłowego uległa zmianie (TOCTOU).",
                    "Source file identity changed (TOCTOU).");
                return status;
            }
            if (isDestinationInsideSource(destDirPath, srcPath)) {
                status.code = AdminPlanValidationResult::TargetInsideSource;
                status.message = trLocal(
                    "Katalog docelowy znajduje się wewnątrz ścieżki źródłowej.",
                    "Destination directory is inside source path.");
                return status;
            }

            // Same filesystem verification via st_dev:
            if (srcId.statBuf.st_dev != destDirId.statBuf.st_dev) {
                status.code = AdminPlanValidationResult::CrossFilesystemMove;
                status.message = trLocal(
                    "Przenoszenie między różnymi systemami plików jest zablokowane (różne st_dev).",
                    "Cross-filesystem move is blocked (different st_dev).");
                return status;
            }

            // Same filesystem verification via statx mount ID:
            const std::optional<uint64_t> srcMnt = mountIdOverride ? mountIdOverride(srcPath) : getLinuxMountId(srcPath);
            const std::optional<uint64_t> destMnt = mountIdOverride ? mountIdOverride(destDirPath) : getLinuxMountId(destDirPath);
            if (srcMnt.has_value() && destMnt.has_value() && *srcMnt != *destMnt) {
                status.code = AdminPlanValidationResult::CrossFilesystemMove;
                status.message = trLocal(
                    "Przenoszenie między różnymi punktami montowania jest zablokowane (wykryto zmianę mount ID).",
                    "Cross-mount move is blocked (different mount ID detected).");
                return status;
            }
        }

        status.code = AdminPlanValidationResult::Valid;
        status.message = trLocal(
            "Plan wykonania jest prawidłowy i bezpieczny.",
            "Execution plan is valid and safe.");
        return status;
    }
};

class AdminRetryExecutor : public QObject
{
    Q_OBJECT

public:
    using MockJobFactory = std::function<KJob*(const AdminExecutionPlan &)>;
    using ArtifactIdentityReader = std::function<AdminLocalIdentity(const QString &)>;

    explicit AdminRetryExecutor(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~AdminRetryExecutor() override
    {
        cancelQuietly();
    }

    static int defaultPermissionsForPlan(AdminPlanType type)
    {
        switch (type) {
        case AdminPlanType::CopyFileFromProtected:
            // Mode 0600 (S_IRUSR | S_IWUSR): Ensure least-privilege destination access
            // so sensitive data copied from an administrative/protected source is
            // readable/writable only by the destination owner and never exposed to other local users.
            return 0600;
        default:
            return -1;
        }
    }

    static int activeTargetCount(const QString &path)
    {
        if (path.isEmpty()) {
            return 0;
        }
        const QString clean = QDir::cleanPath(path);
        QMutexLocker locker(&activeTargetsMutex());
        return activeTargetsMap().value(clean, 0);
    }

    static void acquireTargetPath(const QString &path)
    {
        if (path.isEmpty()) {
            return;
        }
        const QString clean = QDir::cleanPath(path);
        QMutexLocker locker(&activeTargetsMutex());
        activeTargetsMap()[clean] = activeTargetsMap().value(clean, 0) + 1;
    }

    static void releaseTargetPath(const QString &path)
    {
        if (path.isEmpty()) {
            return;
        }
        const QString clean = QDir::cleanPath(path);
        QMutexLocker locker(&activeTargetsMutex());
        const int current = activeTargetsMap().value(clean, 0);
        if (current <= 1) {
            activeTargetsMap().remove(clean);
        } else {
            activeTargetsMap()[clean] = current - 1;
        }
    }

    int lastKioErrorCode() const
    {
        return m_lastKioErrorCode;
    }

    AdminTransferFailureState lastFailureState() const
    {
        return m_lastFailureState;
    }

    AdminArtifactOwnershipAudit lastArtifactAudit() const
    {
        return m_lastArtifactAudit;
    }

    bool isRunning() const
    {
        return m_currentJob != nullptr;
    }

    KJob *currentJob() const
    {
        return m_currentJob;
    }

    AdminExecutionPlan activePlan() const
    {
        return m_activePlan;
    }

    void setMockJobFactory(MockJobFactory factory)
    {
        m_mockJobFactory = std::move(factory);
    }

    void setArtifactIdentityReaderOverride(ArtifactIdentityReader reader)
    {
        m_artifactIdentityReaderOverride = std::move(reader);
    }

    void recordObservedArtifactIdentity(const AdminLocalIdentity &identity)
    {
        m_observedArtifactIdentity = identity;
    }

    void setKioAdminAvailableOverride(bool available)
    {
        m_kioAdminAvailableOverride = available;
    }

    void setStatvfsOverride(AdminStatvfsChecker checker)
    {
        m_statvfsOverride = std::move(checker);
    }

    void setMountIdOverride(AdminMountIdChecker checker)
    {
        m_mountIdOverride = std::move(checker);
    }

    bool execute(AdminExecutionPlan &plan, AdminUserConsent &consent)
    {
        if (m_currentJob) {
            Q_EMIT finished(false, trLocal(
                "Inna operacja administratora jest już w toku.",
                "Another administrator operation is already in progress."),
                AdminErrorCategory::UnverifiableOrUnknown);
            return false;
        }

        const QString candidateDestPath = localPathForFileOrAdmin(plan.destinationUrl.isValid()
            ? plan.destinationUrl
            : plan.effectiveDestinationUrl);
        const QString cleanDestPath = candidateDestPath.isEmpty() ? QString() : QDir::cleanPath(candidateDestPath);

        // Criterion 8: Check if another task is already using the same destination path
        if (!cleanDestPath.isEmpty() && activeTargetCount(cleanDestPath) > 0) {
            m_lastArtifactAudit = {};
            m_lastArtifactAudit.check8_noOtherTaskUsingTarget = false;
            m_lastArtifactAudit.primaryReason = AdminArtifactRejectionReason::TargetUsedByAnotherTask;
            m_lastFailureState = AdminTransferFailureState::UnknownCompletionState;
            Q_EMIT finished(false, trLocal(
                "Inne zadanie korzysta już z tej samej ścieżki docelowej.",
                "Another task is already using the same destination path."),
                AdminErrorCategory::Collision);
            return false;
        }

        const bool kioAdminAvailable = m_kioAdminAvailableOverride.has_value()
            ? m_kioAdminAvailableOverride.value()
            : KProtocolInfo::isKnownProtocol(QStringLiteral("admin"));

        const AdminValidationStatus status = AdminPlanValidator::validate(
            plan,
            consent,
            kioAdminAvailable,
            QDateTime::currentDateTimeUtc(),
            m_statvfsOverride,
            m_mountIdOverride);

        if (!status.isValid()) {
            m_lastArtifactAudit = {};
            if (status.code == AdminPlanValidationResult::TargetCollision) {
                m_lastArtifactAudit.artifactExists = true;
                m_lastArtifactAudit.artifactPreserved = true;
                m_lastArtifactAudit.preservedArtifactPath = candidateDestPath;
                m_lastArtifactAudit.check1_notExistedBefore = false;
                m_lastArtifactAudit.primaryReason = AdminArtifactRejectionReason::TargetExistedBeforeStart;
                m_lastFailureState = AdminTransferFailureState::UnknownCompletionState;
            } else if (status.code == AdminPlanValidationResult::UnverifiablePartialState) {
                m_lastArtifactAudit.artifactExists = true;
                m_lastArtifactAudit.artifactPreserved = true;
                m_lastArtifactAudit.preservedArtifactPath = candidateDestPath + QStringLiteral(".part");
                m_lastArtifactAudit.check9_noPartialOrAmbiguousState = false;
                m_lastArtifactAudit.primaryReason = AdminArtifactRejectionReason::AmbiguousOrPartialState;
                m_lastFailureState = AdminTransferFailureState::UnknownCompletionState;
            } else if (status.code == AdminPlanValidationResult::DestinationIdentityChanged) {
                m_lastArtifactAudit.check5_parentDirUnchanged = false;
                m_lastArtifactAudit.primaryReason = AdminArtifactRejectionReason::ParentDirectoryChanged;
                m_lastFailureState = AdminTransferFailureState::UnknownCompletionState;
            }

            AdminErrorCategory errCat = AdminErrorCategory::UnverifiableOrUnknown;
            switch (status.code) {
            case AdminPlanValidationResult::MissingConsent:
            case AdminPlanValidationResult::ConsentExpired:
            case AdminPlanValidationResult::ConsentMismatch:
            case AdminPlanValidationResult::ConsentAlreadyConsumed:
            case AdminPlanValidationResult::AlreadyRetriedAsAdmin:
                errCat = AdminErrorCategory::AuthorizationCancelledOrDenied;
                break;
            case AdminPlanValidationResult::MissingAdminWorker:
            case AdminPlanValidationResult::UnsupportedPlanType:
            case AdminPlanValidationResult::InvalidPrivilegeMapping:
            case AdminPlanValidationResult::CrossFilesystemMove:
            case AdminPlanValidationResult::SameParentRequired:
            case AdminPlanValidationResult::DirectoryMoveUnsupported:
            case AdminPlanValidationResult::SourceNotDirectory:
            case AdminPlanValidationResult::SourceIsDirectory:
            case AdminPlanValidationResult::SourceNotRegularFile:
                errCat = AdminErrorCategory::UnsupportedBackendOrScheme;
                break;
            case AdminPlanValidationResult::SourceMissing:
            case AdminPlanValidationResult::DestinationMissing:
                errCat = AdminErrorCategory::PathNotFound;
                break;
            case AdminPlanValidationResult::DestinationReadOnly:
                errCat = AdminErrorCategory::ReadOnlyFilesystem;
                break;
            case AdminPlanValidationResult::TargetCollision:
                errCat = AdminErrorCategory::Collision;
                break;
            case AdminPlanValidationResult::InsufficientDiskSpace:
                errCat = AdminErrorCategory::DiskFull;
                break;
            default:
                errCat = AdminErrorCategory::PermissionDenied;
                break;
            }

            Q_EMIT finished(false, status.message, errCat);
            return false;
        }

        // Mark plan and consent as consumed immediately to prevent replay
        plan.consumed = true;
        consent.consumed = true;
        plan.retryCount++;
        m_activePlan = plan;
        m_lastKioErrorCode = 0;
        m_lastFailureState = AdminTransferFailureState::None;
        m_lastArtifactAudit = {};
        m_observedArtifactIdentity = {};

        if (!cleanDestPath.isEmpty()) {
            acquireTargetPath(cleanDestPath);
            m_registeredTargetPath = cleanDestPath;
        }

        KJob *job = nullptr;
        if (m_mockJobFactory) {
            job = m_mockJobFactory(plan);
        } else {
            job = createKJobForPlan(plan);
        }

        if (!job) {
            cleanupCurrentJob();
            Q_EMIT finished(false, trLocal(
                "Nie udało się zainicjować zadania KIO dla operacji administratora.",
                "Failed to initialize KIO job for administrator operation."),
                AdminErrorCategory::UnverifiableOrUnknown);
            return false;
        }

        m_currentJob = job;
        m_currentJob->setProperty("thispcAdminPlanId", plan.planId);
        m_currentJob->setProperty("thispcPrivilegedOperation", true);
        if (plan.sourceUrl.isValid()) {
            m_currentJob->setProperty("thispcSourceUrls", QVariant::fromValue(QList<QUrl>{plan.sourceUrl}));
        }
        if (plan.destinationUrl.isValid()) {
            m_currentJob->setProperty("thispcDestinationUrl", plan.destinationUrl);
        }

        connect(job, &KJob::percentChanged, this, &AdminRetryExecutor::onJobPercent);
        connect(job, &KJob::processedAmountChanged, this, &AdminRetryExecutor::onJobProcessedAmount);
        connect(job, &KJob::result, this, &AdminRetryExecutor::onJobResult);
        connect(job, &QObject::destroyed, this, [this, job]() {
            if (m_currentJob == job) {
                m_currentJob = nullptr;
                cleanupCurrentJob();
            }
        });

        Q_EMIT started(plan.planId);

        if (m_mockJobFactory) {
            job->start();
        }

        return true;
    }

    void cancel()
    {
        if (!m_currentJob) {
            return;
        }
        KJob *job = m_currentJob;
        const QString destPath = localPathForFileOrAdmin(m_activePlan.destinationUrl.isValid()
            ? m_activePlan.destinationUrl
            : m_activePlan.effectiveDestinationUrl);
        const bool targetExistedBefore = m_activePlan.targetItemIdentity.exists;
        const AdminExecutionPlan planCopy = m_activePlan;

        job->kill(KJob::Quietly);
        m_lastFailureState = evaluateAndCleanupDestination(destPath, targetExistedBefore, planCopy, false, 0);
        cleanupCurrentJob();

        QString cancelMsg = trLocal(
            "Operacja administratora została anulowana.",
            "Administrator operation was cancelled.");
        if (m_lastArtifactAudit.artifactPreserved && !m_lastArtifactAudit.preservedArtifactPath.isEmpty()) {
            cancelMsg += trLocal(
                "\n\nW lokalizacji docelowej pozostał niezweryfikowany artefakt (%1). "
                "Ze względów bezpieczeństwa plik został zachowany i nie został automatycznie usunięty.",
                "\n\nAn unverified artifact remains at the destination (%1). "
                "For safety reasons, the file was preserved and not automatically deleted.")
                .arg(m_lastArtifactAudit.preservedArtifactPath);
        }

        Q_EMIT finished(false, cancelMsg, AdminErrorCategory::UserCancelled);
    }

Q_SIGNALS:
    void started(const QString &planId);
    void progress(int percent, qint64 processedBytes, qint64 totalBytes);
    void finished(bool success, const QString &errorMessage, AdminErrorCategory errorCategory);

private Q_SLOTS:
    void onJobPercent(KJob *job, unsigned long percent)
    {
        if (job != m_currentJob) {
            return;
        }
        Q_EMIT progress(static_cast<int>(percent), 0, 0);
    }

    void onJobProcessedAmount(KJob *job, KJob::Unit unit, qulonglong amount)
    {
        if (job != m_currentJob || unit != KJob::Bytes) {
            return;
        }
        const qulonglong total = job->totalAmount(KJob::Bytes);
        Q_EMIT progress(static_cast<int>(job->percent()), static_cast<qint64>(amount), static_cast<qint64>(total));
    }

    void onJobResult(KJob *job)
    {
        if (job != m_currentJob) {
            return;
        }

        m_lastKioErrorCode = job->error();
        const int errCode = job->error();
        const QString errStr = job->errorString();
        const AdminOperationError classified = classifyOperationError(job);

        const QString destPath = localPathForFileOrAdmin(m_activePlan.destinationUrl.isValid()
            ? m_activePlan.destinationUrl
            : m_activePlan.effectiveDestinationUrl);
        const bool targetExistedBefore = m_activePlan.targetItemIdentity.exists;
        const AdminExecutionPlan planCopy = m_activePlan;

        if (errCode == 0) {
            m_lastFailureState = AdminTransferFailureState::None;
            m_lastArtifactAudit = {};
            cleanupCurrentJob();
            Q_EMIT finished(true, QString(), AdminErrorCategory::None);
        } else {
            m_lastFailureState = evaluateAndCleanupDestination(destPath, targetExistedBefore, planCopy, true, errCode);
            cleanupCurrentJob();
            QString finalErr = errStr.isEmpty()
                ? ((classified.category == AdminErrorCategory::UserCancelled)
                    ? trLocal("Operacja administratora została anulowana.", "Administrator operation was cancelled.")
                    : trLocal("Błąd operacji administratora.", "Administrator operation failed."))
                : errStr;
            if (m_lastArtifactAudit.artifactPreserved && !m_lastArtifactAudit.preservedArtifactPath.isEmpty()) {
                finalErr += trLocal(
                    "\n\nW lokalizacji docelowej pozostał niezweryfikowany lub częściowy artefakt (%1). "
                    "Ze względów bezpieczeństwa plik został zachowany i nie został automatycznie usunięty.",
                    "\n\nAn unverified or partial artifact remains at the destination (%1). "
                    "For safety reasons, the file was preserved and not automatically deleted.")
                    .arg(m_lastArtifactAudit.preservedArtifactPath);
            }
            Q_EMIT finished(false, finalErr, classified.category);
        }
    }

private:
    static QMutex &activeTargetsMutex()
    {
        static QMutex mutex;
        return mutex;
    }

    static QHash<QString, int> &activeTargetsMap()
    {
        static QHash<QString, int> map;
        return map;
    }

    AdminLocalIdentity readIdentity(const QString &path) const
    {
        if (m_artifactIdentityReaderOverride) {
            return m_artifactIdentityReaderOverride(path);
        }
        return AdminLocalIdentity::read(path);
    }

    AdminTransferFailureState evaluateAndCleanupDestination(
        const QString &destPath,
        bool targetExistedBefore,
        const AdminExecutionPlan &plan,
        bool jobConfirmedTerminalFailure = true,
        int kioErrorCode = 0)
    {
        AdminArtifactOwnershipAudit audit;
        const QString cleanDest = destPath.isEmpty() ? QString() : QDir::cleanPath(destPath);
        const int ownReservation = (!m_registeredTargetPath.isEmpty() && m_registeredTargetPath == cleanDest) ? 1 : 0;
        const int otherTasks = activeTargetCount(cleanDest) - ownReservation;

        audit.check1_notExistedBefore = !targetExistedBefore && !plan.targetItemIdentity.exists;
        audit.check7_writesConfirmedStopped = jobConfirmedTerminalFailure;
        audit.check8_noOtherTaskUsingTarget = (otherTasks <= 0);

        const QString dirPath = !plan.destinationDirectoryPath.isEmpty()
            ? plan.destinationDirectoryPath
            : (destPath.isEmpty() ? QString() : QFileInfo(destPath).absolutePath());

        bool dirUnchanged = true;
        if (plan.destinationDirectoryIdentity.exists && !dirPath.isEmpty()) {
            const AdminLocalIdentity currentDirId = readIdentity(dirPath);
            if (!currentDirId.exists || !currentDirId.isDirectory || currentDirId.isSymLink
                || !plan.destinationDirectoryIdentity.sameFile(currentDirId)) {
                dirUnchanged = false;
            }
        }
        audit.check5_parentDirUnchanged = dirUnchanged;

        if (destPath.isEmpty()) {
            m_lastArtifactAudit = audit;
            return AdminTransferFailureState::None;
        }

        const QString partPath = destPath + QStringLiteral(".part");
        const AdminLocalIdentity currentTarget = readIdentity(destPath);
        const AdminLocalIdentity partTarget = readIdentity(partPath);

        if (!currentTarget.exists && !partTarget.exists) {
            if (!audit.check5_parentDirUnchanged) {
                audit.primaryReason = AdminArtifactRejectionReason::ParentDirectoryChanged;
                m_lastArtifactAudit = audit;
                return AdminTransferFailureState::UnknownCompletionState;
            }
            m_lastArtifactAudit = audit;
            if (kioErrorCode == KIO::ERR_CANNOT_READ) {
                return AdminTransferFailureState::ReadFailedBeforeDataTransfer;
            }
            return AdminTransferFailureState::None;
        }

        // Fail-closed policy (Stage 4 Final Security Gate):
        // Automatic deletion of destination artifacts is disabled because out-of-process KIO workers
        // cannot guarantee kernel-level creation provenance or race-free inode-pinned unlinking.
        // Always preserve the artifact on disk and perform the full 9-point ownership & safety audit.
        audit.artifactExists = true;
        audit.artifactPreserved = true;
        audit.preservedArtifactPath = (partTarget.exists && !currentTarget.exists) ? partPath : destPath;

        const bool hasUnsafeComponents = (!dirPath.isEmpty() && hasSymlinkInPathComponents(dirPath))
            || hasSymlinkInPathComponents(destPath);
        const bool isSafeRegularSingleLink = currentTarget.exists
            && currentTarget.isRegular
            && !currentTarget.isSymLink
            && !currentTarget.isBrokenSymLink
            && (currentTarget.statBuf.st_nlink == 1);
        audit.check6_noSymlinkOrHardlink = !hasUnsafeComponents && (currentTarget.exists ? isSafeRegularSingleLink : true);

        const bool hasNonZeroData = currentTarget.exists
            && currentTarget.isRegular
            && !currentTarget.isSymLink
            && (currentTarget.statBuf.st_size > 0);
        const bool hasAmbiguousState = partTarget.exists
            || (plan.partialState != AdminPartialCompletionState::NotStarted);
        audit.check9_noPartialOrAmbiguousState = !hasNonZeroData && !hasAmbiguousState;

        audit.check9_ownedByCurrentUser = currentTarget.exists
            ? (currentTarget.statBuf.st_uid == ::getuid())
            : true;

        const bool isUnprivilegedDest = (plan.effectiveDestinationUrl.scheme() == QLatin1String("file"));
        const bool dirWritableByUser = !dirPath.isEmpty()
            && (::access(QFile::encodeName(dirPath).constData(), W_OK) == 0);
        audit.check9_hasSafeDeletePermission = isUnprivilegedDest && dirWritableByUser;

        // Out-of-process KIO workers do not provide verified creation provenance to the GUI client
        audit.check2_createdByThisTask = false;
        audit.check3_knownIdentity = m_observedArtifactIdentity.exists
            && (m_observedArtifactIdentity.statBuf.st_ino != 0);
        audit.check4_identityUnchanged = audit.check3_knownIdentity
            && currentTarget.exists
            && m_observedArtifactIdentity.sameFile(currentTarget);

        if (!audit.check1_notExistedBefore) {
            audit.primaryReason = AdminArtifactRejectionReason::TargetExistedBeforeStart;
        } else if (!audit.check8_noOtherTaskUsingTarget) {
            audit.primaryReason = AdminArtifactRejectionReason::TargetUsedByAnotherTask;
        } else if (!audit.check5_parentDirUnchanged) {
            audit.primaryReason = AdminArtifactRejectionReason::ParentDirectoryChanged;
        } else if (hasAmbiguousState) {
            audit.primaryReason = AdminArtifactRejectionReason::AmbiguousOrPartialState;
        } else if (!audit.check6_noSymlinkOrHardlink) {
            audit.primaryReason = AdminArtifactRejectionReason::TargetIsSymlinkOrHardlink;
        } else if (hasNonZeroData) {
            audit.primaryReason = AdminArtifactRejectionReason::NonZeroPartialData;
        } else if (!audit.check9_ownedByCurrentUser) {
            audit.primaryReason = AdminArtifactRejectionReason::ForeignOrRootOwner;
        } else if (!audit.check9_hasSafeDeletePermission) {
            audit.primaryReason = AdminArtifactRejectionReason::NoSafeDeletePermission;
        } else if (!audit.check7_writesConfirmedStopped) {
            audit.primaryReason = AdminArtifactRejectionReason::TaskWriteNotConfirmedStopped;
        } else if (audit.check3_knownIdentity && !audit.check4_identityUnchanged) {
            audit.primaryReason = AdminArtifactRejectionReason::ArtifactIdentityChanged;
        } else if (!audit.check2_createdByThisTask) {
            audit.primaryReason = AdminArtifactRejectionReason::UnverifiedTaskProvenance;
        } else {
            audit.primaryReason = AdminArtifactRejectionReason::UnknownArtifactIdentity;
        }

        m_lastArtifactAudit = audit;

        if (hasNonZeroData && !hasAmbiguousState && audit.check5_parentDirUnchanged && audit.check6_noSymlinkOrHardlink) {
            return AdminTransferFailureState::PartialDestinationCreated;
        }
        return AdminTransferFailureState::UnknownCompletionState;
    }

    void cancelQuietly()
    {
        if (m_currentJob) {
            KJob *job = m_currentJob;
            cleanupCurrentJob();
            job->kill(KJob::Quietly);
        } else {
            cleanupCurrentJob();
        }
    }

    void cleanupCurrentJob()
    {
        if (!m_registeredTargetPath.isEmpty()) {
            releaseTargetPath(m_registeredTargetPath);
            m_registeredTargetPath.clear();
        }
        if (m_currentJob) {
            disconnect(m_currentJob, nullptr, this, nullptr);
            m_currentJob = nullptr;
        }
        m_activePlan = {};
    }

    KJob *createKJobForPlan(const AdminExecutionPlan &plan)
    {
        switch (plan.planType) {
        case AdminPlanType::CopyFileToProtected:
            return KIO::file_copy(plan.effectiveSourceUrl, plan.effectiveDestinationUrl, defaultPermissionsForPlan(plan.planType), KIO::HideProgressInfo);

        case AdminPlanType::CopyFileFromProtected:
            return KIO::file_copy(plan.effectiveSourceUrl, plan.effectiveDestinationUrl, defaultPermissionsForPlan(plan.planType), KIO::HideProgressInfo);

        case AdminPlanType::CreateFileProtected:
            return KIO::storedPut(plan.fileContents, plan.effectiveDestinationUrl, defaultPermissionsForPlan(plan.planType), KIO::HideProgressInfo);

        case AdminPlanType::CreateFolderProtected:
            return KIO::mkdir(plan.effectiveDestinationUrl);

        case AdminPlanType::RenameFileProtected:
        case AdminPlanType::RenameFolderProtected:
        case AdminPlanType::MoveFileSameFilesystem:
            return KIO::rename(plan.effectiveSourceUrl, plan.effectiveDestinationUrl, KIO::HideProgressInfo);

        case AdminPlanType::Invalid:
        default:
            return nullptr;
        }
    }

    KJob *m_currentJob = nullptr;
    AdminExecutionPlan m_activePlan;
    QString m_registeredTargetPath;
    AdminLocalIdentity m_observedArtifactIdentity;
    MockJobFactory m_mockJobFactory;
    ArtifactIdentityReader m_artifactIdentityReaderOverride;
    std::optional<bool> m_kioAdminAvailableOverride;
    AdminStatvfsChecker m_statvfsOverride;
    AdminMountIdChecker m_mountIdOverride;
    int m_lastKioErrorCode = 0;
    AdminTransferFailureState m_lastFailureState = AdminTransferFailureState::None;
    AdminArtifactOwnershipAudit m_lastArtifactAudit;
};
