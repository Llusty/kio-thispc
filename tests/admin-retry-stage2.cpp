/*
 * Tests for ThisPC 0.43.0 Stage 2 Safe Copy & Create.
 *
 * Exercises execution planning, user consent, pre-execution safety validation,
 * TOCTOU mitigation, minimal privilege elevation, fail-closed handling,
 * error handling, and cancellation without requiring root privileges or invoking Polkit.
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

    void emitCustomProgress(int percent, qulonglong processed, qulonglong total)
    {
        setTotalAmount(KJob::Bytes, total);
        setProcessedAmount(KJob::Bytes, processed);
        setPercent(percent);
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
    QCoreApplication::setOrganizationName(QStringLiteral("thispc-admin-stage2-test"));
    QCoreApplication::setApplicationName(QStringLiteral("admin-retry-stage2-test"));

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "Temporary test directory created");
    const QString rootPath = tempDir.path();

    const QString sourceDir = rootPath + QStringLiteral("/src");
    const QString targetDir = rootPath + QStringLiteral("/dst");
    QDir().mkpath(sourceDir);
    QDir().mkpath(targetDir);

    AdminRetryController controller;

    // -------------------------------------------------------------------------
    // Test 1: Copy to protected target — valid plan produces file:// src and admin:// dst
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test1_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "Source file opened");
        f.write("test content 1");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test1_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Write access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        const AdminExecutionPlan plan = controller.createExecutionPlan(req);

        verify(plan.planType == AdminPlanType::CopyFileToProtected, "Test 1: Plan type is CopyFileToProtected");
        verify(plan.effectiveSourceUrl.scheme() == QStringLiteral("file"), "Test 1: Source URL scheme is file://");
        verify(plan.effectiveDestinationUrl.scheme() == QStringLiteral("admin"), "Test 1: Destination URL scheme is admin://");
        verify(plan.requestedItemName == QStringLiteral("test1_src.txt"), "Test 1: Requested name preserved");

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(status.isValid(), "Test 1: Plan validation succeeds");
    }

    // -------------------------------------------------------------------------
    // Test 2: Copy from protected source — valid plan produces admin:// src and file:// dst
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test2_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "Source file opened");
        f.write("test content 2");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test2_src.txt"))};

        MockAdminJob job(KIO::ERR_CANNOT_READ, QStringLiteral("Cannot read file"));
        job.setProperty("thispcSystemError", EACCES);
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted, EACCES);
        const AdminExecutionPlan plan = controller.createExecutionPlan(req);

        verify(plan.planType == AdminPlanType::CopyFileFromProtected, "Test 2: Plan type is CopyFileFromProtected");
        verify(plan.effectiveSourceUrl.scheme() == QStringLiteral("admin"), "Test 2: Source URL scheme is admin://");
        verify(plan.effectiveDestinationUrl.scheme() == QStringLiteral("file"), "Test 2: Destination URL scheme is file://");

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(status.isValid(), "Test 2: Plan validation succeeds");
    }

    // -------------------------------------------------------------------------
    // Test 3: Create file in protected location — valid plan
    // -------------------------------------------------------------------------
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFile;
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/new_protected.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Permission denied"));
        AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        req.fileContents = "safe initial content";

        const AdminExecutionPlan plan = controller.createExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CreateFileProtected, "Test 3: Plan type is CreateFileProtected");
        verify(plan.effectiveDestinationUrl.scheme() == QStringLiteral("admin"), "Test 3: Destination URL scheme is admin://");
        verify(plan.fileContents == "safe initial content", "Test 3: File contents preserved");

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(status.isValid(), "Test 3: Plan validation succeeds");
    }

    // -------------------------------------------------------------------------
    // Test 4: Create folder in protected location — valid plan
    // -------------------------------------------------------------------------
    {
        FileOperationContext ctx;
        ctx.type = FileOperationType::CreateFolder;
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/new_protected_dir"))};

        MockAdminJob job(KIO::ERR_CANNOT_MKDIR, QStringLiteral("Cannot mkdir"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);

        const AdminExecutionPlan plan = controller.createExecutionPlan(req);
        verify(plan.planType == AdminPlanType::CreateFolderProtected, "Test 4: Plan type is CreateFolderProtected");
        verify(plan.effectiveDestinationUrl.scheme() == QStringLiteral("admin"), "Test 4: Destination URL scheme is admin://");

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(status.isValid(), "Test 4: Plan validation succeeds");
    }

    // -------------------------------------------------------------------------
    // Test 5: Minimal necessary elevation — verify violation rejected
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test5_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 5");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test5_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);

        // Intentionally elevate both sides (privilege violation)
        plan.effectiveSourceUrl.setScheme(QStringLiteral("admin"));
        plan.effectiveDestinationUrl.setScheme(QStringLiteral("admin"));

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 5: Privilege violation detected");
        verify(status.code == AdminPlanValidationResult::InvalidPrivilegeMapping, "Test 5: Code is InvalidPrivilegeMapping");
    }

    // -------------------------------------------------------------------------
    // Test 6: Missing / invalid consent — execution refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test6_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 6");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test6_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);

        AdminUserConsent emptyConsent;
        const AdminValidationStatus status = controller.validatePlan(plan, emptyConsent);
        verify(!status.isValid(), "Test 6: Missing consent rejected");
        verify(status.code == AdminPlanValidationResult::ConsentMismatch, "Test 6: Code is ConsentMismatch");

        AdminRetryExecutor executor;
        bool finishedEmitted = false;
        QObject::connect(&executor, &AdminRetryExecutor::finished, [&](bool success, const QString &, AdminErrorCategory cat) {
            finishedEmitted = true;
            verify(!success, "Test 6: Execution failed as expected");
            verify(cat == AdminErrorCategory::AuthorizationCancelledOrDenied, "Test 6: Error category is AuthorizationCancelledOrDenied");
        });
        const bool execRes = executor.execute(plan, emptyConsent);
        verify(!execRes, "Test 6: Executor execute returned false");
        verify(finishedEmitted, "Test 6: Finished signal emitted");
    }

    // -------------------------------------------------------------------------
    // Test 7: Expired / stale consent (>60s) — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test7_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 7");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test7_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);

        AdminUserConsent consent = controller.createConsent(plan);
        // Simulate consent granted 70 seconds ago
        consent.grantedAt = consent.grantedAt.addSecs(-70);
        verify(consent.isExpired(), "Test 7: Consent recognized as expired");

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 7: Expired consent rejected");
        verify(status.code == AdminPlanValidationResult::ConsentExpired, "Test 7: Code is ConsentExpired");
    }

    // -------------------------------------------------------------------------
    // Test 8: Changed plan destination with old consent — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test8_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 8");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test8_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);

        const AdminUserConsent consent = controller.createConsent(plan);

        // Mutate destination in plan
        plan.destinationUrl = QUrl::fromLocalFile(targetDir + QStringLiteral("/mutated_target.txt"));
        plan.effectiveDestinationUrl = adminUrlForLocalPath(targetDir + QStringLiteral("/mutated_target.txt"));

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 8: Mutated destination rejected");
        verify(status.code == AdminPlanValidationResult::ConsentMismatch, "Test 8: Code is ConsentMismatch");
    }

    // -------------------------------------------------------------------------
    // Test 9: Missing kio-admin worker — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test9_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 9");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test9_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = AdminPlanValidator::validate(plan, consent, false /* isKioAdminAvailable = false */);
        verify(!status.isValid(), "Test 9: Missing admin protocol rejected");
        verify(status.code == AdminPlanValidationResult::MissingAdminWorker, "Test 9: Code is MissingAdminWorker");
    }

    // -------------------------------------------------------------------------
    // Test 10: Polkit denial handled cleanly
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test10_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 10");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test10_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan);

        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            auto *mockJob = new MockAdminJob(KIO::ERR_USER_CANCELED, QStringLiteral("Polkit authorization denied"));
            mockJob->setAutoEmitResult(false);
            QMetaObject::invokeMethod(mockJob, [mockJob]() {
                mockJob->finishWithError(KIO::ERR_USER_CANCELED, QStringLiteral("Polkit authorization denied"));
            }, Qt::QueuedConnection);
            return mockJob;
        });

        bool finishedFired = false;
        QObject::connect(&executor, &AdminRetryExecutor::finished, [&](bool success, const QString &err, AdminErrorCategory cat) {
            finishedFired = true;
            verify(!success, "Test 10: Polkit denial reported as failure");
            verify(cat == AdminErrorCategory::AuthorizationCancelledOrDenied, "Test 10: Error category is AuthorizationCancelledOrDenied");
            verify(err.contains(QStringLiteral("Polkit")), "Test 10: Error string preserved");
        });

        const bool ok = executor.execute(plan, consent);
        verify(ok, "Test 10: Execution started");
        QCoreApplication::processEvents();
        verify(finishedFired, "Test 10: Finished handler invoked");
        verify(!executor.isRunning(), "Test 10: Executor no longer running");
    }

    // -------------------------------------------------------------------------
    // Test 11: Polkit cancellation handled cleanly
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test11_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 11");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test11_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan);

        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            auto *mockJob = new MockAdminJob(KJob::KilledJobError, QStringLiteral("User cancelled"));
            mockJob->setAutoEmitResult(false);
            QMetaObject::invokeMethod(mockJob, [mockJob]() {
                mockJob->finishWithError(KJob::KilledJobError, QStringLiteral("User cancelled"));
            }, Qt::QueuedConnection);
            return mockJob;
        });

        bool finishedFired = false;
        QObject::connect(&executor, &AdminRetryExecutor::finished, [&](bool success, const QString &, AdminErrorCategory cat) {
            finishedFired = true;
            verify(!success, "Test 11: Polkit cancellation reported as failure");
            verify(cat == AdminErrorCategory::UserCancelled, "Test 11: Category is UserCancelled");
        });

        verify(executor.execute(plan, consent), "Test 11: Execution started");
        QCoreApplication::processEvents();
        verify(finishedFired, "Test 11: Finished event processed");
        verify(!executor.isRunning(), "Test 11: Executor stopped");
    }

    // -------------------------------------------------------------------------
    // Test 12: Mutated source identity (TOCTOU) — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test12_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("initial 12");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test12_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        // Mutate source file content and size before execution
        QFile fMut(srcFile);
        verify(fMut.open(QIODevice::Append), "file opened for append");
        fMut.write(" appended payload to trigger TOCTOU");
        fMut.close();

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 12: TOCTOU source change detected");
        verify(status.code == AdminPlanValidationResult::SourceIdentityChanged, "Test 12: Code is SourceIdentityChanged");
    }

    // -------------------------------------------------------------------------
    // Test 13: Mutated destination parent identity (TOCTOU) — refused
    // -------------------------------------------------------------------------
    {
        const QString subTargetDir = targetDir + QStringLiteral("/sub_test13");
        QDir().mkpath(subTargetDir);
        const QString srcFile = sourceDir + QStringLiteral("/test13_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 13");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(subTargetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(subTargetDir + QStringLiteral("/test13_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        // Mutate destination identity snapshot artificially
        plan.destinationDirectoryIdentity.statBuf.st_ino += 999;

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 13: Mutated destination directory detected");
        verify(status.code == AdminPlanValidationResult::DestinationIdentityChanged, "Test 13: Code is DestinationIdentityChanged");
    }

    // -------------------------------------------------------------------------
    // Test 14: Source is symlink — refused
    // -------------------------------------------------------------------------
    {
        const QString realFile = sourceDir + QStringLiteral("/test14_real.txt");
        QFile f(realFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 14 real");
        f.close();

        const QString symlinkFile = sourceDir + QStringLiteral("/test14_link.txt");
        ::symlink(QFile::encodeName(realFile).constData(), QFile::encodeName(symlinkFile).constData());

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(symlinkFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test14_link.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 14: Source symlink rejected");
        verify(status.code == AdminPlanValidationResult::SourceIsSymlink, "Test 14: Code is SourceIsSymlink");
    }

    // -------------------------------------------------------------------------
    // Test 15: Source is broken symlink — refused
    // -------------------------------------------------------------------------
    {
        const QString brokenLink = sourceDir + QStringLiteral("/test15_broken.txt");
        ::symlink("/nonexistent/file/path/here", QFile::encodeName(brokenLink).constData());

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(brokenLink)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test15_broken.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 15: Broken symlink rejected");
        verify(status.code == AdminPlanValidationResult::SourceIsSymlink || status.code == AdminPlanValidationResult::SourceMissing, "Test 15: Code is SourceIsSymlink or SourceMissing");
    }

    // -------------------------------------------------------------------------
    // Test 16: Path component has symlink in source or destination — refused
    // -------------------------------------------------------------------------
    {
        const QString realSubDir = rootPath + QStringLiteral("/real_sub");
        QDir().mkpath(realSubDir);
        const QString linkSubDir = rootPath + QStringLiteral("/link_sub");
        ::symlink(QFile::encodeName(realSubDir).constData(), QFile::encodeName(linkSubDir).constData());

        const QString srcFileInsideLink = linkSubDir + QStringLiteral("/test16_src.txt");
        QFile f(realSubDir + QStringLiteral("/test16_src.txt"));
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 16");
        f.close();

        verify(hasSymlinkInPathComponents(srcFileInsideLink), "Test 16: hasSymlinkInPathComponents detects intermediate symlink");

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFileInsideLink)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test16_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 16: Intermediate symlink path component rejected");
        verify(status.code == AdminPlanValidationResult::SourceSymlinkInComponents, "Test 16: Code is SourceSymlinkInComponents");
    }

    // -------------------------------------------------------------------------
    // Test 17: Target item already exists (collision) — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test17_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 17");
        f.close();

        // Create pre-existing file at target destination
        const QString targetExistingFile = targetDir + QStringLiteral("/test17_src.txt");
        QFile fT(targetExistingFile);
        verify(fT.open(QIODevice::WriteOnly), "file opened");
        fT.write("already here");
        fT.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetExistingFile)};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 17: Target collision rejected");
        verify(status.code == AdminPlanValidationResult::TargetCollision, "Test 17: Code is TargetCollision");
    }

    // -------------------------------------------------------------------------
    // Test 18: Destination out of space — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test18_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 18 payload requiring space");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test18_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        auto diskFullStatvfs = [](const QString &, struct statvfs *out) -> bool {
            out->f_bavail = 0; // 0 blocks available
            out->f_frsize = 4096;
            out->f_flag = 0;
            return true;
        };

        const AdminValidationStatus status = AdminPlanValidator::validate(
            plan, consent, true, QDateTime::currentDateTimeUtc(), diskFullStatvfs);
        verify(!status.isValid(), "Test 18: Disk full condition rejected");
        verify(status.code == AdminPlanValidationResult::InsufficientDiskSpace, "Test 18: Code is InsufficientDiskSpace");
    }

    // -------------------------------------------------------------------------
    // Test 19: Destination filesystem read-only — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test19_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 19");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test19_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        auto readOnlyStatvfs = [](const QString &, struct statvfs *out) -> bool {
            out->f_bavail = 1000000;
            out->f_frsize = 4096;
            out->f_flag = ST_RDONLY;
            return true;
        };

        const AdminValidationStatus status = AdminPlanValidator::validate(
            plan, consent, true, QDateTime::currentDateTimeUtc(), readOnlyStatvfs);
        verify(!status.isValid(), "Test 19: Read-only filesystem rejected");
        verify(status.code == AdminPlanValidationResult::DestinationReadOnly, "Test 19: Code is DestinationReadOnly");
    }

    // -------------------------------------------------------------------------
    // Test 20: Destination inside source (cyclic) — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFolder = rootPath + QStringLiteral("/parent_src_folder");
        const QString nestedDst = srcFolder + QStringLiteral("/nested_dst");
        QDir().mkpath(nestedDst);

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFolder)};
        ctx.destinationDirectory = QUrl::fromLocalFile(nestedDst);
        ctx.destinationUrls = {QUrl::fromLocalFile(nestedDst + QStringLiteral("/child"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 20: Cyclic destination inside source rejected");
        verify(status.code == AdminPlanValidationResult::TargetInsideSource, "Test 20: Code is TargetInsideSource");
    }

    // -------------------------------------------------------------------------
    // Test 21: Unknown partial completion state — fail closed, refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test21_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 21");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test21_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::UnknownState);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 21: Unknown partial state rejected (fail-closed)");
        verify(status.code == AdminPlanValidationResult::UnverifiablePartialState, "Test 21: Code is UnverifiablePartialState");
    }

    // -------------------------------------------------------------------------
    // Test 22: Partially completed batch / partial file created — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test22_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 22");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test22_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::PartialFileCreated);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 22: Partial file created state rejected");
        verify(status.code == AdminPlanValidationResult::UnverifiablePartialState, "Test 22: Code is UnverifiablePartialState");
    }

    // -------------------------------------------------------------------------
    // Test 23: Re-executing same consent / token — refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test23_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 23");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test23_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan);

        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(0, QString());
        });

        verify(executor.execute(plan, consent), "Test 23: First execution succeeds");
        QCoreApplication::processEvents();

        // Second execution with same consumed plan and consent
        bool secondFailed = false;
        QObject::connect(&executor, &AdminRetryExecutor::finished, [&](bool success, const QString &, AdminErrorCategory) {
            secondFailed = !success;
        });

        const bool exec2 = executor.execute(plan, consent);
        verify(!exec2, "Test 23: Re-execution returns false");
        verify(secondFailed, "Test 23: Re-execution rejected");
    }

    // -------------------------------------------------------------------------
    // Test 24: Execution loop prevention — retrying already retried operation refused
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test24_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 24");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test24_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        req.alreadyRetriedAsAdmin = true;

        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        verify(plan.retryCount > 0, "Test 24: Plan records previous admin retry");

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 24: Loop prevention active");
        verify(status.code == AdminPlanValidationResult::AlreadyRetriedAsAdmin, "Test 24: Code is AlreadyRetriedAsAdmin");
    }

    // -------------------------------------------------------------------------
    // Test 25: No auto-overwrite allowed
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test25_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 25");
        f.close();

        const QString destTarget = targetDir + QStringLiteral("/test25_src.txt");
        QFile fT(destTarget);
        verify(fT.open(QIODevice::WriteOnly), "file opened");
        fT.write("existing target");
        fT.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(destTarget)};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        const AdminUserConsent consent = controller.createConsent(plan);

        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 25: Auto-overwrite prevented");
        verify(status.code == AdminPlanValidationResult::TargetCollision, "Test 25: Code is TargetCollision");
    }

    // -------------------------------------------------------------------------
    // Test 26: Failure leaves source and target untouched
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test26_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 26 original content");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test26_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan);

        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(KIO::ERR_ACCESS_DENIED, QStringLiteral("Permission denied"));
        });

        verify(executor.execute(plan, consent), "Test 26: Execution started");
        QCoreApplication::processEvents();

        // Source must still exist with exact content
        QFile fVerify(srcFile);
        verify(fVerify.open(QIODevice::ReadOnly), "Test 26: Source still exists");
        verify(fVerify.readAll() == "test 26 original content", "Test 26: Source file content intact");
        fVerify.close();

        // Destination must NOT exist
        verify(!QFile::exists(targetDir + QStringLiteral("/test26_src.txt")), "Test 26: Destination was not created");
    }

    // -------------------------------------------------------------------------
    // Test 27: No unprivileged Undo recorded
    // -------------------------------------------------------------------------
    {
        AdminRetryRequest req;
        req.operationType = FileOperationType::Copy;
        verify(!controller.shouldRecordUndo(req), "Test 27: shouldRecordUndo is false for privileged retry");
    }

    // -------------------------------------------------------------------------
    // Test 28: Proper signal disconnection & KJob cleanup
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test28_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 28");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test28_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan);

        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(0, QString());
        });

        verify(executor.execute(plan, consent), "Test 28: Execution started");
        verify(executor.isRunning(), "Test 28: Executor is running before event loop");
        QCoreApplication::processEvents();
        verify(!executor.isRunning(), "Test 28: Executor cleaned up after job completion");
        verify(executor.activePlan().planId.isEmpty(), "Test 28: Active plan cleared");
    }

    // -------------------------------------------------------------------------
    // Test 29: Cancellation while running emits clean cancelled result without leaks
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test29_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "file opened");
        f.write("test 29");
        f.close();

        FileOperationContext ctx;
        ctx.type = FileOperationType::Copy;
        ctx.sources = {QUrl::fromLocalFile(srcFile)};
        ctx.destinationDirectory = QUrl::fromLocalFile(targetDir);
        ctx.destinationUrls = {QUrl::fromLocalFile(targetDir + QStringLiteral("/test29_src.txt"))};

        MockAdminJob job(KIO::ERR_WRITE_ACCESS_DENIED, QStringLiteral("Access denied"));
        const AdminRetryRequest req = controller.createRequest(&job, ctx, false, AdminPartialCompletionState::NotStarted);
        AdminExecutionPlan plan = controller.createExecutionPlan(req);
        AdminUserConsent consent = controller.createConsent(plan);

        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            auto *mockJob = new MockAdminJob(0, QString());
            mockJob->setAutoEmitResult(false);
            return mockJob;
        });

        bool cancelFinishedFired = false;
        QObject::connect(&executor, &AdminRetryExecutor::finished, [&](bool success, const QString &, AdminErrorCategory cat) {
            cancelFinishedFired = true;
            verify(!success, "Test 29: Cancelled operation reported as failed");
            verify(cat == AdminErrorCategory::UserCancelled, "Test 29: Error category is UserCancelled");
        });

        verify(executor.execute(plan, consent), "Test 29: Execution started");
        verify(executor.isRunning(), "Test 29: Executor is running");

        executor.cancel();
        verify(!executor.isRunning(), "Test 29: Executor stopped after cancel");
        verify(cancelFinishedFired, "Test 29: Cancel finished signal received");
    }

    // -------------------------------------------------------------------------
    // Test 30: Normal unprivileged operations completely unaffected
    // -------------------------------------------------------------------------
    {
        FileOperationContext normalCtx;
        normalCtx.type = FileOperationType::Copy;
        normalCtx.sources = {QUrl::fromLocalFile(sourceDir + QStringLiteral("/file.txt"))};
        normalCtx.destinationDirectory = QUrl::fromLocalFile(targetDir);

        MockAdminJob normalJob(0, QString());
        const AdminOperationError err = classifyOperationError(&normalJob);
        verify(err.category == AdminErrorCategory::None, "Test 30: Normal successful job has category None");
        verify(!err.isPermissionError(), "Test 30: Normal job is not permission error");
    }

    // -------------------------------------------------------------------------
    // Test 31: CopyFileFromProtected plan with restricted source (chmod 000)
    // -------------------------------------------------------------------------
    {
        const QString secretFile = sourceDir + QStringLiteral("/test31_secret.txt");
        QFile f(secretFile);
        verify(f.open(QIODevice::WriteOnly), "Secret file opened");
        f.write("confidential data 31");
        f.close();
        ::chmod(QFile::encodeName(secretFile).constData(), 0000);

        // Verify unprivileged read fails
        QFile fRead(secretFile);
        verify(!fRead.open(QIODevice::ReadOnly), "Test 31: Unprivileged read confirmed denied on chmod 000 file");

        const QString destRestored = targetDir + QStringLiteral("/test31_secret.txt");
        AdminExecutionPlan plan;
        plan.planId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.createdAt = QDateTime::currentDateTimeUtc();
        plan.planType = AdminPlanType::CopyFileFromProtected;
        plan.sourceUrl = QUrl::fromLocalFile(secretFile);
        plan.effectiveSourceUrl = adminUrlForLocalPath(secretFile);
        plan.sourceIdentity = AdminLocalIdentity::read(secretFile);
        plan.requestedItemName = QStringLiteral("test31_secret.txt");
        plan.destinationDirectory = QUrl::fromLocalFile(targetDir);
        plan.destinationDirectoryPath = targetDir;
        plan.destinationDirectoryIdentity = AdminLocalIdentity::read(targetDir);
        plan.destinationUrl = QUrl::fromLocalFile(destRestored);
        plan.effectiveDestinationUrl = QUrl::fromLocalFile(destRestored);

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(status.isValid(), "Test 31: Plan for CopyFileFromProtected with chmod 000 source validates successfully");

        // Restore permissions for cleanup
        ::chmod(QFile::encodeName(secretFile).constData(), 0644);
    }

    // -------------------------------------------------------------------------
    // Test 32: Unprepared target directory failure (destination missing before execute)
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test32_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "Source file opened");
        f.write("test 32");
        f.close();

        AdminExecutionPlan plan;
        plan.planId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.createdAt = QDateTime::currentDateTimeUtc();
        plan.planType = AdminPlanType::CopyFileToProtected;
        plan.sourceUrl = QUrl::fromLocalFile(srcFile);
        plan.effectiveSourceUrl = QUrl::fromLocalFile(srcFile);
        plan.sourceIdentity = AdminLocalIdentity::read(srcFile);
        plan.requestedItemName = QStringLiteral("test32_src.txt");
        plan.destinationDirectory = QUrl::fromLocalFile(targetDir + QStringLiteral("/nonexistent_dir"));
        plan.destinationDirectoryPath = targetDir + QStringLiteral("/nonexistent_dir");
        plan.destinationUrl = QUrl::fromLocalFile(targetDir + QStringLiteral("/nonexistent_dir/test32_src.txt"));
        plan.effectiveDestinationUrl = adminUrlForLocalPath(targetDir + QStringLiteral("/nonexistent_dir/test32_src.txt"));

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 32: Missing destination directory rejected");
        verify(status.code == AdminPlanValidationResult::DestinationMissing, "Test 32: Code is DestinationMissing");
    }

    // -------------------------------------------------------------------------
    // Test 33: PolicyKit authorization revoked / denied classification
    // -------------------------------------------------------------------------
    {
        MockAdminJob revokedJob(KJob::KilledJobError, QStringLiteral("PolicyKit authorization revoked"));
        const AdminOperationError errRevoked = classifyOperationError(&revokedJob);
        verify(errRevoked.category == AdminErrorCategory::AuthorizationCancelledOrDenied,
               "Test 33: Revoked authorization classified as AuthorizationCancelledOrDenied");

        MockAdminJob deniedJob(KIO::ERR_USER_CANCELED, QStringLiteral("Polkit authentication rejected by user"));
        const AdminOperationError errDenied = classifyOperationError(&deniedJob);
        verify(errDenied.category == AdminErrorCategory::AuthorizationCancelledOrDenied,
               "Test 33: Polkit rejected classified as AuthorizationCancelledOrDenied");
    }

    // -------------------------------------------------------------------------
    // Test 34: Target collision prevents unverified overwrite
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test34_src.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "Source file opened");
        f.write("new content 34");
        f.close();

        const QString targetExisting = targetDir + QStringLiteral("/test34_existing.txt");
        QFile fT(targetExisting);
        verify(fT.open(QIODevice::WriteOnly), "Target existing file opened");
        fT.write("original preserved content 34");
        fT.close();

        AdminExecutionPlan plan;
        plan.planId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.createdAt = QDateTime::currentDateTimeUtc();
        plan.planType = AdminPlanType::CopyFileToProtected;
        plan.sourceUrl = QUrl::fromLocalFile(srcFile);
        plan.effectiveSourceUrl = QUrl::fromLocalFile(srcFile);
        plan.sourceIdentity = AdminLocalIdentity::read(srcFile);
        plan.requestedItemName = QStringLiteral("test34_existing.txt");
        plan.destinationDirectory = QUrl::fromLocalFile(targetDir);
        plan.destinationDirectoryPath = targetDir;
        plan.destinationDirectoryIdentity = AdminLocalIdentity::read(targetDir);
        plan.destinationUrl = QUrl::fromLocalFile(targetExisting);
        plan.effectiveDestinationUrl = adminUrlForLocalPath(targetExisting);

        const AdminUserConsent consent = controller.createConsent(plan);
        const AdminValidationStatus status = controller.validatePlan(plan, consent);
        verify(!status.isValid(), "Test 34: Overwrite collision rejected before execution");
        verify(status.code == AdminPlanValidationResult::TargetCollision, "Test 34: Code is TargetCollision");

        // Verify original content was untouched
        QFile fCheck(targetExisting);
        verify(fCheck.open(QIODevice::ReadOnly), "Target existing file opened for verification");
        verify(fCheck.readAll() == "original preserved content 34", "Test 34: Target content was not modified or overwritten");
        fCheck.close();
    }

    // -------------------------------------------------------------------------
    // Test 35: Administrator Fallback permissions contract policy
    // -------------------------------------------------------------------------
    {
        // CopyFileFromProtected MUST enforce 0600 (owner rw only, group/others 0)
        verify(AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CopyFileFromProtected) == 0600,
               "Test 35: CopyFileFromProtected default permissions are 0600");

        // CopyFileToProtected uses default umask (-1) for administrative target
        verify(AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CopyFileToProtected) == -1,
               "Test 35: CopyFileToProtected default permissions are -1");

        // CreateFileProtected and CreateFolderProtected use standard defaults (-1)
        verify(AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CreateFileProtected) == -1,
               "Test 35: CreateFileProtected default permissions are -1");
        verify(AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CreateFolderProtected) == -1,
               "Test 35: CreateFolderProtected default permissions are -1");
        verify(AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::Invalid) == -1,
               "Test 35: Invalid plan default permissions are -1");
    }

    // -------------------------------------------------------------------------
    // Test 36: KIO::file_copy 0600 permission enforcement on local destination
    // -------------------------------------------------------------------------
    {
        const QString srcFile = sourceDir + QStringLiteral("/test36_src.txt");
        const QString dstFile = targetDir + QStringLiteral("/test36_dst.txt");
        QFile f(srcFile);
        verify(f.open(QIODevice::WriteOnly), "Test 36: Source file opened");
        f.write("confidential data to be protected with 0600");
        f.close();
        ::chmod(QFile::encodeName(srcFile).constData(), 0644);

        const int perms = AdminRetryExecutor::defaultPermissionsForPlan(AdminPlanType::CopyFileFromProtected);
        verify(perms == 0600, "Test 36: Permissions value is 0600");

        KIO::FileCopyJob *job = KIO::file_copy(QUrl::fromLocalFile(srcFile),
                                               QUrl::fromLocalFile(dstFile),
                                               perms,
                                               KIO::HideProgressInfo);
        QEventLoop loop;
        QObject::connect(job, &KJob::result, &loop, &QEventLoop::quit);
        loop.exec();

        verify(job->error() == 0, "Test 36: KIO::file_copy finished without error");
        verify(QFile::exists(dstFile), "Test 36: Destination file exists");

        struct stat sb {};
        verify(::stat(QFile::encodeName(dstFile).constData(), &sb) == 0, "Test 36: stat destination file succeeded");
        verify((sb.st_mode & 0777) == 0600, "Test 36: Destination mode is exactly 0600 (-rw-------)");
        verify(sb.st_uid == ::getuid(), "Test 36: Destination owner matches current user UID");
        verify((sb.st_mode & 0077) == 0, "Test 36: No group or other read/write/execute permissions");
    }

    // -------------------------------------------------------------------------
    // Test 37: Explicit user cancellation semantics (UserCancelled)
    // -------------------------------------------------------------------------
    {
        MockAdminJob cancelJob(KIO::ERR_USER_CANCELED, QStringLiteral("Process killed by user"));
        const AdminOperationError errCancel = classifyOperationError(&cancelJob);
        verify(errCancel.category == AdminErrorCategory::UserCancelled,
               "Test 37: ERR_USER_CANCELED classified as UserCancelled");

        AdminExecutionPlan plan;
        plan.planId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.createdAt = QDateTime::currentDateTimeUtc();
        plan.planType = AdminPlanType::CreateFileProtected;
        plan.destinationDirectory = QUrl::fromLocalFile(targetDir);
        plan.destinationDirectoryPath = targetDir;
        plan.destinationDirectoryIdentity = AdminLocalIdentity::read(targetDir);
        plan.destinationUrl = QUrl::fromLocalFile(targetDir + QStringLiteral("/test37.txt"));
        plan.effectiveDestinationUrl = adminUrlForLocalPath(targetDir + QStringLiteral("/test37.txt"));
        plan.fileContents = "test 37";

        AdminUserConsent consent = controller.createConsent(plan);
        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(KIO::ERR_USER_CANCELED, QStringLiteral("Explicit cancel"));
        });

        bool finishedCalled = false;
        AdminErrorCategory emittedCat = AdminErrorCategory::None;
        QObject::connect(&executor, &AdminRetryExecutor::finished, [&](bool ok, const QString &, AdminErrorCategory cat) {
            finishedCalled = true;
            verify(!ok, "Test 37: Executor finished with failure on cancel");
            emittedCat = cat;
        });

        verify(executor.execute(plan, consent), "Test 37: Executor launched");
        QCoreApplication::processEvents();
        verify(finishedCalled, "Test 37: Finished signal received");
        verify(emittedCat == AdminErrorCategory::UserCancelled, "Test 37: Category is UserCancelled");
        verify(executor.lastKioErrorCode() == KIO::ERR_USER_CANCELED, "Test 37: lastKioErrorCode is ERR_USER_CANCELED");
    }

    // -------------------------------------------------------------------------
    // Test 38: Ambiguous AccessDenied on privileged vs unprivileged operation
    // -------------------------------------------------------------------------
    {
        // Privileged operation (kio-admin mapping D-Bus AccessDenied)
        MockAdminJob privJob(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        privJob.setProperty("thispcPrivilegedOperation", true);
        const AdminOperationError errPriv = classifyOperationError(&privJob);
        verify(errPriv.category == AdminErrorCategory::AuthorizationCancelledOrDenied,
               "Test 38: Privileged ERR_ACCESS_DENIED classified as AuthorizationCancelledOrDenied");

        // Unprivileged operation (standard user permission error)
        MockAdminJob unprivJob(KIO::ERR_ACCESS_DENIED, QStringLiteral("Access denied"));
        unprivJob.setProperty("thispcPrivilegedOperation", false);
        const AdminOperationError errUnpriv = classifyOperationError(&unprivJob);
        verify(errUnpriv.category == AdminErrorCategory::PermissionDenied,
               "Test 38: Unprivileged ERR_ACCESS_DENIED classified as PermissionDenied");
    }

    // -------------------------------------------------------------------------
    // Test 39: Denial without user cancellation does NOT qualify as UserCancelled or retryable
    // -------------------------------------------------------------------------
    {
        AdminOperationError errDenied;
        errDenied.category = AdminErrorCategory::AuthorizationCancelledOrDenied;
        errDenied.kioErrorCode = KIO::ERR_ACCESS_DENIED;
        verify(!errDenied.isPermissionError(),
               "Test 39: AuthorizationCancelledOrDenied is NOT a permission error for retry eligibility");

        AdminRetryRequest req;
        req.error = errDenied;
        req.operationType = FileOperationType::Copy;
        req.backend = AdminOperationBackend::KioCopyJob;
        req.sourceUrls = { QUrl::fromLocalFile(sourceDir + QStringLiteral("/src.txt")) };
        req.destinationDirectory = QUrl::fromLocalFile(targetDir);
        req.partialState = AdminPartialCompletionState::NotStarted;

        const AdminRetryQualification qual = AdminRetryPolicy::evaluate(req, true);
        verify(qual.eligibility == AdminRetryEligibility::Ineligible,
               "Test 39: AuthorizationCancelledOrDenied evaluates to Ineligible");
        verify(qual.reason == AdminRetryIneligibilityReason::NonPermissionError,
               "Test 39: Reason is NonPermissionError");
    }

    // -------------------------------------------------------------------------
    // Test 40: Success with cached authorization
    // -------------------------------------------------------------------------
    {
        AdminExecutionPlan plan;
        plan.planId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.createdAt = QDateTime::currentDateTimeUtc();
        plan.planType = AdminPlanType::CreateFileProtected;
        plan.destinationDirectory = QUrl::fromLocalFile(targetDir);
        plan.destinationDirectoryPath = targetDir;
        plan.destinationDirectoryIdentity = AdminLocalIdentity::read(targetDir);
        plan.destinationUrl = QUrl::fromLocalFile(targetDir + QStringLiteral("/test40.txt"));
        plan.effectiveDestinationUrl = adminUrlForLocalPath(targetDir + QStringLiteral("/test40.txt"));
        plan.fileContents = "test 40 cached auth content";

        AdminUserConsent consent = controller.createConsent(plan);
        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(0, QString());
        });

        bool successEmitted = false;
        AdminErrorCategory emittedCat = AdminErrorCategory::UnverifiableOrUnknown;
        QObject::connect(&executor, &AdminRetryExecutor::finished, [&](bool ok, const QString &, AdminErrorCategory cat) {
            successEmitted = ok;
            emittedCat = cat;
        });

        verify(executor.execute(plan, consent), "Test 40: Execute launched");
        QCoreApplication::processEvents();
        verify(successEmitted, "Test 40: Finished signal emitted success = true");
        verify(emittedCat == AdminErrorCategory::None, "Test 40: Category is None on success");
        verify(executor.lastKioErrorCode() == 0, "Test 40: lastKioErrorCode is 0");
    }

    // -------------------------------------------------------------------------
    // Test 41: Other KIO errors preserved and correctly classified
    // -------------------------------------------------------------------------
    {
        MockAdminJob diskJob(KIO::ERR_DISK_FULL, QStringLiteral("Disk full"));
        diskJob.setProperty("thispcPrivilegedOperation", true);
        verify(classifyOperationError(&diskJob).category == AdminErrorCategory::DiskFull,
               "Test 41: ERR_DISK_FULL is DiskFull");

        MockAdminJob colJob(KIO::ERR_FILE_ALREADY_EXIST, QStringLiteral("File exists"));
        colJob.setProperty("thispcPrivilegedOperation", true);
        verify(classifyOperationError(&colJob).category == AdminErrorCategory::Collision,
               "Test 41: ERR_FILE_ALREADY_EXIST is Collision");

        MockAdminJob noentJob(KIO::ERR_DOES_NOT_EXIST, QStringLiteral("No such file"));
        noentJob.setProperty("thispcPrivilegedOperation", true);
        verify(classifyOperationError(&noentJob).category == AdminErrorCategory::PathNotFound,
               "Test 41: ERR_DOES_NOT_EXIST is PathNotFound");

        MockAdminJob protoJob(KIO::ERR_UNSUPPORTED_PROTOCOL, QStringLiteral("Unsupported protocol"));
        protoJob.setProperty("thispcPrivilegedOperation", true);
        verify(classifyOperationError(&protoJob).category == AdminErrorCategory::UnsupportedBackendOrScheme,
               "Test 41: ERR_UNSUPPORTED_PROTOCOL is UnsupportedBackendOrScheme");
    }

    // -------------------------------------------------------------------------
    // Test 42: No destination file created on rejected or cancelled operation
    // -------------------------------------------------------------------------
    {
        const QString rejectedTarget = targetDir + QStringLiteral("/test42_rejected.txt");
        verify(!QFile::exists(rejectedTarget), "Test 42: Target file does not exist initially");

        AdminExecutionPlan plan;
        plan.planId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.createdAt = QDateTime::currentDateTimeUtc();
        plan.planType = AdminPlanType::CreateFileProtected;
        plan.destinationDirectory = QUrl::fromLocalFile(targetDir);
        plan.destinationDirectoryPath = targetDir;
        plan.destinationDirectoryIdentity = AdminLocalIdentity::read(targetDir);
        plan.destinationUrl = QUrl::fromLocalFile(rejectedTarget);
        plan.effectiveDestinationUrl = adminUrlForLocalPath(rejectedTarget);
        plan.fileContents = "should never be written";

        AdminUserConsent consent = controller.createConsent(plan);
        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(KIO::ERR_ACCESS_DENIED, QStringLiteral("Polkit cancelled"));
        });

        verify(executor.execute(plan, consent), "Test 42: Execute called");
        QCoreApplication::processEvents();
        verify(!QFile::exists(rejectedTarget), "Test 42: Destination file was NOT created after error");
    }

    // -------------------------------------------------------------------------
    // Test 43: Complete prevention of automatic retry loops (fail-closed)
    // -------------------------------------------------------------------------
    {
        AdminExecutionPlan plan;
        plan.planId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.createdAt = QDateTime::currentDateTimeUtc();
        plan.planType = AdminPlanType::CreateFileProtected;
        plan.destinationDirectory = QUrl::fromLocalFile(targetDir);
        plan.destinationDirectoryPath = targetDir;
        plan.destinationDirectoryIdentity = AdminLocalIdentity::read(targetDir);
        plan.destinationUrl = QUrl::fromLocalFile(targetDir + QStringLiteral("/test43.txt"));
        plan.effectiveDestinationUrl = adminUrlForLocalPath(targetDir + QStringLiteral("/test43.txt"));
        plan.fileContents = "test 43";

        AdminUserConsent consent = controller.createConsent(plan);
        AdminRetryExecutor executor;
        executor.setMockJobFactory([](const AdminExecutionPlan &) -> KJob* {
            return new MockAdminJob(KIO::ERR_ACCESS_DENIED, QStringLiteral("Denied"));
        });

        verify(executor.execute(plan, consent), "Test 43: First execution launched");
        QCoreApplication::processEvents();

        // Verify plan and consent are marked consumed and retryCount incremented
        verify(plan.consumed, "Test 43: Plan marked consumed");
        verify(consent.consumed, "Test 43: Consent marked consumed");
        verify(plan.retryCount == 1, "Test 43: Retry count incremented to 1");

        // Attempting to validate the consumed plan again is rejected
        const AdminValidationStatus reval = controller.validatePlan(plan, consent);
        verify(!reval.isValid(), "Test 43: Consumed plan re-validation fails");
        verify(reval.code == AdminPlanValidationResult::ConsentAlreadyConsumed,
               "Test 43: Code is ConsentAlreadyConsumed");

        // Re-executing the plan directly is refused
        bool reExecutionRejected = false;
        QObject::connect(&executor, &AdminRetryExecutor::finished, [&](bool ok, const QString &, AdminErrorCategory) {
            if (!ok) reExecutionRejected = true;
        });
        const bool launched = executor.execute(plan, consent);
        verify(!launched, "Test 43: Re-execution refused by executor");
        verify(reExecutionRejected, "Test 43: Finished signal emitted failure on re-execution attempt");
    }

    printf("STAGE 2 ADMIN RETRY TESTS: All 43 tests passed (%d assertions)\n", checks);
    return 0;
}
