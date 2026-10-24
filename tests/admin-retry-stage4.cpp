/*
 * Tests for ThisPC 0.43.0 Stage 4 GUI Integration — Safe Administrator Retry.
 *
 * Verifies GUI admin retry qualification, central policy allowlist (allowing only
 * Copy, CreateFile, CreateFolder, while strictly blocking Rename, Move, Delete, Trash, etc.),
 * contextual error qualification, two-stage explicit confirmation safety, execution planning,
 * single-use consent, fail-closed partial states, KJob and OperationManager integration,
 * permissions enforcement (0600 for Copy From Protected), undo suppression,
 * and multi-pane / split-view context preservation.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "adminretrycontract.h"
#include "adminretrycontroller.h"
#include "adminretryexecutor.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) {
        qFatal("FAIL: %s", description);
    }
    ++checks;
}

class MockAdminJob final : public KJob
{
public:
    explicit MockAdminJob(int errorCode = 0, const QString &errorString = QString())
    {
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

    bool doKill() override
    {
        setError(KJob::KilledJobError);
        setErrorText(QStringLiteral("Job killed"));
        emitResult();
        return true;
    }

private:
    bool m_started = false;
    bool m_autoEmitResult = true;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("thispc-admin-stage4-test"));
    QCoreApplication::setApplicationName(QStringLiteral("admin-retry-stage4-test"));

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "Temporary directory created successfully");
    const QString baseDir = tempDir.path();

    const QString testSrcDir = baseDir + QStringLiteral("/src");
    const QString testDestDir = baseDir + QStringLiteral("/dest");
    QDir().mkpath(testSrcDir);
    QDir().mkpath(testDestDir);

    const QString sampleSrcFile = testSrcDir + QStringLiteral("/document.txt");
    {
        QFile f(sampleSrcFile);
        verify(f.open(QIODevice::WriteOnly | QIODevice::Text), "Create sample source file");
        f.write("Administrator retry Stage 4 payload\n");
        f.close();
    }

    AdminRetryController controller;

    // --- Section 1: Central GUI Allowlist Helper ---
    // Test 1: Copy is allowed in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::Copy) == true,
           "Test 1: isGuiAdminRetryAllowed returns true for Copy");

    // Test 2: CreateFile is allowed in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::CreateFile) == true,
           "Test 2: isGuiAdminRetryAllowed returns true for CreateFile");

    // Test 3: CreateFolder is allowed in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::CreateFolder) == true,
           "Test 3: isGuiAdminRetryAllowed returns true for CreateFolder");

    // Test 4: Rename is strictly blocked in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::Rename) == false,
           "Test 4: isGuiAdminRetryAllowed returns false for Rename");

    // Test 5: Move is strictly blocked in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::Move) == false,
           "Test 5: isGuiAdminRetryAllowed returns false for Move");

    // Test 6: Delete is strictly blocked in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::Delete) == false,
           "Test 6: isGuiAdminRetryAllowed returns false for Delete");

    // Test 7: Trash is strictly blocked in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::Trash) == false,
           "Test 7: isGuiAdminRetryAllowed returns false for Trash");

    // Test 8: Restore is strictly blocked in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::Restore) == false,
           "Test 8: isGuiAdminRetryAllowed returns false for Restore");

    // Test 9: Custom is strictly blocked in GUI
    verify(isGuiAdminRetryAllowed(FileOperationType::Custom) == false,
           "Test 9: isGuiAdminRetryAllowed returns false for Custom");

    // --- Section 2: Policy Enforcement for GUI Requests ---
    // Test 10: GUI Request on Rename fails closed with OperationDisallowedInGui
    {
        AdminRetryRequest req;
        req.isGuiRequest = true;
        req.operationType = FileOperationType::Rename;
        req.error.category = AdminErrorCategory::PermissionDenied;
        const AdminRetryQualification q = AdminRetryPolicy::evaluate(req, true);
        verify(!q.canAttemptElevation(), "Test 10a: Rename is not eligible for GUI elevation");
        verify(q.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "Test 10b: Ineligibility reason is OperationDisallowedInGui");
    }

    // Test 11: GUI Request on Move fails closed with OperationDisallowedInGui
    {
        AdminRetryRequest req;
        req.isGuiRequest = true;
        req.operationType = FileOperationType::Move;
        req.error.category = AdminErrorCategory::PermissionDenied;
        const AdminRetryQualification q = AdminRetryPolicy::evaluate(req, true);
        verify(!q.canAttemptElevation(), "Test 11a: Move is not eligible for GUI elevation");
        verify(q.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "Test 11b: Ineligibility reason is OperationDisallowedInGui");
    }

    // Test 12: GUI Request on Delete fails closed with OperationDisallowedInGui
    {
        AdminRetryRequest req;
        req.isGuiRequest = true;
        req.operationType = FileOperationType::Delete;
        req.error.category = AdminErrorCategory::PermissionDenied;
        const AdminRetryQualification q = AdminRetryPolicy::evaluate(req, true);
        verify(!q.canAttemptElevation(), "Test 12a: Delete is not eligible for GUI elevation");
        verify(q.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "Test 12b: Ineligibility reason is OperationDisallowedInGui");
    }

    // Test 13: GUI Request on Trash fails closed with OperationDisallowedInGui
    {
        AdminRetryRequest req;
        req.isGuiRequest = true;
        req.operationType = FileOperationType::Trash;
        req.error.category = AdminErrorCategory::PermissionDenied;
        const AdminRetryQualification q = AdminRetryPolicy::evaluate(req, true);
        verify(!q.canAttemptElevation(), "Test 13a: Trash is not eligible for GUI elevation");
        verify(q.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "Test 13b: Ineligibility reason is OperationDisallowedInGui");
    }

    // Test 14: Non-GUI request preserves non-GUI evaluation logic
    {
        AdminRetryRequest req;
        req.isGuiRequest = false;
        req.operationType = FileOperationType::Rename;
        req.sourceUrls = {QUrl::fromLocalFile(sampleSrcFile)};
        req.destinationDirectory = QUrl::fromLocalFile(testSrcDir);
        req.requestedName = QStringLiteral("renamed.txt");
        req.error.category = AdminErrorCategory::PermissionDenied;
        req.partialState = AdminPartialCompletionState::NotStarted;
        const AdminRetryQualification q = AdminRetryPolicy::evaluate(req, true);
        verify(q.reason != AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "Test 14: Non-GUI rename is not blocked by OperationDisallowedInGui");
    }

    // --- Section 3: createGuiExecutionPlan Allowlist Enforcement ---
    // Test 15: createGuiExecutionPlan for Copy produces valid plan
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Copy;
        req.sourceUrls = {QUrl::fromLocalFile(sampleSrcFile)};
        req.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        req.requestedName = QStringLiteral("document.txt");
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CopyFileToProtected,
               "Test 15: GUI Copy produces CopyFileToProtected plan");
    }

    // Test 16: createGuiExecutionPlan for CreateFile produces valid plan
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::CreateFile;
        req.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        req.requestedName = QStringLiteral("new_gui_file.txt");
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CreateFileProtected,
               "Test 16: GUI CreateFile produces CreateFileProtected plan");
    }

    // Test 17: createGuiExecutionPlan for CreateFolder produces valid plan
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::CreateFolder;
        req.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        req.requestedName = QStringLiteral("new_gui_folder");
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CreateFolderProtected,
               "Test 17: GUI CreateFolder produces CreateFolderProtected plan");
    }

    // Test 18: createGuiExecutionPlan for Rename returns Invalid plan
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Rename;
        req.sourceUrls = {QUrl::fromLocalFile(sampleSrcFile)};
        req.destinationDirectory = QUrl::fromLocalFile(testSrcDir);
        req.requestedName = QStringLiteral("renamed.txt");
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::Invalid,
               "Test 18: GUI Rename produces Invalid plan");
    }

    // Test 19: createGuiExecutionPlan for Move returns Invalid plan
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Move;
        req.sourceUrls = {QUrl::fromLocalFile(sampleSrcFile)};
        req.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        req.requestedName = QStringLiteral("document.txt");
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::Invalid,
               "Test 19: GUI Move produces Invalid plan");
    }

    // Test 20: createGuiExecutionPlan for Delete returns Invalid plan
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Delete;
        req.sourceUrls = {QUrl::fromLocalFile(sampleSrcFile)};
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::Invalid,
               "Test 20: GUI Delete produces Invalid plan");
    }

    // --- Section 4: inspectGuiFailedOperation Validation ---
    // Test 21: Permission denied on Copy To Protected -> Eligible
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/document.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Permission denied"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(q.canAttemptElevation(), "Test 21a: Copy to protected is eligible for GUI retry");
        verify(q.eligibility == AdminRetryEligibility::Eligible, "Test 21b: Eligibility is Eligible");
    }

    // Test 22: Permission denied on Copy From Protected -> Eligible
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/read_protected.txt"))};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read file"));
        job.setProperty("thispcSystemError", EACCES);
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false, AdminPartialCompletionState::NotStarted, EACCES);
        verify(q.canAttemptElevation(), "Test 22: Copy from protected is eligible for GUI retry");
    }

    // Test 23: Permission denied on CreateFile -> Eligible
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/created_file.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Cannot create file"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(q.canAttemptElevation(), "Test 23: CreateFile is eligible for GUI retry");
    }

    // Test 24: Permission denied on CreateFolder -> Eligible
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFolder;
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/created_folder"))};

        MockAdminJob job(KIO::ERR_CANNOT_MKDIR, QStringLiteral("Cannot create directory"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(q.canAttemptElevation(), "Test 24: CreateFolder is eligible for GUI retry");
    }

    // Test 25: Permission denied on Rename in GUI -> Ineligible
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Rename;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testSrcDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testSrcDir + QStringLiteral("/renamed.txt"))};

        MockAdminJob job(KIO::ERR_CANNOT_RENAME, QStringLiteral("Permission denied"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(), "Test 25a: Rename in GUI is ineligible");
        verify(q.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "Test 25b: Reason is OperationDisallowedInGui");
    }

    // Test 26: Permission denied on Move in GUI -> Ineligible
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Move;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/moved.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Permission denied"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(), "Test 26a: Move in GUI is ineligible");
        verify(q.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "Test 26b: Reason is OperationDisallowedInGui");
    }

    // --- Section 5: Fail-Closed Error Categories in GUI ---
    // Test 27: Disk full error -> Ineligible (DiskFull)
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);

        MockAdminJob job(KIO::ERR_DISK_FULL, QStringLiteral("Disk is full"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(), "Test 27a: Disk full is not eligible");
        verify(q.reason == AdminRetryIneligibilityReason::DiskFull, "Test 27b: Reason is DiskFull");
    }

    // Test 28: Target collision -> Ineligible (TargetCollision)
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(testSrcDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(sampleSrcFile)}; // sampleSrcFile already exists!

        MockAdminJob job(KIO::ERR_FILE_ALREADY_EXIST, QStringLiteral("File exists"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(), "Test 28a: Collision is not eligible");
    }

    // Test 29: Path not found -> Ineligible (SourceMissing)
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(baseDir + QStringLiteral("/non_existent_file.txt"))};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);

        MockAdminJob job(KIO::ERR_DOES_NOT_EXIST, QStringLiteral("File not found"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(), "Test 29: Source missing is not eligible");
    }

    // Test 30: User cancelled job -> Ineligible (UserCancelled)
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);

        MockAdminJob job(KIO::ERR_USER_CANCELED, QStringLiteral("Cancelled"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, true);
        verify(!q.canAttemptElevation(), "Test 30a: Cancelled job is not eligible");
        verify(q.reason == AdminRetryIneligibilityReason::UserCancelled, "Test 30b: Reason is UserCancelled");
    }

    // Test 31: Already retried operation prevents retry loops
    {
        AdminRetryRequest req;
        req.isGuiRequest = true;
        req.operationType = FileOperationType::Copy;
        req.error.category = AdminErrorCategory::PermissionDenied;
        req.alreadyRetriedAsAdmin = true;
        const AdminRetryQualification q = AdminRetryPolicy::evaluate(req, true);
        verify(!q.canAttemptElevation(), "Test 31a: Already retried is not eligible");
        verify(q.reason == AdminRetryIneligibilityReason::AlreadyRetriedAsAdmin, "Test 31b: Loop prevention");
    }

    // Test 32: Missing admin worker protocol blocks elevation
    {
        AdminRetryRequest req;
        req.isGuiRequest = true;
        req.operationType = FileOperationType::Copy;
        req.error.category = AdminErrorCategory::PermissionDenied;
        const AdminRetryQualification q = AdminRetryPolicy::evaluate(req, false /* isKioAdminAvailable = false */);
        verify(!q.canAttemptElevation(), "Test 32a: Missing admin worker blocks elevation");
        verify(q.reason == AdminRetryIneligibilityReason::MissingAdminWorker, "Test 32b: MissingAdminWorker");
    }

    // --- Section 6: User Consent Lifecycle ---
    // Test 33: Consent creation binds planId and destinationUrl
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Copy;
        req.sourceUrls = {QUrl::fromLocalFile(sampleSrcFile)};
        req.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        req.requestedName = QStringLiteral("document.txt");
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan, 60000);

        verify(!consent.consentId.isEmpty(), "Test 33a: Consent has unique ID");
        verify(consent.planId == plan.planId, "Test 33b: Consent is bound to planId");
        verify(consent.confirmedDestinationUrl == plan.destinationUrl, "Test 33c: Consent binds destinationUrl");
        verify(!consent.consumed, "Test 33d: Consent is initially unconsumed");
    }

    // Test 34: Consent expiration detection
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Copy;
        req.sourceUrls = {QUrl::fromLocalFile(sampleSrcFile)};
        req.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        req.requestedName = QStringLiteral("document.txt");
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 5000);

        // Advance 10 seconds into the future
        const QDateTime expiredTime = consent.grantedAt.addSecs(10);
        verify(consent.isExpired(expiredTime) == true, "Test 34: Expired consent detected");
    }

    // Test 35: Consent planId mismatch is rejected
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Copy;
        req.sourceUrls = {QUrl::fromLocalFile(sampleSrcFile)};
        req.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        req.requestedName = QStringLiteral("document.txt");
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent mismatchedConsent = controller.createConsent(plan, 60000);
        mismatchedConsent.planId = QStringLiteral("different-plan-id");

        const AdminValidationStatus status = controller.validatePlan(plan, mismatchedConsent);
        verify(!status.isValid(), "Test 35a: Plan validation fails on ID mismatch");
        verify(status.code == AdminPlanValidationResult::ConsentMismatch, "Test 35b: ConsentMismatch result");
    }

    // --- Section 7: Default Permissions Policy ---
    // Test 36: CopyFileFromProtected enforces mode 0600
    {
        const int perms = AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CopyFileFromProtected);
        verify(perms == 0600, "Test 36: CopyFileFromProtected enforces least-privilege mode 0600");
    }

    // Test 37: CopyFileToProtected uses system default permissions (-1)
    {
        const int perms = AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CopyFileToProtected);
        verify(perms == -1, "Test 37: CopyFileToProtected uses default permissions (-1)");
    }

    // --- Section 8: Execution via AdminRetryExecutor ---
    // Test 38: Execution of Copy To Protected with mock job succeeds and emits finished
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/document_retried.txt"))};
        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        exec.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(0);
        });

        bool finishedFired = false;
        bool finishedSuccess = false;
        QObject::connect(&exec, &AdminRetryExecutor::finished,
            [&finishedFired, &finishedSuccess](bool success, const QString &, AdminErrorCategory) {
                finishedFired = true;
                finishedSuccess = success;
            });

        const bool started = exec.execute(plan, consent);
        verify(started, "Test 38a: Executor successfully started Copy To Protected");
        verify(plan.consumed, "Test 38b: Plan is marked consumed immediately");
        verify(consent.consumed, "Test 38c: Consent is marked consumed immediately");

        QEventLoop loop;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();

        verify(finishedFired, "Test 38d: Finished signal fired");
        verify(finishedSuccess, "Test 38e: Execution finished with success");
    }

    // Test 39: Properties are set correctly on KJob during privileged execution
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/tracked_job_file.txt"))};
        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock](const AdminExecutionPlan &) -> KJob* {
            return mock;
        });

        const bool started = exec.execute(plan, consent);
        verify(started, "Test 39a: CreateFile started");
        KJob *jobPtr = exec.currentJob();
        verify(jobPtr != nullptr, "Test 39b: currentJob is accessible");
        verify(jobPtr->property("thispcPrivilegedOperation").toBool() == true, "Test 39c: thispcPrivilegedOperation property is true");
        verify(jobPtr->property("thispcAdminPlanId").toString() == plan.planId, "Test 39d: thispcAdminPlanId property matches planId");

        mock->finishWithSuccess();
        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // Test 40: Attempting to execute an Invalid plan (e.g. disallowed Rename) is rejected
    {
        AdminExecutionPlan invalidPlan;
        invalidPlan.planType = AdminPlanType::Invalid;
        AdminUserConsent consent = controller.createConsent(invalidPlan, 60000);

        AdminRetryExecutor exec;
        bool finishedFired = false;
        bool finishedSuccess = true;
        QObject::connect(&exec, &AdminRetryExecutor::finished,
            [&finishedFired, &finishedSuccess](bool success, const QString &, AdminErrorCategory) {
                finishedFired = true;
                finishedSuccess = success;
            });

        const bool started = exec.execute(invalidPlan, consent);
        verify(!started, "Test 40a: Execution on Invalid plan fails pre-execution check");
        verify(finishedFired, "Test 40b: Finished signal fired on rejection");
        verify(!finishedSuccess, "Test 40c: Execution failed as expected");
    }

    // Test 41: Cancellation during execution kills job quietly and emits UserCancelled
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/to_cancel.txt"))};
        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock](const AdminExecutionPlan &) -> KJob* {
            return mock;
        });

        bool finishedFired = false;
        AdminErrorCategory errCat = AdminErrorCategory::None;
        QObject::connect(&exec, &AdminRetryExecutor::finished,
            [&finishedFired, &errCat](bool, const QString &, AdminErrorCategory cat) {
                finishedFired = true;
                errCat = cat;
            });

        verify(exec.execute(plan, consent), "Test 41a: Operation started");
        exec.cancel();

        verify(finishedFired, "Test 41b: Finished signal fired upon cancellation");
        verify(errCat == AdminErrorCategory::UserCancelled, "Test 41c: Error category is UserCancelled");
        verify(!exec.isRunning(), "Test 41d: Executor is no longer running");
    }

    // Test 42: Polkit cancellation / AccessDenied during privileged execution
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFolder;
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/polkit_cancel_folder"))};
        MockAdminJob job(KIO::ERR_CANNOT_MKDIR, QStringLiteral("Cannot mkdir"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock](const AdminExecutionPlan &) -> KJob* {
            return mock;
        });

        bool finishedFired = false;
        AdminErrorCategory errCat = AdminErrorCategory::None;
        QObject::connect(&exec, &AdminRetryExecutor::finished,
            [&finishedFired, &errCat](bool, const QString &, AdminErrorCategory cat) {
                finishedFired = true;
                errCat = cat;
            });

        verify(exec.execute(plan, consent), "Test 42a: Operation started");
        mock->finishWithError(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied by polkit"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(finishedFired, "Test 42b: Finished signal fired on auth denial");
        verify(errCat == AdminErrorCategory::AuthorizationCancelledOrDenied,
               "Test 42c: Classified as AuthorizationCancelledOrDenied");
    }

    // --- Section 9: Undo Policy & Architectural Safety ---
    // Test 43: Privileged operations do not record undo
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Copy;
        verify(controller.shouldRecordUndo(req) == false,
               "Test 43: shouldRecordUndo returns false for all privileged operations");
    }

    // Test 44: Context preservation (sources and destination directory preserved from initial failure)
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/ctx_test.txt"))};

        const AdminRetryRequest req = controller.createGuiRequest(nullptr, ctx, false);
        verify(req.isGuiRequest == true, "Test 44a: Request marked as GUI request");
        verify(req.sourceUrls == ctx.sources, "Test 44b: Source URLs preserved exactly");
        verify(req.destinationDirectory == ctx.destinationDirectory, "Test 44c: Destination directory preserved exactly");
    }

    // Test 45: TOCTOU protection when destination file appears before execution
    {
        const QString targetPath = testDestDir + QStringLiteral("/preexisting_file.txt");
        {
            QFile f(targetPath);
            verify(f.open(QIODevice::WriteOnly), "Create pre-existing file");
            f.write("existing content");
            f.close();
        }

        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetPath)};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Permission denied"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(), "Test 45: Ineligible because target item exists (TOCTOU / collision)");
    }

    // Test 46: Double execution prevention on single-use consent
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/double_exec.txt"))};
        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        exec.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(0);
        });

        verify(exec.execute(plan, consent), "Test 46a: First execution succeeds");
        verify(consent.consumed == true, "Test 46b: Consent is marked consumed");

        // Attempt second execution with same consent
        const bool secondStarted = exec.execute(plan, consent);
        verify(!secondStarted, "Test 46c: Second execution with consumed consent is rejected");
    }

    // --- Section 10: Copy From Protected Direction & Destination Failure Cleanup ---
    // Test 47: Unreadable local source (e.g. mode 0000) with user-writable destination resolves to CopyFileFromProtected
    {
        const QString unreadableSrc = testSrcDir + QStringLiteral("/unreadable_mode0000.txt");
        {
            QFile f(unreadableSrc);
            verify(f.open(QIODevice::WriteOnly), "Create unreadable source file");
            f.write("sensitive data from protected source");
            f.close();
            verify(::chmod(QFile::encodeName(unreadableSrc).constData(), 0000) == 0, "chmod 0000");
        }

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(unreadableSrc)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/unreadable_copy.txt"))};

        MockAdminJob failedInitialJob(KJob::UserDefinedError, QStringLiteral("Nie można odczytać pliku źródłowego"));
        failedInitialJob.setProperty("thispcSystemError", EACCES);

        const AdminRetryQualification qual = controller.inspectGuiFailedOperation(&failedInitialJob, ctx, false);
        verify(qual.canAttemptElevation(), "Test 47a: Mode 0000 local source qualifies for elevation");

        const AdminRetryRequest req = controller.createGuiRequest(&failedInitialJob, ctx, false);
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);

        verify(plan.planType == AdminPlanType::CopyFileFromProtected,
               "Test 47b: Local-to-local copy with unreadable source selected CopyFileFromProtected");
        verify(plan.effectiveSourceUrl.scheme() == QStringLiteral("admin"),
               "Test 47c: Effective source URL uses admin://");
        verify(plan.effectiveDestinationUrl.scheme() == QStringLiteral("file"),
               "Test 47d: Effective destination URL uses file://");
        verify(plan.destinationUrl.isLocalFile(),
               "Test 47e: Destination URL remains local file://");

        // Clean up chmod to allow directory deletion
        ::chmod(QFile::encodeName(unreadableSrc).constData(), 0644);
    }

    // Test 48: Default permissions for CopyFileFromProtected are 0600
    {
        verify(AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CopyFileFromProtected) == 0600,
               "Test 48: Default permissions for CopyFileFromProtected are strictly 0600");
        verify(AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CopyFileToProtected) == -1,
               "Test 48b: Default permissions for CopyFileToProtected are default (-1)");
    }

    // Test 49: Dual-side elevation protection (both source unreadable and destination unwritable)
    {
        const QString dualSrc = testSrcDir + QStringLiteral("/dual_src.txt");
        const QString unwritableDestDir = testDestDir + QStringLiteral("/unwritable_dest");
        {
            QFile f(dualSrc);
            verify(f.open(QIODevice::WriteOnly), "Create dual source file");
            f.write("content");
            f.close();
            verify(::chmod(QFile::encodeName(dualSrc).constData(), 0000) == 0, "chmod 0000 src");

            verify(QDir().mkdir(unwritableDestDir), "Create unwritable dest dir");
            verify(::chmod(QFile::encodeName(unwritableDestDir).constData(), 0555) == 0, "chmod 0555 dest dir");
        }

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(dualSrc)};
        ctx.destinationDirectory = QUrl::fromLocalFile(unwritableDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(unwritableDestDir + QStringLiteral("/dual_out.txt"))};

        MockAdminJob job(KJob::UserDefinedError, QStringLiteral("Access denied"));
        job.setProperty("thispcSystemError", EACCES);

        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);

        verify(plan.planType == AdminPlanType::Invalid,
               "Test 49: Dual-side unreadable source and unwritable destination yields Invalid plan");

        // Restore permissions for cleanup
        ::chmod(QFile::encodeName(dualSrc).constData(), 0644);
        ::chmod(QFile::encodeName(unwritableDestDir).constData(), 0755);
    }

    // Test 50: Fail-closed policy on failed CopyFileFromProtected (no auto-unlink of unverified 0-byte file)
    {
        const QString unreadableSrc = testSrcDir + QStringLiteral("/src_for_clean50.txt");
        {
            QFile f(unreadableSrc);
            verify(f.open(QIODevice::WriteOnly), "Create src_for_clean50");
            f.write("content");
            f.close();
            verify(::chmod(QFile::encodeName(unreadableSrc).constData(), 0000) == 0, "chmod 0000");
        }

        // Case 50A: Read fails before any destination file is created on disk
        const QString noFileDestPath = testDestDir + QStringLiteral("/no_file_created.txt");
        {
            FileOperationContext ctx;
            ctx.type = FileOperationType::Copy;
            ctx.sources = {QUrl::fromLocalFile(unreadableSrc)};
            ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
            ctx.destinationUrls = {QUrl::fromLocalFile(noFileDestPath)};

            MockAdminJob job(KJob::UserDefinedError, QStringLiteral("Permission denied"));
            job.setProperty("thispcSystemError", EACCES);
            const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
            AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
            AdminUserConsent consent = controller.createConsent(plan, 60000);

            AdminRetryExecutor exec;
            exec.setKioAdminAvailableOverride(true);
            MockAdminJob *mock = new MockAdminJob(0);
            mock->setAutoEmitResult(false);
            exec.setMockJobFactory([mock](const AdminExecutionPlan &) -> KJob* {
                return mock;
            });

            verify(exec.execute(plan, consent), "Test 50a: Execution started (no destination file created)");
            mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read protected source"));

            QEventLoop loop;
            QTimer::singleShot(50, &loop, &QEventLoop::quit);
            loop.exec();

            verify(!QFile::exists(noFileDestPath), "Test 50b: No destination file exists");
            verify(exec.lastFailureState() == AdminTransferFailureState::ReadFailedBeforeDataTransfer,
                   "Test 50c: Classified as ReadFailedBeforeDataTransfer when no artifact was created");
            verify(!exec.lastArtifactAudit().artifactExists, "Test 50d: Audit confirms no artifact exists");
        }

        // Case 50B: Unverified 0-byte file created during transfer (or by another task) is PRESERVED (fail-closed)
        const QString destCleanPath = testDestDir + QStringLiteral("/cleanup_zero_byte.txt");
        {
            FileOperationContext ctx;
            ctx.type = FileOperationType::Copy;
            ctx.sources = {QUrl::fromLocalFile(unreadableSrc)};
            ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
            ctx.destinationUrls = {QUrl::fromLocalFile(destCleanPath)};

            MockAdminJob job(KJob::UserDefinedError, QStringLiteral("Permission denied"));
            job.setProperty("thispcSystemError", EACCES);
            const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
            AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
            AdminUserConsent consent = controller.createConsent(plan, 60000);

            AdminRetryExecutor exec;
            exec.setKioAdminAvailableOverride(true);

            QString reportedError;
            QObject::connect(&exec, &AdminRetryExecutor::finished,
                [&reportedError](bool, const QString &msg, AdminErrorCategory) {
                    reportedError = msg;
                });

            MockAdminJob *mock = new MockAdminJob(0);
            mock->setAutoEmitResult(false);
            exec.setMockJobFactory([mock, destCleanPath](const AdminExecutionPlan &) -> KJob* {
                // Simulate unverified 0-byte file appearing at destination during job:
                QFile f(destCleanPath);
                f.open(QIODevice::WriteOnly);
                f.close();
                return mock;
            });

            verify(exec.execute(plan, consent), "Test 50e: Execution started");
            verify(QFile::exists(destCleanPath), "Test 50f: 0-byte file exists during job");

            mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read protected source"));

            QEventLoop loop;
            QTimer::singleShot(50, &loop, &QEventLoop::quit);
            loop.exec();

            verify(QFile::exists(destCleanPath), "Test 50g: Unverified 0-byte file is PRESERVED (fail-closed, not auto-unlinked)");
            verify(exec.lastFailureState() == AdminTransferFailureState::UnknownCompletionState,
                   "Test 50h: Classified as UnknownCompletionState");
            verify(exec.lastArtifactAudit().artifactPreserved, "Test 50i: Audit marks artifact as preserved");
            verify(!exec.lastArtifactAudit().check2_createdByThisTask, "Test 50j: Provenance check2 is false for unverified artifact");
            verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::UnverifiedTaskProvenance,
                   "Test 50k: Primary rejection reason is UnverifiedTaskProvenance");
            verify(reportedError.contains(destCleanPath), "Test 50l: Error message informs user about the preserved artifact");

            const AdminRetryQualification nextQual = controller.inspectGuiFailedOperation(&job, ctx, false);
            verify(!nextQual.canAttemptElevation(), "Test 50m: Subsequent retry blocked to prevent overwriting preserved artifact");

            QFile::remove(destCleanPath);
        }

        ::chmod(QFile::encodeName(unreadableSrc).constData(), 0644);
        QFile::remove(unreadableSrc);
    }

    // Test 51: Partial destination (>0 bytes) is preserved and blocks subsequent retry
    {
        const QString partialDestPath = testDestDir + QStringLiteral("/partial_data.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(partialDestPath)};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Permission denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);

        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, partialDestPath](const AdminExecutionPlan &) -> KJob* {
            // Simulate partial data written:
            QFile f(partialDestPath);
            f.open(QIODevice::WriteOnly);
            f.write("partial 50 bytes of transferred data written before connection dropped");
            f.close();
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 51a: Execution started");

        mock->finishWithError(KIO::ERR_CANNOT_WRITE, QStringLiteral("I/O error mid transfer"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(partialDestPath), "Test 51b: Partial destination file (>0 bytes) is preserved");
        verify(exec.lastFailureState() == AdminTransferFailureState::PartialDestinationCreated,
               "Test 51c: Classified as PartialDestinationCreated");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::NonZeroPartialData,
               "Test 51d: Audit primary reason is NonZeroPartialData");

        // Verify subsequent retry attempt is rejected by TOCTOU / Collision protection:
        const AdminRetryQualification nextQual = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!nextQual.canAttemptElevation(), "Test 51e: Subsequent retry blocked because target already exists");
    }

    // Test 52: Cancellation during transfer preserves 0-byte destination artifact (fail-closed, worker write stop unconfirmed)
    {
        const QString unreadableSrc = testSrcDir + QStringLiteral("/src_for_clean52.txt");
        {
            QFile f(unreadableSrc);
            verify(f.open(QIODevice::WriteOnly), "Create src_for_clean52");
            f.write("content");
            f.close();
            verify(::chmod(QFile::encodeName(unreadableSrc).constData(), 0000) == 0, "chmod 0000");
        }

        const QString cancelDestPath = testDestDir + QStringLiteral("/cancel_zero_byte.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(unreadableSrc)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(cancelDestPath)};

        MockAdminJob job(KJob::UserDefinedError, QStringLiteral("Permission denied"));
        job.setProperty("thispcSystemError", EACCES);
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);

        QString cancelMsg;
        QObject::connect(&exec, &AdminRetryExecutor::finished,
            [&cancelMsg](bool, const QString &msg, AdminErrorCategory) {
                cancelMsg = msg;
            });

        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, cancelDestPath](const AdminExecutionPlan &) -> KJob* {
            QFile f(cancelDestPath);
            f.open(QIODevice::WriteOnly);
            f.close();
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 52a: Execution started");
        verify(QFile::exists(cancelDestPath), "Test 52b: 0-byte file exists while job running");

        exec.cancel();

        verify(QFile::exists(cancelDestPath), "Test 52c: 0-byte file is PRESERVED on cancel (fail-closed)");
        verify(exec.lastFailureState() == AdminTransferFailureState::UnknownCompletionState,
               "Test 52d: Cancel with 0-byte file classified as UnknownCompletionState");
        verify(!exec.lastArtifactAudit().check7_writesConfirmedStopped,
               "Test 52e: Audit check7_writesConfirmedStopped is false on asynchronous cancel");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TaskWriteNotConfirmedStopped,
               "Test 52f: Primary rejection reason is TaskWriteNotConfirmedStopped");
        verify(cancelMsg.contains(cancelDestPath), "Test 52g: Cancel message informs user about preserved artifact");

        QFile::remove(cancelDestPath);
        ::chmod(QFile::encodeName(unreadableSrc).constData(), 0644);
        QFile::remove(unreadableSrc);
    }

    // --- Section 11: Create Protected File, KIO Error 177 & .part Safety Audits ---
    // Test 53: Create File with normal ERR_WRITE_ACCESS_DENIED qualifies for Admin Retry
    {
        const QString newFilePath = testDestDir + QStringLiteral("/normal_create_file.txt");
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(newFilePath)};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Permission denied"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(q.canAttemptElevation(), "Test 53a: CreateFile with normal write access denial qualifies");

        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CreateFileProtected,
               "Test 53b: Plan type is CreateFileProtected");
        verify(plan.effectiveDestinationUrl.scheme() == QStringLiteral("admin"),
               "Test 53c: Effective destination URL uses admin://");
    }

    // Test 54: Create File with KIO error 177 (ERR_CANNOT_TRUNCATE) and confirmed structural EACCES on directory
    {
        const QString protDir = baseDir + QStringLiteral("/protected_create_dir");
        verify(QDir().mkpath(protDir), "Create protected dir");
        verify(::chmod(QFile::encodeName(protDir).constData(), 0555) == 0, "chmod 0555 on protDir");

        const QString targetPath = protDir + QStringLiteral("/created_file.txt");
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(protDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetPath)};

        MockAdminJob job(KIO::ERR_CANNOT_TRUNCATE, QStringLiteral("Cannot truncate"));
        job.setCustomErrorText(targetPath + QStringLiteral(".part"));

        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(q.canAttemptElevation(),
               "Test 54a: CreateFile with ERR_CANNOT_TRUNCATE and structural EACCES qualifies for Admin Retry");

        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        const AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CreateFileProtected,
               "Test 54b: Plan type is CreateFileProtected");
        verify(plan.effectiveDestinationUrl.scheme() == QStringLiteral("admin"),
               "Test 54c: Effective destination URL uses admin://");
        verify(plan.requestedItemName == QStringLiteral("created_file.txt"),
               "Test 54d: Requested item name preserved exactly");

        ::chmod(QFile::encodeName(protDir).constData(), 0755);
        QDir(protDir).removeRecursively();
    }

    // Test 55: Create File with KIO error 177 when directory is writable -> BLOCKED (no structural permission error)
    {
        const QString writableDir = baseDir + QStringLiteral("/writable_dir");
        verify(QDir().mkpath(writableDir), "Create writable dir");

        const QString targetPath = writableDir + QStringLiteral("/non_perm_file.txt");
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(writableDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetPath)};

        MockAdminJob job(KIO::ERR_CANNOT_TRUNCATE, QStringLiteral("Truncate failed"));
        job.setCustomErrorText(targetPath + QStringLiteral(".part"));

        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(),
               "Test 55: ERR_CANNOT_TRUNCATE in a writable directory does NOT qualify (no structural permission error)");

        QDir(writableDir).removeRecursively();
    }

    // Test 56: Create File with KIO error 177 when target file already exists -> BLOCKED by collision
    {
        const QString protDir = baseDir + QStringLiteral("/prot_exist_dir");
        verify(QDir().mkpath(protDir), "Create prot_exist_dir");
        const QString targetPath = protDir + QStringLiteral("/already_exists.txt");
        {
            QFile f(targetPath);
            verify(f.open(QIODevice::WriteOnly), "Create pre-existing file");
            f.write("content");
            f.close();
        }
        verify(::chmod(QFile::encodeName(protDir).constData(), 0555) == 0, "chmod 0555");

        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(protDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetPath)};

        MockAdminJob job(KIO::ERR_CANNOT_TRUNCATE, QStringLiteral("Cannot truncate"));
        job.setCustomErrorText(targetPath + QStringLiteral(".part"));

        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(),
               "Test 56: ERR_CANNOT_TRUNCATE with pre-existing target file is BLOCKED by collision");

        ::chmod(QFile::encodeName(protDir).constData(), 0755);
        QDir(protDir).removeRecursively();
    }

    // Test 57: Create File with KIO error 177 when .part file already exists -> BLOCKED
    {
        const QString protDir = baseDir + QStringLiteral("/prot_part_dir");
        verify(QDir().mkpath(protDir), "Create prot_part_dir");
        const QString targetPath = protDir + QStringLiteral("/part_exists.txt");
        const QString partPath = targetPath + QStringLiteral(".part");
        {
            QFile f(partPath);
            verify(f.open(QIODevice::WriteOnly), "Create pre-existing .part file");
            f.write("partial remnant");
            f.close();
        }
        verify(::chmod(QFile::encodeName(protDir).constData(), 0555) == 0, "chmod 0555");

        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(protDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetPath)};

        MockAdminJob job(KIO::ERR_CANNOT_TRUNCATE, QStringLiteral("Cannot truncate"));
        job.setCustomErrorText(partPath);

        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(),
               "Test 57: ERR_CANNOT_TRUNCATE with pre-existing .part file is BLOCKED (no overwrite of unmanaged partial)");

        ::chmod(QFile::encodeName(protDir).constData(), 0755);
        QDir(protDir).removeRecursively();
    }

    // Test 58: AdminPlanValidator blocks execution if .part file is present at destination
    {
        const QString targetPath = testDestDir + QStringLiteral("/validator_part_target.txt");
        const QString partPath = targetPath + QStringLiteral(".part");
        {
            QFile f(partPath);
            verify(f.open(QIODevice::WriteOnly), "Create .part file");
            f.write("remnant");
            f.close();
        }

        AdminExecutionPlan plan;
        plan.planId = QStringLiteral("part-check-plan");
        plan.planType = AdminPlanType::CreateFileProtected;
        plan.requestedItemName = QStringLiteral("validator_part_target.txt");
        plan.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        plan.destinationDirectoryPath = testDestDir;
        plan.destinationUrl = QUrl::fromLocalFile(targetPath);
        plan.effectiveDestinationUrl = QUrl(QStringLiteral("admin:") + targetPath);
        plan.destinationDirectoryIdentity = AdminLocalIdentity::read(testDestDir);

        AdminUserConsent consent = AdminUserConsent::create(plan, 60000);

        const AdminValidationStatus status = AdminPlanValidator::validate(
            plan, consent, true, QDateTime::currentDateTimeUtc());

        verify(!status.isValid(), "Test 58a: Validation fails when .part file exists");
        verify(status.code == AdminPlanValidationResult::UnverifiablePartialState,
               "Test 58b: Validation code is UnverifiablePartialState");

        QFile::remove(partPath);
    }

    // Test 59: Operation without CreateFile/Copy/CreateFolder context is blocked in GUI
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::Rename;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(testDestDir + QStringLiteral("/renamed.txt"))};

        MockAdminJob job(KIO::ERR_CANNOT_TRUNCATE, QStringLiteral("Cannot truncate"));
        const AdminRetryQualification q = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!q.canAttemptElevation(),
               "Test 59: Disallowed operation type in GUI with ERR_CANNOT_TRUNCATE is strictly BLOCKED");
        verify(q.reason == AdminRetryIneligibilityReason::OperationDisallowedInGui,
               "Test 59b: Ineligibility reason is OperationDisallowedInGui");
    }

    // Test 60: Successful execution of CreateFileProtected via mock job
    {
        const QString targetPath = testDestDir + QStringLiteral("/exec_created.txt");
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetPath)};

        MockAdminJob initialJob(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Permission denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&initialJob, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, targetPath](const AdminExecutionPlan &p) -> KJob* {
            verify(p.planType == AdminPlanType::CreateFileProtected, "Job plan is CreateFileProtected");
            verify(p.effectiveDestinationUrl.scheme() == QStringLiteral("admin"), "Destination uses admin://");
            // Simulate successful creation without any .part remnant:
            QFile f(targetPath);
            f.open(QIODevice::WriteOnly);
            f.close();
            return mock;
        });

        bool successResult = false;
        QObject::connect(&exec, &AdminRetryExecutor::finished,
            [&successResult](bool ok, const QString &, AdminErrorCategory) {
                successResult = ok;
            });

        verify(exec.execute(plan, consent), "Test 60a: CreateFileProtected started");
        mock->finishWithSuccess();

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(successResult, "Test 60b: CreateFileProtected completed with success");
        verify(QFile::exists(targetPath), "Test 60c: Target file exists on filesystem");
        verify(!QFile::exists(targetPath + QStringLiteral(".part")), "Test 60d: No .part file remains");

        QFile::remove(targetPath);
    }

    // Test 61: Audited safety: validator blocks execution if destination directory changed, and multi-link 0-byte files are not unlinked
    {
        const QString fakeDestPath = testDestDir + QStringLiteral("/toctou_test.txt");
        const QString hardlinkPath = testDestDir + QStringLiteral("/toctou_hardlink.txt");

        const QString unreadableSrc = testSrcDir + QStringLiteral("/src_for_clean61.txt");
        {
            QFile f(unreadableSrc);
            verify(f.open(QIODevice::WriteOnly), "Create src_for_clean61");
            f.write("content");
            f.close();
            verify(::chmod(QFile::encodeName(unreadableSrc).constData(), 0000) == 0, "chmod 0000");
        }

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(unreadableSrc)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(fakeDestPath)};

        MockAdminJob job(KJob::UserDefinedError, QStringLiteral("Denied"));
        job.setProperty("thispcSystemError", EACCES);
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        // First test: Corrupted directory identity blocks plan validation before start:
        AdminExecutionPlan corruptedDirPlan = plan;
        corruptedDirPlan.destinationDirectoryIdentity.statBuf.st_ino = 999999999;
        AdminRetryExecutor exec1;
        exec1.setKioAdminAvailableOverride(true);
        verify(!exec1.execute(corruptedDirPlan, consent), "Test 61a: Execution blocked before start if directory identity changed");

        // Second test: Multi-link 0-byte file (st_nlink > 1) is NOT unlinked on failure (fail-closed):
        AdminUserConsent consent2 = controller.createConsent(plan, 60000);
        AdminRetryExecutor exec2;
        exec2.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec2.setMockJobFactory([mock, fakeDestPath, hardlinkPath](const AdminExecutionPlan &) -> KJob* {
            // Simulate destination file and hardlink created during worker transfer:
            QFile f(fakeDestPath);
            f.open(QIODevice::WriteOnly);
            f.close();
            ::link(QFile::encodeName(fakeDestPath).constData(), QFile::encodeName(hardlinkPath).constData());
            return mock;
        });

        verify(exec2.execute(plan, consent2), "Test 61b: Execution started");
        mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Read failed"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(fakeDestPath), "Test 61c: 0-byte file with hardlinks is NOT unlinked");
        verify(exec2.lastFailureState() == AdminTransferFailureState::UnknownCompletionState,
               "Test 61d: State classified as UnknownCompletionState (fail-closed)");

        ::unlink(QFile::encodeName(hardlinkPath).constData());
        ::unlink(QFile::encodeName(fakeDestPath).constData());
        ::chmod(QFile::encodeName(unreadableSrc).constData(), 0644);
        QFile::remove(unreadableSrc);
    }

    // Test 62: Audited cleanup: destination cleanup refuses to unlink privileged admin:// destination
    {
        const QString adminDestPath = testDestDir + QStringLiteral("/admin_dest_test.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(adminDestPath)};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, adminDestPath](const AdminExecutionPlan &) -> KJob* {
            // Simulate 0-byte destination created on privileged destination:
            QFile f(adminDestPath);
            f.open(QIODevice::WriteOnly);
            f.close();
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 62a: Execution started");
        mock->finishWithError(KIO::ERR_ACCESS_DENIED, QStringLiteral("Polkit denied"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(adminDestPath), "Test 62b: File NOT unlinked for privileged admin:// plan");
        verify(exec.lastFailureState() == AdminTransferFailureState::UnknownCompletionState,
               "Test 62c: State classified as UnknownCompletionState (fail-closed)");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::NoSafeDeletePermission,
               "Test 62d: Audit primary reason is NoSafeDeletePermission for admin:// target");

        QFile::remove(adminDestPath);
    }

    // --- Section 12: Final Security Gate — Failed Transfer Artifact Ownership & Fail-Closed Tests ---

    // Test 63: Pre-existing 0-byte empty file before operation is blocked and preserved
    {
        const QString preExistingEmpty = testDestDir + QStringLiteral("/preexisting_empty.txt");
        {
            QFile f(preExistingEmpty);
            verify(f.open(QIODevice::WriteOnly), "Create pre-existing 0-byte file");
            f.close();
        }
        const AdminLocalIdentity origId = AdminLocalIdentity::read(preExistingEmpty);
        verify(origId.exists && origId.statBuf.st_size == 0, "Test 63a: Pre-existing 0-byte file exists");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(preExistingEmpty)};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Denied"));
        const AdminRetryQualification qual = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!qual.canAttemptElevation(), "Test 63b: Qualification rejects pre-existing 0-byte file");

        // Even if someone forces a plan with NotStarted state, validator and executor block and preserve:
        AdminRetryRequest forcedReq = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(forcedReq);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        verify(!exec.execute(plan, consent), "Test 63c: Executor refuses to start when 0-byte target already exists");
        verify(QFile::exists(preExistingEmpty), "Test 63d: Pre-existing 0-byte file is preserved");
        verify(origId.sameFile(AdminLocalIdentity::read(preExistingEmpty)), "Test 63e: Pre-existing inode untouched");
        verify(!exec.lastArtifactAudit().check1_notExistedBefore, "Test 63f: Audit check1_notExistedBefore is false");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TargetExistedBeforeStart,
               "Test 63g: Audit primary reason is TargetExistedBeforeStart");

        QFile::remove(preExistingEmpty);
    }

    // Test 64: Inode swap (ArtifactIdentityChanged) during failed transfer preserves replacement file
    {
        const QString inodeSwapPath = testDestDir + QStringLiteral("/inode_swap_target.txt");
        const QString replacementTmpPath = testDestDir + QStringLiteral("/inode_swap_replacement.tmp");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(inodeSwapPath)};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([&exec, mock, inodeSwapPath, replacementTmpPath](const AdminExecutionPlan &) -> KJob* {
            // Create initial 0-byte file (inode A) and record its identity:
            QFile f1(inodeSwapPath);
            f1.open(QIODevice::WriteOnly);
            f1.close();
            const AdminLocalIdentity idA = AdminLocalIdentity::read(inodeSwapPath);
            exec.recordObservedArtifactIdentity(idA);

            // Create replacement file while f1 still exists so filesystem assigns distinct inode B != A:
            QFile f2(replacementTmpPath);
            f2.open(QIODevice::WriteOnly);
            f2.close();
            ::rename(QFile::encodeName(replacementTmpPath).constData(), QFile::encodeName(inodeSwapPath).constData());
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 64a: Execution started");
        mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Read failed"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(inodeSwapPath), "Test 64b: Swapped inode file is preserved (not unlinked)");
        verify(exec.lastArtifactAudit().check3_knownIdentity, "Test 64c: Initial identity was known");
        verify(!exec.lastArtifactAudit().check4_identityUnchanged, "Test 64d: Audit detected inode change");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::ArtifactIdentityChanged,
               "Test 64e: Primary rejection reason is ArtifactIdentityChanged");
        verify(exec.lastFailureState() == AdminTransferFailureState::UnknownCompletionState,
               "Test 64f: State is UnknownCompletionState");

        QFile::remove(inodeSwapPath);
    }

    // Test 65: Parent directory replacement / swap (ParentDirectoryChanged) during transfer preserves file
    {
        const QString swapDir = baseDir + QStringLiteral("/swap_parent_dir");
        const QString displacedDir = baseDir + QStringLiteral("/swap_parent_dir_displaced");
        verify(QDir().mkpath(swapDir), "Create swap_parent_dir");
        const QString targetInSwapDir = swapDir + QStringLiteral("/file_in_swapped_dir.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(swapDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetInSwapDir)};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, swapDir, displacedDir, targetInSwapDir](const AdminExecutionPlan &) -> KJob* {
            // Displace original directory and create new directory at swapDir with distinct inode:
            ::rename(QFile::encodeName(swapDir).constData(), QFile::encodeName(displacedDir).constData());
            QDir().mkpath(swapDir);
            QFile f(targetInSwapDir);
            f.open(QIODevice::WriteOnly);
            f.close();
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 65a: Execution started");
        mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Read failed"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(targetInSwapDir), "Test 65b: File in swapped parent directory is preserved");
        verify(!exec.lastArtifactAudit().check5_parentDirUnchanged, "Test 65c: Audit detected parent directory swap");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::ParentDirectoryChanged,
               "Test 65d: Primary rejection reason is ParentDirectoryChanged");
        verify(exec.lastFailureState() == AdminTransferFailureState::UnknownCompletionState,
               "Test 65e: State is UnknownCompletionState");

        QDir(swapDir).removeRecursively();
        QDir(displacedDir).removeRecursively();
    }

    // Test 66: Symlink at destination is preserved and never unlinked
    {
        const QString realTarget = testDestDir + QStringLiteral("/real_symlink_target.txt");
        {
            QFile f(realTarget);
            verify(f.open(QIODevice::WriteOnly), "Create real target");
            f.write("important data");
            f.close();
        }
        const QString symlinkDest = testDestDir + QStringLiteral("/symlink_artifact.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(symlinkDest)};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, realTarget, symlinkDest](const AdminExecutionPlan &) -> KJob* {
            ::symlink(QFile::encodeName(realTarget).constData(), QFile::encodeName(symlinkDest).constData());
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 66a: Execution started");
        mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Read failed"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        const AdminLocalIdentity symId = AdminLocalIdentity::read(symlinkDest);
        verify(symId.exists && symId.isSymLink, "Test 66b: Symlink itself is preserved");
        verify(QFile::exists(realTarget), "Test 66c: Symlink target file is preserved");
        verify(!exec.lastArtifactAudit().check6_noSymlinkOrHardlink, "Test 66d: Audit detected symlink");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TargetIsSymlinkOrHardlink,
               "Test 66e: Primary rejection reason is TargetIsSymlinkOrHardlink");

        ::unlink(QFile::encodeName(symlinkDest).constData());
        QFile::remove(realTarget);
    }

    // Test 67: Broken symlink at destination is preserved and never unlinked
    {
        const QString brokenSymlinkDest = testDestDir + QStringLiteral("/broken_symlink_artifact.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(brokenSymlinkDest)};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, brokenSymlinkDest](const AdminExecutionPlan &) -> KJob* {
            ::symlink("/nonexistent/thispc_dangling_target_0430", QFile::encodeName(brokenSymlinkDest).constData());
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 67a: Execution started");
        mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Read failed"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        const AdminLocalIdentity brokenId = AdminLocalIdentity::read(brokenSymlinkDest);
        verify(brokenId.exists && brokenId.isSymLink && brokenId.isBrokenSymLink,
               "Test 67b: Broken symlink is preserved on disk");
        verify(!exec.lastArtifactAudit().check6_noSymlinkOrHardlink, "Test 67c: Audit detected broken symlink");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TargetIsSymlinkOrHardlink,
               "Test 67d: Primary rejection reason is TargetIsSymlinkOrHardlink");

        ::unlink(QFile::encodeName(brokenSymlinkDest).constData());
    }

    // Test 68: Ambiguous state after error (.part file left on disk) preserves .part and blocks retry
    {
        const QString ambigTarget = testDestDir + QStringLiteral("/ambiguous_state_target.txt");
        const QString ambigPart = ambigTarget + QStringLiteral(".part");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(ambigTarget)};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, ambigPart](const AdminExecutionPlan &) -> KJob* {
            QFile f(ambigPart);
            f.open(QIODevice::WriteOnly);
            f.close();
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 68a: Execution started");
        mock->finishWithError(KIO::ERR_CANNOT_WRITE, QStringLiteral("Worker interrupted"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(ambigPart), "Test 68b: .part artifact is preserved");
        verify(!exec.lastArtifactAudit().check9_noPartialOrAmbiguousState, "Test 68c: Audit detected ambiguous .part state");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::AmbiguousOrPartialState,
               "Test 68d: Primary rejection reason is AmbiguousOrPartialState");
        verify(exec.lastFailureState() == AdminTransferFailureState::UnknownCompletionState,
               "Test 68e: Failure state is UnknownCompletionState");

        const AdminRetryQualification nextQual = controller.inspectGuiFailedOperation(&job, ctx, false);
        verify(!nextQual.canAttemptElevation(), "Test 68f: Subsequent retry blocked when .part exists");

        QFile::remove(ambigPart);
    }

    // Test 69: Root-owned artifact (ForeignOrRootOwner) is preserved and never unlinked
    {
        const QString rootOwnedDest = testDestDir + QStringLiteral("/root_owned_artifact.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(rootOwnedDest)};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        exec.setArtifactIdentityReaderOverride([rootOwnedDest](const QString &p) -> AdminLocalIdentity {
            AdminLocalIdentity id = AdminLocalIdentity::read(p);
            if (QDir::cleanPath(p) == QDir::cleanPath(rootOwnedDest) && id.exists) {
                id.statBuf.st_uid = 0;
                id.statBuf.st_gid = 0;
            }
            return id;
        });

        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, rootOwnedDest](const AdminExecutionPlan &) -> KJob* {
            QFile f(rootOwnedDest);
            f.open(QIODevice::WriteOnly);
            f.close();
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 69a: Execution started");
        mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Read failed"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(rootOwnedDest), "Test 69b: Root-owned 0-byte artifact is preserved");
        verify(!exec.lastArtifactAudit().check9_ownedByCurrentUser, "Test 69c: Audit detected root/foreign owner");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::ForeignOrRootOwner,
               "Test 69d: Primary rejection reason is ForeignOrRootOwner");
        verify(exec.lastFailureState() == AdminTransferFailureState::UnknownCompletionState,
               "Test 69e: State is UnknownCompletionState");

        QFile::remove(rootOwnedDest);
    }

    // Test 70: Lack of permission for safe deletion (unwritable parent directory at cleanup time)
    {
        const QString revocableDir = baseDir + QStringLiteral("/revocable_perm_dir");
        verify(QDir().mkpath(revocableDir), "Create revocable_perm_dir");
        const QString revocableTarget = revocableDir + QStringLiteral("/locked_artifact.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(revocableDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(revocableTarget)};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        const AdminRetryRequest req = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan = controller.createGuiExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan, 60000);

        AdminRetryExecutor exec;
        exec.setKioAdminAvailableOverride(true);
        MockAdminJob *mock = new MockAdminJob(0);
        mock->setAutoEmitResult(false);
        exec.setMockJobFactory([mock, revocableDir, revocableTarget](const AdminExecutionPlan &) -> KJob* {
            QFile f(revocableTarget);
            f.open(QIODevice::WriteOnly);
            f.close();
            // Revoke write permission on parent directory before job fails:
            ::chmod(QFile::encodeName(revocableDir).constData(), 0555);
            return mock;
        });

        verify(exec.execute(plan, consent), "Test 70a: Execution started");
        mock->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Read failed"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(revocableTarget), "Test 70b: File in unwritable directory is preserved");
        verify(!exec.lastArtifactAudit().check9_hasSafeDeletePermission, "Test 70c: Audit detected lack of delete permission");
        verify(exec.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::NoSafeDeletePermission,
               "Test 70d: Primary rejection reason is NoSafeDeletePermission");

        ::chmod(QFile::encodeName(revocableDir).constData(), 0755);
        QDir(revocableDir).removeRecursively();
    }

    // Test 71: Two concurrent operations targeting the same destination path
    {
        const QString sharedTarget = testDestDir + QStringLiteral("/concurrent_shared_target.txt");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(sharedTarget)};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        const AdminRetryRequest req1 = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan1 = controller.createGuiExecutionPlan(req1);
        AdminUserConsent consent1 = controller.createConsent(plan1, 60000);

        const AdminRetryRequest req2 = controller.createGuiRequest(&job, ctx, false);
        AdminExecutionPlan plan2 = controller.createGuiExecutionPlan(req2);
        AdminUserConsent consent2 = controller.createConsent(plan2, 60000);

        AdminRetryExecutor exec1;
        exec1.setKioAdminAvailableOverride(true);
        MockAdminJob *mock1 = new MockAdminJob(0);
        mock1->setAutoEmitResult(false);
        exec1.setMockJobFactory([mock1, sharedTarget](const AdminExecutionPlan &) -> KJob* {
            QFile f(sharedTarget);
            f.open(QIODevice::WriteOnly);
            f.close();
            return mock1;
        });

        verify(exec1.execute(plan1, consent1), "Test 71a: First executor started on sharedTarget");
        verify(AdminRetryExecutor::activeTargetCount(sharedTarget) == 1, "Test 71b: Active target count is 1");

        // Second executor tries to execute on the same destination path while exec1 is active:
        AdminRetryExecutor exec2;
        exec2.setKioAdminAvailableOverride(true);
        verify(!exec2.execute(plan2, consent2), "Test 71c: Second executor blocked on same target path");
        verify(!exec2.lastArtifactAudit().check8_noOtherTaskUsingTarget, "Test 71d: exec2 audit check8 is false");
        verify(exec2.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TargetUsedByAnotherTask,
               "Test 71e: exec2 primary reason is TargetUsedByAnotherTask");

        // Simulate another task holding a reference when exec1 fails:
        AdminRetryExecutor::acquireTargetPath(sharedTarget);
        mock1->finishWithError(KIO::ERR_CANNOT_READ, QStringLiteral("Read failed"));

        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();

        verify(QFile::exists(sharedTarget), "Test 71f: Shared target file preserved when another task uses target");
        verify(!exec1.lastArtifactAudit().check8_noOtherTaskUsingTarget, "Test 71g: exec1 audit detected concurrent task");
        verify(exec1.lastArtifactAudit().primaryReason == AdminArtifactRejectionReason::TargetUsedByAnotherTask,
               "Test 71h: exec1 primary reason is TargetUsedByAnotherTask");

        AdminRetryExecutor::releaseTargetPath(sharedTarget);
        verify(AdminRetryExecutor::activeTargetCount(sharedTarget) == 0, "Test 71i: Active target count returned to 0");
        QFile::remove(sharedTarget);
    }

    // Test 72: No regression of valid Copy To/From Protected and Create File/Folder
    {
        // 72A: CopyFileFromProtected succeeds with mode 0600 and no preserved failure artifact
        const QString okFromProt = testDestDir + QStringLiteral("/ok_from_protected.txt");
        FileOperationContext ctxFrom;
        ctxFrom.type = FileOperationType::Copy;
        ctxFrom.sources = {QUrl::fromLocalFile(sampleSrcFile)};
        ctxFrom.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctxFrom.destinationUrls = {QUrl::fromLocalFile(okFromProt)};

        MockAdminJob jobFrom(KIO::ERR_CANNOT_READ, QStringLiteral("Read denied"));
        AdminExecutionPlan planFrom = controller.createGuiExecutionPlan(controller.createGuiRequest(&jobFrom, ctxFrom, false));
        AdminUserConsent consentFrom = controller.createConsent(planFrom, 60000);

        AdminRetryExecutor execFrom;
        execFrom.setKioAdminAvailableOverride(true);
        MockAdminJob *mockFrom = new MockAdminJob(0);
        mockFrom->setAutoEmitResult(false);
        execFrom.setMockJobFactory([mockFrom, okFromProt](const AdminExecutionPlan &p) -> KJob* {
            verify(p.planType == AdminPlanType::CopyFileFromProtected, "Test 72a: CopyFileFromProtected plan");
            verify(AdminRetryExecutor::defaultPermissionsForPlan(p.planType) == 0600, "Test 72b: Mode 0600");
            QFile f(okFromProt);
            f.open(QIODevice::WriteOnly);
            f.write("copied data");
            f.close();
            return mockFrom;
        });
        verify(execFrom.execute(planFrom, consentFrom), "Test 72c: CopyFileFromProtected started");
        mockFrom->finishWithSuccess();
        {
            QEventLoop loop;
            QTimer::singleShot(30, &loop, &QEventLoop::quit);
            loop.exec();
        }
        verify(execFrom.lastFailureState() == AdminTransferFailureState::None, "Test 72d: CopyFileFromProtected state is None");
        verify(!execFrom.lastArtifactAudit().artifactPreserved, "Test 72e: No failure artifact flagged on success");
        QFile::remove(okFromProt);

        // 72B: CreateFolderProtected succeeds cleanly
        const QString okFolder = testDestDir + QStringLiteral("/ok_created_folder");
        FileOperationContext ctxDir;
        ctxDir.type = FileOperationType::CreateFolder;
        ctxDir.destinationDirectory = QUrl::fromLocalFile(testDestDir);
        ctxDir.destinationUrls = {QUrl::fromLocalFile(okFolder)};

        MockAdminJob jobDir(KIO::ERR_CANNOT_MKDIR, QStringLiteral("Mkdir denied"));
        AdminExecutionPlan planDir = controller.createGuiExecutionPlan(controller.createGuiRequest(&jobDir, ctxDir, false));
        AdminUserConsent consentDir = controller.createConsent(planDir, 60000);

        AdminRetryExecutor execDir;
        execDir.setKioAdminAvailableOverride(true);
        MockAdminJob *mockDir = new MockAdminJob(0);
        mockDir->setAutoEmitResult(false);
        execDir.setMockJobFactory([mockDir, okFolder](const AdminExecutionPlan &p) -> KJob* {
            verify(p.planType == AdminPlanType::CreateFolderProtected, "Test 72f: CreateFolderProtected plan");
            QDir().mkpath(okFolder);
            return mockDir;
        });
        verify(execDir.execute(planDir, consentDir), "Test 72g: CreateFolderProtected started");
        mockDir->finishWithSuccess();
        {
            QEventLoop loop;
            QTimer::singleShot(30, &loop, &QEventLoop::quit);
            loop.exec();
        }
        verify(execDir.lastFailureState() == AdminTransferFailureState::None, "Test 72h: CreateFolderProtected state is None");
        QDir(okFolder).removeRecursively();
    }

    qDebug() << "ALL 72 TESTS PASSED. Total checks:" << checks;
    return 0;
}
