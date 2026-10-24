/*
 * Administrator retry controller for ThisPC 0.43.0.
 *
 * Provides inspection, request building, execution planning, user consent,
 * and undo-policy seams for operations failing due to permission denial.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "adminretrycontract.h"
#include "adminretryexecutor.h"

#include <QObject>

class AdminRetryController final : public QObject
{
    Q_OBJECT

public:
    explicit AdminRetryController(QObject *parent = nullptr)
        : QObject(parent)
        , m_executor(new AdminRetryExecutor(this))
    {
    }

    static bool isKioAdminAvailable()
    {
        return KProtocolInfo::isKnownProtocol(QStringLiteral("admin"));
    }

    AdminRetryExecutor *executor() const
    {
        return m_executor;
    }

    bool isPrivilegedJob(KJob *job) const
    {
        if (!job) {
            return false;
        }
        const QString scheme = job->property("thispcDestinationUrl").toUrl().scheme();
        return scheme == QStringLiteral("admin") || job->property("thispcPrivilegedOperation").toBool();
    }

    bool shouldRecordUndo(const AdminRetryRequest &request) const
    {
        Q_UNUSED(request);
        // Conservative policy: privileged operations executed via admin://
        // do not register unprivileged undo commands.
        return false;
    }

    AdminRetryRequest createRequest(
        KJob *job,
        const FileOperationContext &context,
        bool wasCancelled = false,
        AdminPartialCompletionState partialState = AdminPartialCompletionState::UnknownState,
        int explicitErrno = 0) const
    {
        AdminRetryRequest request;
        request.operationType = context.type;
        request.sourceUrls = context.sources;
        request.destinationDirectory = context.destinationDirectory;
        request.destinationUrls = context.destinationUrls;
        request.wasCancelled = wasCancelled;
        request.partialState = partialState;
        request.originalContext = context;

        if (job) {
            const QVariant contentsVar = job->property("thispcFileContents");
            if (contentsVar.isValid()) {
                request.fileContents = contentsVar.toByteArray();
            } else if (auto *storedJob = qobject_cast<KIO::StoredTransferJob*>(job)) {
                request.fileContents = storedJob->data();
            }
        }

        if (!context.destinationUrls.isEmpty() && !context.destinationUrls.first().fileName().isEmpty()) {
            request.requestedName = context.destinationUrls.first().fileName();
        }

        for (const QUrl &src : context.sources) {
            const QString path = localPathForFileOrAdmin(src);
            if (!path.isEmpty()) {
                request.sourceIdentities.append(AdminLocalIdentity::read(path));
            }
        }

        const QString destPath = localPathForFileOrAdmin(context.destinationDirectory);
        QString targetItemPath;
        if (!destPath.isEmpty()) {
            request.destinationDirectoryIdentity = AdminLocalIdentity::read(destPath);
            if (!request.requestedName.isEmpty()) {
                targetItemPath = QDir(destPath).filePath(request.requestedName);
                request.targetItemIdentity = AdminLocalIdentity::read(targetItemPath);
            }
        }
        if (targetItemPath.isEmpty() && !context.destinationUrls.isEmpty()) {
            targetItemPath = localPathForFileOrAdmin(context.destinationUrls.first());
        }

        QString partPath;
        if (!targetItemPath.isEmpty()) {
            partPath = targetItemPath + QLatin1String(".part");
        } else if (job && !job->errorText().isEmpty()) {
            partPath = job->errorText();
        }

        request.error = classifyOperationError(job, explicitErrno, destPath, targetItemPath, partPath);

        return request;
    }

    AdminRetryQualification inspectFailedOperation(
        KJob *job,
        const FileOperationContext &context,
        bool wasCancelled = false,
        AdminPartialCompletionState partialState = AdminPartialCompletionState::UnknownState,
        int explicitErrno = 0) const
    {
        const AdminRetryRequest request = createRequest(job, context, wasCancelled, partialState, explicitErrno);
        return AdminRetryPolicy::evaluate(request, isKioAdminAvailable());
    }

    AdminRetryRequest createGuiRequest(
        KJob *job,
        const FileOperationContext &context,
        bool wasCancelled = false,
        AdminPartialCompletionState partialState = AdminPartialCompletionState::NotStarted,
        int explicitErrno = 0) const
    {
        if (partialState == AdminPartialCompletionState::NotStarted) {
            QString destItemPath;
            const QString destDir = localPathForFileOrAdmin(context.destinationDirectory);
            if (!context.destinationUrls.isEmpty()) {
                destItemPath = localPathForFileOrAdmin(context.destinationUrls.first());
            } else if (!destDir.isEmpty() && !context.sources.isEmpty()) {
                destItemPath = QDir(destDir).filePath(context.sources.first().fileName());
            }
            if (!destItemPath.isEmpty()) {
                const AdminLocalIdentity destId = AdminLocalIdentity::read(destItemPath);
                if (destId.exists) {
                    partialState = AdminPartialCompletionState::PartialFileCreated;
                }
                const QString partPath = destItemPath + QLatin1String(".part");
                const AdminLocalIdentity partId = AdminLocalIdentity::read(partPath);
                if (partId.exists) {
                    partialState = AdminPartialCompletionState::PartialFileCreated;
                }
            }
            if (job && !job->errorText().isEmpty()) {
                const AdminLocalIdentity errTextId = AdminLocalIdentity::read(job->errorText());
                if (errTextId.exists) {
                    partialState = AdminPartialCompletionState::PartialFileCreated;
                }
            }
        }

        AdminRetryRequest request = createRequest(job, context, wasCancelled, partialState, explicitErrno);
        request.isGuiRequest = true;
        return request;
    }

    AdminRetryQualification inspectGuiFailedOperation(
        KJob *job,
        const FileOperationContext &context,
        bool wasCancelled = false,
        AdminPartialCompletionState partialState = AdminPartialCompletionState::NotStarted,
        int explicitErrno = 0) const
    {
        const AdminRetryRequest request = createGuiRequest(job, context, wasCancelled, partialState, explicitErrno);
        return AdminRetryPolicy::evaluate(request, isKioAdminAvailable());
    }

    AdminExecutionPlan createGuiExecutionPlan(const AdminRetryRequest &request) const
    {
        if (!isGuiAdminRetryAllowed(request.operationType)) {
            AdminExecutionPlan invalidPlan;
            invalidPlan.planType = AdminPlanType::Invalid;
            return invalidPlan;
        }
        return createExecutionPlan(request);
    }

    AdminExecutionPlan createExecutionPlan(const AdminRetryRequest &request) const
    {
        AdminExecutionPlan plan;
        plan.planId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        plan.createdAt = QDateTime::currentDateTimeUtc();
        plan.partialState = request.partialState;
        plan.retryCount = request.alreadyRetriedAsAdmin ? 1 : 0;
        plan.destinationDirectory = request.destinationDirectory;
        plan.destinationDirectoryIdentity = request.destinationDirectoryIdentity;
        plan.targetItemIdentity = request.targetItemIdentity;
        plan.requestedItemName = request.requestedName;
        plan.fileContents = request.fileContents;

        const QString destDirPath = localPathForFileOrAdmin(request.destinationDirectory);
        plan.destinationDirectoryPath = destDirPath;

        if (!request.sourceUrls.isEmpty()) {
            plan.sourceUrl = request.sourceUrls.first();
        }
        if (!request.sourceIdentities.isEmpty()) {
            plan.sourceIdentity = request.sourceIdentities.first();
        }

        if (request.operationType == FileOperationType::Copy && request.sourceUrls.size() == 1) {
            const QUrl srcUrl = request.sourceUrls.first();
            const QString srcPath = localPathForFileOrAdmin(srcUrl);
            const QString itemName = !request.requestedName.isEmpty() ? request.requestedName : QFileInfo(srcPath).fileName();
            plan.requestedItemName = itemName;
            const QString destItemPath = QDir(destDirPath).filePath(itemName);
            plan.destinationUrl = QUrl::fromLocalFile(destItemPath);

            const bool srcIsAdmin = srcUrl.scheme() == QLatin1String("admin");
            const bool destIsAdmin = request.destinationDirectory.scheme() == QLatin1String("admin");

            if (srcIsAdmin) {
                plan.planType = AdminPlanType::CopyFileFromProtected;
                plan.effectiveSourceUrl = srcUrl;
                plan.effectiveDestinationUrl = QUrl::fromLocalFile(destItemPath);
            } else if (destIsAdmin) {
                plan.planType = AdminPlanType::CopyFileToProtected;
                plan.effectiveSourceUrl = QUrl::fromLocalFile(srcPath);
                plan.effectiveDestinationUrl = adminUrlForLocalPath(destItemPath);
            } else {
                const bool srcUnreadable = (!srcPath.isEmpty() && ::access(QFile::encodeName(srcPath).constData(), R_OK) != 0);
                const bool destUnwritable = (!destDirPath.isEmpty() && ::access(QFile::encodeName(destDirPath).constData(), W_OK) != 0);

                if (srcUnreadable && destUnwritable) {
                    // Dual-side elevation is blocked to respect least-privilege boundary
                    plan.planType = AdminPlanType::Invalid;
                } else if (srcUnreadable) {
                    plan.planType = AdminPlanType::CopyFileFromProtected;
                    plan.effectiveSourceUrl = adminUrlForLocalPath(srcPath);
                    plan.effectiveDestinationUrl = QUrl::fromLocalFile(destItemPath);
                } else if (destUnwritable) {
                    plan.planType = AdminPlanType::CopyFileToProtected;
                    plan.effectiveSourceUrl = QUrl::fromLocalFile(srcPath);
                    plan.effectiveDestinationUrl = adminUrlForLocalPath(destItemPath);
                } else if (request.error.kioErrorCode == KIO::ERR_CANNOT_READ) {
                    plan.planType = AdminPlanType::CopyFileFromProtected;
                    plan.effectiveSourceUrl = adminUrlForLocalPath(srcPath);
                    plan.effectiveDestinationUrl = QUrl::fromLocalFile(destItemPath);
                } else if (request.error.category == AdminErrorCategory::WriteAccessDenied
                           || request.error.kioErrorCode == KIO::ERR_WRITE_ACCESS_DENIED) {
                    plan.planType = AdminPlanType::CopyFileToProtected;
                    plan.effectiveSourceUrl = QUrl::fromLocalFile(srcPath);
                    plan.effectiveDestinationUrl = adminUrlForLocalPath(destItemPath);
                } else {
                    const QString errLower = request.error.rawErrorString.toLower();
                    if (errLower.contains(QStringLiteral("read")) || errLower.contains(QStringLiteral("odczyt"))) {
                        plan.planType = AdminPlanType::CopyFileFromProtected;
                        plan.effectiveSourceUrl = adminUrlForLocalPath(srcPath);
                        plan.effectiveDestinationUrl = QUrl::fromLocalFile(destItemPath);
                    } else {
                        plan.planType = AdminPlanType::CopyFileToProtected;
                        plan.effectiveSourceUrl = QUrl::fromLocalFile(srcPath);
                        plan.effectiveDestinationUrl = adminUrlForLocalPath(destItemPath);
                    }
                }
            }
        } else if (request.operationType == FileOperationType::CreateFile) {
            plan.planType = AdminPlanType::CreateFileProtected;
            const QString destItemPath = QDir(destDirPath).filePath(request.requestedName);
            plan.destinationUrl = QUrl::fromLocalFile(destItemPath);
            plan.effectiveDestinationUrl = adminUrlForLocalPath(destItemPath);
        } else if (request.operationType == FileOperationType::CreateFolder) {
            plan.planType = AdminPlanType::CreateFolderProtected;
            const QString destItemPath = QDir(destDirPath).filePath(request.requestedName);
            plan.destinationUrl = QUrl::fromLocalFile(destItemPath);
            plan.effectiveDestinationUrl = adminUrlForLocalPath(destItemPath);
        } else if (request.operationType == FileOperationType::Rename && request.sourceUrls.size() == 1) {
            const QUrl srcUrl = request.sourceUrls.first();
            const QString srcPath = localPathForFileOrAdmin(srcUrl);
            const QFileInfo srcFi(srcPath);
            const QString parentDirPath = !destDirPath.isEmpty() ? destDirPath : srcFi.absolutePath();
            const QString itemName = request.requestedName;
            const QString destItemPath = QDir(parentDirPath).filePath(itemName);

            const bool isDir = (!request.sourceIdentities.isEmpty() && request.sourceIdentities.first().isDirectory)
                || srcFi.isDir();

            plan.planType = isDir ? AdminPlanType::RenameFolderProtected : AdminPlanType::RenameFileProtected;
            plan.requestedItemName = itemName;
            plan.destinationDirectory = QUrl::fromLocalFile(parentDirPath);
            plan.destinationDirectoryPath = parentDirPath;
            plan.destinationUrl = QUrl::fromLocalFile(destItemPath);
            plan.effectiveSourceUrl = adminUrlForLocalPath(srcPath);
            plan.effectiveDestinationUrl = adminUrlForLocalPath(destItemPath);
        } else if (request.operationType == FileOperationType::Move && request.sourceUrls.size() == 1) {
            const QUrl srcUrl = request.sourceUrls.first();
            const QString srcPath = localPathForFileOrAdmin(srcUrl);
            const QFileInfo srcFi(srcPath);
            const bool isDir = (!request.sourceIdentities.isEmpty() && request.sourceIdentities.first().isDirectory)
                || srcFi.isDir();

            if (isDir) {
                plan.planType = AdminPlanType::Invalid;
            } else {
                const QString itemName = !request.requestedName.isEmpty() ? request.requestedName : srcFi.fileName();
                plan.requestedItemName = itemName;
                const QString destItemPath = QDir(destDirPath).filePath(itemName);
                plan.planType = AdminPlanType::MoveFileSameFilesystem;
                plan.destinationUrl = QUrl::fromLocalFile(destItemPath);
                plan.effectiveSourceUrl = adminUrlForLocalPath(srcPath);
                plan.effectiveDestinationUrl = adminUrlForLocalPath(destItemPath);
            }
        } else {
            plan.planType = AdminPlanType::Invalid;
        }

        return plan;
    }

    AdminUserConsent createConsent(const AdminExecutionPlan &plan, qint64 validityMs = 60000) const
    {
        return AdminUserConsent::create(plan, validityMs);
    }

    AdminValidationStatus validatePlan(
        const AdminExecutionPlan &plan,
        const AdminUserConsent &consent,
        const QDateTime &currentTime = QDateTime::currentDateTimeUtc(),
        AdminStatvfsChecker statvfsOverride = nullptr,
        AdminMountIdChecker mountIdOverride = nullptr) const
    {
        return AdminPlanValidator::validate(plan, consent, isKioAdminAvailable(), currentTime, statvfsOverride, mountIdOverride);
    }

private:
    AdminRetryExecutor *m_executor = nullptr;
};
