/*
 * ThisPC 0.41.2 — Stage 2.3.1: Smart Delete Fallback — Test Contract & Specification Helpers
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#ifndef SMART_DELETE_FALLBACK_CONTRACT_SPEC_H
#define SMART_DELETE_FALLBACK_CONTRACT_SPEC_H

#include "browsercommon.h"
#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <algorithm>

namespace SmartDeleteFallbackContract {

// ===========================================================================
// 1. Contract Identification (DF-001 through DF-025)
// ===========================================================================
enum class ContractId {
    DF_001_PlainDeleteLocalTrashPreservation,
    DF_002_PlainDeleteMtpFallbackActivation,
    DF_003_FallbackDialogReasonExplanation,
    DF_004_FallbackDialogIrreversibleWarning,
    DF_005_DialogCancellationNoKioDispatch,
    DF_006_DefaultButtonCancelSafety,
    DF_007_ConfirmationLaunchesDeleteJob,
    DF_008_NoDoubleConfirmationDialog,
    DF_009_StrictSelectionIntegrity,
    DF_010_ProtectedRootDeletionBlocked,
    DF_011_UnavailableDeletionAbortsWithoutJob,
    DF_012_GenericPermissionErrorNoSpuriousFallback,
    DF_013_DisconnectedDeviceNoSpuriousFallback,
    DF_014_ShiftDeleteNonRegression,
    DF_015_ContextMenuPermanentDeleteNonRegression,
    DF_016_SplitViewIsolationPreservation,
    DF_017_DirectoryRefreshAfterFallbackDelete,
    DF_018_CancellationOrFailureNoPhantomRemoval,
    DF_019_LocalTrashUndoIntegrationPreservation,
    DF_020_NoFalseUndoOnFallbackPermanentDelete,
    DF_021_MixedSelectionLocalAndRemoteBlocked,
    DF_022_UnknownRemoteProtocolNoFallback,
    DF_023_RevalidationBeforeJobDispatch,
    DF_024_TrashErrorNoSpuriousPermanentDelete,
    DF_025_HomogeneousMtpExclusivity
};

inline const char *contractIdString(ContractId id)
{
    switch (id) {
    case ContractId::DF_001_PlainDeleteLocalTrashPreservation: return "DF-001";
    case ContractId::DF_002_PlainDeleteMtpFallbackActivation: return "DF-002";
    case ContractId::DF_003_FallbackDialogReasonExplanation: return "DF-003";
    case ContractId::DF_004_FallbackDialogIrreversibleWarning: return "DF-004";
    case ContractId::DF_005_DialogCancellationNoKioDispatch: return "DF-005";
    case ContractId::DF_006_DefaultButtonCancelSafety: return "DF-006";
    case ContractId::DF_007_ConfirmationLaunchesDeleteJob: return "DF-007";
    case ContractId::DF_008_NoDoubleConfirmationDialog: return "DF-008";
    case ContractId::DF_009_StrictSelectionIntegrity: return "DF-009";
    case ContractId::DF_010_ProtectedRootDeletionBlocked: return "DF-010";
    case ContractId::DF_011_UnavailableDeletionAbortsWithoutJob: return "DF-011";
    case ContractId::DF_012_GenericPermissionErrorNoSpuriousFallback: return "DF-012";
    case ContractId::DF_013_DisconnectedDeviceNoSpuriousFallback: return "DF-013";
    case ContractId::DF_014_ShiftDeleteNonRegression: return "DF-014";
    case ContractId::DF_015_ContextMenuPermanentDeleteNonRegression: return "DF-015";
    case ContractId::DF_016_SplitViewIsolationPreservation: return "DF-016";
    case ContractId::DF_017_DirectoryRefreshAfterFallbackDelete: return "DF-017";
    case ContractId::DF_018_CancellationOrFailureNoPhantomRemoval: return "DF-018";
    case ContractId::DF_019_LocalTrashUndoIntegrationPreservation: return "DF-019";
    case ContractId::DF_020_NoFalseUndoOnFallbackPermanentDelete: return "DF-020";
    case ContractId::DF_021_MixedSelectionLocalAndRemoteBlocked: return "DF-021";
    case ContractId::DF_022_UnknownRemoteProtocolNoFallback: return "DF-022";
    case ContractId::DF_023_RevalidationBeforeJobDispatch: return "DF-023";
    case ContractId::DF_024_TrashErrorNoSpuriousPermanentDelete: return "DF-024";
    case ContractId::DF_025_HomogeneousMtpExclusivity: return "DF-025";
    }
    return "DF-UNKNOWN";
}

// ===========================================================================
// 2. Fallback Evaluation & Decision Models
// ===========================================================================
enum class DeleteRoutingDecision {
    MoveToTrash,                    // All local files with trash support
    SmartDeleteFallbackPrompt,      // Homogeneous verified MTP files inside storage
    MixedSelectionBlocked,          // Mixed local + remote or mixed protocols
    RemoteTrashUnavailableInfo,     // Other remote protocol without verified trash (e.g. sftp) -> info message
    BlockedUndeletable              // Protected roots, invalid URLs, empty
};

struct SmartDeleteEvaluation {
    DeleteRoutingDecision decision = DeleteRoutingDecision::BlockedUndeletable;
    bool fallbackPromptRequired = false;
    bool isAllowed = false;
    QString blockReason;
};

inline SmartDeleteEvaluation evaluateDeleteRouting(
    const QList<QUrl> &targets,
    const QUrl &currentDirectory)
{
    SmartDeleteEvaluation eval;
    if (targets.isEmpty()) {
        eval.blockReason = QStringLiteral("Selection is empty");
        return eval;
    }

    if (sameLocation(currentDirectory, kThisPcUrl)) {
        eval.blockReason = QStringLiteral("Cannot delete items from This PC virtual root");
        return eval;
    }

    for (const QUrl &u : targets) {
        if (isProtectedRootUrl(u)) {
            eval.decision = DeleteRoutingDecision::BlockedUndeletable;
            eval.isAllowed = false;
            eval.blockReason = QStringLiteral("Protected root, virtual location, or invalid URL cannot be deleted or moved to trash");
            return eval;
        }
    }

    // Classify protocol uniformity
    bool hasLocal = false;
    bool hasNonLocal = false;
    bool allMtp = true;
    bool mixedSchemes = false;
    const QString firstScheme = targets.first().scheme().toLower();

    for (const QUrl &u : targets) {
        if (u.isLocalFile()) {
            hasLocal = true;
        } else {
            hasNonLocal = true;
        }
        if (u.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) != 0) {
            allMtp = false;
        }
        if (u.scheme().toLower() != firstScheme) {
            mixedSchemes = true;
        }
    }

    // Case A: Mixed selection (e.g. local + MTP, or multiple remote schemes)
    if (mixedSchemes || (hasLocal && hasNonLocal)) {
        eval.decision = DeleteRoutingDecision::MixedSelectionBlocked;
        eval.fallbackPromptRequired = false;
        eval.isAllowed = false;
        eval.blockReason = QStringLiteral("Selection contains items from different protocols or locations");
        return eval;
    }

    // Case B: All items are local files -> Standard Trash
    if (hasLocal && !hasNonLocal) {
        eval.decision = DeleteRoutingDecision::MoveToTrash;
        eval.fallbackPromptRequired = false;
        eval.isAllowed = true;
        return eval;
    }

    // Case C: All items are MTP files/folders inside device storage
    if (allMtp) {
        eval.decision = DeleteRoutingDecision::SmartDeleteFallbackPrompt;
        eval.fallbackPromptRequired = true;
        eval.isAllowed = true;
        return eval;
    }

    // Case D: Other remote protocol without verified trash (e.g. sftp, smb, unknown)
    eval.decision = DeleteRoutingDecision::RemoteTrashUnavailableInfo;
    eval.fallbackPromptRequired = false;
    eval.isAllowed = false;
    eval.blockReason = QStringLiteral("Remote protocol does not support trash; use Shift+Delete");
    return eval;
}

// ===========================================================================
// 3. Fallback Dialog Specification
// ===========================================================================
struct FallbackDialogSpec {
    QString title;
    QString message;
    bool defaultIsCancel = true;
    bool showsFileName = false;
    bool showsItemCount = false;
    bool warnsFolderRecursive = false;
    bool explainsTrashUnavailable = false;
    bool warnsIrreversible = false;
};

inline FallbackDialogSpec generateFallbackDialogSpec(
    const QList<QUrl> &targets,
    bool hasFolders,
    bool isPolish)
{
    FallbackDialogSpec spec;
    spec.defaultIsCancel = true;
    spec.title = isPolish ? QStringLiteral("Nie można przenieść do Kosza")
                          : QStringLiteral("Cannot move to Trash");

    if (targets.size() == 1) {
        spec.showsFileName = true;
        const QString name = targets.first().fileName();
        if (hasFolders) {
            spec.warnsFolderRecursive = true;
            spec.message = isPolish
                ? QStringLiteral("Ta lokalizacja nie obsługuje przenoszenia plików do Kosza.\n\nCzy zamiast tego chcesz trwale usunąć folder «%1» wraz z całą jego zawartością?\n\nTej operacji nie można cofnąć.").arg(name)
                : QStringLiteral("This location does not support moving files to Trash.\n\nDo you want to permanently delete the folder \"%1\" and all its contents instead?\n\nThis operation cannot be undone.").arg(name);
        } else {
            spec.message = isPolish
                ? QStringLiteral("Ta lokalizacja nie obsługuje przenoszenia plików do Kosza.\n\nCzy zamiast tego chcesz trwale usunąć plik «%1»?\n\nTej operacji nie można cofnąć.").arg(name)
                : QStringLiteral("This location does not support moving files to Trash.\n\nDo you want to permanently delete the file \"%1\" instead?\n\nThis operation cannot be undone.").arg(name);
        }
    } else {
        spec.showsItemCount = true;
        if (hasFolders) {
            spec.warnsFolderRecursive = true;
            spec.message = isPolish
                ? QStringLiteral("Ta lokalizacja nie obsługuje przenoszenia plików do Kosza.\n\nCzy zamiast tego chcesz trwale usunąć zaznaczone elementy (%1)?\n\nWszystkie zawarte w nich pliki i foldery zostaną trwale skasowane.\n\nTej operacji nie można cofnąć.").arg(targets.size())
                : QStringLiteral("This location does not support moving files to Trash.\n\nDo you want to permanently delete the selected items (%1)?\n\nAll files and folders contained within will be permanently destroyed.\n\nThis operation cannot be undone.").arg(targets.size());
        } else {
            spec.message = isPolish
                ? QStringLiteral("Ta lokalizacja nie obsługuje przenoszenia plików do Kosza.\n\nCzy zamiast tego chcesz trwale usunąć zaznaczone elementy (%1)?\n\nTej operacji nie można cofnąć.").arg(targets.size())
                : QStringLiteral("This location does not support moving files to Trash.\n\nDo you want to permanently delete the selected items (%1)?\n\nThis operation cannot be undone.").arg(targets.size());
        }
    }

    spec.explainsTrashUnavailable = spec.message.contains(
        isPolish ? QStringLiteral("Ta lokalizacja nie obsługuje przenoszenia")
                 : QStringLiteral("This location does not support moving"));
    spec.warnsIrreversible = spec.message.contains(
        isPolish ? QStringLiteral("Tej operacji nie można cofnąć")
                 : QStringLiteral("This operation cannot be undone"));

    return spec;
}

// ===========================================================================
// 4. Re-validation Specification
// ===========================================================================
struct RevalidationEvaluation {
    bool passed = false;
    QString failureReason;
};

inline RevalidationEvaluation revalidateForDeletion(
    const QList<QUrl> &targetsBeforePrompt,
    const QList<QUrl> &targetsAfterPrompt,
    bool deviceConnected = true)
{
    RevalidationEvaluation res;
    if (!deviceConnected) {
        res.failureReason = QStringLiteral("Device disconnected during confirmation dialog");
        return res;
    }
    if (targetsBeforePrompt != targetsAfterPrompt) {
        res.failureReason = QStringLiteral("Selection changed during confirmation dialog");
        return res;
    }
    for (const QUrl &u : targetsAfterPrompt) {
        if (isProtectedRootUrl(u) || !isDeletableMtpUrl(u)) {
            res.failureReason = QStringLiteral("URL became invalid or protected root");
            return res;
        }
    }
    res.passed = true;
    return res;
}

} // namespace SmartDeleteFallbackContract

#endif // SMART_DELETE_FALLBACK_CONTRACT_SPEC_H
