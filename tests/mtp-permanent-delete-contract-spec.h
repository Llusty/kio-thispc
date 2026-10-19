/*
 * ThisPC 0.41.2 — Stage 2.1: MTP Permanent Delete — Test Contract & Specification Helpers
 *
 * Included inside test suite namespaces by tests/run-pane-actions.py or mtp-permanent-delete.cpp.
 * Do not add #pragma once so each test suite namespace gets its own internal copy.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "browsercommon.h"
#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <algorithm>

static int pendingPermanentDeleteSpecs = 0;

static void verifyPendingPermanentDeleteSpec(bool condition, const char *contractId, const char *description)
{
    if (qgetenv("THISPC_RUN_PENDING_PERMANENT_DELETE_SPECS") == "1") {
        if (!condition) {
            qFatal("FAIL [%s] (PENDING_PERMANENT_DELETE_SPEC enforced): %s", contractId, description);
        }
        return;
    }
    if (condition) {
        return;
    }
    ++pendingPermanentDeleteSpecs;
    qInfo("RED / NOT IMPLEMENTED [%s]: %s", contractId, description);
}

namespace MtpPermanentDeleteContract {

// ===========================================================================
// 1. Contract Identification (PD-001 through PD-030)
// ===========================================================================
enum class ContractId {
    PD_001_MtpActionAvailabilityActivation,
    PD_002_MtpTrashRemainsDisabled,
    PD_003_ContextMenuEntryPresence,
    PD_004_ShiftDeleteShortcutBinding,
    PD_005_PlainDeleteLocalTrashPreservation,
    PD_006_MandatoryIrreversibleDialogPrompt,
    PD_007_SingleFileNamePresentation,
    PD_008_MultipleFilesCountPresentation,
    PD_009_FolderRecursiveWarningPresentation,
    PD_010_DefaultFocusOnCancelButton,
    PD_011_DialogCancellationNoKioDispatch,
    PD_012_DialogCancellationPreservesViewState,
    PD_013_SingleFileDeleteJobDispatch,
    PD_014_SingleFileSuccessfulViewRemoval,
    PD_015_MultiFileBatchDeleteJobDispatch,
    PD_016_MultiFileBatchSuccessfulViewRemoval,
    PD_017_FolderDeleteJobDispatch,
    PD_018_FolderRecursiveDeletionExecution,
    PD_019_DisconnectedDeviceErrorHandling,
    PD_020_DisconnectedDeviceNoPhantomRemoval,
    PD_021_KioPermissionErrorPresentation,
    PD_022_KioErrorRetainsListingState,
    PD_023_PartialBatchFailureActualStateSync,
    PD_024_JobCancellationStopsSubsequentDeletes,
    PD_025_FileOperationContextDeleteIntegration,
    PD_026_DirectoryRefreshDr068Fulfillment,
    PD_027_SplitViewIsolationOnPrimaryDelete,
    PD_028_SplitViewConcurrentSameMtpSync,
    PD_029_LocalTrashOperationsNonRegression,
    PD_030_NoFalseUndoRegistration
};

inline const char *contractIdString(ContractId id)
{
    switch (id) {
    case ContractId::PD_001_MtpActionAvailabilityActivation: return "PD-001";
    case ContractId::PD_002_MtpTrashRemainsDisabled: return "PD-002";
    case ContractId::PD_003_ContextMenuEntryPresence: return "PD-003";
    case ContractId::PD_004_ShiftDeleteShortcutBinding: return "PD-004";
    case ContractId::PD_005_PlainDeleteLocalTrashPreservation: return "PD-005";
    case ContractId::PD_006_MandatoryIrreversibleDialogPrompt: return "PD-006";
    case ContractId::PD_007_SingleFileNamePresentation: return "PD-007";
    case ContractId::PD_008_MultipleFilesCountPresentation: return "PD-008";
    case ContractId::PD_009_FolderRecursiveWarningPresentation: return "PD-009";
    case ContractId::PD_010_DefaultFocusOnCancelButton: return "PD-010";
    case ContractId::PD_011_DialogCancellationNoKioDispatch: return "PD-011";
    case ContractId::PD_012_DialogCancellationPreservesViewState: return "PD-012";
    case ContractId::PD_013_SingleFileDeleteJobDispatch: return "PD-013";
    case ContractId::PD_014_SingleFileSuccessfulViewRemoval: return "PD-014";
    case ContractId::PD_015_MultiFileBatchDeleteJobDispatch: return "PD-015";
    case ContractId::PD_016_MultiFileBatchSuccessfulViewRemoval: return "PD-016";
    case ContractId::PD_017_FolderDeleteJobDispatch: return "PD-017";
    case ContractId::PD_018_FolderRecursiveDeletionExecution: return "PD-018";
    case ContractId::PD_019_DisconnectedDeviceErrorHandling: return "PD-019";
    case ContractId::PD_020_DisconnectedDeviceNoPhantomRemoval: return "PD-020";
    case ContractId::PD_021_KioPermissionErrorPresentation: return "PD-021";
    case ContractId::PD_022_KioErrorRetainsListingState: return "PD-022";
    case ContractId::PD_023_PartialBatchFailureActualStateSync: return "PD-023";
    case ContractId::PD_024_JobCancellationStopsSubsequentDeletes: return "PD-024";
    case ContractId::PD_025_FileOperationContextDeleteIntegration: return "PD-025";
    case ContractId::PD_026_DirectoryRefreshDr068Fulfillment: return "PD-026";
    case ContractId::PD_027_SplitViewIsolationOnPrimaryDelete: return "PD-027";
    case ContractId::PD_028_SplitViewConcurrentSameMtpSync: return "PD-028";
    case ContractId::PD_029_LocalTrashOperationsNonRegression: return "PD-029";
    case ContractId::PD_030_NoFalseUndoRegistration: return "PD-030";
    }
    return "PD-UNKNOWN";
}

// ===========================================================================
// 2. Safety Bounds Evaluation Structs
// ===========================================================================
struct DeleteCandidateEvaluation {
    bool isAllowed = false;
    bool isIrreversible = true;
    QString safetyRejectionReason;
};

inline DeleteCandidateEvaluation evaluateDeleteCandidate(
    const QList<QUrl> &targets,
    const QUrl &currentDirectory)
{
    DeleteCandidateEvaluation eval;
    if (targets.isEmpty()) {
        eval.safetyRejectionReason = QStringLiteral("Selection is empty");
        return eval;
    }

    // Safety Gate: disallow deleting virtual roots or drives list
    if (sameLocation(currentDirectory, kThisPcUrl)) {
        eval.safetyRejectionReason = QStringLiteral("Cannot delete items from This PC root");
        return eval;
    }

    for (const QUrl &u : targets) {
        if (isProtectedRootUrl(u)) {
            eval.safetyRejectionReason = QStringLiteral("Cannot delete protected root or invalid URL");
            return eval;
        }
    }

    eval.isAllowed = true;
    eval.isIrreversible = true;
    return eval;
}

struct ConfirmationDialogSpec {
    QString title;
    QString message;
    bool defaultIsCancel = true;
    bool showsFileName = false;
    bool showsItemCount = false;
    bool warnsFolderRecursive = false;
};

inline ConfirmationDialogSpec generateConfirmationSpec(
    const QList<QUrl> &targets,
    bool isFolder,
    bool isPolish)
{
    ConfirmationDialogSpec spec;
    spec.defaultIsCancel = true;
    spec.title = isPolish ? QStringLiteral("Usuń trwale") : QStringLiteral("Delete Permanently");

    if (targets.size() == 1) {
        spec.showsFileName = true;
        const QString name = targets.first().fileName();
        if (isFolder) {
            spec.warnsFolderRecursive = true;
            spec.message = isPolish
                ? QStringLiteral("Czy na pewno chcesz nieodwracalnie usunąć ten folder i całą jego zawartość?\n\n%1").arg(name)
                : QStringLiteral("Are you sure you want to permanently delete this folder and all its contents?\n\n%1").arg(name);
        } else {
            spec.message = isPolish
                ? QStringLiteral("Czy na pewno chcesz nieodwracalnie usunąć ten plik?\n\n%1").arg(name)
                : QStringLiteral("Are you sure you want to permanently delete this file?\n\n%1").arg(name);
        }
    } else {
        spec.showsItemCount = true;
        spec.message = isPolish
            ? QStringLiteral("Czy na pewno chcesz nieodwracalnie usunąć zaznaczone elementy (%1)?").arg(targets.size())
            : QStringLiteral("Are you sure you want to permanently delete the selected items (%1)?").arg(targets.size());
    }

    return spec;
}

} // namespace MtpPermanentDeleteContract
