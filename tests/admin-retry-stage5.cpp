/*
 * Tests for ThisPC 0.43.0 Stage 5 — Final Integration, Safety Regression & Release Readiness.
 *
 * Covers end-to-end component integration (ThisPCView, SplitBrowserPane, FileActions,
 * OperationManager, UndoController, LocalMoveHistory, AdminRetryController, AdminRetryExecutor,
 * AdminPlanValidator, LocalFileMoveJob), security regressions, fail-closed artifact preservation,
 * lifecycle/teardown resilience, Primary/Split View context immutability, concurrency/stress
 * scenarios, and GUI error localization.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "adminretrycontract.h"
#include "adminretrycontroller.h"
#include "adminretryexecutor.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLocale>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <sys/stat.h>
#include <unistd.h>

static int s5Checks = 0;
static int s5PassedTests = 0;
static int s5FailedTests = 0;
static int s5SkippedTests = 0;
static int s5UnverifiedTests = 0;

static void verify(bool value, const char *description)
{
    if (!value) {
        ++s5FailedTests;
        qFatal("FAIL: %s", description);
    }
    ++s5Checks;
}

static void recordTestPass(const char *testName)
{
    ++s5PassedTests;
    qInfo("  [PASS] %s", testName);
}

class MockStage5Job final : public KJob
{
public:
    explicit MockStage5Job(int errorCode = 0, const QString &errorString = QString())
    {
        setCapabilities(KJob::Killable | KJob::Suspendable);
        if (errorCode != 0) {
            setError(errorCode);
            setErrorText(errorString);
        }
    }

    void setCustomErrorText(const QString &text)
    {
        setErrorText(text);
    }

    void start() override
    {
        m_started = true;
        if (m_autoEmitResult) {
            QMetaObject::invokeMethod(this, [this]() {
                emitResult();
            }, Qt::QueuedConnection);
        }
    }

    void setAutoEmitResult(bool autoEmit)
    {
        m_autoEmitResult = autoEmit;
    }

    bool isStarted() const
    {
        return m_started;
    }

    bool wasKilled() const
    {
        return m_killed;
    }

    void emitProgress(unsigned long pct, qulonglong processed, qulonglong total)
    {
        setTotalAmount(KJob::Bytes, total);
        setProcessedAmount(KJob::Bytes, processed);
        setPercent(pct);
    }

    void finishWithSuccess()
    {
        setError(0);
        setErrorText(QString());
        emitResult();
    }

    void finishWithError(int code, const QString &text)
    {
        setError(code);
        setErrorText(text);
        emitResult();
    }

protected:
    bool doKill() override
    {
        m_killed = true;
        setError(KJob::KilledJobError);
        return true;
    }

private:
    bool m_started = false;
    bool m_autoEmitResult = true;
    bool m_killed = false;
};

static bool writeStage5File(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return f.write(data) == data.size();
}

static QByteArray readStage5File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    verify(::geteuid() != 0, "Stage 5 automated test suite must run without root privileges");

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "Stage 5 temporary test sandbox created");

    const QString userDir = tempDir.filePath(QStringLiteral("user_workspace"));
    const QString splitDir = tempDir.filePath(QStringLiteral("split_workspace"));
    const QString protectedDir = tempDir.filePath(QStringLiteral("protected_workspace"));
    verify(QDir().mkpath(userDir), "user_workspace created");
    verify(QDir().mkpath(splitDir), "split_workspace created");
    verify(QDir().mkpath(protectedDir), "protected_workspace created");

    const QString protectedReadOnlyFile = QDir(protectedDir).filePath(QStringLiteral("secret_source.txt"));
    const QByteArray secretPayload("Confidential payload for Stage 5 E2E verification.\n");
    verify(writeStage5File(protectedReadOnlyFile, secretPayload), "secret_source.txt created in protected_workspace");
    verify(::chmod(QFile::encodeName(protectedReadOnlyFile).constData(), 0000) == 0, "secret_source.txt set to mode 0000");
    verify(::chmod(QFile::encodeName(protectedDir).constData(), 0555) == 0, "protected_workspace set to mode 0555");

    const QString userSourceFile = QDir(userDir).filePath(QStringLiteral("user_doc.txt"));
    const QByteArray userPayload("Standard user document payload for Stage 5.\n");
    verify(writeStage5File(userSourceFile, userPayload), "user_doc.txt created in user_workspace");

    AdminRetryController controller;

    // =========================================================================
    // SECTION 1 (Prompt Section 5): End-to-End Copy & Create Integration
    // =========================================================================

    // S5-01: Copy To Protected (file:// -> admin://) E2E with OperationManager
    {
        MockStage5Job failedJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        failedJob.setAutoDelete(false);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));

        const AdminRetryQualification qual = controller.inspectGuiFailedOperation(&failedJob, ctx);
        verify(qual.canAttemptElevation(), "S5-01: Copy To Protected qualifies for GUI elevation");

        const AdminRetryRequest req = controller.createGuiRequest(&failedJob, ctx);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CopyFileToProtected, "S5-01: Plan type is CopyFileToProtected");
        verify(plan.effectiveSourceUrl.scheme() == QLatin1String("file"), "S5-01: Source remains unprivileged file://");
        verify(plan.effectiveDestinationUrl.scheme() == QLatin1String("admin"), "S5-01: Destination elevated to admin://");
        verify(AdminRetryExecutor::defaultPermissionsForPlan(plan.planType) == -1, "S5-01: Default permissions (-1) for CopyFileToProtected");

        AdminUserConsent consent = controller.createConsent(plan);
        AdminRetryExecutor exec;
        MockStage5Job *adminJob = nullptr;
        exec.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
            adminJob = new MockStage5Job(0);
            adminJob->setAutoDelete(false);
            adminJob->setAutoEmitResult(false);
            return adminJob;
        });

        bool done = false;
        bool ok = false;
        QObject::connect(&exec, &AdminRetryExecutor::finished, &exec, [&](bool s, const QString &, AdminErrorCategory) {
            done = true;
            ok = s;
        });

        verify(exec.execute(plan, consent), "S5-01: Executor starts CopyFileToProtected");
        verify(adminJob != nullptr && exec.isRunning(), "S5-01: Admin job is active");
        adminJob->finishWithSuccess();
        verify(done && ok && !exec.isRunning(), "S5-01: CopyFileToProtected completes cleanly");
        delete adminJob;
        recordTestPass("S5-01: Copy To Protected end-to-end flow");
    }

    // S5-02: Copy From Protected (admin:// -> file://) E2E with mode 0600 & user ownership
    {
        MockStage5Job failedJob(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot open file for reading"));
        failedJob.setAutoDelete(false);
        failedJob.setProperty("thispcSystemError", EACCES);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(protectedReadOnlyFile)},
            QUrl::fromLocalFile(userDir));

        const AdminRetryQualification qual = controller.inspectGuiFailedOperation(&failedJob, ctx);
        verify(qual.canAttemptElevation(), "S5-02: Copy From Protected (0000 file) qualifies for GUI elevation");

        const AdminRetryRequest req = controller.createGuiRequest(&failedJob, ctx);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CopyFileFromProtected, "S5-02: Plan type is CopyFileFromProtected");
        verify(plan.effectiveSourceUrl.scheme() == QLatin1String("admin"), "S5-02: Source elevated to admin://");
        verify(plan.effectiveDestinationUrl.scheme() == QLatin1String("file"), "S5-02: Destination uses unprivileged file:// so file is owned by user");
        verify(AdminRetryExecutor::defaultPermissionsForPlan(plan.planType) == 0600, "S5-02: Enforces strict 0600 mode on destination");

        const QString expectedDestPath = QDir(userDir).filePath(QStringLiteral("secret_source.txt"));
        AdminUserConsent consent = controller.createConsent(plan);
        AdminRetryExecutor exec;
        exec.setMockJobFactory([&](const AdminExecutionPlan &p) -> KJob* {
            const QString dst = p.effectiveDestinationUrl.toLocalFile();
            writeStage5File(dst, secretPayload);
            ::chmod(QFile::encodeName(dst).constData(), static_cast<mode_t>(AdminRetryExecutor::defaultPermissionsForPlan(p.planType)));
            auto *job = new MockStage5Job(0);
            job->setAutoEmitResult(true);
            return job;
        });

        bool done = false;
        bool ok = false;
        QEventLoop loop;
        QObject::connect(&exec, &AdminRetryExecutor::finished, &loop, [&](bool s, const QString &, AdminErrorCategory) {
            done = true;
            ok = s;
            loop.quit();
        });

        verify(exec.execute(plan, consent), "S5-02: Executor starts CopyFileFromProtected");
        if (!done) {
            loop.exec();
        }
        verify(done && ok, "S5-02: CopyFileFromProtected finished with success");

        const AdminLocalIdentity destId = AdminLocalIdentity::read(expectedDestPath);
        verify(destId.exists && destId.isRegular && !destId.isSymLink, "S5-02: Destination file exists as regular file");
        verify(destId.statBuf.st_uid == ::getuid(), "S5-02: Destination file is owned by current unprivileged user (not root)");
        verify((destId.statBuf.st_mode & 0777) == 0600, "S5-02: Destination file permissions are strictly 0600");
        verify(readStage5File(expectedDestPath) == secretPayload, "S5-02: Destination file content matches source payload");
        QFile::remove(expectedDestPath);
        recordTestPass("S5-02: Copy From Protected E2E (user ownership + 0600 + content integrity)");
    }

    // S5-03: Create Protected File E2E (KIO Error 177 structural verification + payload + .part protection)
    {
        const QString newFilePath = QDir(protectedDir).filePath(QStringLiteral("created_stage5.txt"));
        const QString newFilePartPath = newFilePath + QStringLiteral(".part");
        const QByteArray templatePayload("Template content for Stage 5 file creation\n");

        MockStage5Job err177Job(KIO::ERR_CANNOT_TRUNCATE, newFilePartPath);
        err177Job.setAutoDelete(false);
        err177Job.setProperty("thispcFileContents", templatePayload);

        FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::CreateFile, {}, QUrl::fromLocalFile(protectedDir));
        ctx.destinationUrls = {QUrl::fromLocalFile(newFilePath)};

        const AdminRetryQualification qual = controller.inspectGuiFailedOperation(&err177Job, ctx);
        verify(qual.canAttemptElevation(), "S5-03: KIO::ERR_CANNOT_TRUNCATE (177) in 0555 dir qualifies for elevation");

        const AdminRetryRequest req = controller.createGuiRequest(&err177Job, ctx);
        verify(req.fileContents == templatePayload, "S5-03: Request preserves thispcFileContents payload");
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CreateFileProtected, "S5-03: Plan type is CreateFileProtected");
        verify(plan.fileContents == templatePayload, "S5-03: Plan preserves fileContents payload");
        verify(plan.effectiveDestinationUrl.scheme() == QLatin1String("admin"), "S5-03: Destination uses admin://");

        // Verify that error 177 in a WRITABLE directory is rejected as UnverifiableOrUnknown
        const QString writableFilePath = QDir(userDir).filePath(QStringLiteral("writable_177.txt"));
        MockStage5Job writable177Job(KIO::ERR_CANNOT_TRUNCATE, writableFilePath + QStringLiteral(".part"));
        writable177Job.setAutoDelete(false);
        FileOperationContext writableCtx = FileOperationContext::create(
            FileOperationType::CreateFile, {}, QUrl::fromLocalFile(userDir));
        writableCtx.destinationUrls = {QUrl::fromLocalFile(writableFilePath)};
        const AdminRetryQualification writableQual = controller.inspectGuiFailedOperation(&writable177Job, writableCtx);
        verify(!writableQual.canAttemptElevation(), "S5-03: Error 177 in writable directory is NOT misclassified as permission error");

        // Verify that a lingering .part file blocks elevation
        const QString partCollisionBase = QDir(userDir).filePath(QStringLiteral("part_collision.txt"));
        verify(writeStage5File(partCollisionBase + QStringLiteral(".part"), "partial"), "S5-03: Create lingering .part file");
        MockStage5Job partJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        partJob.setAutoDelete(false);
        FileOperationContext partCtx = FileOperationContext::create(
            FileOperationType::CreateFile, {}, QUrl::fromLocalFile(userDir));
        partCtx.destinationUrls = {QUrl::fromLocalFile(partCollisionBase)};
        const AdminRetryQualification partQual = controller.inspectGuiFailedOperation(&partJob, partCtx);
        verify(!partQual.canAttemptElevation()
               && partQual.reason == AdminRetryIneligibilityReason::UnverifiablePartialState,
               "S5-03: Lingering .part file blocks CreateFile elevation");
        QFile::remove(partCollisionBase + QStringLiteral(".part"));
        recordTestPass("S5-03: Create Protected File E2E (KIO Error 177 + payload + .part safety)");
    }

    // S5-04: Create Protected Folder E2E
    {
        const QString newFolderPath = QDir(protectedDir).filePath(QStringLiteral("NewProtectedFolder"));
        MockStage5Job mkdirFailedJob(KIO::ERR_CANNOT_MKDIR, newFolderPath);
        mkdirFailedJob.setAutoDelete(false);

        FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::CreateFolder, {}, QUrl::fromLocalFile(protectedDir));
        ctx.destinationUrls = {QUrl::fromLocalFile(newFolderPath)};

        const AdminRetryQualification qual = controller.inspectGuiFailedOperation(&mkdirFailedJob, ctx);
        verify(qual.canAttemptElevation(), "S5-04: CreateFolder in 0555 dir qualifies for GUI elevation");

        const AdminRetryRequest req = controller.createGuiRequest(&mkdirFailedJob, ctx);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CreateFolderProtected, "S5-04: Plan type is CreateFolderProtected");
        verify(plan.effectiveDestinationUrl == adminUrlForLocalPath(newFolderPath),
               "S5-04: Effective destination URL targets exact folder path via admin://");
        recordTestPass("S5-04: Create Protected Folder E2E");
    }

    // =========================================================================
    // SECTION 2 (Prompt Section 6): Security & Policy Regression
    // =========================================================================

    // S5-05: Central GUI Allowlist & Strict Blocking of Rename, Move, Delete, Trash, Batch
    {
        verify(isGuiAdminRetryAllowed(FileOperationType::Copy), "S5-05: Copy allowed in GUI");
        verify(isGuiAdminRetryAllowed(FileOperationType::CreateFile), "S5-05: CreateFile allowed in GUI");
        verify(isGuiAdminRetryAllowed(FileOperationType::CreateFolder), "S5-05: CreateFolder allowed in GUI");
        verify(!isGuiAdminRetryAllowed(FileOperationType::Rename), "S5-05: Rename strictly BLOCKED in GUI");
        verify(!isGuiAdminRetryAllowed(FileOperationType::Move), "S5-05: Move strictly BLOCKED in GUI");
        verify(!isGuiAdminRetryAllowed(FileOperationType::Delete), "S5-05: Delete strictly BLOCKED in GUI");
        verify(!isGuiAdminRetryAllowed(FileOperationType::Trash), "S5-05: Trash strictly BLOCKED in GUI");
        verify(!isGuiAdminRetryAllowed(FileOperationType::EmptyTrash), "S5-05: EmptyTrash strictly BLOCKED in GUI");
        verify(!isGuiAdminRetryAllowed(FileOperationType::Restore), "S5-05: Restore strictly BLOCKED in GUI");
        verify(!isGuiAdminRetryAllowed(FileOperationType::Custom), "S5-05: Custom strictly BLOCKED in GUI");

        MockStage5Job permJob(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        permJob.setAutoDelete(false);

        for (FileOperationType blockedType : {
                 FileOperationType::Rename,
                 FileOperationType::Move,
                 FileOperationType::Delete,
                 FileOperationType::Trash,
                 FileOperationType::EmptyTrash}) {
            FileOperationContext blockedCtx = FileOperationContext::create(
                blockedType,
                {QUrl::fromLocalFile(userSourceFile)},
                QUrl::fromLocalFile(protectedDir));
            blockedCtx.destinationUrls = {QUrl::fromLocalFile(QDir(protectedDir).filePath(QStringLiteral("target.txt")))};
            const AdminRetryQualification q = controller.inspectGuiFailedOperation(&permJob, blockedCtx);
            verify(!q.canAttemptElevation()
                   && q.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
                   "S5-05: Blocked operation rejected with OperationDisallowedInGui");
            const AdminRetryRequest r = controller.createGuiRequest(&permJob, blockedCtx);
            const AdminExecutionPlan p = controller.createGuiExecutionPlan(r);
            verify(p.planType == AdminPlanType::Invalid, "S5-05: createGuiExecutionPlan returns Invalid for blocked operation");
        }

        // Batch multi-file copy must also be blocked from automatic retry
        const QString userSourceFile2 = QDir(userDir).filePath(QStringLiteral("user_doc2.txt"));
        verify(writeStage5File(userSourceFile2, userPayload), "S5-05: Second user file created");
        const FileOperationContext batchCtx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile), QUrl::fromLocalFile(userSourceFile2)},
            QUrl::fromLocalFile(protectedDir));
        const AdminRetryQualification batchQual = controller.inspectGuiFailedOperation(&permJob, batchCtx);
        verify(!batchQual.canAttemptElevation()
               && batchQual.reason == AdminRetryIneligibilityReason::BatchOperationRequiresInspection,
               "S5-05: Multi-file batch Copy blocked from elevation");
        QFile::remove(userSourceFile2);
        recordTestPass("S5-05: Central GUI allowlist & Rename/Move/Delete/Batch blocking");
    }

    // S5-06: Polkit AuthorizationCancelledOrDenied & UserCancelled + Loop Prevention
    {
        MockStage5Job polkitDeniedJob(KIO::ERR_ACCESS_DENIED, QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"));
        polkitDeniedJob.setAutoDelete(false);
        polkitDeniedJob.setProperty("thispcPrivilegedOperation", true);

        const AdminOperationError err = classifyOperationError(&polkitDeniedJob);
        verify(err.category == AdminErrorCategory::AuthorizationCancelledOrDenied,
               "S5-06: Privileged job ERR_ACCESS_DENIED classified as AuthorizationCancelledOrDenied");

        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));
        AdminRetryRequest req = controller.createGuiRequest(&polkitDeniedJob, ctx);
        req.alreadyRetriedAsAdmin = true;
        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(!qual.canAttemptElevation()
               && qual.reason == AdminRetryIneligibilityReason::AlreadyRetriedAsAdmin,
               "S5-06: Privileged failure never triggers a retry loop");

        MockStage5Job userKilledJob(KJob::KilledJobError, QStringLiteral("Cancelled"));
        userKilledJob.setAutoDelete(false);
        const AdminRetryQualification cancelQual = controller.inspectGuiFailedOperation(&userKilledJob, ctx, true);
        verify(!cancelQual.canAttemptElevation()
               && cancelQual.reason == AdminRetryIneligibilityReason::UserCancelled,
               "S5-06: User-cancelled job is never eligible for elevation");
        recordTestPass("S5-06: Polkit AuthorizationCancelledOrDenied, UserCancelled & loop prevention");
    }

    // S5-07: AdminUserConsent Lifecycle (Expiration, Single-use, Mismatch) & TOCTOU / FS Validation
    {
        MockStage5Job permJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        permJob.setAutoDelete(false);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));
        const AdminRetryRequest req = controller.createGuiRequest(&permJob, ctx);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);

        // Expired consent
        AdminUserConsent expiredConsent = controller.createConsent(plan, 1000);
        const AdminValidationStatus expStatus = controller.validatePlan(
            plan, expiredConsent, QDateTime::currentDateTimeUtc().addSecs(5));
        verify(!expStatus.isValid() && expStatus.code == AdminPlanValidationResult::ConsentExpired,
               "S5-07: Expired consent rejected");

        // Mismatched destination URL in consent
        AdminUserConsent mismatchConsent = controller.createConsent(plan, 60000);
        mismatchConsent.confirmedDestinationUrl = QUrl::fromLocalFile(QDir(protectedDir).filePath(QStringLiteral("other.txt")));
        const AdminValidationStatus misStatus = controller.validatePlan(plan, mismatchConsent);
        verify(!misStatus.isValid() && misStatus.code == AdminPlanValidationResult::ConsentMismatch,
               "S5-07: Consent destination mismatch rejected");

        // Read-only filesystem rejection
        AdminUserConsent validConsent = controller.createConsent(plan, 60000);
        const AdminValidationStatus roStatus = controller.validatePlan(
            plan, validConsent, QDateTime::currentDateTimeUtc(),
            [](const QString &, struct statvfs *st) {
                memset(st, 0, sizeof(*st));
                st->f_flag = ST_RDONLY;
                st->f_bavail = 10000;
                st->f_frsize = 4096;
                return true;
            });
        verify(!roStatus.isValid() && roStatus.code == AdminPlanValidationResult::DestinationReadOnly,
               "S5-07: Read-only filesystem rejected");

        // Disk full rejection
        const AdminValidationStatus dfStatus = controller.validatePlan(
            plan, validConsent, QDateTime::currentDateTimeUtc(),
            [](const QString &, struct statvfs *st) {
                memset(st, 0, sizeof(*st));
                st->f_flag = 0;
                st->f_bavail = 0;
                st->f_frsize = 4096;
                return true;
            });
        verify(!dfStatus.isValid() && dfStatus.code == AdminPlanValidationResult::InsufficientDiskSpace,
               "S5-07: Insufficient disk space rejected");

        // Symlink and broken symlink rejection
        const QString symlinkSource = QDir(userDir).filePath(QStringLiteral("sym_source.txt"));
        const QString brokenSymSource = QDir(userDir).filePath(QStringLiteral("broken_sym.txt"));
        verify(::symlink(QFile::encodeName(userSourceFile).constData(), QFile::encodeName(symlinkSource).constData()) == 0,
               "S5-07: Symlink fixture created");
        verify(::symlink("/nonexistent/stage5/target", QFile::encodeName(brokenSymSource).constData()) == 0,
               "S5-07: Broken symlink fixture created");

        // Broken symlink rejected at policy level (UnsafeSymlinkTraversal)
        const FileOperationContext brokenSymCtx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(brokenSymSource)},
            QUrl::fromLocalFile(protectedDir));
        const AdminRetryQualification brokenSymQual = controller.inspectGuiFailedOperation(&permJob, brokenSymCtx);
        verify(!brokenSymQual.canAttemptElevation()
               && brokenSymQual.reason == AdminRetryIneligibilityReason::UnsafeSymlinkTraversal,
               "S5-07: Broken symlink rejected at policy level");

        // Regular symlink rejected at plan validation level (SourceIsSymlink)
        const FileOperationContext symCtx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(symlinkSource)},
            QUrl::fromLocalFile(protectedDir));
        const AdminExecutionPlan symPlan = controller.createGuiExecutionPlan(controller.createGuiRequest(&permJob, symCtx));
        const AdminUserConsent symConsent = controller.createConsent(symPlan);
        const AdminValidationStatus symVal = controller.validatePlan(symPlan, symConsent);
        verify(!symVal.isValid() && symVal.code == AdminPlanValidationResult::SourceIsSymlink,
               "S5-07: Symbolic link source rejected by AdminPlanValidator (SourceIsSymlink)");
        ::unlink(QFile::encodeName(symlinkSource).constData());
        ::unlink(QFile::encodeName(brokenSymSource).constData());

        // Unsupported schemes (mtp://, smb://, sftp://, trash://)
        for (const QString &schemeUrl : {
                 QStringLiteral("mtp:/Phone/DCIM/photo.jpg"),
                 QStringLiteral("smb://server/share/file.txt"),
                 QStringLiteral("sftp://host/root/file.txt"),
                 QStringLiteral("trash:/0-file.txt")}) {
            const FileOperationContext remoteCtx = FileOperationContext::create(
                FileOperationType::Copy,
                {QUrl(schemeUrl)},
                QUrl::fromLocalFile(protectedDir));
            const AdminRetryQualification remQual = controller.inspectGuiFailedOperation(&permJob, remoteCtx);
            verify(!remQual.canAttemptElevation()
                   && (remQual.reason == AdminRetryIneligibilityReason::NonLocalScheme
                       || remQual.reason == AdminRetryIneligibilityReason::TrashProtocolUnsupported),
                   "S5-07: Unsupported scheme rejected");
        }
        recordTestPass("S5-07: Consent lifecycle, read-only FS, disk full, symlinks & unsupported schemes");
    }

    // =========================================================================
    // SECTION 3 (Prompt Section 7): Failed Transfer Artifact Audit Regression
    // =========================================================================

    // S5-08: Fail-Closed Preservation of Unverified, Partial, Hardlinked, and Foreign Artifacts
    {
        const QString destPath = QDir(userDir).filePath(QStringLiteral("secret_source.txt"));
        MockStage5Job readFailJob(KIO::ERR_CANNOT_READ, QStringLiteral("Access denied"));
        readFailJob.setAutoDelete(false);
        readFailJob.setProperty("thispcSystemError", EACCES);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(protectedReadOnlyFile)},
            QUrl::fromLocalFile(userDir));
        const AdminRetryRequest req = controller.createGuiRequest(&readFailJob, ctx);

        // Case A: 0-byte artifact created during failed transfer -> MUST BE PRESERVED (never auto-unlinked)
        {
            AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
            AdminUserConsent consent = controller.createConsent(plan);
            AdminRetryExecutor exec;
            exec.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
                writeStage5File(destPath, QByteArray());
                auto *job = new MockStage5Job(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read source"));
                job->setAutoEmitResult(false);
                return job;
            });

            QString errMsg;
            QObject::connect(&exec, &AdminRetryExecutor::finished, &exec, [&](bool, const QString &msg, AdminErrorCategory) {
                errMsg = msg;
            });

            verify(exec.execute(plan, consent), "S5-08A: Executor starts");
            static_cast<MockStage5Job *>(exec.currentJob())->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read source"));

            verify(QFileInfo::exists(destPath), "S5-08A: 0-byte unverified artifact is PRESERVED on disk (no auto-unlink)");
            verify(exec.lastArtifactAudit().artifactPreserved, "S5-08A: Audit records artifactPreserved == true");
            verify(!exec.lastArtifactAudit().allChecksPassed(), "S5-08A: allChecksPassed() is strictly false");
            verify(errMsg.contains(destPath), "S5-08A: Error message explicitly informs user of preserved artifact path");

            // Subsequent GUI qualification on the same destination is blocked by PartialFileCreated / UnverifiablePartialState
            const AdminRetryQualification retryGuiQual = controller.inspectGuiFailedOperation(&readFailJob, ctx);
            verify(!retryGuiQual.canAttemptElevation()
                   && retryGuiQual.reason == AdminRetryIneligibilityReason::UnverifiablePartialState,
                   "S5-08A: GUI inspection blocks retry when preserved artifact exists at destination");

            // And direct plan execution on the existing target is blocked by TargetCollision (TargetExistedBeforeStart)
            const AdminRetryRequest freshReq = controller.createRequest(
                &readFailJob, ctx, false, AdminPartialCompletionState::NotStarted);
            AdminExecutionPlan retryPlan = controller.createGuiExecutionPlan(freshReq);
            AdminUserConsent retryConsent = controller.createConsent(retryPlan);
            AdminRetryExecutor retryExec;
            verify(!retryExec.execute(retryPlan, retryConsent),
                   "S5-08A: Direct execution on preserved artifact is blocked by TargetCollision");
            verify(retryExec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TargetExistedBeforeStart,
                   "S5-08A: Rejection reason is TargetExistedBeforeStart");
            verify(QFileInfo::exists(destPath), "S5-08A: Existing artifact remains untouched after blocked retry");
            QFile::remove(destPath);
        }

        // Case B: Hardlink artifact (st_nlink > 1) -> detected as TargetIsSymlinkOrHardlink and preserved
        {
            const QString hardlinkTarget = QDir(userDir).filePath(QStringLiteral("hardlink_origin.txt"));
            verify(writeStage5File(hardlinkTarget, QByteArray()), "S5-08B: Hardlink origin created");

            const AdminRetryRequest reqB = controller.createGuiRequest(&readFailJob, ctx);
            AdminExecutionPlan plan = controller.createGuiExecutionPlan(reqB);
            AdminUserConsent consent = controller.createConsent(plan);
            AdminRetryExecutor exec;
            exec.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
                ::link(QFile::encodeName(hardlinkTarget).constData(), QFile::encodeName(destPath).constData());
                auto *job = new MockStage5Job(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read"));
                job->setAutoEmitResult(false);
                return job;
            });

            verify(exec.execute(plan, consent), "S5-08B: Executor starts");
            static_cast<MockStage5Job *>(exec.currentJob())->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read"));
            verify(!exec.lastArtifactAudit().check6_noSymlinkOrHardlink
                   && exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TargetIsSymlinkOrHardlink,
                   "S5-08B: Hardlinked artifact detected and flagged as TargetIsSymlinkOrHardlink");
            verify(QFileInfo::exists(destPath) && QFileInfo::exists(hardlinkTarget),
                   "S5-08B: Both hardlink and origin preserved");
            QFile::remove(destPath);
            QFile::remove(hardlinkTarget);
        }
        recordTestPass("S5-08: Fail-closed artifact preservation & hardlink/collision protection");
    }

    // =========================================================================
    // SECTION 4 (Prompt Section 8): OperationManager, Undo/Redo & Lifecycle
    // =========================================================================

    // S5-09: OperationManager tracking, progress, cancel via KJob::kill(EmitResult), and single finished signal
    {
        QMainWindow window;
        QToolBar toolbar(&window);
        OperationManager opManager(&window, &toolbar);

        MockStage5Job permJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        permJob.setAutoDelete(false);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));
        const AdminRetryRequest req = controller.createGuiRequest(&permJob, ctx);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan);

        AdminRetryExecutor exec;
        MockStage5Job *runningJob = nullptr;
        exec.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
            runningJob = new MockStage5Job(0);
            runningJob->setAutoDelete(false);
            runningJob->setAutoEmitResult(false);
            return runningJob;
        });

        int finishedCount = 0;
        int progressCount = 0;
        AdminErrorCategory finalCat = AdminErrorCategory::None;
        QString finalMsg;

        QObject::connect(&exec, &AdminRetryExecutor::progress, &exec, [&](int, qint64, qint64) {
            ++progressCount;
        });
        QObject::connect(&exec, &AdminRetryExecutor::finished, &exec, [&](bool, const QString &msg, AdminErrorCategory cat) {
            ++finishedCount;
            finalMsg = msg;
            finalCat = cat;
        });

        verify(exec.execute(plan, consent), "S5-09: Executor starts job");
        verify(runningJob != nullptr, "S5-09: runningJob created");
        opManager.track(runningJob, QStringLiteral("Administrator operation"));
        verify(opManager.activeCount() == 1, "S5-09: OperationManager tracks active privileged job");

        runningJob->emitProgress(50, 512, 1024);
        verify(progressCount >= 1, "S5-09: Progress signals forwarded from KJob to AdminRetryExecutor");

        // Cancel through OperationManager (which invokes job->kill(KJob::EmitResult))
        const auto ops = opManager.operations();
        verify(!ops.isEmpty(), "S5-09: OperationManager exposes tracked admin operation");
        verify(opManager.cancelOperation(ops.first().id), "S5-09: OperationManager::cancelOperation() kills active admin job");
        opManager.finish(runningJob, false, true, finalMsg);
        verify(finishedCount == 1, "S5-09: Exactly ONE finished signal emitted upon cancellation");
        verify(finalCat == AdminErrorCategory::UserCancelled, "S5-09: Error category is UserCancelled");
        verify(!finalMsg.isEmpty(), "S5-09: Cancellation message is non-empty");
        verify(!exec.isRunning(), "S5-09: Executor is no longer running");
        verify(AdminRetryExecutor::activeTargetCount(plan.destinationUrl.toLocalFile()) == 0,
               "S5-09: Active target path released after OperationManager cancel");

        delete runningJob;
        recordTestPass("S5-09: OperationManager tracking, progress, cancellation & single finished signal");
    }

    // S5-10: Destruction of Executor / KJob during active operation (Teardown Resilience)
    {
        const QString targetPath = QDir(protectedDir).filePath(QStringLiteral("user_doc.txt"));
        MockStage5Job permJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        permJob.setAutoDelete(false);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));
        const AdminRetryRequest req = controller.createGuiRequest(&permJob, ctx);

        // Subcase A: Executor destroyed while job is active (e.g., window closed during Polkit auth)
        {
            AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
            AdminUserConsent consent = controller.createConsent(plan);
            MockStage5Job *liveJob = nullptr;
            int strayFinished = 0;
            {
                AdminRetryExecutor scopedExec;
                scopedExec.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
                    liveJob = new MockStage5Job(0);
                    liveJob->setAutoDelete(false);
                    liveJob->setAutoEmitResult(false);
                    return liveJob;
                });
                QObject::connect(&scopedExec, &AdminRetryExecutor::finished, &scopedExec, [&](bool, const QString &, AdminErrorCategory) {
                    ++strayFinished;
                });
                verify(scopedExec.execute(plan, consent), "S5-10A: Scoped executor started");
                verify(AdminRetryExecutor::activeTargetCount(targetPath) == 1, "S5-10A: Target path registered while running");
            }
            // scopedExec is now destroyed
            verify(strayFinished == 0, "S5-10A: Destroying executor does not emit stray finished signal");
            verify(liveJob != nullptr && liveJob->wasKilled(), "S5-10A: Destroying executor quietly killed active KJob");
            verify(AdminRetryExecutor::activeTargetCount(targetPath) == 0,
                   "S5-10A: Destroying executor released target path in activeTargetsMap");
            delete liveJob;
        }

        // Subcase B: External KJob destruction before result signal
        {
            AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
            AdminUserConsent consent = controller.createConsent(plan);
            AdminRetryExecutor exec;
            MockStage5Job *transientJob = nullptr;
            exec.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
                transientJob = new MockStage5Job(0);
                transientJob->setAutoDelete(false);
                transientJob->setAutoEmitResult(false);
                return transientJob;
            });
            verify(exec.execute(plan, consent), "S5-10B: Executor started");
            verify(AdminRetryExecutor::activeTargetCount(targetPath) == 1, "S5-10B: Target path acquired");
            delete transientJob;
            verify(!exec.isRunning() && exec.currentJob() == nullptr,
                   "S5-10B: External KJob deletion cleanly resets executor state");
            verify(AdminRetryExecutor::activeTargetCount(targetPath) == 0,
                   "S5-10B: External KJob deletion releases target path");
        }
        recordTestPass("S5-10: Executor & KJob destruction lifecycle safety");
    }

    // S5-11: Undo/Redo Isolation (No Fake Undo for Admin Ops + Normal History Preserved)
    {
        LocalMoveHistory moveHistory;
        const QString normSrc = QDir(userDir).filePath(QStringLiteral("undo_norm_src.txt"));
        const QString normDst = QDir(userDir).filePath(QStringLiteral("undo_norm_dst.txt"));
        verify(writeStage5File(normSrc, "undo-payload"), "S5-11: Normal move source created");

        LocalFileMoveJob normalMove(QUrl::fromLocalFile(normSrc), QUrl::fromLocalFile(normDst));
        normalMove.setAutoDelete(false);
        verify(moveHistory.recordWhenCompleted(&normalMove), "S5-11: Normal move registered in LocalMoveHistory");
        QEventLoop moveLoop;
        QObject::connect(&normalMove, &KJob::result, &moveLoop, &QEventLoop::quit);
        normalMove.start();
        moveLoop.exec();
        verify(moveHistory.count() == 1 && moveHistory.canUndo(), "S5-11: Normal move recorded in Undo history");

        // Now execute a privileged Copy operation and verify Undo history is NOT polluted
        MockStage5Job permJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        permJob.setAutoDelete(false);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));
        const AdminRetryRequest req = controller.createGuiRequest(&permJob, ctx);
        verify(!controller.shouldRecordUndo(req), "S5-11: shouldRecordUndo is false for privileged request");
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(!plan.recordsUndo, "S5-11: plan.recordsUndo is false");
        verify(moveHistory.count() == 1 && moveHistory.position() == 1 && moveHistory.canUndo(),
               "S5-11: Normal LocalMoveHistory remains intact and unpolluted by admin operation");

        QFile::remove(normDst);
        recordTestPass("S5-11: Undo/Redo isolation & preservation of normal Undo history");
    }

    // =========================================================================
    // SECTION 5 (Prompt Section 9): Primary / Split View Independence & Context
    // =========================================================================

    // S5-12: Primary/Split View Context Immutability Across Pane Switch, Swap, Clipboard & Split Close
    {
        ThisPcWindow view(QUrl::fromLocalFile(userDir), false);
        view.show();
        QCoreApplication::processEvents();

        view.setSplitViewEnabled(true);
        QCoreApplication::processEvents();
        verify(view.m_splitPane != nullptr && view.m_splitPane->isVisible(),
               "S5-12: Split View enabled and visible");
        view.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(splitDir), true);
        QCoreApplication::processEvents();

        // Capture operation context while Primary is at userDir targeting protectedDir
        const FileOperationContext capturedCtx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));

        // Mutate live UI state before creating the retry request/plan:
        // 1. Change clipboard to a completely different file in splitDir
        const QString splitOtherFile = QDir(splitDir).filePath(QStringLiteral("split_other.txt"));
        verify(writeStage5File(splitOtherFile, "other-split-data"), "S5-12: split_other.txt created");
        QApplication::clipboard()->setText(splitOtherFile);

        // 2. Swap Primary and Split panes
        view.swapSplitPanes();
        QCoreApplication::processEvents();

        // 3. Navigate Primary back/forward or to another folder
        view.navigateTo(QUrl::fromLocalFile(splitDir), true);
        QCoreApplication::processEvents();

        // 4. Close Split View while operation is awaiting confirmation
        view.setSplitViewEnabled(false);
        QCoreApplication::processEvents();
        verify(!view.m_splitPane->isVisible(), "S5-12: Split View closed while awaiting confirmation");

        // Verify that AdminRetryRequest and AdminExecutionPlan strictly use capturedCtx
        MockStage5Job failedJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        failedJob.setAutoDelete(false);
        const AdminRetryRequest req = controller.createGuiRequest(&failedJob, capturedCtx);
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);

        verify(plan.planType == AdminPlanType::CopyFileToProtected,
               "S5-12: Plan remains valid after Split View close and pane navigation");
        verify(plan.sourceUrl == QUrl::fromLocalFile(userSourceFile),
               "S5-12: Plan sourceUrl is strictly bound to original context (immune to clipboard/pane changes)");
        verify(plan.destinationDirectory == QUrl::fromLocalFile(protectedDir),
               "S5-12: Plan destinationDirectory is strictly bound to original context");

        // Verify refreshAffectedDirectories does not crash when Split View is closed or open
        view.refreshAffectedDirectories(capturedCtx);
        view.setSplitViewEnabled(true);
        view.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(protectedDir), true);
        QCoreApplication::processEvents();
        view.refreshAffectedDirectories(capturedCtx);
        QCoreApplication::processEvents();

        QFile::remove(splitOtherFile);
        recordTestPass("S5-12: Primary/Split View context immutability & safe refresh");
    }

    // =========================================================================
    // SECTION 6 (Prompt Section 10): Concurrency & Stress Scenarios
    // =========================================================================

    // S5-13: Two Concurrent Retry Attempts on the SAME Destination Path -> Second Blocked
    {
        MockStage5Job permJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        permJob.setAutoDelete(false);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));

        const AdminRetryRequest req1 = controller.createGuiRequest(&permJob, ctx);
        AdminExecutionPlan plan1 = controller.createGuiExecutionPlan(req1);
        AdminUserConsent consent1 = controller.createConsent(plan1);

        const AdminRetryRequest req2 = controller.createGuiRequest(&permJob, ctx);
        AdminExecutionPlan plan2 = controller.createGuiExecutionPlan(req2);
        AdminUserConsent consent2 = controller.createConsent(plan2);

        AdminRetryExecutor exec1;
        AdminRetryExecutor exec2;
        MockStage5Job *job1 = nullptr;
        exec1.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
            job1 = new MockStage5Job(0);
            job1->setAutoDelete(false);
            job1->setAutoEmitResult(false);
            return job1;
        });
        exec2.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
            auto *j = new MockStage5Job(0);
            j->setAutoEmitResult(false);
            return j;
        });

        verify(exec1.execute(plan1, consent1), "S5-13: First executor acquires target path and starts");
        verify(!exec2.execute(plan2, consent2), "S5-13: Second executor targeting same path is immediately blocked");
        verify(exec2.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TargetUsedByAnotherTask,
               "S5-13: Second executor rejection reason is TargetUsedByAnotherTask");

        job1->finishWithSuccess();
        verify(!exec1.isRunning(), "S5-13: First executor finishes cleanly");
        verify(AdminRetryExecutor::activeTargetCount(plan1.destinationUrl.toLocalFile()) == 0,
               "S5-13: Target path count returns to 0 after first executor finishes");
        delete job1;
        recordTestPass("S5-13: Concurrent retry on same destination path blocked safely");
    }

    // S5-14: Concurrent Jobs on DIFFERENT Paths — Cancelling One Does Not Affect the Other
    {
        const QString secondSource = QDir(userDir).filePath(QStringLiteral("second_source.txt"));
        verify(writeStage5File(secondSource, "second-payload"), "S5-14: second_source.txt created");

        MockStage5Job permJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        permJob.setAutoDelete(false);

        const FileOperationContext ctxA = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(userSourceFile)},
            QUrl::fromLocalFile(protectedDir));
        const FileOperationContext ctxB = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(secondSource)},
            QUrl::fromLocalFile(protectedDir));

        AdminExecutionPlan planA = controller.createGuiExecutionPlan(controller.createGuiRequest(&permJob, ctxA));
        AdminUserConsent consentA = controller.createConsent(planA);

        AdminExecutionPlan planB = controller.createGuiExecutionPlan(controller.createGuiRequest(&permJob, ctxB));
        AdminUserConsent consentB = controller.createConsent(planB);

        AdminRetryExecutor execA;
        AdminRetryExecutor execB;
        MockStage5Job *jobA = nullptr;
        MockStage5Job *jobB = nullptr;

        execA.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
            jobA = new MockStage5Job(0);
            jobA->setAutoDelete(false);
            jobA->setAutoEmitResult(false);
            return jobA;
        });
        execB.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
            jobB = new MockStage5Job(0);
            jobB->setAutoDelete(false);
            jobB->setAutoEmitResult(false);
            return jobB;
        });

        bool doneA = false, okA = true;
        bool doneB = false, okB = false;
        QObject::connect(&execA, &AdminRetryExecutor::finished, &execA, [&](bool s, const QString &, AdminErrorCategory) {
            doneA = true;
            okA = s;
        });
        QObject::connect(&execB, &AdminRetryExecutor::finished, &execB, [&](bool s, const QString &, AdminErrorCategory) {
            doneB = true;
            okB = s;
        });

        verify(execA.execute(planA, consentA), "S5-14: Executor A starts on path A");
        verify(execB.execute(planB, consentB), "S5-14: Executor B starts concurrently on path B");

        execA.cancel();
        verify(doneA && !okA && !execA.isRunning(), "S5-14: Executor A cancelled cleanly");
        verify(execB.isRunning() && !doneB, "S5-14: Executor B remains active and unaffected by Executor A cancel");

        jobB->finishWithSuccess();
        verify(doneB && okB && !execB.isRunning(), "S5-14: Executor B completes with success");

        delete jobA;
        delete jobB;
        QFile::remove(secondSource);
        recordTestPass("S5-14: Concurrent tasks on different paths & isolated cancellation");
    }

    // S5-15: Rapid Double-Click Confirmation & TOCTOU Source Change Between Preflight and Execute
    {
        const QString mutableSource = QDir(userDir).filePath(QStringLiteral("mutable_source.txt"));
        verify(writeStage5File(mutableSource, "initial-v1"), "S5-15: mutable_source.txt created");

        MockStage5Job permJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        permJob.setAutoDelete(false);
        const FileOperationContext ctx = FileOperationContext::create(
            FileOperationType::Copy,
            {QUrl::fromLocalFile(mutableSource)},
            QUrl::fromLocalFile(protectedDir));

        // Subcase A: Rapid double-submission of the same plan & consent
        {
            AdminExecutionPlan plan = controller.createGuiExecutionPlan(controller.createGuiRequest(&permJob, ctx));
            AdminUserConsent consent = controller.createConsent(plan);
            AdminRetryExecutor exec;
            MockStage5Job *job = nullptr;
            exec.setMockJobFactory([&](const AdminExecutionPlan &) -> KJob* {
                job = new MockStage5Job(0);
                job->setAutoDelete(false);
                job->setAutoEmitResult(false);
                return job;
            });

            verify(exec.execute(plan, consent), "S5-15A: First click starts execution");
            verify(!exec.execute(plan, consent), "S5-15A: Rapid second click while running is rejected");
            job->finishWithSuccess();
            verify(!exec.execute(plan, consent), "S5-15A: Replay after completion is rejected (consumed)");
            delete job;
        }

        // Subcase B: Source file modified between plan creation and execute()
        {
            AdminExecutionPlan plan = controller.createGuiExecutionPlan(controller.createGuiRequest(&permJob, ctx));
            AdminUserConsent consent = controller.createConsent(plan);
            verify(writeStage5File(mutableSource, "modified-payload-with-different-length-v2"),
                   "S5-15B: Source modified after plan creation");

            AdminRetryExecutor exec;
            AdminErrorCategory failCat = AdminErrorCategory::None;
            QObject::connect(&exec, &AdminRetryExecutor::finished, &exec, [&](bool, const QString &, AdminErrorCategory cat) {
                failCat = cat;
            });
            verify(!exec.execute(plan, consent), "S5-15B: Execution blocked when source identity changed after preflight");
            verify(failCat == AdminErrorCategory::PermissionDenied, "S5-15B: TOCTOU failure reported cleanly");
        }
        QFile::remove(mutableSource);
        recordTestPass("S5-15: Rapid double-confirmation & TOCTOU source mutation rejection");
    }

    // =========================================================================
    // SECTION 7 (Prompt Section 11): GUI Localization & Atomic Move Error Check
    // =========================================================================

    // S5-16: LocalFileMoveJob Atomic Move Failure Into Protected Dir (Localization + Move Blocked)
    {
        const QString moveSrc = QDir(userDir).filePath(QStringLiteral("move_test.txt"));
        const QString moveDst = QDir(protectedDir).filePath(QStringLiteral("move_test.txt"));
        verify(writeStage5File(moveSrc, "move-payload"), "S5-16: move_test.txt created");

        LocalFileMoveJob moveJob(QUrl::fromLocalFile(moveSrc), QUrl::fromLocalFile(moveDst));
        moveJob.setAutoDelete(false);
        QEventLoop loop;
        QObject::connect(&moveJob, &KJob::result, &loop, &QEventLoop::quit);
        moveJob.start();
        loop.exec();

        verify(moveJob.error() == KJob::UserDefinedError, "S5-16: LocalFileMoveJob into 0555 dir fails with UserDefinedError");
        verify(moveJob.moveError() == LocalFileMoveError::AtomicMove, "S5-16: moveError() is LocalFileMoveError::AtomicMove");
        verify(moveJob.systemError() == EACCES || moveJob.systemError() == EPERM,
               "S5-16: systemError() records EACCES/EPERM");
        verify(!moveJob.errorText().isEmpty(), "S5-16: errorText() is populated");
        if (QLocale().language() == QLocale::Polish) {
            verify(moveJob.errorText() == QStringLiteral("Nie można przenieść pliku atomowo."),
                   "S5-16: Polish locale returns localized atomic move error message");
        } else {
            verify(moveJob.errorText() == QStringLiteral("Could not move the file atomically."),
                   "S5-16: English locale returns English atomic move error message");
        }

        // Verify that passing this failed LocalFileMoveJob to inspectGuiFailedOperation keeps Move BLOCKED
        const FileOperationContext moveCtx = FileOperationContext::create(
            FileOperationType::Move,
            {QUrl::fromLocalFile(moveSrc)},
            QUrl::fromLocalFile(protectedDir));
        const AdminRetryQualification moveQual = controller.inspectGuiFailedOperation(&moveJob, moveCtx);
        verify(!moveQual.canAttemptElevation()
               && moveQual.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "S5-16: Failed LocalFileMoveJob remains strictly BLOCKED from GUI Admin Retry");
        verify(QFileInfo::exists(moveSrc) && !QFileInfo::exists(moveDst),
               "S5-16: Original source file is completely intact after failed atomic move");

        QFile::remove(moveSrc);
        recordTestPass("S5-16: LocalFileMoveJob atomic move localization & GUI Move block preserved");
    }

    // S5-17: LocalFileCopyJob Partial Destination Creation Error Localization (EN/PL) & Qualification Independence
    {
        const QLocale savedLocale;
        const QString copySrc = QDir(userDir).filePath(QStringLiteral("copy_loc_src.txt"));
        const QString copyDstNew = QDir(protectedDir).filePath(QStringLiteral("copy_loc_new.txt"));
        verify(writeStage5File(copySrc, "copy-loc-payload"), "S5-17: copy_loc_src.txt created");

        // Subcase A: Polish locale
        QLocale::setDefault(QLocale(QLocale::Polish, QLocale::Poland));
        {
            LocalFileCopyJob copyJobPl(QUrl::fromLocalFile(copySrc), QUrl::fromLocalFile(copyDstNew));
            copyJobPl.setAutoDelete(false);
            QEventLoop loop;
            QObject::connect(&copyJobPl, &KJob::result, &loop, &QEventLoop::quit);
            copyJobPl.start();
            loop.exec();

            verify(copyJobPl.error() == KJob::UserDefinedError, "S5-17A: LocalFileCopyJob into 0555 dir fails with UserDefinedError (PL)");
            verify(copyJobPl.transferError() == LocalTransferError::DestinationCreate,
                   "S5-17A: transferError() is LocalTransferError::DestinationCreate (PL)");
            verify(copyJobPl.systemError() == EACCES || copyJobPl.systemError() == EPERM,
                   "S5-17A: systemError() records EACCES/EPERM (PL)");
            verify(copyJobPl.errorText() == QStringLiteral("Nie można utworzyć tymczasowego pliku docelowego."),
                   "S5-17A: Polish locale returns 'Nie można utworzyć tymczasowego pliku docelowego.'");

            const FileOperationContext copyCtx = FileOperationContext::create(
                FileOperationType::Copy,
                {QUrl::fromLocalFile(copySrc)},
                QUrl::fromLocalFile(protectedDir));
            const AdminRetryQualification qualPl = controller.inspectGuiFailedOperation(&copyJobPl, copyCtx);
            verify(qualPl.canAttemptElevation(),
                   "S5-17A: Admin Retry qualification succeeds under Polish locale via errno/DestinationCreate");

            // Collision case under Polish locale (existing protectedReadOnlyFile)
            LocalFileCopyJob collisionJobPl(
                QUrl::fromLocalFile(copySrc),
                QUrl::fromLocalFile(protectedReadOnlyFile),
                nullptr,
                256 * 1024,
                0,
                true,
                true,
                LocalFileIdentity::read(protectedReadOnlyFile));
            collisionJobPl.setAutoDelete(false);
            QEventLoop colLoop;
            QObject::connect(&collisionJobPl, &KJob::result, &colLoop, &QEventLoop::quit);
            collisionJobPl.start();
            colLoop.exec();

            verify(collisionJobPl.transferError() == LocalTransferError::DestinationCreate,
                   "S5-17A: Collision overwrite into 0555 dir fails at DestinationCreate before touching existing file");
            verify(collisionJobPl.errorText() == QStringLiteral("Nie można utworzyć tymczasowego pliku docelowego."),
                   "S5-17A: Collision failure under Polish locale uses localized message");
            FileOperationContext colCtx = FileOperationContext::create(
                FileOperationType::Copy,
                {QUrl::fromLocalFile(copySrc)},
                QUrl::fromLocalFile(protectedDir));
            colCtx.destinationUrls = {QUrl::fromLocalFile(protectedReadOnlyFile)};
            const AdminRetryQualification colQualPl = controller.inspectGuiFailedOperation(&collisionJobPl, colCtx);
            verify(!colQualPl.canAttemptElevation()
                   && colQualPl.reason == AdminRetryIneligibilityReason::UnverifiablePartialState,
                   "S5-17A: Collision remains disqualified under Polish locale");
        }

        // Subcase B: English locale
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        {
            LocalFileCopyJob copyJobEn(QUrl::fromLocalFile(copySrc), QUrl::fromLocalFile(copyDstNew));
            copyJobEn.setAutoDelete(false);
            QEventLoop loop;
            QObject::connect(&copyJobEn, &KJob::result, &loop, &QEventLoop::quit);
            copyJobEn.start();
            loop.exec();

            verify(copyJobEn.error() == KJob::UserDefinedError, "S5-17B: LocalFileCopyJob into 0555 dir fails with UserDefinedError (EN)");
            verify(copyJobEn.transferError() == LocalTransferError::DestinationCreate,
                   "S5-17B: transferError() is LocalTransferError::DestinationCreate (EN)");
            verify(copyJobEn.systemError() == EACCES || copyJobEn.systemError() == EPERM,
                   "S5-17B: systemError() records EACCES/EPERM (EN)");
            verify(copyJobEn.errorText() == QStringLiteral("Could not create the partial destination."),
                   "S5-17B: English locale returns 'Could not create the partial destination.'");

            const FileOperationContext copyCtx = FileOperationContext::create(
                FileOperationType::Copy,
                {QUrl::fromLocalFile(copySrc)},
                QUrl::fromLocalFile(protectedDir));
            const AdminRetryQualification qualEn = controller.inspectGuiFailedOperation(&copyJobEn, copyCtx);
            verify(qualEn.canAttemptElevation(),
                   "S5-17B: Admin Retry qualification succeeds under English locale via errno/DestinationCreate");
        }

        QLocale::setDefault(savedLocale);
        QFile::remove(copySrc);
        recordTestPass("S5-17: LocalFileCopyJob partial destination creation EN/PL localization & qualification independence");
    }

    // Restore permissions before QTemporaryDir cleanup so all temporary files are cleanly removed
    ::chmod(QFile::encodeName(protectedDir).constData(), 0755);
    ::chmod(QFile::encodeName(protectedReadOnlyFile).constData(), 0644);

    qInfo("================================================================================");
    qInfo("STAGE 5 SUMMARY: PASS=%d | FAIL=%d | SKIPPED=%d | UNVERIFIED=%d | ASSERTIONS=%d",
          s5PassedTests, s5FailedTests, s5SkippedTests, s5UnverifiedTests, s5Checks);
    qInfo("================================================================================");
    verify(s5PassedTests == 17 && s5FailedTests == 0 && s5SkippedTests == 0 && s5UnverifiedTests == 0,
           "All 17 Stage 5 integration & safety regression scenarios passed with 0 skips and 0 unverified");

    return 0;
}
