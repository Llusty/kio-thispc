/*
 * Tests for ThisPC 0.43.0 Stage 1 Admin Retry Contract.
 *
 * Exercises structural error classification, qualification, TOCTOU identity snapshots,
 * fail-closed partial state handling, and undo policy without performing any elevated
 * operations or invoking Polkit.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "adminretrycontract.h"
#include "adminretrycontroller.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) {
        qFatal("FAIL: %s", description);
    }
    ++checks;
}

class MockOperationJob final : public KJob
{
public:
    explicit MockOperationJob(int errorCode = 0, const QString &errorString = QString())
    {
        if (errorCode != 0) {
            setError(errorCode);
            setErrorText(errorString);
        }
    }

    void start() override {}

    void setJobError(int code, const QString &text)
    {
        setError(code);
        setErrorText(text);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("thispc-admin-test"));
    QCoreApplication::setApplicationName(QStringLiteral("admin-retry-test"));

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "Temporary test directory created");
    const QString rootPath = tempDir.path();

    AdminRetryController controller;

    // -------------------------------------------------------------------------
    // Test 1: KIO read permission denied (ERR_ACCESS_DENIED, ERR_CANNOT_READ with EACCES)
    // -------------------------------------------------------------------------
    {
        MockOperationJob job(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminOperationError err = classifyOperationError(&job);
        verify(err.category == AdminErrorCategory::PermissionDenied, "KIO::ERR_ACCESS_DENIED classified as PermissionDenied");
        verify(err.isPermissionError(), "err.isPermissionError() is true for ERR_ACCESS_DENIED");

        MockOperationJob readJob(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read file"));
        readJob.setProperty("thispcSystemError", EACCES);
        const AdminOperationError readErr = classifyOperationError(&readJob);
        verify(readErr.category == AdminErrorCategory::PermissionDenied, "ERR_CANNOT_READ with EACCES is PermissionDenied");
    }

    // -------------------------------------------------------------------------
    // Test 2: KIO write permission denied (ERR_WRITE_ACCESS_DENIED, ERR_CANNOT_WRITE, ERR_CANNOT_MKDIR, ERR_CANNOT_RENAME)
    // -------------------------------------------------------------------------
    {
        MockOperationJob writeJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Write access denied"));
        const AdminOperationError errWrite = classifyOperationError(&writeJob);
        verify(errWrite.category == AdminErrorCategory::WriteAccessDenied, "ERR_WRITE_ACCESS_DENIED classified as WriteAccessDenied");
        verify(errWrite.isPermissionError(), "isPermissionError() is true for WriteAccessDenied");

        MockOperationJob mkdirJob(KIO::ERR_CANNOT_MKDIR, QStringLiteral("Cannot make directory"));
        verify(classifyOperationError(&mkdirJob).category == AdminErrorCategory::WriteAccessDenied, "ERR_CANNOT_MKDIR is WriteAccessDenied");

        MockOperationJob renameJob(KIO::ERR_CANNOT_RENAME, QStringLiteral("Cannot rename"));
        verify(classifyOperationError(&renameJob).category == AdminErrorCategory::WriteAccessDenied, "ERR_CANNOT_RENAME is WriteAccessDenied");

        MockOperationJob deleteJob(KIO::ERR_CANNOT_DELETE, QStringLiteral("Cannot delete"));
        verify(classifyOperationError(&deleteJob).category == AdminErrorCategory::WriteAccessDenied, "ERR_CANNOT_DELETE is WriteAccessDenied");
    }

    // -------------------------------------------------------------------------
    // Test 3: POSIX EACCES via native transfer job property or explicit errno
    // -------------------------------------------------------------------------
    {
        MockOperationJob nativeJob(KJob::UserDefinedError, QStringLiteral("Could not open file"));
        nativeJob.setProperty("thispcSystemError", EACCES);
        const AdminOperationError err = classifyOperationError(&nativeJob);
        verify(err.category == AdminErrorCategory::PermissionDenied, "POSIX EACCES classified as PermissionDenied");
        verify(err.posixErrno == EACCES, "posixErrno preserved as EACCES");
    }

    // -------------------------------------------------------------------------
    // Test 4: POSIX EPERM via native job property
    // -------------------------------------------------------------------------
    {
        MockOperationJob nativeJob(KJob::UserDefinedError, QStringLiteral("Operation not permitted"));
        nativeJob.setProperty("thispcSystemError", EPERM);
        const AdminOperationError err = classifyOperationError(&nativeJob);
        verify(err.category == AdminErrorCategory::PermissionDenied, "POSIX EPERM classified as PermissionDenied");
        verify(err.posixErrno == EPERM, "posixErrno preserved as EPERM");
    }

    // -------------------------------------------------------------------------
    // Test 5: ENOSPC (Disk full)
    // -------------------------------------------------------------------------
    {
        MockOperationJob fullJob(KIO::ERR_DISK_FULL, QStringLiteral("No space left on device"));
        AdminRetryRequest req;
        req.error = classifyOperationError(&fullJob);
        req.partialState = AdminPartialCompletionState::NotStarted;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "ENOSPC is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::DiskFull, "Ineligibility reason is DiskFull");
    }

    // -------------------------------------------------------------------------
    // Test 6: EDQUOT (Quota exceeded)
    // -------------------------------------------------------------------------
    {
        MockOperationJob quotaJob(KJob::UserDefinedError, QStringLiteral("Quota exceeded"));
        quotaJob.setProperty("thispcSystemError", EDQUOT);
        AdminRetryRequest req;
        req.error = classifyOperationError(&quotaJob);
        req.partialState = AdminPartialCompletionState::NotStarted;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "EDQUOT is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::DiskFull, "EDQUOT classified as DiskFull");
    }

    // -------------------------------------------------------------------------
    // Test 7: EROFS (Read-only filesystem)
    // -------------------------------------------------------------------------
    {
        MockOperationJob roJob(KJob::UserDefinedError, QStringLiteral("Read-only file system"));
        roJob.setProperty("thispcSystemError", EROFS);
        AdminRetryRequest req;
        req.error = classifyOperationError(&roJob);
        req.partialState = AdminPartialCompletionState::NotStarted;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "EROFS is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::ReadOnlyFilesystem, "Reason is ReadOnlyFilesystem");
    }

    // -------------------------------------------------------------------------
    // Test 8: Target collision (ERR_FILE_ALREADY_EXIST)
    // -------------------------------------------------------------------------
    {
        MockOperationJob existJob(KIO::ERR_FILE_ALREADY_EXIST, QStringLiteral("File already exists"));
        AdminRetryRequest req;
        req.error = classifyOperationError(&existJob);
        req.partialState = AdminPartialCompletionState::NotStarted;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Collision is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::TargetCollision, "Reason is TargetCollision");
    }

    // -------------------------------------------------------------------------
    // Test 9: Missing source (ERR_DOES_NOT_EXIST / ENOENT)
    // -------------------------------------------------------------------------
    {
        MockOperationJob noentJob(KIO::ERR_DOES_NOT_EXIST, QStringLiteral("Does not exist"));
        AdminRetryRequest req;
        req.error = classifyOperationError(&noentJob);
        req.partialState = AdminPartialCompletionState::NotStarted;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Missing source is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::SourceMissing, "Reason is SourceMissing");
    }

    // -------------------------------------------------------------------------
    // Test 10: Operation cancelled by user
    // -------------------------------------------------------------------------
    {
        MockOperationJob cancelJob(KIO::ERR_USER_CANCELED, QStringLiteral("User cancelled"));
        AdminRetryRequest req;
        req.wasCancelled = true;
        req.error = classifyOperationError(&cancelJob);
        req.partialState = AdminPartialCompletionState::NotStarted;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Cancelled operation is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::UserCancelled, "Reason is UserCancelled");
    }

    // -------------------------------------------------------------------------
    // Test 11: Authorization cancelled or denied (Polkit dialog rejected)
    // -------------------------------------------------------------------------
    {
        MockOperationJob authJob(KJob::KilledJobError, QStringLiteral("Polkit authorization was cancelled"));
        const AdminOperationError err = classifyOperationError(&authJob);
        verify(err.category == AdminErrorCategory::AuthorizationCancelledOrDenied, "Polkit cancellation classified as AuthorizationCancelledOrDenied");
    }

    // -------------------------------------------------------------------------
    // Test 12: Missing kio-admin worker
    // -------------------------------------------------------------------------
    {
        MockOperationJob job(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest req;
        req.error = classifyOperationError(&job);
        req.partialState = AdminPartialCompletionState::NotStarted;
        req.sourceUrls = {QUrl::fromLocalFile(rootPath + QStringLiteral("/a.txt"))};
        req.destinationDirectory = QUrl::fromLocalFile(rootPath);
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, false /* worker unavailable */);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Missing kio-admin worker makes retry Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::MissingAdminWorker, "Reason is MissingAdminWorker");
    }

    // -------------------------------------------------------------------------
    // Test 13: Unsupported remote backend (MTP, SMB, SFTP)
    // -------------------------------------------------------------------------
    {
        MockOperationJob job(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest mtpReq;
        mtpReq.error = classifyOperationError(&job);
        mtpReq.partialState = AdminPartialCompletionState::NotStarted;
        mtpReq.sourceUrls = {QUrl(QStringLiteral("mtp:/device/DCIM/photo.jpg"))};
        mtpReq.destinationDirectory = QUrl::fromLocalFile(rootPath);
        const AdminRetryQualification mtpQual = AdminRetryPolicy::evaluate(mtpReq, true);
        verify(mtpQual.eligibility == AdminRetryEligibility::UnsupportedBackend, "MTP is UnsupportedBackend");
        verify(mtpQual.reason == AdminRetryIneligibilityReason::NonLocalScheme, "Reason is NonLocalScheme");

        AdminRetryRequest smbReq;
        smbReq.error = classifyOperationError(&job);
        smbReq.partialState = AdminPartialCompletionState::NotStarted;
        smbReq.sourceUrls = {QUrl::fromLocalFile(rootPath + QStringLiteral("/a.txt"))};
        smbReq.destinationDirectory = QUrl(QStringLiteral("smb://server/share/folder"));
        const AdminRetryQualification smbQual = AdminRetryPolicy::evaluate(smbReq, true);
        verify(smbQual.eligibility == AdminRetryEligibility::UnsupportedBackend, "SMB is UnsupportedBackend");

        AdminRetryRequest sftpReq;
        sftpReq.error = classifyOperationError(&job);
        sftpReq.partialState = AdminPartialCompletionState::NotStarted;
        sftpReq.sourceUrls = {QUrl(QStringLiteral("sftp://user@host/path/file.txt"))};
        sftpReq.destinationDirectory = QUrl::fromLocalFile(rootPath);
        const AdminRetryQualification sftpQual = AdminRetryPolicy::evaluate(sftpReq, true);
        verify(sftpQual.eligibility == AdminRetryEligibility::UnsupportedBackend, "SFTP is UnsupportedBackend");
    }

    // -------------------------------------------------------------------------
    // Test 14: Trash protocol (trash:/)
    // -------------------------------------------------------------------------
    {
        MockOperationJob job(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest trashReq;
        trashReq.operationType = FileOperationType::Trash;
        trashReq.backend = AdminOperationBackend::KioTrashJob;
        trashReq.error = classifyOperationError(&job);
        trashReq.partialState = AdminPartialCompletionState::NotStarted;
        trashReq.sourceUrls = {QUrl::fromLocalFile(rootPath + QStringLiteral("/a.txt"))};
        trashReq.destinationDirectory = QUrl(QStringLiteral("trash:/"));
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(trashReq, true);
        verify(qual.eligibility == AdminRetryEligibility::UnsupportedBackend, "Trash is UnsupportedBackend");
        verify(qual.reason == AdminRetryIneligibilityReason::TrashProtocolUnsupported, "Reason is TrashProtocolUnsupported");
    }

    // -------------------------------------------------------------------------
    // Test 15: Operation already retried as admin (loop prevention)
    // -------------------------------------------------------------------------
    {
        MockOperationJob job(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied again"));
        AdminRetryRequest loopReq;
        loopReq.alreadyRetriedAsAdmin = true;
        loopReq.error = classifyOperationError(&job);
        loopReq.partialState = AdminPartialCompletionState::NotStarted;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(loopReq, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Repeated retry is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::AlreadyRetriedAsAdmin, "Reason is AlreadyRetriedAsAdmin");
    }

    // -------------------------------------------------------------------------
    // Test 16: Missing/Unknown partial state (Fail-closed policy)
    // -------------------------------------------------------------------------
    {
        MockOperationJob job(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest req;
        req.error = classifyOperationError(&job);
        req.partialState = AdminPartialCompletionState::UnknownState;
        req.sourceUrls = {QUrl::fromLocalFile(rootPath + QStringLiteral("/a.txt"))};
        req.destinationDirectory = QUrl::fromLocalFile(rootPath);
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Unknown partial state fails closed as Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::UnverifiablePartialState, "Reason is UnverifiablePartialState");
    }

    // -------------------------------------------------------------------------
    // Test 17: Confirmed partial execution (PartiallyCompleted requires inspection)
    // -------------------------------------------------------------------------
    {
        MockOperationJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Permission denied on item 3"));
        AdminRetryRequest req;
        req.error = classifyOperationError(&job);
        req.partialState = AdminPartialCompletionState::PartiallyCompleted;
        req.sourceUrls = {QUrl::fromLocalFile(rootPath + QStringLiteral("/a.txt"))};
        req.destinationDirectory = QUrl::fromLocalFile(rootPath);
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::RequiresInspection, "Partially completed operation RequiresInspection");
        verify(qual.reason == AdminRetryIneligibilityReason::UnverifiablePartialState, "Reason is UnverifiablePartialState");
    }

    // -------------------------------------------------------------------------
    // Test 18: Changed source identity (TOCTOU)
    // -------------------------------------------------------------------------
    {
        const QString srcFile = rootPath + QStringLiteral("/toctou_src.txt");
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly) && f.write("initial content") > 0, "Initial source written");
        }
        const AdminLocalIdentity snapshot = AdminLocalIdentity::read(srcFile);
        verify(snapshot.exists && snapshot.isRegular, "Source snapshot valid");

        // Mutate source
        {
            QFile f(srcFile);
            verify(f.open(QIODevice::WriteOnly | QIODevice::Append) && f.write(" appended") > 0, "Source modified");
        }
        verify(!snapshot.matches(srcFile), "Snapshot detects modification");

        MockOperationJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest req;
        req.error = classifyOperationError(&job);
        req.partialState = AdminPartialCompletionState::NotStarted;
        req.sourceUrls = {QUrl::fromLocalFile(srcFile)};
        req.destinationDirectory = QUrl::fromLocalFile(rootPath);
        req.sourceIdentities = {snapshot};
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Source identity change makes retry Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::SourceIdentityChanged, "Reason is SourceIdentityChanged");
    }

    // -------------------------------------------------------------------------
    // Test 19: Changed destination identity (TOCTOU)
    // -------------------------------------------------------------------------
    {
        const QString dstDir = rootPath + QStringLiteral("/dst_dir");
        QDir().mkdir(dstDir);
        const AdminLocalIdentity dstSnapshot = AdminLocalIdentity::read(dstDir);

        // Delete destination dir and recreate as file
        QDir().rmdir(dstDir);
        {
            QFile f(dstDir);
            verify(f.open(QIODevice::WriteOnly) && f.write("now a file") > 0, "Destination replaced with file");
        }
        verify(!dstSnapshot.matches(dstDir), "Destination mismatch detected");

        MockOperationJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest req;
        req.error = classifyOperationError(&job);
        req.partialState = AdminPartialCompletionState::NotStarted;
        req.sourceUrls = {QUrl::fromLocalFile(rootPath + QStringLiteral("/toctou_src.txt"))};
        req.destinationDirectory = QUrl::fromLocalFile(dstDir);
        req.destinationDirectoryIdentity = dstSnapshot;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Destination identity change makes retry Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::DestinationIdentityChanged, "Reason is DestinationIdentityChanged");
    }

    // -------------------------------------------------------------------------
    // Test 20: Symlink and broken symlink detection
    // -------------------------------------------------------------------------
    {
        const QString targetFile = rootPath + QStringLiteral("/link_target.txt");
        const QString validLink = rootPath + QStringLiteral("/valid_symlink.txt");
        const QString brokenLink = rootPath + QStringLiteral("/broken_symlink.txt");
        {
            QFile f(targetFile);
            verify(f.open(QIODevice::WriteOnly), "Link target created");
        }
        ::symlink(targetFile.toLocal8Bit().constData(), validLink.toLocal8Bit().constData());
        ::symlink("/nonexistent/file/path", brokenLink.toLocal8Bit().constData());

        const AdminLocalIdentity validId = AdminLocalIdentity::read(validLink);
        verify(validId.isSymLink && !validId.isBrokenSymLink, "Valid symlink recognized");

        const AdminLocalIdentity brokenId = AdminLocalIdentity::read(brokenLink);
        verify(brokenId.isSymLink && brokenId.isBrokenSymLink, "Broken symlink recognized");

        MockOperationJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest brokenReq;
        brokenReq.error = classifyOperationError(&job);
        brokenReq.partialState = AdminPartialCompletionState::NotStarted;
        brokenReq.sourceUrls = {QUrl::fromLocalFile(brokenLink)};
        brokenReq.destinationDirectory = QUrl::fromLocalFile(rootPath);
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(brokenReq, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Broken symlink is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::UnsafeSymlinkTraversal, "Reason is UnsafeSymlinkTraversal");
    }

    // -------------------------------------------------------------------------
    // Test 21: Cross-filesystem move check
    // -------------------------------------------------------------------------
    {
        // Destination inside source directory (cyclic move)
        const QString parentDir = rootPath + QStringLiteral("/parent_dir");
        const QString childDir = parentDir + QStringLiteral("/nested_child");
        QDir().mkpath(childDir);

        MockOperationJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest cyclicReq;
        cyclicReq.operationType = FileOperationType::Move;
        cyclicReq.error = classifyOperationError(&job);
        cyclicReq.partialState = AdminPartialCompletionState::NotStarted;
        cyclicReq.sourceUrls = {QUrl::fromLocalFile(parentDir)};
        cyclicReq.destinationDirectory = QUrl::fromLocalFile(childDir);
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(cyclicReq, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible, "Cyclic destination is Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::TargetInsideSourceDirectory, "Reason is TargetInsideSourceDirectory");
    }

    // -------------------------------------------------------------------------
    // Test 22: Multi-file operations (Batch requires inspection)
    // -------------------------------------------------------------------------
    {
        MockOperationJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest batchReq;
        batchReq.operationType = FileOperationType::Copy;
        batchReq.error = classifyOperationError(&job);
        batchReq.partialState = AdminPartialCompletionState::NotStarted;
        batchReq.sourceUrls = {
            QUrl::fromLocalFile(rootPath + QStringLiteral("/file1.txt")),
            QUrl::fromLocalFile(rootPath + QStringLiteral("/file2.txt"))
        };
        batchReq.destinationDirectory = QUrl::fromLocalFile(rootPath);
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(batchReq, true);
        verify(qual.eligibility == AdminRetryEligibility::RequiresInspection, "Multi-file batch RequiresInspection");
        verify(qual.reason == AdminRetryIneligibilityReason::BatchOperationRequiresInspection, "Reason is BatchOperationRequiresInspection");
    }

    // -------------------------------------------------------------------------
    // Test 23: Undo policy contract (No unprivileged undo for admin operations)
    // -------------------------------------------------------------------------
    {
        AdminRetryRequest privilegedReq;
        privilegedReq.operationType = FileOperationType::CreateFile;
        privilegedReq.destinationDirectory = QUrl(QStringLiteral("admin:///etc"));
        verify(!controller.shouldRecordUndo(privilegedReq), "shouldRecordUndo is false for privileged retry");

        MockOperationJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest singleReq;
        singleReq.operationType = FileOperationType::CreateFile;
        singleReq.error = classifyOperationError(&job);
        singleReq.partialState = AdminPartialCompletionState::NotStarted;
        singleReq.destinationDirectory = QUrl::fromLocalFile(rootPath);
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(singleReq, true);
        verify(qual.eligibility == AdminRetryEligibility::Eligible, "Single CreateFile is Eligible");
        verify(!qual.isReversible, "isReversible is false for privileged operation qualification");
    }

    // -------------------------------------------------------------------------
    // Test 24: No automatic retry (Policy requires explicit qualification & consent)
    // -------------------------------------------------------------------------
    {
        AdminRetryRequest req;
        verify(req.partialState == AdminPartialCompletionState::UnknownState, "Default request partialState is UnknownState");
        const AdminRetryQualification defaultQual = AdminRetryPolicy::evaluate(req, true);
        verify(!defaultQual.canAttemptElevation(), "Default empty request cannot attempt elevation");
    }

    // -------------------------------------------------------------------------
    // Test 25: Passive inspection seam via controller
    // -------------------------------------------------------------------------
    {
        MockOperationJob permJob(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFolder;
        ctx.destinationDirectory = QUrl::fromLocalFile(rootPath);
        ctx.destinationUrls = {QUrl::fromLocalFile(rootPath + QStringLiteral("/new_folder"))};

        const AdminRetryRequest req = controller.createRequest(&permJob, ctx, false, AdminPartialCompletionState::NotStarted);
        verify(req.operationType == FileOperationType::CreateFolder, "createRequest captures operationType");
        verify(req.error.isPermissionError(), "createRequest captures permission error");

        const AdminRetryQualification qual = controller.inspectFailedOperation(&permJob, ctx, false, AdminPartialCompletionState::NotStarted);
        verify(qual.eligibility == AdminRetryEligibility::Eligible, "inspectFailedOperation qualifies CreateFolder as Eligible");
    }

    qInfo("PASS: %d Admin Retry Contract assertions", checks);
    return 0;
}
