/*
 * TransferQueueController — scheduler core of the 0.44 transfer queue.
 *
 * Stage 1 (Queue Core & Scheduler). The controller is NOT wired into any
 * production dispatch path yet: Copy/Paste, Cut/Move, Drag & Drop and Send To
 * still create their jobs exactly as in 0.43. It is exercised only through
 * isolated job factories (test doubles).
 *
 * Design rules (see the Stage 0 report, sections 17-18):
 *  - The queue stores DESCRIPTORS, never started jobs. The factory creates the
 *    KJob only after a slot and the path reservations were granted and the
 *    preflight revalidation passed.
 *  - Scheduler state (TransferTaskState) is separate from the transfer state of
 *    the backend. Paused means the live job accepted suspend().
 *  - No signal is emitted while state is being mutated. Mutations append
 *    events that are flushed in order once the state is consistent, so
 *    handlers may call back into the controller (enqueue, cancel, limit...).
 *  - Slots and reservations are tied to one flag per task and released exactly
 *    once, on the single terminal transition.
 *  - Shutdown never terminates threads, never calls QThread::wait() and never
 *    closes anything on a timer. The watchdog only reports delays. Completion
 *    needs real confirmation: every task terminal AND every auto-deleting job
 *    object destroyed.
 *  - Administrator Retry is out of scope: `admin:` URLs are rejected and no
 *    consent token can be stored.
 *
 * Not covered: Undo/Redo integration (the history numbering is untouched),
 * UI, persistence/recovery, throttling.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "localfilecopyjob.h"
#include "transferbandwidthlimiter.h"
#include "transferhistorymanager.h"
#include "transferqueuesettings.h"
#include "transferreservationregistry.h"
#include "transfertaskdescriptor.h"

#include <KIO/Job>
#include <KJob>

#include <QHash>
#include <QElapsedTimer>
#include <QMetaMethod>
#include <QPair>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>

Q_DECLARE_METATYPE(TransferTaskState)

class TransferQueueController final : public QObject
{
    Q_OBJECT

public:
    static constexpr int StarvationBypassThresholdNormal = 3;
    static constexpr int StarvationBypassThresholdLow = 6;

    explicit TransferQueueController(QObject *parent = nullptr)
        : QObject(parent)
        , m_watchdog(new QTimer(this))
        , m_bandwidthLimiter(std::make_shared<TransferBandwidthLimiter>(nullptr))
        , m_ownedHistoryManager(new TransferHistoryManager(QString(), this))
        , m_historyManager(m_ownedHistoryManager)
    {
        qRegisterMetaType<TransferTaskState>();
        qRegisterMetaType<std::shared_ptr<TransferBandwidthLimiter>>();
        m_watchdog->setSingleShot(true);
        m_watchdog->setInterval(m_watchdogIntervalMs);
        connect(m_watchdog, &QTimer::timeout, this, [this] { onWatchdog(); });
        connect(m_bandwidthLimiter.get(), &TransferBandwidthLimiter::limitChanged,
                this, [this](bool enabled, quint64 bytesPerSec) {
                    Q_EMIT bandwidthLimitChanged(enabled, bytesPerSec);
                    markQueueChanged();
                });
    }

    ~TransferQueueController() override
    {
        m_shuttingDown = true;
        if (m_historyManager) {
            m_historyManager->flushSync();
        }
        for (const auto &entry : m_tasks) {
            for (const auto &connection : entry.second->connections) {
                QObject::disconnect(connection);
            }
            entry.second->connections.clear();
            if (entry.second->job) {
                entry.second->job->kill(KJob::Quietly);
            }
        }
        if (nonTerminalCount() > 0) {
            qWarning("TransferQueueController destroyed with %d non-terminal task(s); "
                     "complete shutdown first", nonTerminalCount());
        }
    }

    // ------------------------------------------------------------------
    // Submission
    // ------------------------------------------------------------------

    // Accepts a descriptor WITHOUT creating or starting any job.
    TransferEnqueueResult enqueue(TransferTaskDescriptor descriptor)
    {
        OperationScope scope(this);
        TransferEnqueueResult result;
        auto reject = [&result](TransferEnqueueRejection kind, const QString &text) {
            result.rejection = kind;
            result.diagnostic = text;
            return result;
        };

        if (m_shuttingDown) {
            return reject(TransferEnqueueRejection::ShuttingDown,
                          QStringLiteral("queue is shutting down"));
        }
        if (descriptor.backend == TransferBackendKind::Unknown) {
            return reject(TransferEnqueueRejection::UnsupportedBackend,
                          QStringLiteral("backend kind is not set"));
        }
        if (!descriptor.jobFactory) {
            return reject(TransferEnqueueRejection::MissingJobFactory,
                          QStringLiteral("descriptor has no job factory"));
        }
        if (descriptor.sources.isEmpty() || !descriptor.destination.isValid()
            || descriptor.destination.isEmpty()) {
            return reject(TransferEnqueueRejection::InvalidRequest,
                          QStringLiteral("sources or destination missing"));
        }
        if (containsAdminScheme(descriptor)) {
            return reject(TransferEnqueueRejection::AdminSchemeForbidden,
                          QStringLiteral("admin:// operations are never queued"));
        }

        auto task = std::make_unique<Task>();
        task->enqueuedTimeUtc = QDateTime::currentDateTimeUtc();
        task->reservation = TransferReservation::build(
            descriptor.type, descriptor.sources, descriptor.destination,
            descriptor.targetUrls);
        if (!task->reservation.valid) {
            return reject(TransferEnqueueRejection::InvalidRequest,
                          QStringLiteral("reservation could not be built"));
        }
        descriptor.id = m_nextId++;
        descriptor.submissionSequence = m_nextSequence++;
        task->desc = std::move(descriptor);
        task->waitReason = TransferWaitReason::SlotLimit;

        result.id = task->desc.id;
        result.reservationPrecise = task->reservation.precise;
        result.reservationNote = task->reservation.imprecisionReason;

        const quint64 id = task->desc.id;
        m_order.push_back(id);
        int insertIdx = m_queuedOrder.size();
        for (int i = 0; i < m_queuedOrder.size(); ++i) {
            const Task *other = find(m_queuedOrder.at(i));
            if (other && static_cast<int>(other->desc.priority) < static_cast<int>(descriptor.priority)) {
                insertIdx = i;
                break;
            }
        }
        m_queuedOrder.insert(insertIdx, id);
        m_tasks.emplace(id, std::move(task));
        pushEvent({Event::Enqueued, id});
        markQueueChanged();
        return result;
    }

    // ------------------------------------------------------------------
    // Modes, limits and dispatch gate (Stage 2)
    // ------------------------------------------------------------------

    TransferQueueMode executionMode() const { return m_mode; }

    // Switches between Sequential (effective limit 1) and Parallel (uses
    // parallelConcurrencyLimit). Does not cancel, restart, kill or suspend any
    // running task. Preserves the configured parallel limit across mode switches.
    void setExecutionMode(TransferQueueMode mode)
    {
        OperationScope scope(this);
        if (m_mode == mode) return;
        m_mode = mode;
        markQueueChanged();
        Q_EMIT modeChanged(m_mode);
    }

    // Configured limit for Parallel mode, clamped to [1, 8]. Default is 2.
    int parallelConcurrencyLimit() const { return m_parallelLimit; }

    void setParallelConcurrencyLimit(int limit)
    {
        OperationScope scope(this);
        const int clamped = TransferQueueSettings::clampLimit(limit);
        if (clamped == m_parallelLimit) return;
        m_parallelLimit = clamped;
        markQueueChanged();
        Q_EMIT parallelLimitChanged(m_parallelLimit);
    }

    // Effective active slot limit: 1 in Sequential, parallelConcurrencyLimit() in Parallel.
    int effectiveConcurrencyLimit() const
    {
        return m_mode == TransferQueueMode::Sequential ? 1 : m_parallelLimit;
    }

    bool isDraining() const
    {
        return activeCount() > effectiveConcurrencyLimit();
    }

    // Backwards-compatible alias from Stage 1: returns the effective limit.
    int concurrencyLimit() const { return effectiveConcurrencyLimit(); }

    // Backwards-compatible mutator: updates the parallel limit (clamped to 1..8).
    void setConcurrencyLimit(int limit)
    {
        setParallelConcurrencyLimit(limit);
    }

    // ------------------------------------------------------------------
    // Bandwidth Throttling (Stage 6)
    // ------------------------------------------------------------------

    std::shared_ptr<TransferBandwidthLimiter> bandwidthLimiter() const
    {
        return m_bandwidthLimiter;
    }

    void setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter)
    {
        if (m_bandwidthLimiter == limiter) return;
        if (m_bandwidthLimiter) {
            disconnect(m_bandwidthLimiter.get(), nullptr, this, nullptr);
        }
        m_bandwidthLimiter = limiter ? limiter : std::make_shared<TransferBandwidthLimiter>(nullptr);
        connect(m_bandwidthLimiter.get(), &TransferBandwidthLimiter::limitChanged,
                this, [this](bool enabled, quint64 bytesPerSec) {
                    Q_EMIT bandwidthLimitChanged(enabled, bytesPerSec);
                    markQueueChanged();
                });
        markQueueChanged();
    }

    bool isBandwidthLimitEnabled() const
    {
        return m_bandwidthLimiter ? m_bandwidthLimiter->isEnabled() : false;
    }

    quint64 bandwidthLimitBytesPerSec() const
    {
        return m_bandwidthLimiter ? m_bandwidthLimiter->bytesPerSecond() : 0ULL;
    }

    quint64 configuredBandwidthLimitBytesPerSec() const
    {
        if (m_bandwidthLimitValue > 0) {
            return bandwidthToBytesPerSec(m_bandwidthLimitValue, m_bandwidthLimitUnit);
        }
        return m_bandwidthLimiter ? m_bandwidthLimiter->bytesPerSecond() : 0ULL;
    }

    BandwidthUnit bandwidthLimitUnit() const { return m_bandwidthLimitUnit; }
    int bandwidthLimitValue() const { return m_bandwidthLimitValue; }

    void setBandwidthLimit(bool enabled, quint64 bytesPerSec)
    {
        BandwidthUnit unit = BandwidthUnit::MiB_s;
        m_bandwidthLimitValue = bytesPerSecToUnitValue(bytesPerSec, unit);
        m_bandwidthLimitUnit = unit;
        if (m_bandwidthLimiter) {
            m_bandwidthLimiter->setLimit(enabled, bytesPerSec);
        }
    }

    void setBandwidthLimit(bool enabled, quint64 bytesPerSec, BandwidthUnit unit, int value)
    {
        m_bandwidthLimitUnit = unit;
        m_bandwidthLimitValue = value;
        if (m_bandwidthLimiter) {
            m_bandwidthLimiter->setLimit(enabled, bytesPerSec);
        }
    }

    void setBandwidthLimitEnabled(bool enabled)
    {
        if (m_bandwidthLimiter) {
            const quint64 bytes = (enabled && m_bandwidthLimiter->bytesPerSecond() == 0)
                ? configuredBandwidthLimitBytesPerSec()
                : m_bandwidthLimiter->bytesPerSecond();
            m_bandwidthLimiter->setLimit(enabled, bytes);
        }
    }

    void setBandwidthLimitBytesPerSec(quint64 bytesPerSec)
    {
        if (m_bandwidthLimiter) {
            m_bandwidthLimiter->setLimit(m_bandwidthLimiter->isEnabled(), bytesPerSec);
        }
    }

    int throttledActiveTasksCount() const
    {
        int count = 0;
        for (const auto &entry : m_tasks) {
            if (entry.second->slotHeld
                && (entry.second->state == TransferTaskState::Running
                    || entry.second->state == TransferTaskState::Paused)
                && taskSupportsThrottling(*entry.second)) {
                ++count;
            }
        }
        return count;
    }

    int unthrottledActiveTasksCount() const
    {
        int count = 0;
        for (const auto &entry : m_tasks) {
            if (entry.second->slotHeld
                && (entry.second->state == TransferTaskState::Running
                    || entry.second->state == TransferTaskState::Paused)
                && !taskSupportsThrottling(*entry.second)) {
                ++count;
            }
        }
        return count;
    }

    // Configuration persistence helpers
    TransferQueueConfig exportConfig() const
    {
        TransferQueueConfig cfg;
        cfg.mode = m_mode;
        cfg.parallelLimit = m_parallelLimit;
        cfg.closeWindowOnComplete = m_closeWindowOnComplete;
        cfg.desktopNotificationsEnabled = m_desktopNotificationsEnabled;
        cfg.bandwidthLimitUnit = m_bandwidthLimitUnit;
        cfg.bandwidthLimitValue = m_bandwidthLimitValue;
        if (m_bandwidthLimiter) {
            cfg.bandwidthLimitEnabled = m_bandwidthLimiter->isEnabled();
            if (cfg.bandwidthLimitEnabled) {
                cfg.bandwidthLimitBytesPerSec = m_bandwidthLimiter->bytesPerSecond();
                if (cfg.bandwidthLimitValue <= 0 ||
                    bandwidthToBytesPerSec(cfg.bandwidthLimitValue, cfg.bandwidthLimitUnit) != cfg.bandwidthLimitBytesPerSec) {
                    BandwidthUnit derivedUnit = BandwidthUnit::MiB_s;
                    cfg.bandwidthLimitValue = bytesPerSecToUnitValue(cfg.bandwidthLimitBytesPerSec, derivedUnit);
                    cfg.bandwidthLimitUnit = derivedUnit;
                }
            } else {
                cfg.bandwidthLimitBytesPerSec = configuredBandwidthLimitBytesPerSec();
            }
        }
        return cfg;
    }

    void applyConfig(const TransferQueueConfig &config)
    {
        OperationScope scope(this);
        const int clamped = TransferQueueSettings::clampLimit(config.parallelLimit);
        const bool modeChangedFlag = m_mode != config.mode;
        const bool limitChangedFlag = m_parallelLimit != clamped;
        const bool closeChangedFlag = m_closeWindowOnComplete != config.closeWindowOnComplete;
        const bool notifChangedFlag = m_desktopNotificationsEnabled != config.desktopNotificationsEnabled;
        m_mode = config.mode;
        m_parallelLimit = clamped;
        m_closeWindowOnComplete = config.closeWindowOnComplete;
        m_desktopNotificationsEnabled = config.desktopNotificationsEnabled;
        m_bandwidthLimitUnit = config.bandwidthLimitUnit;
        m_bandwidthLimitValue = config.bandwidthLimitValue > 0 ? config.bandwidthLimitValue : 1;

        if (m_bandwidthLimiter) {
            m_bandwidthLimiter->setLimit(config.bandwidthLimitEnabled, config.bandwidthLimitBytesPerSec);
        }

        markQueueChanged();
        if (modeChangedFlag) Q_EMIT modeChanged(m_mode);
        if (limitChangedFlag) Q_EMIT parallelLimitChanged(m_parallelLimit);
        if (closeChangedFlag) Q_EMIT closeWindowOnCompleteChanged(m_closeWindowOnComplete);
        if (notifChangedFlag) Q_EMIT desktopNotificationsEnabledChanged(m_desktopNotificationsEnabled);
    }

    bool closeWindowOnComplete() const { return m_closeWindowOnComplete; }
    void setCloseWindowOnComplete(bool close)
    {
        if (m_closeWindowOnComplete == close) return;
        m_closeWindowOnComplete = close;
        markQueueChanged();
        Q_EMIT closeWindowOnCompleteChanged(close);
    }

    bool desktopNotificationsEnabled() const { return m_desktopNotificationsEnabled; }
    void setDesktopNotificationsEnabled(bool enabled)
    {
        if (m_desktopNotificationsEnabled == enabled) return;
        m_desktopNotificationsEnabled = enabled;
        markQueueChanged();
        Q_EMIT desktopNotificationsEnabledChanged(enabled);
    }

    void loadSettings(QSettings &settings)
    {
        applyConfig(TransferQueueSettings::load(settings));
    }

    void saveSettings(QSettings &settings) const
    {
        TransferQueueSettings::save(settings, exportConfig());
    }

    TransferHistoryManager *historyManager() const { return m_historyManager; }
    void setHistoryManager(TransferHistoryManager *manager)
    {
        if (m_historyManager == manager) return;
        m_historyManager = manager;
    }

    // Stops STARTING further tasks; running tasks are untouched. This is not
    // a transfer pause.
    void setDispatchPaused(bool paused)
    {
        OperationScope scope(this);
        if (m_dispatchPaused == paused) return;
        m_dispatchPaused = paused;
        markQueueChanged();
    }
    bool dispatchPaused() const { return m_dispatchPaused; }

    // ------------------------------------------------------------------
    // Per-task control (idempotent)
    // ------------------------------------------------------------------

    // True when the task is, after the call, Cancelled or Cancelling.
    // False for unknown ids, tasks that already completed/failed and for
    // backends that refuse cancellation.
    bool cancelTask(quint64 id)
    {
        OperationScope scope(this);
        return cancelTaskImpl(id);
    }

    // Pause is effective only when the live job accepts suspend(). A backend
    // that cannot pause yields false and the state stays Running.
    bool pauseTask(quint64 id)
    {
        OperationScope scope(this);
        Task *task = find(id);
        if (!task) return false;
        if (task->state == TransferTaskState::Paused) return true;
        if (task->state != TransferTaskState::Running) return false;
        KJob *job = task->job.data();
        if (!job || !task->desc.capabilities.canPause
            || !job->capabilities().testFlag(KJob::Suspendable)) {
            return false;
        }
        task->individuallyPaused = true;
        task->pausedByGlobal = false;
        const bool accepted = job->suspend();
        task = find(id);  // the job may have finished while suspending
        if (!task || isTerminalTransferState(task->state) || !accepted) {
            if (task) task->individuallyPaused = false;
            return false;
        }
        if (task->state == TransferTaskState::Running) {
            setState(task, TransferTaskState::Paused);
        }
        return task->state == TransferTaskState::Paused;
    }

    bool resumeTask(quint64 id)
    {
        OperationScope scope(this);
        Task *task = find(id);
        if (!task) return false;
        if (task->state == TransferTaskState::Running) return true;
        if (task->state != TransferTaskState::Paused) return false;
        KJob *job = task->job.data();
        if (!job || !task->desc.capabilities.canResume) return false;
        const bool accepted = job->resume();
        task = find(id);
        if (!task || isTerminalTransferState(task->state) || !accepted) return false;
        task->individuallyPaused = false;
        task->pausedByGlobal = false;
        if (task->state == TransferTaskState::Paused) {
            setState(task, TransferTaskState::Running);
        }
        return task->state == TransferTaskState::Running;
    }

    // ------------------------------------------------------------------
    // Global Queue Control API (Stage 5)
    // ------------------------------------------------------------------

    TransferQueueGlobalControlState globalControlState() const
    {
        if (m_cancellingAll) {
            return TransferQueueGlobalControlState::CancellingAll;
        }
        if (m_globalPauseActive) {
            for (const auto &entry : m_tasks) {
                if (entry.second->slotHeld && entry.second->state == TransferTaskState::Running) {
                    return TransferQueueGlobalControlState::PartiallyPaused;
                }
            }
            return TransferQueueGlobalControlState::Paused;
        }
        if (m_dispatchPaused) {
            return TransferQueueGlobalControlState::DispatchPaused;
        }
        return TransferQueueGlobalControlState::Normal;
    }

    bool isGlobalPauseActive() const { return m_globalPauseActive; }
    bool isCancellingAll() const { return m_cancellingAll; }

    // Global Pause All: locks dispatch atomically and attempts to suspend all
    // running tasks. Tasks with backends refusing suspend stay Running.
    bool pauseAll()
    {
        OperationScope scope(this);
        if (m_cancellingAll) return false;
        m_globalPauseActive = true;
        m_dispatchPaused = true;

        const QList<quint64> ids = m_order;
        for (quint64 id : ids) {
            Task *task = find(id);
            if (!task || task->state != TransferTaskState::Running) continue;

            KJob *job = task->job.data();
            if (job && task->desc.capabilities.canPause
                && job->capabilities().testFlag(KJob::Suspendable)) {
                task->pausedByGlobal = true;
                task->individuallyPaused = false;
                const bool accepted = job->suspend();
                task = find(id);
                if (!task || isTerminalTransferState(task->state) || !accepted) {
                    if (task) task->pausedByGlobal = false;
                } else {
                    if (task->state == TransferTaskState::Running) {
                        setState(task, TransferTaskState::Paused);
                    }
                }
            }
        }
        markQueueChanged();
        return true;
    }

    // Global Resume All: resumes active tasks that were suspended by global pause.
    // Tasks paused individually by the user remain paused. Unlocks dispatch.
    bool resumeAll()
    {
        OperationScope scope(this);
        if (m_cancellingAll) return false;
        if (!m_globalPauseActive && !m_dispatchPaused) return false;

        const QList<quint64> ids = m_order;
        for (quint64 id : ids) {
            Task *task = find(id);
            if (!task || task->state != TransferTaskState::Paused) continue;
            if (!task->pausedByGlobal) {
                // Preserved: user paused this task individually
                continue;
            }

            KJob *job = task->job.data();
            if (job && task->desc.capabilities.canResume) {
                const bool accepted = job->resume();
                task = find(id);
                if (task && !isTerminalTransferState(task->state) && accepted) {
                    task->pausedByGlobal = false;
                    if (task->state == TransferTaskState::Paused) {
                        setState(task, TransferTaskState::Running);
                    }
                }
            }
        }

        m_globalPauseActive = false;
        m_dispatchPaused = false;
        markQueueChanged();
        return true;
    }

    // Global Cancel All: locks dispatch, cancels queued tasks without starting,
    // and requests cancellation on active tasks. Idempotent.
    bool cancelAll()
    {
        OperationScope scope(this);
        if (m_cancellingAll) return true; // idempotent

        m_cancellingAll = true;
        m_globalPauseActive = false;
        m_dispatchPaused = true;

        // 1. Cancel all queued tasks immediately (no I/O, no job creation)
        const QList<quint64> ids = m_order;
        for (quint64 id : ids) {
            Task *task = find(id);
            if (task && task->state == TransferTaskState::Queued) {
                finalize(task, TransferTaskState::Cancelled, TransferFailureKind::None, 0, {});
            }
        }

        if (m_bandwidthLimiter) {
            m_bandwidthLimiter->cancelAll();
        }

        // 2. Request cancellation for all active tasks
        for (quint64 id : ids) {
            Task *task = find(id);
            if (!task) continue;
            if (task->state == TransferTaskState::Running || task->state == TransferTaskState::Paused) {
                cancelTaskImpl(id);
            }
        }

        if (m_cancellingAll && activeCount() == 0) {
            m_cancellingAll = false;
            m_dispatchPaused = false;
            pushEvent({Event::CancelAllCompleted});
        }

        markQueueChanged();
        return true;
    }

    // ------------------------------------------------------------------
    // Queue-wide internal API (UI belongs to Stage 5)
    // ------------------------------------------------------------------

    // Cancels every Queued task. Performs no I/O and creates no job.
    int cancelQueuedTasks()
    {
        OperationScope scope(this);
        return cancelQueuedImpl();
    }

    // Asks each active task to cancel through its backend contract. Returns
    // the number of tasks that accepted. Completion is reported later through
    // the normal terminal transition, never assumed here.
    int requestCancelActive()
    {
        OperationScope scope(this);
        return requestCancelActiveImpl();
    }

    // Tasks that are not terminal (Queued, Running, Paused, Cancelling).
    // The Undo/Redo barrier of a later stage must use exactly this set.
    bool isQueueIdle() const { return nonTerminalCount() == 0; }
    int nonTerminalCount() const
    {
        int count = 0;
        for (const auto &entry : m_tasks) {
            if (!isTerminalTransferState(entry.second->state)) ++count;
        }
        return count;
    }
    QList<quint64> nonTerminalTaskIds() const
    {
        QList<quint64> ids;
        for (quint64 id : m_order) {
            const Task *task = find(id);
            if (task && !isTerminalTransferState(task->state)) ids.push_back(id);
        }
        return ids;
    }
    int queuedCount() const { return countState(TransferTaskState::Queued); }
    // Tasks holding a slot: Running, Paused and Cancelling.
    int activeCount() const
    {
        int count = 0;
        for (const auto &entry : m_tasks) {
            if (entry.second->slotHeld) ++count;
        }
        return count;
    }
    bool hasNonTerminalTasks() const { return nonTerminalCount() > 0; }
    const TransferReservationRegistry &reservationRegistry() const { return m_registry; }

    // ------------------------------------------------------------------
    // Priority and reordering (Stage 4)
    // ------------------------------------------------------------------

    bool setTaskPriority(quint64 id, TransferPriority priority)
    {
        OperationScope scope(this);
        Task *task = find(id);
        if (!task || task->state != TransferTaskState::Queued) return false;
        if (task->desc.priority == priority) return true;

        const TransferPriority oldPriority = task->desc.priority;
        task->desc.priority = priority;

        m_queuedOrder.removeAll(id);
        int insertIdx = m_queuedOrder.size();
        for (int i = 0; i < m_queuedOrder.size(); ++i) {
            const Task *other = find(m_queuedOrder.at(i));
            if (other && static_cast<int>(other->desc.priority) < static_cast<int>(priority)) {
                insertIdx = i;
                break;
            }
        }
        m_queuedOrder.insert(insertIdx, id);

        if (priority == TransferPriority::High) {
            task->boostSequence = 0;
        } else if (isStarved(task) && task->boostSequence == 0) {
            task->boostSequence = m_nextBoostSequence++;
        } else if (!isStarved(task)) {
            task->boostSequence = 0;
        }

        Event event{Event::PriorityChanged};
        event.id = id;
        event.fromPriority = oldPriority;
        event.toPriority = priority;
        pushEvent(event);
        markQueueChanged();
        return true;
    }

    bool canMoveTaskUp(quint64 id) const
    {
        const Task *task = find(id);
        if (!task || task->state != TransferTaskState::Queued) return false;
        const int idx = m_queuedOrder.indexOf(id);
        if (idx <= 0) return false;
        const Task *prevTask = find(m_queuedOrder.at(idx - 1));
        if (!prevTask || prevTask->state != TransferTaskState::Queued) return false;
        return prevTask->desc.priority == task->desc.priority;
    }

    bool canMoveTaskDown(quint64 id) const
    {
        const Task *task = find(id);
        if (!task || task->state != TransferTaskState::Queued) return false;
        const int idx = m_queuedOrder.indexOf(id);
        if (idx < 0 || idx >= m_queuedOrder.size() - 1) return false;
        const Task *nextTask = find(m_queuedOrder.at(idx + 1));
        if (!nextTask || nextTask->state != TransferTaskState::Queued) return false;
        return nextTask->desc.priority == task->desc.priority;
    }

    bool moveTaskUp(quint64 id)
    {
        OperationScope scope(this);
        if (!canMoveTaskUp(id)) return false;
        const int idx = m_queuedOrder.indexOf(id);
        const quint64 prevId = m_queuedOrder.at(idx - 1);
        Task *task = find(id);
        Task *prev = find(prevId);
        if (task && prev && task->boostSequence > 0 && prev->boostSequence > 0) {
            std::swap(task->boostSequence, prev->boostSequence);
        }
        m_queuedOrder.swapItemsAt(idx, idx - 1);
        Event event{Event::TaskOrderChanged};
        event.id = id;
        pushEvent(event);
        markQueueChanged();
        return true;
    }

    bool moveTaskDown(quint64 id)
    {
        OperationScope scope(this);
        if (!canMoveTaskDown(id)) return false;
        const int idx = m_queuedOrder.indexOf(id);
        const quint64 nextId = m_queuedOrder.at(idx + 1);
        Task *task = find(id);
        Task *next = find(nextId);
        if (task && next && task->boostSequence > 0 && next->boostSequence > 0) {
            std::swap(task->boostSequence, next->boostSequence);
        }
        m_queuedOrder.swapItemsAt(idx, idx + 1);
        Event event{Event::TaskOrderChanged};
        event.id = id;
        pushEvent(event);
        markQueueChanged();
        return true;
    }

    // Removes terminal task records. Returns how many were removed.
    int clearFinished()
    {
        OperationScope scope(this);
        int removed = 0;
        for (qsizetype index = m_order.size() - 1; index >= 0; --index) {
            const quint64 id = m_order.at(index);
            const Task *task = find(id);
            if (task && isTerminalTransferState(task->state)) {
                m_tasks.erase(id);
                m_order.removeAt(index);
                m_queuedOrder.removeAll(id);
                ++removed;
            }
        }
        if (removed > 0) markQueueChanged();
        return removed;
    }

    // Removes a single terminal task record from history. Returns true if removed.
    bool removeFinishedTask(quint64 id)
    {
        OperationScope scope(this);
        const Task *task = find(id);
        if (!task || !isTerminalTransferState(task->state)) {
            return false;
        }
        m_tasks.erase(id);
        m_order.removeAll(id);
        m_queuedOrder.removeAll(id);
        markQueueChanged();
        return true;
    }

    // ------------------------------------------------------------------
    // Snapshots
    // ------------------------------------------------------------------

    TransferQueueSnapshot snapshot() const
    {
        TransferQueueSnapshot result;
        result.mode = m_mode;
        result.globalState = globalControlState();
        result.parallelLimit = m_parallelLimit;
        result.effectiveLimit = effectiveConcurrencyLimit();
        result.concurrencyLimit = effectiveConcurrencyLimit();
        result.activeCount = activeCount();
        result.queuedCount = queuedCount();
        result.dispatchPaused = m_dispatchPaused;
        result.globalPauseActive = m_globalPauseActive;
        result.cancellingAll = m_cancellingAll;
        result.shuttingDown = m_shuttingDown;
        result.idle = isQueueIdle();
        if (m_bandwidthLimiter) {
            result.bandwidthLimitEnabled = m_bandwidthLimiter->isEnabled();
            result.bandwidthLimitBytesPerSec = m_bandwidthLimiter->bytesPerSecond();
        }
        result.throttledActiveCount = throttledActiveTasksCount();
        result.unthrottledActiveCount = unthrottledActiveTasksCount();
        const QList<quint64> queued = sortedQueuedIds();
        for (quint64 id : m_order) {
            if (const Task *task = find(id)) {
                result.tasks.push_back(makeSnapshot(
                    *task, static_cast<int>(queued.indexOf(id)) + 1));
            }
        }
        return result;
    }

    std::optional<TransferTaskSnapshot> taskSnapshot(quint64 id) const
    {
        const Task *task = find(id);
        if (!task) return std::nullopt;
        return makeSnapshot(*task, static_cast<int>(sortedQueuedIds().indexOf(id)) + 1);
    }

    // ------------------------------------------------------------------
    // Controlled shutdown
    // ------------------------------------------------------------------

    // Stops the scheduler, cancels waiting tasks without I/O and asks active
    // tasks to cancel. Never blocks, never terminates anything.
    void beginShutdown()
    {
        OperationScope scope(this);
        if (m_shuttingDown) return;
        m_shuttingDown = true;
        cancelQueuedImpl();
        requestCancelActiveImpl();
        m_watchdog->start(m_watchdogIntervalMs);
        markQueueChanged();
    }
    bool isShuttingDown() const { return m_shuttingDown; }

    // True only when every task is terminal AND every auto-deleting job object
    // has actually been destroyed (its worker thread, if any, has finished).
    bool isShutdownComplete() const
    {
        return m_shuttingDown && nonTerminalCount() == 0 && m_awaitingDestruction == 0;
    }

    // Diagnostic watchdog interval (default 5 s). Reports delays only.
    void setShutdownDiagnosticInterval(int milliseconds)
    {
        m_watchdogIntervalMs = std::max(1, milliseconds);
        m_watchdog->setInterval(m_watchdogIntervalMs);
    }

    QStringList shutdownDiagnostics() const
    {
        QStringList lines;
        for (quint64 id : m_order) {
            const Task *task = find(id);
            if (!task || isTerminalTransferState(task->state)) continue;
            lines << QStringLiteral("task %1 \"%2\": state=%3 cancelRequested=%4 backend=%5 job=%6")
                         .arg(id)
                         .arg(task->desc.title)
                         .arg(stateName(task->state))
                         .arg(task->cancelRequested ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(static_cast<int>(task->desc.backend))
                         .arg(task->job ? QStringLiteral("alive") : QStringLiteral("none"));
        }
        if (m_awaitingDestruction > 0) {
            lines << QStringLiteral("%1 finished job object(s) not yet destroyed")
                         .arg(m_awaitingDestruction);
        }
        return lines;
    }

Q_SIGNALS:
    void taskEnqueued(quint64 id);
    void taskStateChanged(quint64 id, TransferTaskState from, TransferTaskState to);
    // Emitted exactly once per task, after its terminal taskStateChanged.
    void taskFinished(quint64 id, TransferTaskState terminalState);
    void queueChanged();
    void queueBecameIdle();
    void shutdownCompleted();
    // Diagnostic only. Receiving it must never force the application to close.
    void shutdownDelayed(const QStringList &diagnostics);
    void taskProgressChanged(quint64 id, quint64 processedBytes, quint64 totalBytes, int percent);
    void modeChanged(TransferQueueMode mode);
    void parallelLimitChanged(int limit);
    void drainingChanged(bool draining);
    void taskPriorityChanged(quint64 id, TransferPriority priority);
    void taskOrderChanged(quint64 id);
    void globalControlStateChanged(TransferQueueGlobalControlState state);
    void cancelAllCompleted();
    void bandwidthLimitChanged(bool enabled, quint64 bytesPerSec);
    void closeWindowOnCompleteChanged(bool close);
    void desktopNotificationsEnabledChanged(bool enabled);

private:
    struct Task {
        TransferTaskDescriptor desc;
        ReservationRequest reservation;
        TransferTaskState state = TransferTaskState::Queued;
        TransferWaitReason waitReason = TransferWaitReason::None;
        quint64 waitBlocker = 0;
        QPointer<KJob> job;
        bool slotHeld = false;
        bool cancelRequested = false;
        bool cancelBeforeJobExists = false;
        bool individuallyPaused = false;
        bool pausedByGlobal = false;
        TransferTaskState stateBeforeCancel = TransferTaskState::Running;
        TransferFailureKind failureKind = TransferFailureKind::None;
        int errorCode = 0;
        QString errorText;
        quint64 processedBytes = 0;
        quint64 totalBytes = 0;
        int percent = 0;
        int starvationBypassCount = 0;
        quint64 boostSequence = 0;
        QElapsedTimer elapsedTimer;
        qulonglong speedBytesPerSecond = 0;
        qulonglong averageSpeedBytesPerSecond = 0;
        qint64 etaSeconds = -1;
        QList<qulonglong> speedSamples;
        quint64 lastSampleProcessedBytes = 0;
        qint64 lastSampleElapsedMs = 0;
        QString contextText;
        QUrl currentSourceUrl;
        QUrl currentDestinationUrl;
        QList<QMetaObject::Connection> connections;
        QDateTime enqueuedTimeUtc;
        QDateTime startTimeUtc;
        QDateTime endTimeUtc;
        bool historyRecorded = false;
    };

    struct Event {
        enum Kind { Enqueued, StateChanged, Finished, QueueChanged, BecameIdle,
                    ShutdownCompleted, ShutdownDelayed, PriorityChanged, TaskOrderChanged,
                    GlobalControlStateChanged, CancelAllCompleted };
        Kind kind = QueueChanged;
        quint64 id = 0;
        TransferTaskState from = TransferTaskState::Queued;
        TransferTaskState to = TransferTaskState::Queued;
        QStringList diagnostics {};
        TransferPriority fromPriority = TransferPriority::Normal;
        TransferPriority toPriority = TransferPriority::Normal;
        TransferQueueGlobalControlState globalState = TransferQueueGlobalControlState::Normal;
    };

    enum class LaunchOutcome { Launched, FailedTerminal, Blocked };

    // Settles after every public entry point: dispatches, updates idle and
    // shutdown tracking and flushes queued signals.
    struct OperationScope {
        explicit OperationScope(TransferQueueController *owner) : controller(owner) {}
        ~OperationScope() { if (controller) controller->settle(); }
        QPointer<TransferQueueController> controller;
    };

    static QString stateName(TransferTaskState state)
    {
        switch (state) {
        case TransferTaskState::Queued: return QStringLiteral("Queued");
        case TransferTaskState::Running: return QStringLiteral("Running");
        case TransferTaskState::Paused: return QStringLiteral("Paused");
        case TransferTaskState::Cancelling: return QStringLiteral("Cancelling");
        case TransferTaskState::Completed: return QStringLiteral("Completed");
        case TransferTaskState::Cancelled: return QStringLiteral("Cancelled");
        case TransferTaskState::Failed: return QStringLiteral("Failed");
        }
        return {};
    }

    static bool isAdminUrl(const QUrl &url)
    {
        return url.scheme().compare(QStringLiteral("admin"), Qt::CaseInsensitive) == 0;
    }

    static bool containsAdminScheme(const TransferTaskDescriptor &desc)
    {
        auto any = [](const QList<QUrl> &urls) {
            return std::any_of(urls.begin(), urls.end(), isAdminUrl);
        };
        return isAdminUrl(desc.destination) || any(desc.sources) || any(desc.targetUrls)
            || isAdminUrl(desc.context.destinationDirectory)
            || any(desc.context.sources) || any(desc.context.destinationUrls);
    }

    Task *find(quint64 id) const
    {
        const auto found = m_tasks.find(id);
        return found == m_tasks.end() ? nullptr : found->second.get();
    }

    int countState(TransferTaskState state) const
    {
        int count = 0;
        for (const auto &entry : m_tasks) {
            if (entry.second->state == state) ++count;
        }
        return count;
    }

    bool taskSupportsThrottling(const Task &task) const
    {
        if (task.desc.capabilities.supportsThrottling) return true;
        if (task.desc.backend == TransferBackendKind::LocalNative && task.desc.type == TransferOperationType::Copy) {
            return true;
        }
        if (task.job) {
            if (qobject_cast<LocalFileCopyJob *>(task.job.data())) {
                return true;
            }
            const QMetaObject *mo = task.job->metaObject();
            for (int i = 0; i < mo->methodCount(); ++i) {
                if (mo->method(i).name() == "setBandwidthLimiter") {
                    return true;
                }
            }
        }
        return false;
    }

    bool isStarved(const Task *task) const
    {
        if (!task || task->state != TransferTaskState::Queued) return false;
        if (task->desc.priority == TransferPriority::Normal) {
            return task->starvationBypassCount >= StarvationBypassThresholdNormal;
        }
        if (task->desc.priority == TransferPriority::Low) {
            return task->starvationBypassCount >= StarvationBypassThresholdLow;
        }
        return false;
    }

    int taskDispatchTier(const Task *task) const
    {
        if (!task) return 99;
        if (task->boostSequence > 0) {
            return 1; // Boosted Tier (both Normal and Low)
        }
        if (task->desc.priority == TransferPriority::High) {
            return 2; // Regular High
        }
        if (task->desc.priority == TransferPriority::Normal) {
            return 3; // Regular Normal
        }
        return 4; // Regular Low
    }

    QList<quint64> sortedQueuedIds() const
    {
        QList<quint64> ids;
        ids.reserve(m_queuedOrder.size());
        for (quint64 id : m_queuedOrder) {
            const Task *task = find(id);
            if (task && task->state == TransferTaskState::Queued) {
                ids.push_back(id);
            }
        }
        if (ids.size() < queuedCount()) {
            for (quint64 id : m_order) {
                const Task *task = find(id);
                if (task && task->state == TransferTaskState::Queued && !ids.contains(id)) {
                    ids.push_back(id);
                }
            }
        }
        std::stable_sort(ids.begin(), ids.end(), [this](quint64 aId, quint64 bId) {
            const Task *a = find(aId);
            const Task *b = find(bId);
            const int tierA = taskDispatchTier(a);
            const int tierB = taskDispatchTier(b);
            if (tierA != tierB) {
                return tierA < tierB;
            }
            if (tierA == 1 && a && b && a->boostSequence != b->boostSequence) {
                return a->boostSequence < b->boostSequence;
            }
            return false;
        });
        return ids;
    }

    TransferTaskSnapshot makeSnapshot(const Task &task, int queueIndexOneBased) const
    {
        TransferTaskSnapshot snap;
        snap.id = task.desc.id;
        snap.title = task.desc.title;
        snap.type = task.desc.type;
        snap.backend = task.desc.backend;
        snap.sources = task.desc.sources;
        snap.destination = task.desc.destination;
        snap.priority = task.desc.priority;
        snap.state = task.state;
        snap.terminal = isTerminalTransferState(task.state);
        snap.queuePosition = task.state == TransferTaskState::Queued ? queueIndexOneBased : 0;
        snap.waitReason = task.state == TransferTaskState::Queued
            ? task.waitReason : TransferWaitReason::None;
        snap.waitBlockerId = task.state == TransferTaskState::Queued ? task.waitBlocker : 0;
        snap.declared = task.desc.capabilities;
        snap.reservationPrecise = task.reservation.precise;
        snap.failureKind = task.failureKind;
        snap.errorCode = task.errorCode;
        snap.errorText = task.errorText;
        const KJob *job = task.job.data();
        const bool active = task.state == TransferTaskState::Running
            || task.state == TransferTaskState::Paused;
        snap.canCancel = task.state == TransferTaskState::Queued
            || (active && job && task.desc.capabilities.canCancelRunning
                && job->capabilities().testFlag(KJob::Killable));
        snap.canPause = active && job && task.desc.capabilities.canPause
            && job->capabilities().testFlag(KJob::Suspendable);
        snap.jobSuspended = task.state == TransferTaskState::Paused
            && job && job->isSuspended();
        snap.processedBytes = task.processedBytes;
        snap.totalBytes = task.totalBytes;
        snap.percent = task.percent;
        snap.starvationBypassCount = task.starvationBypassCount;
        snap.boostSequence = task.boostSequence;
        snap.individuallyPaused = task.individuallyPaused;
        snap.pausedByGlobal = task.pausedByGlobal;
        snap.supportsThrottling = taskSupportsThrottling(task);
        snap.throttlingActive = snap.supportsThrottling && active
            && m_bandwidthLimiter && m_bandwidthLimiter->isEnabled();
        snap.speedBytesPerSecond = task.speedBytesPerSecond;
        snap.averageSpeedBytesPerSecond = task.averageSpeedBytesPerSecond;
        snap.etaSeconds = task.etaSeconds;
        snap.speedSamples = task.speedSamples;
        snap.contextText = task.contextText;
        snap.currentSourceUrl = task.currentSourceUrl;
        snap.currentDestinationUrl = task.currentDestinationUrl;
        return snap;
    }

    void updateTaskProgress(quint64 id, quint64 processedBytes, quint64 totalBytes, int percent)
    {
        Task *task = find(id);
        if (!task) return;
        bool changed = false;
        if (processedBytes > 0 && task->processedBytes != processedBytes) {
            task->processedBytes = processedBytes;
            changed = true;
        }
        if (totalBytes > 0 && task->totalBytes != totalBytes) {
            task->totalBytes = totalBytes;
            changed = true;
        }
        if (percent >= 0 && task->percent != percent) {
            task->percent = percent;
            changed = true;
        } else if (task->totalBytes > 0 && task->processedBytes > 0) {
            const int calc = static_cast<int>((task->processedBytes * 100) / task->totalBytes);
            if (task->percent != calc) {
                task->percent = calc;
                changed = true;
            }
        }
        if (task->elapsedTimer.isValid()) {
            const qint64 nowMs = task->elapsedTimer.elapsed();
            if (nowMs - task->lastSampleElapsedMs >= 500) {
                const qint64 dt = nowMs - task->lastSampleElapsedMs;
                if (dt > 0 && task->processedBytes >= task->lastSampleProcessedBytes) {
                    const quint64 dBytes = task->processedBytes - task->lastSampleProcessedBytes;
                    const qulonglong calcSpeed = (dBytes * 1000) / dt;
                    if (task->speedBytesPerSecond == 0) {
                        task->speedBytesPerSecond = calcSpeed;
                    }
                    task->speedSamples.push_back(task->speedBytesPerSecond > 0 ? task->speedBytesPerSecond : calcSpeed);
                    if (task->speedSamples.size() > 120) {
                        task->speedSamples.removeFirst();
                    }
                }
                task->lastSampleElapsedMs = nowMs;
                task->lastSampleProcessedBytes = task->processedBytes;
                changed = true;
            }
            if (nowMs > 0 && task->processedBytes > 0) {
                task->averageSpeedBytesPerSecond = (task->processedBytes * 1000) / nowMs;
            }
            if (task->speedBytesPerSecond > 0 && task->totalBytes > task->processedBytes) {
                task->etaSeconds = static_cast<qint64>((task->totalBytes - task->processedBytes) / task->speedBytesPerSecond);
            } else if (task->averageSpeedBytesPerSecond > 0 && task->totalBytes > task->processedBytes) {
                task->etaSeconds = static_cast<qint64>((task->totalBytes - task->processedBytes) / task->averageSpeedBytesPerSecond);
            } else {
                task->etaSeconds = -1;
            }
        }
        if (changed) {
            Q_EMIT taskProgressChanged(id, task->processedBytes, task->totalBytes, task->percent);
        }
    }

    void updateTaskSpeed(quint64 id, qulonglong speed)
    {
        Task *task = find(id);
        if (!task) return;
        task->speedBytesPerSecond = speed;
        task->speedSamples.push_back(speed);
        if (task->speedSamples.size() > 120) {
            task->speedSamples.removeFirst();
        }
        if (task->elapsedTimer.isValid()) {
            const qint64 elapsedMs = task->elapsedTimer.elapsed();
            if (elapsedMs > 0 && task->processedBytes > 0) {
                task->averageSpeedBytesPerSecond = (task->processedBytes * 1000) / elapsedMs;
            }
        }
        if (speed > 0 && task->totalBytes > task->processedBytes) {
            task->etaSeconds = static_cast<qint64>((task->totalBytes - task->processedBytes) / speed);
        } else if (task->averageSpeedBytesPerSecond > 0 && task->totalBytes > task->processedBytes) {
            task->etaSeconds = static_cast<qint64>((task->totalBytes - task->processedBytes) / task->averageSpeedBytesPerSecond);
        } else {
            task->etaSeconds = -1;
        }
        markQueueChanged();
        flushEvents();
    }

    void updateTaskDescription(quint64 id, KJob *job,
                               const QPair<QString, QString> &field1,
                               const QPair<QString, QString> &field2)
    {
        Task *task = find(id);
        if (!task) return;
        if (job) {
            const QUrl curSrc = job->property("thispcCurrentSourceUrl").value<QUrl>();
            if (curSrc.isValid()) {
                task->currentSourceUrl = curSrc;
                task->currentDestinationUrl = job->property("thispcCurrentDestinationUrl").value<QUrl>();
            }
        }
        QString context;
        if (!field1.second.isEmpty()) {
            context = field1.first.isEmpty() ? field1.second : QStringLiteral("%1: %2").arg(field1.first, field1.second);
        }
        if (!field2.second.isEmpty()) {
            const QString second = field2.first.isEmpty() ? field2.second : QStringLiteral("%1: %2").arg(field2.first, field2.second);
            context = context.isEmpty() ? second : context + QStringLiteral("  •  ") + second;
        }
        task->contextText = context;
        markQueueChanged();
        flushEvents();
    }

    // ---------------- events (deferred signal emission) ----------------

    void pushEvent(const Event &event) { m_events.push_back(event); }

    void markQueueChanged()
    {
        if (m_queueChangedPending) return;
        m_queueChangedPending = true;
        pushEvent({Event::QueueChanged});
    }

    void flushEvents()
    {
        if (m_flushing) return;
        m_flushing = true;
        QPointer<TransferQueueController> self(this);
        while (!m_events.isEmpty()) {
            const Event event = m_events.takeFirst();
            switch (event.kind) {
            case Event::Enqueued: Q_EMIT taskEnqueued(event.id); break;
            case Event::StateChanged:
                Q_EMIT taskStateChanged(event.id, event.from, event.to);
                break;
            case Event::Finished: Q_EMIT taskFinished(event.id, event.to); break;
            case Event::QueueChanged:
                m_queueChangedPending = false;
                Q_EMIT queueChanged();
                break;
            case Event::BecameIdle: Q_EMIT queueBecameIdle(); break;
            case Event::ShutdownCompleted: Q_EMIT shutdownCompleted(); break;
            case Event::ShutdownDelayed: Q_EMIT shutdownDelayed(event.diagnostics); break;
            case Event::PriorityChanged:
                Q_EMIT taskPriorityChanged(event.id, event.toPriority);
                break;
            case Event::TaskOrderChanged:
                Q_EMIT taskOrderChanged(event.id);
                break;
            case Event::GlobalControlStateChanged:
                Q_EMIT globalControlStateChanged(event.globalState);
                break;
            case Event::CancelAllCompleted:
                Q_EMIT cancelAllCompleted();
                break;
            }
            if (!self) return;  // a handler destroyed the controller
        }
        m_flushing = false;
    }

    void settle()
    {
        if (m_settling) {
            m_redispatch = true;
            return;
        }
        m_settling = true;
        do {
            m_redispatch = false;
            flushEvents();
            processQueue();
            updateIdleState();
            checkShutdownComplete();
            if (m_cancellingAll && activeCount() == 0) {
                m_cancellingAll = false;
                m_dispatchPaused = false;
                pushEvent({Event::CancelAllCompleted});
                markQueueChanged();
                m_redispatch = true;
            }
            const TransferQueueGlobalControlState gState = globalControlState();
            if (m_lastGlobalControlState != gState) {
                m_lastGlobalControlState = gState;
                Event gEvent{Event::GlobalControlStateChanged};
                gEvent.globalState = gState;
                pushEvent(gEvent);
            }
            flushEvents();
            const bool draining = isDraining();
            if (m_lastDraining != draining) {
                m_lastDraining = draining;
                Q_EMIT drainingChanged(draining);
            }
        } while (m_redispatch || !m_events.isEmpty());
        m_settling = false;
    }

    void updateIdleState()
    {
        const bool idle = isQueueIdle();
        if (idle && !m_wasIdle) pushEvent({Event::BecameIdle});
        m_wasIdle = idle;
    }

    void checkShutdownComplete()
    {
        if (!isShutdownComplete() || m_shutdownReported) return;
        m_shutdownReported = true;
        m_watchdog->stop();
        pushEvent({Event::ShutdownCompleted});
    }

    void onWatchdog()
    {
        OperationScope scope(this);
        if (!m_shuttingDown || isShutdownComplete()) return;
        Event event{Event::ShutdownDelayed};
        event.diagnostics = shutdownDiagnostics();
        pushEvent(event);
        m_watchdog->start(m_watchdogIntervalMs);  // keep reporting; never force
    }

    // ---------------- state transitions ----------------

    void setState(Task *task, TransferTaskState next)
    {
        const TransferTaskState previous = task->state;
        if (previous == next) return;
        task->state = next;
        if (next == TransferTaskState::Running) {
            if (!task->startTimeUtc.isValid()) {
                task->startTimeUtc = QDateTime::currentDateTimeUtc();
            }
            if (!task->elapsedTimer.isValid()) {
                task->elapsedTimer.start();
            }
        } else if (isTerminalTransferState(next)) {
            task->speedBytesPerSecond = 0;
            task->etaSeconds = (next == TransferTaskState::Completed) ? 0 : -1;
            if (!task->speedSamples.isEmpty() && task->speedSamples.constLast() != 0) {
                task->speedSamples.push_back(0);
                if (task->speedSamples.size() > 120) task->speedSamples.removeFirst();
            }
        }
        if (next != TransferTaskState::Queued) {
            task->boostSequence = 0;
            m_queuedOrder.removeAll(task->desc.id);
        }
        if (next != TransferTaskState::Paused) {
            task->individuallyPaused = false;
            task->pausedByGlobal = false;
        }
        Event event{Event::StateChanged};
        event.id = task->desc.id;
        event.from = previous;
        event.to = next;
        pushEvent(event);
        markQueueChanged();
    }

    void releaseResources(Task *task)
    {
        if (task->slotHeld) {
            task->slotHeld = false;
        }
        if (m_bandwidthLimiter) {
            m_bandwidthLimiter->cancelTask(task->desc.id);
        }
        m_registry.release(task->desc.id);  // idempotent
    }

    // The single terminal transition: idempotent, releases the slot and the
    // reservations exactly once and emits Finished exactly once.
    void finalize(Task *task, TransferTaskState terminal, TransferFailureKind kind,
                  int errorCode, const QString &errorText)
    {
        if (isTerminalTransferState(task->state)) return;
        for (const auto &connection : std::as_const(task->connections)) {
            QObject::disconnect(connection);
        }
        task->connections.clear();
        if (KJob *job = task->job.data()) {
            if (job->isAutoDelete()) {
                ++m_awaitingDestruction;
                connect(job, &QObject::destroyed, this, [this] {
                    if (m_awaitingDestruction > 0) --m_awaitingDestruction;
                    OperationScope scope(this);
                });
            }
        }
        task->waitReason = TransferWaitReason::None;
        task->waitBlocker = 0;
        task->failureKind = kind;
        task->errorCode = errorCode;
        task->errorText = errorText;
        task->endTimeUtc = QDateTime::currentDateTimeUtc();
        recordHistoryForTask(task, terminal);
        releaseResources(task);
        setState(task, terminal);
        Event event{Event::Finished};
        event.id = task->desc.id;
        event.to = terminal;
        pushEvent(event);
    }

    void recordHistoryForTask(Task *task, TransferTaskState terminal)
    {
        if (!task || task->historyRecorded) {
            return;
        }
        task->historyRecorded = true;

        if (!m_historyManager) {
            return;
        }

        TransferHistoryRecord rec;
        rec.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        rec.operationType = task->desc.type;
        rec.backendKind = task->desc.backend;
        rec.terminalState = terminal;
        rec.sourceUrls = TransferHistoryRecord::sanitizeUrls(task->desc.sources);
        rec.destinationUrl = TransferHistoryRecord::sanitizeUrl(task->desc.destination);
        rec.startTimeUtc = task->startTimeUtc;
        rec.endTimeUtc = task->endTimeUtc.isValid() ? task->endTimeUtc : QDateTime::currentDateTimeUtc();

        if (rec.startTimeUtc.isValid() && rec.endTimeUtc.isValid()) {
            rec.durationMs = std::max<qint64>(0, rec.startTimeUtc.msecsTo(rec.endTimeUtc));
        } else {
            rec.durationMs = 0;
        }

        rec.totalBytes = task->totalBytes;
        rec.processedBytes = task->processedBytes;

        if (task->averageSpeedBytesPerSecond > 0) {
            rec.averageSpeedBytesPerSecond = task->averageSpeedBytesPerSecond;
        } else if (rec.durationMs > 500 && task->processedBytes > 0) {
            rec.averageSpeedBytesPerSecond = static_cast<quint64>((task->processedBytes * 1000ULL) / rec.durationMs);
        }

        rec.errorCode = task->errorCode;
        rec.errorText = task->errorText;
        rec.title = task->desc.title;
        rec.itemCount = task->desc.sources.size();

        m_historyManager->addRecord(rec, true);
    }

    int cancelQueuedImpl()
    {
        int cancelled = 0;
        const QList<quint64> ids = m_order;
        for (quint64 id : ids) {
            Task *task = find(id);
            if (task && task->state == TransferTaskState::Queued) {
                finalize(task, TransferTaskState::Cancelled, TransferFailureKind::None, 0, {});
                ++cancelled;
            }
        }
        return cancelled;
    }

    int requestCancelActiveImpl()
    {
        int accepted = 0;
        const QList<quint64> ids = m_order;
        for (quint64 id : ids) {
            Task *task = find(id);
            if (!task) continue;
            const bool active = task->state == TransferTaskState::Running
                || task->state == TransferTaskState::Paused;
            if (active && cancelTaskImpl(id)) ++accepted;
        }
        return accepted;
    }

    bool cancelTaskImpl(quint64 id)
    {
        Task *task = find(id);
        if (!task) return false;
        switch (task->state) {
        case TransferTaskState::Cancelled:
        case TransferTaskState::Cancelling:
            return true;
        case TransferTaskState::Completed:
        case TransferTaskState::Failed:
            return false;
        case TransferTaskState::Queued:
            finalize(task, TransferTaskState::Cancelled, TransferFailureKind::None, 0, {});
            return true;
        case TransferTaskState::Running:
        case TransferTaskState::Paused:
            break;
        }
        if (!task->desc.capabilities.canCancelRunning) return false;
        KJob *job = task->job.data();
        if (!job) {
            // Reentrant request while the factory is still running: honoured
            // as soon as the job exists.
            task->cancelBeforeJobExists = true;
            task->stateBeforeCancel = task->state;
            setState(task, TransferTaskState::Cancelling);
            return true;
        }
        if (!job->capabilities().testFlag(KJob::Killable)) return false;

        task->stateBeforeCancel = task->state;
        task->cancelRequested = true;
        setState(task, TransferTaskState::Cancelling);
        const bool accepted = job->kill(KJob::EmitResult);
        task = find(id);  // kill() may complete the job synchronously
        if (!task || isTerminalTransferState(task->state)) return true;
        if (!accepted) {
            task->cancelRequested = false;
            setState(task, task->stateBeforeCancel);
            return false;
        }
        return true;
    }

    // ---------------- job callbacks ----------------

    void onJobResult(quint64 id, KJob *job)
    {
        OperationScope scope(this);
        Task *task = find(id);
        if (!task || isTerminalTransferState(task->state)) return;  // double signal
        if (task->job.data() != job) return;
        const int error = job->error();
        THISPC_TRACE("TransferQueueController: onJobResult for task #%llu, error=%d (%s)",
                     static_cast<unsigned long long>(id), error,
                     job ? qPrintable(job->errorString()) : "");
        if (error == 0) {
            finalize(task, TransferTaskState::Completed, TransferFailureKind::None, 0, {});
        } else if (error == KJob::KilledJobError || error == KIO::ERR_USER_CANCELED) {
            finalize(task, TransferTaskState::Cancelled, TransferFailureKind::None, error, {});
        } else {
            const QString errMsg = (job->property("thispcPartialMoveOutcome").toBool() && !job->errorText().isEmpty())
                ? job->errorText()
                : job->errorString();
            finalize(task, TransferTaskState::Failed, TransferFailureKind::JobError,
                     error, errMsg);
        }
    }

    void onJobDestroyed(quint64 id)
    {
        OperationScope scope(this);
        Task *task = find(id);
        if (!task || isTerminalTransferState(task->state)) return;
        finalize(task, TransferTaskState::Failed, TransferFailureKind::JobLost, 0,
                 QStringLiteral("job object disappeared without reporting a result"));
    }

    void onJobSuspendedChanged(quint64 id, bool suspended)
    {
        OperationScope scope(this);
        Task *task = find(id);
        if (!task) return;
        if (suspended && task->state == TransferTaskState::Running) {
            setState(task, TransferTaskState::Paused);
        } else if (!suspended && task->state == TransferTaskState::Paused) {
            setState(task, TransferTaskState::Running);
        }
    }

    // ---------------- scheduling ----------------

    void processQueue()
    {
        if (m_dispatching) {
            m_redispatch = true;
            return;
        }
        m_dispatching = true;
        do {
            m_redispatch = false;
            dispatchPass();
        } while (m_redispatch);
        m_dispatching = false;
    }

    void setWait(Task *task, TransferWaitReason reason, quint64 blocker)
    {
        if (task->waitReason == reason && task->waitBlocker == blocker) return;
        task->waitReason = reason;
        task->waitBlocker = blocker;
        markQueueChanged();
    }

    // One ordered pass. Order: priority (High first), then submission order.
    // A task may be overtaken only by tasks that do not conflict with it, so a
    // blocked task is never starved by later conflicting submissions; an
    // imprecise (whole-queue) task acts as a barrier for everything behind it.
    void dispatchPass()
    {
        const QList<quint64> candidates = sortedQueuedIds();
        QList<QPair<quint64, ReservationRequest>> earlierWaiting;
        quint64 barrierId = 0;

        for (quint64 id : candidates) {
            Task *task = find(id);
            if (!task || task->state != TransferTaskState::Queued) continue;

            TransferWaitReason reason = TransferWaitReason::None;
            quint64 blocker = 0;
            if (m_shuttingDown) {
                reason = TransferWaitReason::ShuttingDown;
            } else if (m_dispatchPaused) {
                reason = TransferWaitReason::DispatchPaused;
            } else if (activeCount() >= effectiveConcurrencyLimit()) {
                reason = TransferWaitReason::SlotLimit;
            } else if (barrierId != 0) {
                reason = TransferWaitReason::ExclusiveBarrier;
                blocker = barrierId;
            } else {
                for (const auto &waiting : std::as_const(earlierWaiting)) {
                    if (TransferReservation::conflicts(task->reservation, waiting.second)) {
                        reason = TransferWaitReason::BlockedByReservation;
                        blocker = waiting.first;
                        break;
                    }
                }
                if (reason == TransferWaitReason::None) {
                    if (const auto conflict = m_registry.findConflict(task->reservation)) {
                        reason = TransferWaitReason::BlockedByReservation;
                        blocker = conflict->holder;
                    }
                }
            }

            if (reason == TransferWaitReason::None) {
                const LaunchOutcome outcome = tryLaunch(id);
                if (outcome != LaunchOutcome::Blocked) continue;
                task = find(id);
                if (!task) continue;
                reason = TransferWaitReason::SlotLimit;  // state changed re-entrantly
                m_redispatch = true;
            }

            setWait(task, reason, blocker);
            earlierWaiting.push_back({id, task->reservation});
            if (!task->reservation.precise && barrierId == 0) barrierId = id;
        }
    }

    // Revalidates, takes the slot + reservations, then (and only then) asks the
    // factory for the real job.
    LaunchOutcome tryLaunch(quint64 id)
    {
        Task *task = find(id);
        if (!task || task->state != TransferTaskState::Queued) return LaunchOutcome::Blocked;

        QString problem = TransferReservation::revalidate(
            task->reservation, task->desc.type, task->desc.sources,
            task->desc.destination, task->desc.targetUrls);
        if (problem.isEmpty() && task->desc.preflight) {
            problem = task->desc.preflight();  // user code: may re-enter
        }
        task = find(id);
        if (!task || task->state != TransferTaskState::Queued) return LaunchOutcome::Blocked;
        if (!problem.isEmpty()) {
            finalize(task, TransferTaskState::Failed, TransferFailureKind::PreflightFailed,
                     0, problem);
            return LaunchOutcome::FailedTerminal;
        }

        if (m_shuttingDown || m_dispatchPaused || activeCount() >= effectiveConcurrencyLimit()
            || !m_registry.tryAcquire(id, task->reservation)) {
            return LaunchOutcome::Blocked;
        }
        task->slotHeld = true;
        task->waitReason = TransferWaitReason::None;
        task->waitBlocker = 0;
        task->starvationBypassCount = 0;
        task->boostSequence = 0;
        setState(task, TransferTaskState::Running);

        for (quint64 otherId : m_queuedOrder) {
            Task *other = find(otherId);
            if (other && other->state == TransferTaskState::Queued) {
                if (static_cast<int>(other->desc.priority) < static_cast<int>(task->desc.priority)) {
                    ++other->starvationBypassCount;
                    if (isStarved(other) && other->boostSequence == 0) {
                        other->boostSequence = m_nextBoostSequence++;
                    }
                }
            }
        }

        KJob *job = task->desc.jobFactory ? task->desc.jobFactory() : nullptr;
        task = find(id);
        if (!task) {
            if (job) job->deleteLater();
            return LaunchOutcome::FailedTerminal;
        }
        if (!job) {
            finalize(task, TransferTaskState::Failed, TransferFailureKind::JobFactoryFailed,
                     0, QStringLiteral("job factory returned no job"));
            return LaunchOutcome::FailedTerminal;
        }

        task->job = job;

        THISPC_TRACE("TransferQueueController: starting task #%llu (%s), type=%d, backend=%d, limit=%llu B/s",
                     static_cast<unsigned long long>(task->desc.id),
                     qPrintable(task->desc.title),
                     static_cast<int>(task->desc.type),
                     static_cast<int>(task->desc.backend),
                     static_cast<unsigned long long>(m_bandwidthLimiter && m_bandwidthLimiter->isEnabled() ? m_bandwidthLimiter->bytesPerSecond() : 0));

        if (m_bandwidthLimiter) {
            // Configure rate limiter on jobs that support it
            if (auto *copyJob = qobject_cast<LocalFileCopyJob *>(job)) {
                THISPC_TRACE("Configuring rate limiter on LocalFileCopyJob, id=%llu, enabled=%d, rate=%llu",
                             static_cast<unsigned long long>(id),
                             m_bandwidthLimiter->isEnabled(),
                             static_cast<unsigned long long>(m_bandwidthLimiter->bytesPerSecond()));
                copyJob->setBandwidthLimiter(m_bandwidthLimiter, id);
            } else {
                THISPC_TRACE("Job %p is NOT LocalFileCopyJob, className=%s",
                             static_cast<void*>(job), job ? job->metaObject()->className() : "null");
                const QMetaObject *mo = job->metaObject();
                bool invoked = false;
                for (int i = 0; i < mo->methodCount(); ++i) {
                    const QMetaMethod mm = mo->method(i);
                    if (mm.name() == "setBandwidthLimiter" && mm.parameterCount() == 2) {
                        invoked = mm.invoke(job, Qt::DirectConnection,
                                            Q_ARG(std::shared_ptr<TransferBandwidthLimiter>, m_bandwidthLimiter),
                                            Q_ARG(quint64, id));
                        if (invoked) break;
                    }
                }
                if (!invoked) {
                    for (int i = 0; i < mo->methodCount(); ++i) {
                        const QMetaMethod mm = mo->method(i);
                        if (mm.name() == "setBandwidthLimiter" && mm.parameterCount() == 1) {
                            mm.invoke(job, Qt::DirectConnection,
                                      Q_ARG(std::shared_ptr<TransferBandwidthLimiter>, m_bandwidthLimiter));
                            break;
                        }
                    }
                }
            }
        }

        task->connections << connect(job, &KJob::result, this,
                                     [this, id](KJob *finished) { onJobResult(id, finished); });
        task->connections << connect(job, &QObject::destroyed, this,
                                     [this, id] { onJobDestroyed(id); });
        task->connections << connect(job, &KJob::suspended, this,
                                     [this, id](KJob *) { onJobSuspendedChanged(id, true); });
        task->connections << connect(job, &KJob::resumed, this,
                                     [this, id](KJob *) { onJobSuspendedChanged(id, false); });
        task->connections << connect(job, &KJob::percentChanged, this,
                                     [this, id](KJob *, unsigned long p) {
                                         updateTaskProgress(id, 0, 0, static_cast<int>(p));
                                     });
        task->connections << connect(job, &KJob::processedAmountChanged, this,
                                     [this, id](KJob *, KJob::Unit unit, qulonglong amount) {
                                         if (unit == KJob::Bytes) {
                                             updateTaskProgress(id, static_cast<quint64>(amount), 0, -1);
                                         }
                                     });
        task->connections << connect(job, &KJob::totalAmountChanged, this,
                                     [this, id](KJob *, KJob::Unit unit, qulonglong amount) {
                                         if (unit == KJob::Bytes) {
                                             updateTaskProgress(id, 0, static_cast<quint64>(amount), -1);
                                         }
                                     });
        task->connections << connect(job, &KJob::speed, this,
                                     [this, id](KJob *, unsigned long speed) {
                                         updateTaskSpeed(id, static_cast<qulonglong>(speed));
                                     });
        task->connections << connect(job, &KJob::description, this,
                                     [this, id](KJob *j, const QString &,
                                                const QPair<QString, QString> &f1,
                                                const QPair<QString, QString> &f2) {
                                         updateTaskDescription(id, j, f1, f2);
                                     });

        if (task->cancelBeforeJobExists) {
            task->cancelBeforeJobExists = false;
            task->cancelRequested = true;
            if (task->desc.startMode == TransferJobStartMode::ControllerStarts) {
                // Never started: nothing was touched on disk.
                job->kill(KJob::Quietly);
                finalize(task, TransferTaskState::Cancelled, TransferFailureKind::None, 0, {});
                if (job->isAutoDelete()) job->deleteLater();
            } else {
                job->kill(KJob::EmitResult);
            }
            return LaunchOutcome::Launched;
        }

        if (task->desc.startMode == TransferJobStartMode::ControllerStarts) {
            job->start();  // may finish synchronously; handled by onJobResult
        }
        return LaunchOutcome::Launched;
    }

    std::map<quint64, std::unique_ptr<Task>> m_tasks;
    QList<quint64> m_order;  // submission order
    QList<quint64> m_queuedOrder;  // explicit queued candidate order
    TransferReservationRegistry m_registry;
    QList<Event> m_events;
    QTimer *m_watchdog = nullptr;
    int m_watchdogIntervalMs = 5000;
    TransferQueueMode m_mode = TransferQueueMode::Parallel;
    int m_parallelLimit = TransferQueueDefaultParallelLimit;
    std::shared_ptr<TransferBandwidthLimiter> m_bandwidthLimiter;
    BandwidthUnit m_bandwidthLimitUnit = BandwidthUnit::MiB_s;
    int m_bandwidthLimitValue = 1;
    int m_awaitingDestruction = 0;
    quint64 m_nextId = 1;
    quint64 m_nextSequence = 1;
    quint64 m_nextBoostSequence = 1;
    bool m_dispatchPaused = false;
    bool m_globalPauseActive = false;
    bool m_cancellingAll = false;
    bool m_closeWindowOnComplete = false;
    bool m_desktopNotificationsEnabled = true;
    TransferQueueGlobalControlState m_lastGlobalControlState = TransferQueueGlobalControlState::Normal;
    bool m_shuttingDown = false;
    bool m_shutdownReported = false;
    bool m_dispatching = false;
    bool m_redispatch = false;
    bool m_flushing = false;
    bool m_settling = false;
    bool m_queueChangedPending = false;
    bool m_wasIdle = true;
    bool m_lastDraining = false;
    TransferHistoryManager *m_ownedHistoryManager = nullptr;
    TransferHistoryManager *m_historyManager = nullptr;
};
