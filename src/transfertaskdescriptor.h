/*
 * Transfer task descriptor and value types for the 0.44 transfer queue.
 *
 * Stage 1 (Queue Core & Scheduler). A descriptor is a plain value that
 * DESCRIBES a transfer; it never owns or references a running KJob and never
 * references widgets. The real job is created by `jobFactory` only after the
 * scheduler has granted a slot (see transferqueuecontroller.h).
 *
 * Administrator Retry (0.43) operations are deliberately NOT representable
 * here: there is no admin backend kind, no consent-token field, and the
 * controller rejects `admin:` URLs.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <KJob>

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <cstdio>
#include <QCoreApplication>

inline bool isTransferTraceEnabled()
{
    static const bool enabled = (qEnvironmentVariable("THISPC_TRANSFER_TRACE") == QLatin1String("1"));
    return enabled;
}

#define THISPC_TRACE(...) do { \
    if (isTransferTraceEnabled()) { \
        const qint64 _pid = QCoreApplication::instance() ? QCoreApplication::applicationPid() : 0; \
        fprintf(stderr, "[TRANSFER_TRACE pid=%lld] ", static_cast<long long>(_pid)); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        fflush(stderr); \
    } \
} while (0)

enum class TransferOperationType {
    Copy,
    Move,
};

// `Unknown` is the default so that an uninitialised descriptor is rejected
// instead of being scheduled on a guessed backend.
enum class TransferBackendKind {
    Unknown,
    LocalNative,  // LocalFileCopyJob / LocalFileMoveJob / LocalTransferJob
    Kio,          // KIO::CopyJob and friends
};

enum class TransferPriority {
    Low = 0,
    Normal = 1,
    High = 2,
};

// Scheduler state. This is NOT the transfer state of the backend: a task in
// `Paused` means the backend job really accepted suspend() (see controller).
enum class TransferTaskState {
    Queued,
    Running,
    Paused,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

inline bool isTerminalTransferState(TransferTaskState state)
{
    return state == TransferTaskState::Completed
        || state == TransferTaskState::Cancelled
        || state == TransferTaskState::Failed;
}

enum class TransferWaitReason {
    None,
    SlotLimit,             // concurrency limit reached
    DispatchPaused,        // queue-wide dispatch gate is closed
    BlockedByReservation,  // path conflict with a running / earlier waiting task
    ExclusiveBarrier,      // an earlier waiting task needs the whole queue
    ShuttingDown,          // controlled shutdown in progress
};

enum class TransferFailureKind {
    None,
    PreflightFailed,   // revalidation before start failed; no job was created
    JobFactoryFailed,  // the factory produced no job
    JobError,          // the backend job reported an error
    JobLost,           // the job object vanished without a result
};

// What the backend promises when a running transfer is cancelled. Nothing in
// this module claims a full rollback: published files may remain.
enum class TransferCancelOutcome {
    NoGuarantee,
    PartialResultsMayRemain,
    CleanBeforePublish,
};

// DECLARED capabilities. The effective state is always taken from the live job
// (KJob::capabilities() and the real return value of suspend()/resume()).
struct TransferCapabilities {
    bool canCancelRunning = true;
    bool canPause = false;
    bool canResume = false;
    bool supportsThrottling = false;
    TransferCancelOutcome cancelOutcome = TransferCancelOutcome::NoGuarantee;
};

// Conservative defaults taken from the Stage 0 capability matrix. Same-fs
// Move is an atomic rename, so no pause is declared for Move.
inline TransferCapabilities defaultTransferCapabilities(
    TransferBackendKind backend, TransferOperationType type)
{
    TransferCapabilities caps;
    switch (backend) {
    case TransferBackendKind::LocalNative:
        caps.canCancelRunning = true;
        caps.canPause = type == TransferOperationType::Copy;
        caps.canResume = caps.canPause;
        caps.supportsThrottling = type == TransferOperationType::Copy;
        caps.cancelOutcome = TransferCancelOutcome::PartialResultsMayRemain;
        break;
    case TransferBackendKind::Kio:
        caps.canCancelRunning = true;
        caps.canPause = false;  // suspend() is not an exact pause for KIO
        caps.canResume = false;
        caps.supportsThrottling = false;
        caps.cancelOutcome = TransferCancelOutcome::NoGuarantee;
        break;
    case TransferBackendKind::Unknown:
        caps.canCancelRunning = false;
        break;
    }
    return caps;
}

// Who starts the job produced by the factory. Native jobs are started by the
// controller; KIO::copy()/move() self-schedule and must not be started twice.
enum class TransferJobStartMode {
    ControllerStarts,
    FactoryStarts,
};

// Value describing one transfer. `id` and `submissionSequence` are assigned by
// the controller on enqueue and never change afterwards.
struct TransferTaskDescriptor {
    quint64 id = 0;
    quint64 submissionSequence = 0;

    QString title;
    TransferOperationType type = TransferOperationType::Copy;
    TransferBackendKind backend = TransferBackendKind::Unknown;

    QList<QUrl> sources;
    QUrl destination;               // destination directory
    QList<QUrl> targetUrls;         // optional exact targets, parallel to sources
    FileOperationContext context;   // immutable snapshot of the originating view

    TransferPriority priority = TransferPriority::Normal;
    TransferCapabilities capabilities;

    // Creates the real job. Called exactly once, only after a slot and the
    // reservations were granted and the preflight succeeded. Never called for
    // tasks cancelled while queued.
    std::function<KJob *()> jobFactory;
    TransferJobStartMode startMode = TransferJobStartMode::ControllerStarts;

    // Optional extra revalidation immediately before the job is created.
    // Returns an empty string when the task may start, else a diagnostic.
    std::function<QString()> preflight;
};

// Strict weak ordering used by the scheduler: higher priority first, then
// submission order (FIFO). Interactive reordering (Stage 4) only has to change
// the keys, not the algorithm.
inline bool transferDispatchBefore(
    const TransferTaskDescriptor &left, const TransferTaskDescriptor &right)
{
    if (left.priority != right.priority) {
        return static_cast<int>(left.priority) > static_cast<int>(right.priority);
    }
    return left.submissionSequence < right.submissionSequence;
}

enum class TransferEnqueueRejection {
    None,
    InvalidRequest,
    MissingJobFactory,
    UnsupportedBackend,
    AdminSchemeForbidden,
    ShuttingDown,
};

struct TransferEnqueueResult {
    quint64 id = 0;  // 0 when rejected
    TransferEnqueueRejection rejection = TransferEnqueueRejection::None;
    QString diagnostic;
    bool reservationPrecise = false;
    QString reservationNote;  // why the whole queue is used as a barrier
    bool accepted() const { return id != 0; }
};

// Immutable view of one task for presentation. Contains no pointers to jobs or
// widgets; the UI renders snapshots only.
struct TransferTaskSnapshot {
    quint64 id = 0;
    QString title;
    TransferOperationType type = TransferOperationType::Copy;
    TransferBackendKind backend = TransferBackendKind::Unknown;
    QList<QUrl> sources;
    QUrl destination;
    TransferPriority priority = TransferPriority::Normal;
    int queuePosition = 0;  // 1-based among Queued tasks, 0 otherwise
    TransferTaskState state = TransferTaskState::Queued;
    TransferWaitReason waitReason = TransferWaitReason::None;
    quint64 waitBlockerId = 0;
    TransferCapabilities declared;
    bool canCancel = false;      // right now, per live job
    bool canPause = false;       // right now, per live job
    bool jobSuspended = false;   // actual backend state, not scheduler intent
    bool reservationPrecise = true;
    TransferFailureKind failureKind = TransferFailureKind::None;
    int errorCode = 0;
    QString errorText;
    bool terminal = false;
    quint64 processedBytes = 0;
    quint64 totalBytes = 0;
    int percent = 0;
    int starvationBypassCount = 0;
    quint64 boostSequence = 0;
    bool individuallyPaused = false;
    bool pausedByGlobal = false;
    bool supportsThrottling = false;
    bool throttlingActive = false;
    qulonglong speedBytesPerSecond = 0;
    qulonglong averageSpeedBytesPerSecond = 0;
    qint64 etaSeconds = -1;
    QList<qulonglong> speedSamples;
    QString contextText;
    QUrl currentSourceUrl;
    QUrl currentDestinationUrl;
};

// Stage 5: Global Queue Control States
enum class TransferQueueGlobalControlState {
    Normal,          // Standard scheduling
    DispatchPaused,  // Dispatch gate paused; active tasks continue
    Paused,          // All active tasks suspended AND dispatch paused
    PartiallyPaused, // Dispatch paused, some active tasks suspended, but >= 1 active task cannot be suspended
    CancellingAll    // Global cancel in progress across queued and active tasks
};

inline const char *transferQueueGlobalControlStateName(TransferQueueGlobalControlState state)
{
    switch (state) {
    case TransferQueueGlobalControlState::Normal: return "Normal";
    case TransferQueueGlobalControlState::DispatchPaused: return "DispatchPaused";
    case TransferQueueGlobalControlState::Paused: return "Paused";
    case TransferQueueGlobalControlState::PartiallyPaused: return "PartiallyPaused";
    case TransferQueueGlobalControlState::CancellingAll: return "CancellingAll";
    }
    return "Normal";
}

enum class TransferQueueMode {
    Sequential,
    Parallel
};

inline const char *transferQueueModeName(TransferQueueMode mode)
{
    switch (mode) {
    case TransferQueueMode::Sequential: return "Sequential";
    case TransferQueueMode::Parallel: return "Parallel";
    }
    return "Parallel";
}

constexpr int TransferQueueMinParallelLimit = 1;
constexpr int TransferQueueMaxParallelLimit = 8;
constexpr int TransferQueueDefaultParallelLimit = 2;

struct TransferQueueSnapshot {
    QList<TransferTaskSnapshot> tasks;  // submission order
    TransferQueueMode mode = TransferQueueMode::Parallel;
    TransferQueueGlobalControlState globalState = TransferQueueGlobalControlState::Normal;
    int concurrencyLimit = TransferQueueDefaultParallelLimit;  // effective limit
    int parallelLimit = TransferQueueDefaultParallelLimit;     // configured parallel limit
    int effectiveLimit = TransferQueueDefaultParallelLimit;
    int activeCount = 0;       // tasks holding a slot (Running/Paused/Cancelling)
    int queuedCount = 0;
    bool dispatchPaused = false;
    bool globalPauseActive = false;
    bool cancellingAll = false;
    bool shuttingDown = false;
    bool idle = true;          // no non-terminal task
    bool bandwidthLimitEnabled = false;
    quint64 bandwidthLimitBytesPerSec = 0;
    int throttledActiveCount = 0;
    int unthrottledActiveCount = 0;
};

inline QString transferWaitReasonText(TransferWaitReason reason)
{
    switch (reason) {
    case TransferWaitReason::None: return QStringLiteral("none");
    case TransferWaitReason::SlotLimit: return QStringLiteral("waiting for a free slot");
    case TransferWaitReason::DispatchPaused: return QStringLiteral("dispatch paused");
    case TransferWaitReason::BlockedByReservation: return QStringLiteral("blocked by a conflicting transfer");
    case TransferWaitReason::ExclusiveBarrier: return QStringLiteral("waiting behind an exclusive task");
    case TransferWaitReason::ShuttingDown: return QStringLiteral("shutting down");
    }
    return {};
}
