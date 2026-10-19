/*
 * ThisPC 0.41.2 — Stage 1.2: Automatic Directory Refresh — Test Contract & Specification Helpers
 *
 * Included inside test suite namespaces by tests/run-pane-actions.py or directory-refresh.cpp.
 * Do not add #pragma once so each test suite namespace gets its own internal copy.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "browsercommon.h"
#include <QDateTime>
#include <QList>
#include <QMimeData>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <algorithm>

static int pendingRefreshSpecs = 0;

static void verifyPendingRefreshSpec(bool condition, const char *contractId, const char *description)
{
    if (qgetenv("THISPC_RUN_PENDING_REFRESH_SPECS") == "1") {
        if (!condition) {
            qFatal("FAIL [%s] (PENDING_REFRESH_SPEC enforced): %s", contractId, description);
        }
        return;
    }
    if (condition) {
        return;
    }
    ++pendingRefreshSpecs;
    qInfo("RED / NOT IMPLEMENTED [%s]: %s", contractId, description);
}

namespace DirectoryRefreshContract {

// ===========================================================================
// 1. Contract Identification
// ===========================================================================
enum class ContractId {
    DR_001_ClipboardCutSemantics,
    DR_002_SourceRemovalOnMove,
    DR_003_DestinationInsertionOnMove,
    DR_004_CrossContainerMoveSync,
    DR_005_MenuAndShortcutEquivalence,
    DR_006_OperationCancellationIntegrity,
    DR_007_OperationFailureIntegrity,
    DR_008_PartialSuccessBatchAccuracy,
    DR_009_ExternalMoveThisPcToKdeDesktop,
    DR_010_ExternalMoveKdeDesktopToThisPc,
    DR_011_ExternalMoveThisPcToDolphin,
    DR_012_ExternalMoveDolphinToThisPc,
    DR_013_ExternalFileCreationCli,
    DR_014_ExternalFileDeletionCli,
    DR_015_ExternalFileRenameCli,
    DR_016_ExternalFileModification,
    DR_017_InteractiveWaylandBoundary,
    DR_018_NoPreListingClear,
    DR_019_NoLoadingPlaceholderOnSilentRefresh,
    DR_020_ScrollPositionPreservation,
    DR_021_ExistingItemSelectionPreservation,
    DR_022_DeadItemSelectionPruning,
    DR_023_KeyboardFocusPreservation,
    DR_024_SortOrderPreservation,
    DR_025_GroupingModePreservation,
    DR_026_ActiveSearchFilterPreservation,
    DR_027_ViewModeStability,
    DR_028_DeduplicationGuarantee,
    DR_029_AntiFlickerDoubleBuffering,
    DR_030_BurstCoalescing20Events,
    DR_031_MultiDirectoryIndependentDebounce,
    DR_032_SharedDirectorySingleListing,
    DR_033_ContinuousStreamStarvationPrevention,
    DR_034_DebounceTimingBounds,
    DR_035_InternalVsExternalDeduplication,
    DR_036_ZeroSpuriousRefreshOnIdle,
    DR_037_NavigationPreemption,
    DR_038_RapidPingPongNavigation,
    DR_039_GenerationTokenStaleRejection,
    DR_040_PaneClosureSafety,
    DR_041_TabSwitchAsynchrony,
    DR_042_MediaDisconnectDuringListing,
    DR_043_CurrentDirExternalDeletion,
    DR_044_CurrentDirExternalRename,
    DR_045_CancelledOperationStateTruth,
    DR_046_CleanShutdownWithPendingTimers,
    DR_047_PrimaryPaneRefresh,
    DR_048_SplitPaneRefresh,
    DR_049_TransferLeftToRight,
    DR_050_TransferRightToLeft,
    DR_051_DualPaneSameDirectorySync,
    DR_052_BackgroundTabSync,
    DR_053_MultiWindowCrossProcessSync,
    DR_054_TabClosureObserverCleanup,
    DR_055_CrossPaneIsolation,
    DR_056_ReferenceCountedWatchRegistry,
    DR_057_LocalBtrfsFilesystem,
    DR_058_NtfsFusePartitions,
    DR_059_VfatFat32UsbFlash,
    DR_060_MtpAndroidNoPolling,
    DR_061_NetworkAndSpecialProtocols,
    DR_062_SearchResultViewRefresh,
    DR_063_HiddenFilesAndUnicodeNames,
    DR_064_ProtocolDiversityNonEquivalence,
    DR_065_InternalOperationDirectTracking,
    DR_066_ExternalKDirNotifyListener,
    DR_067_NonKioLocalInotifyMonitoring,
    DR_068_MtpPermanentDeleteRefreshForecast,
    DR_069_MtpPermanentDeleteSafetyBounds,
    TotalContractCount = 69
};

inline const char *contractIdString(ContractId id)
{
    switch (id) {
    case ContractId::DR_001_ClipboardCutSemantics: return "DR-001";
    case ContractId::DR_002_SourceRemovalOnMove: return "DR-002";
    case ContractId::DR_003_DestinationInsertionOnMove: return "DR-003";
    case ContractId::DR_004_CrossContainerMoveSync: return "DR-004";
    case ContractId::DR_005_MenuAndShortcutEquivalence: return "DR-005";
    case ContractId::DR_006_OperationCancellationIntegrity: return "DR-006";
    case ContractId::DR_007_OperationFailureIntegrity: return "DR-007";
    case ContractId::DR_008_PartialSuccessBatchAccuracy: return "DR-008";
    case ContractId::DR_009_ExternalMoveThisPcToKdeDesktop: return "DR-009";
    case ContractId::DR_010_ExternalMoveKdeDesktopToThisPc: return "DR-010";
    case ContractId::DR_011_ExternalMoveThisPcToDolphin: return "DR-011";
    case ContractId::DR_012_ExternalMoveDolphinToThisPc: return "DR-012";
    case ContractId::DR_013_ExternalFileCreationCli: return "DR-013";
    case ContractId::DR_014_ExternalFileDeletionCli: return "DR-014";
    case ContractId::DR_015_ExternalFileRenameCli: return "DR-015";
    case ContractId::DR_016_ExternalFileModification: return "DR-016";
    case ContractId::DR_017_InteractiveWaylandBoundary: return "DR-017";
    case ContractId::DR_018_NoPreListingClear: return "DR-018";
    case ContractId::DR_019_NoLoadingPlaceholderOnSilentRefresh: return "DR-019";
    case ContractId::DR_020_ScrollPositionPreservation: return "DR-020";
    case ContractId::DR_021_ExistingItemSelectionPreservation: return "DR-021";
    case ContractId::DR_022_DeadItemSelectionPruning: return "DR-022";
    case ContractId::DR_023_KeyboardFocusPreservation: return "DR-023";
    case ContractId::DR_024_SortOrderPreservation: return "DR-024";
    case ContractId::DR_025_GroupingModePreservation: return "DR-025";
    case ContractId::DR_026_ActiveSearchFilterPreservation: return "DR-026";
    case ContractId::DR_027_ViewModeStability: return "DR-027";
    case ContractId::DR_028_DeduplicationGuarantee: return "DR-028";
    case ContractId::DR_029_AntiFlickerDoubleBuffering: return "DR-029";
    case ContractId::DR_030_BurstCoalescing20Events: return "DR-030";
    case ContractId::DR_031_MultiDirectoryIndependentDebounce: return "DR-031";
    case ContractId::DR_032_SharedDirectorySingleListing: return "DR-032";
    case ContractId::DR_033_ContinuousStreamStarvationPrevention: return "DR-033";
    case ContractId::DR_034_DebounceTimingBounds: return "DR-034";
    case ContractId::DR_035_InternalVsExternalDeduplication: return "DR-035";
    case ContractId::DR_036_ZeroSpuriousRefreshOnIdle: return "DR-036";
    case ContractId::DR_037_NavigationPreemption: return "DR-037";
    case ContractId::DR_038_RapidPingPongNavigation: return "DR-038";
    case ContractId::DR_039_GenerationTokenStaleRejection: return "DR-039";
    case ContractId::DR_040_PaneClosureSafety: return "DR-040";
    case ContractId::DR_041_TabSwitchAsynchrony: return "DR-041";
    case ContractId::DR_042_MediaDisconnectDuringListing: return "DR-042";
    case ContractId::DR_043_CurrentDirExternalDeletion: return "DR-043";
    case ContractId::DR_044_CurrentDirExternalRename: return "DR-044";
    case ContractId::DR_045_CancelledOperationStateTruth: return "DR-045";
    case ContractId::DR_046_CleanShutdownWithPendingTimers: return "DR-046";
    case ContractId::DR_047_PrimaryPaneRefresh: return "DR-047";
    case ContractId::DR_048_SplitPaneRefresh: return "DR-048";
    case ContractId::DR_049_TransferLeftToRight: return "DR-049";
    case ContractId::DR_050_TransferRightToLeft: return "DR-050";
    case ContractId::DR_051_DualPaneSameDirectorySync: return "DR-051";
    case ContractId::DR_052_BackgroundTabSync: return "DR-052";
    case ContractId::DR_053_MultiWindowCrossProcessSync: return "DR-053";
    case ContractId::DR_054_TabClosureObserverCleanup: return "DR-054";
    case ContractId::DR_055_CrossPaneIsolation: return "DR-055";
    case ContractId::DR_056_ReferenceCountedWatchRegistry: return "DR-056";
    case ContractId::DR_057_LocalBtrfsFilesystem: return "DR-057";
    case ContractId::DR_058_NtfsFusePartitions: return "DR-058";
    case ContractId::DR_059_VfatFat32UsbFlash: return "DR-059";
    case ContractId::DR_060_MtpAndroidNoPolling: return "DR-060";
    case ContractId::DR_061_NetworkAndSpecialProtocols: return "DR-061";
    case ContractId::DR_062_SearchResultViewRefresh: return "DR-062";
    case ContractId::DR_063_HiddenFilesAndUnicodeNames: return "DR-063";
    case ContractId::DR_064_ProtocolDiversityNonEquivalence: return "DR-064";
    case ContractId::DR_065_InternalOperationDirectTracking: return "DR-065";
    case ContractId::DR_066_ExternalKDirNotifyListener: return "DR-066";
    case ContractId::DR_067_NonKioLocalInotifyMonitoring: return "DR-067";
    case ContractId::DR_068_MtpPermanentDeleteRefreshForecast: return "DR-068";
    case ContractId::DR_069_MtpPermanentDeleteSafetyBounds: return "DR-069";
    default: return "DR-UNKNOWN";
    }
}

// ===========================================================================
// 2. Data Structures & Enums
// ===========================================================================

enum class RefreshTriggerKind {
    ManualUserF5,
    InternalOperationSuccess,
    InternalOperationCancelled,
    ExternalKDirNotify,
    LocalKDirWatch,
    DeviceMounted,
    DeviceUnmounted,
    TabActivated
};

enum class RefreshPresentationStrategy {
    SilentInPlaceDiff,
    FullViewReload,
    BackgroundDirtyMark
};

struct DebounceConfig {
    qint64 nominalWindowMs = 150;
    qint64 toleranceMs = 30;
    qint64 maxDeadlineMs = 500;
};

struct DirectoryDiffResult {
    QList<QUrl> addedUrls;
    QList<QUrl> removedUrls;
    QList<QUrl> modifiedUrls;
    QList<QUrl> unchangedUrls;

    bool hasChanges() const {
        return !addedUrls.isEmpty() || !removedUrls.isEmpty() || !modifiedUrls.isEmpty();
    }
};

struct SelectionDiffResult {
    QSet<QUrl> preservedUrls;
    QSet<QUrl> prunedUrls;
    QUrl fallbackFocusedUrl;
};

// Sequence token preventing asynchronous out-of-order listing results from clobbering views
struct GenerationToken {
    quint64 sequenceId = 0;
    QUrl boundLocation;

    bool isValidFor(quint64 incomingSequence, const QUrl &incomingLocation) const {
        return sequenceId == incomingSequence && sameLocation(boundLocation, incomingLocation);
    }
};

// Reference-counted entry in the centralized watch registry
struct WatchRegistryEntry {
    QUrl directoryUrl;
    QString localFilePath;
    int refCount = 0;
    bool isLocalInotify = false;
    bool isKDirNotifyRegistered = false;
    bool isMtp = false;
    qint64 firstEventEpochMs = 0;
    qint64 lastEventEpochMs = 0;
    bool timerPending = false;
};

// D-Bus org.kde.KDirNotify parsed payload representation
struct KDirNotifySignalData {
    enum class Kind {
        FilesAdded,
        FilesRemoved,
        FilesChanged,
        FileMoved,
        FileRenamed,
        EnteredDirectory,
        LeftDirectory,
        Unknown
    };

    Kind kind = Kind::Unknown;
    QUrl directoryUrl;
    QList<QUrl> affectedUrls;
    QUrl sourceUrl;
    QUrl destinationUrl;
};

// ===========================================================================
// 3. Pure Algorithmic Contracts & Verification Methods
// ===========================================================================

// Validates DR-001: Cut operation sets clipboard metadata without touching disk
inline bool validateCutClipboardMime(const QMimeData *mime, const QList<QUrl> &expectedUrls)
{
    if (!mime) {
        return false;
    }
    if (!mime->hasUrls() || mime->urls().size() != expectedUrls.size()) {
        return false;
    }
    const QByteArray cutData = mime->data(QStringLiteral("application/x-kde-cutselection"));
    if (cutData != QByteArrayLiteral("1")) {
        return false;
    }
    for (int i = 0; i < expectedUrls.size(); ++i) {
        if (!sameLocation(mime->urls().at(i), expectedUrls.at(i))) {
            return false;
        }
    }
    return true;
}

// Validates DR-021 & DR-022: Selection preservation & dead item pruning
inline SelectionDiffResult computeSelectionPreservation(
    const QSet<QUrl> &oldSelection,
    const QList<QUrl> &newListingUrls,
    const QUrl &oldFocusedItem)
{
    SelectionDiffResult result;
    QSet<QUrl> newUrlSet;
    for (const auto &url : newListingUrls) {
        newUrlSet.insert(url);
    }

    for (const auto &url : oldSelection) {
        if (newUrlSet.contains(url)) {
            result.preservedUrls.insert(url);
        } else {
            result.prunedUrls.insert(url);
        }
    }

    if (newUrlSet.contains(oldFocusedItem)) {
        result.fallbackFocusedUrl = oldFocusedItem;
    } else if (!result.preservedUrls.isEmpty()) {
        result.fallbackFocusedUrl = *result.preservedUrls.begin();
    } else if (!newListingUrls.isEmpty()) {
        result.fallbackFocusedUrl = newListingUrls.first();
    } else {
        result.fallbackFocusedUrl = QUrl();
    }

    return result;
}

// Validates DR-028 & DR-029: Model diff computation without pre-clearing
inline DirectoryDiffResult computeListingDiff(
    const QList<FileInfo> &oldFiles,
    const QList<FileInfo> &newFiles)
{
    DirectoryDiffResult result;
    QMap<QString, FileInfo> oldMap;
    for (const auto &f : oldFiles) {
        oldMap.insert(f.url.toString(), f);
    }

    QMap<QString, FileInfo> newMap;
    for (const auto &f : newFiles) {
        newMap.insert(f.url.toString(), f);
    }

    for (const auto &f : newFiles) {
        const QString key = f.url.toString();
        if (!oldMap.contains(key)) {
            result.addedUrls.append(f.url);
        } else {
            const FileInfo &old = oldMap.value(key);
            if (old.size != f.size || old.modificationTime != f.modificationTime || old.name != f.name) {
                result.modifiedUrls.append(f.url);
            } else {
                result.unchangedUrls.append(f.url);
            }
        }
    }

    for (const auto &f : oldFiles) {
        const QString key = f.url.toString();
        if (!newMap.contains(key)) {
            result.removedUrls.append(f.url);
        }
    }

    return result;
}

// Validates DR-030, DR-033, DR-034: Debounce and starvation deadline math
inline bool isDebounceDeadlineExpired(qint64 firstEventEpochMs, qint64 currentEpochMs, qint64 deadlineMs)
{
    return (currentEpochMs - firstEventEpochMs) >= deadlineMs;
}

inline qint64 calculateNextDebounceDelay(
    qint64 firstEventEpochMs,
    qint64 currentEpochMs,
    const DebounceConfig &config)
{
    const qint64 elapsedSinceFirst = currentEpochMs - firstEventEpochMs;
    const qint64 remainingUntilDeadline = config.maxDeadlineMs - elapsedSinceFirst;
    if (remainingUntilDeadline <= 0) {
        return 0; // Trigger immediately: deadline expired!
    }
    return std::min(config.nominalWindowMs, remainingUntilDeadline);
}

// Validates DR-056: Reference-counted watch registry logic
inline void registerWatchPath(QMap<QString, WatchRegistryEntry> &registry, const QUrl &url)
{
    const QString key = url.toString(QUrl::StripTrailingSlash);
    if (!registry.contains(key)) {
        WatchRegistryEntry entry;
        entry.directoryUrl = url;
        entry.localFilePath = url.isLocalFile() ? url.toLocalFile() : QString();
        entry.refCount = 1;
        entry.isLocalInotify = url.isLocalFile();
        entry.isMtp = (url.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0);
        registry.insert(key, entry);
    } else {
        registry[key].refCount++;
    }
}

inline bool unregisterWatchPath(QMap<QString, WatchRegistryEntry> &registry, const QUrl &url)
{
    const QString key = url.toString(QUrl::StripTrailingSlash);
    if (!registry.contains(key)) {
        return false;
    }
    registry[key].refCount--;
    if (registry[key].refCount <= 0) {
        registry.remove(key);
        return true; // Completely unwatched
    }
    return false; // Still watched by other container
}

// Validates DR-060 & DR-064: Protocol capability and polling rules
inline bool isPollingAllowedForProtocol(const QUrl &url)
{
    if (url.scheme().compare(QStringLiteral("mtp"), Qt::CaseInsensitive) == 0) {
        return false; // MTP polling is strictly forbidden!
    }
    return false; // ThisPC does not use polling as a primary mechanism
}

inline bool isPollingForbidden(const QUrl &url)
{
    return !isPollingAllowedForProtocol(url);
}

inline bool protocolSupportsInotify(const QUrl &url)
{
    return url.isLocalFile();
}

inline bool protocolSupportsKDirNotify(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return scheme == QStringLiteral("file")
        || scheme == QStringLiteral("trash")
        || scheme == QStringLiteral("sftp")
        || scheme == QStringLiteral("smb");
}

// Validates DR-066: KDirNotify D-Bus signature decoding
inline KDirNotifySignalData parseKDirNotifySignal(
    const QString &signalName,
    const QVariantList &arguments)
{
    KDirNotifySignalData data;
    if (signalName == QStringLiteral("FilesAdded")) {
        data.kind = KDirNotifySignalData::Kind::FilesAdded;
        if (!arguments.isEmpty()) {
            data.directoryUrl = QUrl(arguments.at(0).toString());
        }
    } else if (signalName == QStringLiteral("FilesRemoved")) {
        data.kind = KDirNotifySignalData::Kind::FilesRemoved;
        if (!arguments.isEmpty()) {
            const QStringList list = arguments.at(0).toStringList();
            for (const auto &item : list) {
                data.affectedUrls.append(QUrl(item));
            }
        }
    } else if (signalName == QStringLiteral("FilesChanged")) {
        data.kind = KDirNotifySignalData::Kind::FilesChanged;
        if (!arguments.isEmpty()) {
            const QStringList list = arguments.at(0).toStringList();
            for (const auto &item : list) {
                data.affectedUrls.append(QUrl(item));
            }
        }
    } else if (signalName == QStringLiteral("FileMoved")) {
        data.kind = KDirNotifySignalData::Kind::FileMoved;
        if (arguments.size() >= 2) {
            data.sourceUrl = QUrl(arguments.at(0).toString());
            data.destinationUrl = QUrl(arguments.at(1).toString());
        }
    } else if (signalName == QStringLiteral("FileRenamed")) {
        data.kind = KDirNotifySignalData::Kind::FileRenamed;
        if (arguments.size() >= 2) {
            data.sourceUrl = QUrl(arguments.at(0).toString());
            data.destinationUrl = QUrl(arguments.at(1).toString());
        }
    }
    return data;
}

} // namespace DirectoryRefreshContract
