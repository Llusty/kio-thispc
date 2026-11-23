/*
 * Application-owned background worker pools and explicit shutdown coordinator.
 *
 * Pure Qt 6 Core lifecycle primitives (zero KF6 dependencies) designed to:
 * - own bounded QThreadPools tied to QCoreApplication lifetime (AUD-A-09, AUD-A-10),
 * - avoid QThreadPool::clear() on QtConcurrent futures via cooperative per-generation
 *   cancellation tokens so every QFuture reaches a terminal state (AUD-1A-01),
 * - track active main windows so closing one window never poisons surviving windows
 *   or races an in-progress drain (AUD-1A-02, AUD-1A-04),
 * - coordinate deterministic asynchronous shutdown with QEventLoopLocker and a
 *   non-destructive diagnostic watchdog across queue, OperationManager and pools
 *   (AUD-A-07, AUD-1A-03),
 * - guard against reentrant close-confirmation dialogs (AUD-A-13).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QCoreApplication>
#include <QEventLoopLocker>
#include <QFuture>
#include <QFutureInterface>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QPromise>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

class ApplicationWorkerPools final : public QObject
{
    Q_OBJECT

public:
    explicit ApplicationWorkerPools(QObject *parent = nullptr)
        : QObject(parent)
        , m_state(std::make_shared<SharedState>(this))
        , m_metadataPool(new QThreadPool(this))
        , m_xattrReadPool(new QThreadPool(this))
        , m_xattrWritePool(new QThreadPool(this))
    {
        configurePool(m_metadataPool, 2);
        configurePool(m_xattrReadPool, 2);
        configurePool(m_xattrWritePool, 2);
    }

    ~ApplicationWorkerPools() override
    {
        drainForQuit();
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->owner = nullptr;
    }

    static ApplicationWorkerPools *forApplication()
    {
        QCoreApplication *app = QCoreApplication::instance();
        if (!app) {
            return nullptr;
        }
        static QPointer<ApplicationWorkerPools> s_instance;
        if (!s_instance || s_instance->parent() != app) {
            auto *created = new ApplicationWorkerPools(app);
            connect(app, &QCoreApplication::aboutToQuit, created, &ApplicationWorkerPools::drainForQuit);
            s_instance = created;
        }
        return s_instance.data();
    }

    QThreadPool *metadataPool() const { return m_metadataPool; }
    QThreadPool *xattrReadPool() const { return m_xattrReadPool; }
    QThreadPool *xattrWritePool() const { return m_xattrWritePool; }

    // Main-window registration (called on main thread).
    void registerWindow(QObject *window)
    {
        if (!window) {
            return;
        }
        const bool inserted = !m_activeWindows.contains(window);
        if (inserted) {
            m_activeWindows.insert(window);
            connect(window, &QObject::destroyed, this, [this, window]() {
                m_activeWindows.remove(window);
            });
        }
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (!m_state->appQuitting && !QCoreApplication::closingDown()) {
            // Re-open submission for the newly active window with a fresh read
            // cancellation generation WITHOUT discarding any pending drain waiters
            // from an earlier closing window.
            if (m_state->appShuttingDown) {
                m_state->appShuttingDown = false;
                ++m_state->currentGeneration;
                m_state->readCancelToken = std::make_shared<std::atomic<bool>>(false);
            }
        }
    }

    void unregisterWindow(QObject *window)
    {
        if (!window) {
            return;
        }
        m_activeWindows.remove(window);
    }

    int activeWindowCount() const
    {
        return static_cast<int>(m_activeWindows.size());
    }

    bool hasOtherActiveWindows(const QObject *closingWindow) const
    {
        for (const QObject *w : m_activeWindows) {
            if (w && w != closingWindow) {
                return true;
            }
        }
        return false;
    }

    int activeReadTaskCount() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->activeReadTasks;
    }

    int activeWriteTaskCount() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->activeWriteTasks;
    }

    int activeTaskCount() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->activeReadTasks + m_state->activeWriteTasks;
    }

    bool isShuttingDown() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->appShuttingDown || m_state->appQuitting;
    }

    bool isDrained() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return (m_state->appShuttingDown || m_state->appQuitting)
            && (m_state->activeReadTasks + m_state->activeWriteTasks) == 0;
    }

    bool hasActiveTasksUpTo(quint64 generation) const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->activeCountUpToLocked(generation) > 0;
    }

    template <typename Fn>
    auto runMetadataTask(Fn &&fn) -> QFuture<std::invoke_result_t<std::decay_t<Fn>>>
    {
        return runTracked(m_metadataPool, false, std::forward<Fn>(fn));
    }

    template <typename Fn>
    auto runXattrRead(Fn &&fn) -> QFuture<std::invoke_result_t<std::decay_t<Fn>>>
    {
        return runTracked(m_xattrReadPool, false, std::forward<Fn>(fn));
    }

    template <typename Fn>
    auto runXattrWrite(Fn &&fn) -> QFuture<std::invoke_result_t<std::decay_t<Fn>>>
    {
        return runTracked(m_xattrWritePool, true, std::forward<Fn>(fn));
    }

    // Begins non-blocking shutdown drain for all tasks submitted up to now.
    // Returns the generation watermark that will be emitted via drainCompleted(gen).
    // Never calls QThreadPool::clear() (AUD-1A-01); instead signals cooperative
    // cancellation to queued read tasks so every QFuture reaches a terminal state.
    // Never cancels accepted xattr writes.
    quint64 beginShutdownDrain()
    {
        quint64 drainGen = 0;
        bool alreadyComplete = false;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->appShuttingDown = true;
            if (m_state->readCancelToken) {
                m_state->readCancelToken->store(true, std::memory_order_release);
            }
            drainGen = m_state->currentGeneration;
            if (m_state->activeCountUpToLocked(drainGen) == 0) {
                alreadyComplete = true;
            } else {
                m_state->pendingDrainGenerations.push_back(drainGen);
            }
        }
        if (alreadyComplete) {
            Q_EMIT drainCompleted(drainGen);
            if (activeTaskCount() == 0) {
                Q_EMIT allTasksDrained();
            }
        }
        return drainGen;
    }

    // Convenience alias for callers/tests expecting beginShutdown().
    void beginShutdown()
    {
        beginShutdownDrain();
    }

    // Deterministic synchronous drain. Never calls QThreadPool::clear() or
    // QThread::terminate(). Completes accepted xattr writes first, then waits
    // for cooperatively-cancelled read tasks to finish cleanly.
    void drainAll()
    {
        std::vector<quint64> completedGens;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->appShuttingDown = true;
            m_state->drainingSync = true;
            if (m_state->readCancelToken) {
                m_state->readCancelToken->store(true, std::memory_order_release);
            }
        }

        // Wait without holding m_state->mutex so workers can update counters freely.
        m_xattrWritePool->waitForDone();
        m_xattrReadPool->waitForDone();
        m_metadataPool->waitForDone();

        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->drainingSync = false;
            completedGens = m_state->takeCompletedDrainGenerationsLocked();
        }
        for (quint64 gen : completedGens) {
            Q_EMIT drainCompleted(gen);
        }
        if (activeTaskCount() == 0) {
            Q_EMIT allTasksDrained();
        }
    }

    // Connected to QCoreApplication::aboutToQuit: permanently refuses new tasks
    // and synchronously drains every pool before ~QCoreApplication().
    void drainForQuit()
    {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->appQuitting = true;
            m_state->appShuttingDown = true;
        }
        drainAll();
    }

    void resetForTesting()
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->activeReadTasks == 0 && m_state->activeWriteTasks == 0) {
            m_state->appQuitting = false;
            m_state->appShuttingDown = false;
            m_state->drainingSync = false;
            m_state->pendingDrainGenerations.clear();
            ++m_state->currentGeneration;
            m_state->readCancelToken = std::make_shared<std::atomic<bool>>(false);
        }
    }

Q_SIGNALS:
    void drainCompleted(quint64 generation);
    void allTasksDrained();

private:
    struct SharedState
    {
        explicit SharedState(ApplicationWorkerPools *o)
            : owner(o)
            , readCancelToken(std::make_shared<std::atomic<bool>>(false))
        {
        }

        int activeCountUpToLocked(quint64 maxGen) const
        {
            int total = 0;
            for (const auto &kv : activeByGeneration) {
                if (kv.first <= maxGen) {
                    total += kv.second;
                }
            }
            return total;
        }

        std::vector<quint64> takeCompletedDrainGenerationsLocked()
        {
            std::vector<quint64> completed;
            auto it = pendingDrainGenerations.begin();
            while (it != pendingDrainGenerations.end()) {
                if (activeCountUpToLocked(*it) == 0) {
                    completed.push_back(*it);
                    it = pendingDrainGenerations.erase(it);
                } else {
                    ++it;
                }
            }
            return completed;
        }

        mutable std::mutex mutex;
        ApplicationWorkerPools *owner = nullptr;
        int activeReadTasks = 0;
        int activeWriteTasks = 0;
        quint64 currentGeneration = 1;
        std::unordered_map<quint64, int> activeByGeneration;
        std::vector<quint64> pendingDrainGenerations;
        std::shared_ptr<std::atomic<bool>> readCancelToken;
        bool appShuttingDown = false;
        bool drainingSync = false;
        bool appQuitting = false;
    };

    struct TaskTracker
    {
        std::shared_ptr<SharedState> state;
        quint64 generation = 0;
        bool isWrite = false;
        bool active = false;

        TaskTracker(std::shared_ptr<SharedState> s, quint64 gen, bool write)
            : state(std::move(s))
            , generation(gen)
            , isWrite(write)
            , active(true)
        {
        }

        ~TaskTracker()
        {
            if (!active || !state) {
                return;
            }
            ApplicationWorkerPools *notifyOwner = nullptr;
            std::vector<quint64> completedGens;
            bool notifyAllDrained = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (isWrite) {
                    --state->activeWriteTasks;
                } else {
                    --state->activeReadTasks;
                }
                auto it = state->activeByGeneration.find(generation);
                if (it != state->activeByGeneration.end()) {
                    if (--it->second <= 0) {
                        state->activeByGeneration.erase(it);
                    }
                }
                if (!state->drainingSync && state->owner) {
                    completedGens = state->takeCompletedDrainGenerationsLocked();
                    const int totalRemaining = state->activeReadTasks + state->activeWriteTasks;
                    if (totalRemaining == 0 && (state->appShuttingDown || state->appQuitting)) {
                        notifyAllDrained = true;
                    }
                    if (!completedGens.empty() || notifyAllDrained) {
                        notifyOwner = state->owner;
                    }
                }
            }
            if (notifyOwner) {
                QMetaObject::invokeMethod(
                    notifyOwner,
                    [notifyOwner, completedGens = std::move(completedGens), notifyAllDrained]() {
                        for (quint64 gen : completedGens) {
                            Q_EMIT notifyOwner->drainCompleted(gen);
                        }
                        if (notifyAllDrained && notifyOwner->activeTaskCount() == 0) {
                            Q_EMIT notifyOwner->allTasksDrained();
                        }
                    },
                    Qt::QueuedConnection);
            }
        }
    };

    static void configurePool(QThreadPool *pool, int maxThreads)
    {
        pool->setMaxThreadCount(maxThreads);
        pool->setExpiryTimeout(10000);
    }

    template <typename Fn>
    auto runTracked(QThreadPool *pool, bool isWrite, Fn &&fn)
        -> QFuture<std::invoke_result_t<std::decay_t<Fn>>>
    {
        using ResultT = std::invoke_result_t<std::decay_t<Fn>>;

        std::shared_ptr<TaskTracker> tracker;
        std::shared_ptr<std::atomic<bool>> readCancelToken;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->appShuttingDown || m_state->appQuitting) {
                return QFutureInterface<ResultT>::canceledResult().future();
            }
            const quint64 gen = m_state->currentGeneration;
            if (isWrite) {
                ++m_state->activeWriteTasks;
            } else {
                ++m_state->activeReadTasks;
                readCancelToken = m_state->readCancelToken;
            }
            ++m_state->activeByGeneration[gen];
            tracker = std::make_shared<TaskTracker>(m_state, gen, isWrite);
        }

        auto callable = std::forward<Fn>(fn);
        return QtConcurrent::run(
            pool,
            [tracker = std::move(tracker),
             readCancelToken = std::move(readCancelToken),
             isWrite,
             callable = std::move(callable)](QPromise<ResultT> &promise) mutable {
                if (!isWrite && readCancelToken
                    && readCancelToken->load(std::memory_order_acquire)) {
                    promise.future().cancel();
                    return;
                }
                if constexpr (std::is_void_v<ResultT>) {
                    callable();
                } else {
                    ResultT value = callable();
                    if (!isWrite && readCancelToken
                        && readCancelToken->load(std::memory_order_acquire)) {
                        promise.future().cancel();
                        return;
                    }
                    promise.addResult(std::move(value));
                }
            });
    }

    std::shared_ptr<SharedState> m_state;
    QThreadPool *m_metadataPool = nullptr;
    QThreadPool *m_xattrReadPool = nullptr;
    QThreadPool *m_xattrWritePool = nullptr;
    QSet<QObject *> m_activeWindows;
};

class ShutdownCoordinator final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Confirming,
        Draining,
        Completed
    };

    class ConfirmationScope final
    {
    public:
        explicit ConfirmationScope(ShutdownCoordinator *coordinator)
            : m_coordinator(coordinator)
        {
            if (m_coordinator) {
                m_entered = m_coordinator->beginConfirmation();
            }
        }

        ~ConfirmationScope()
        {
            if (m_entered && m_coordinator) {
                m_coordinator->endConfirmation();
            }
        }

        ConfirmationScope(const ConfirmationScope &) = delete;
        ConfirmationScope &operator=(const ConfirmationScope &) = delete;

        bool entered() const { return m_entered; }

    private:
        QPointer<ShutdownCoordinator> m_coordinator;
        bool m_entered = false;
    };

    explicit ShutdownCoordinator(QObject *parent = nullptr)
        : QObject(parent)
        , m_watchdog(new QTimer(this))
    {
        m_watchdog->setInterval(5000);
        m_watchdog->setSingleShot(false);
        connect(m_watchdog, &QTimer::timeout, this, [this]() {
            if (m_state == State::Draining) {
                Q_EMIT shutdownDelayed(currentDiagnostics());
            }
        });
    }

    ~ShutdownCoordinator() override
    {
        m_watchdog->stop();
        m_exitLocker.reset();
    }

    State state() const { return m_state; }
    bool isIdle() const { return m_state == State::Idle; }
    bool isConfirming() const { return m_state == State::Confirming; }
    bool isDraining() const { return m_state == State::Draining; }
    bool isCompleted() const { return m_state == State::Completed; }
    bool isEventLoopLocked() const { return m_exitLocker != nullptr; }

    void setDiagnosticInterval(int milliseconds)
    {
        m_watchdog->setInterval(std::max(1, milliseconds));
    }

    void setQueueHooks(std::function<bool()> isCompleteFn,
                       std::function<void()> beginShutdownFn,
                       std::function<QStringList()> diagnosticsFn = {})
    {
        m_isQueueComplete = std::move(isCompleteFn);
        m_beginQueueShutdown = std::move(beginShutdownFn);
        m_queueDiagnostics = std::move(diagnosticsFn);
    }

    void setOperationsHooks(std::function<int()> activeCountFn,
                            std::function<void()> cancelDirectFn,
                            std::function<int()> nonKillableCountFn = {})
    {
        m_activeOperationsCount = std::move(activeCountFn);
        m_cancelDirectOperations = std::move(cancelDirectFn);
        m_nonKillableOperationsCount = std::move(nonKillableCountFn);
    }

    void setWorkerPools(ApplicationWorkerPools *pools)
    {
        if (m_workerPools == pools) {
            return;
        }
        if (m_workerPools) {
            disconnect(m_workerPools, nullptr, this, nullptr);
        }
        m_workerPools = pools;
        if (m_workerPools) {
            connect(m_workerPools, &ApplicationWorkerPools::drainCompleted,
                    this, [this](quint64 gen) {
                        if (m_drainPools && gen >= m_poolDrainGeneration) {
                            m_poolDrainDone = true;
                            checkDrainCompletion();
                        }
                    });
            connect(m_workerPools, &ApplicationWorkerPools::allTasksDrained,
                    this, [this]() {
                        if (m_drainPools) {
                            m_poolDrainDone = true;
                            checkDrainCompletion();
                        }
                    });
        }
    }

    bool beginConfirmation()
    {
        if (m_state != State::Idle) {
            return false;
        }
        m_state = State::Confirming;
        Q_EMIT stateChanged(m_state);
        return true;
    }

    void endConfirmation()
    {
        if (m_state == State::Confirming) {
            m_state = State::Idle;
            Q_EMIT stateChanged(m_state);
        }
    }

    // Allows a window that was closed and later legitimately re-shown to
    // participate in normal close handling again (AUD-1A-07).
    void resetToIdle()
    {
        if (m_state == State::Completed) {
            m_state = State::Idle;
            m_drainPools = false;
            m_poolDrainDone = true;
            m_poolDrainGeneration = 0;
            Q_EMIT stateChanged(m_state);
        }
    }

    // Transitions into Draining, acquires an explicit QEventLoopLocker so
    // QCoreApplication::exec() cannot terminate prematurely while draining,
    // initiates cooperative cancellation/draining of tracked subsystems, and
    // completes immediately if already idle.
    // Set drainProcessPools=false when another active main window still needs
    // the shared process-wide ApplicationWorkerPools (AUD-1A-02).
    bool beginShutdown(bool drainProcessPools = true)
    {
        if (m_state == State::Draining || m_state == State::Completed) {
            return false;
        }
        m_state = State::Draining;
        m_drainPools = drainProcessPools && (m_workerPools != nullptr);
        m_poolDrainDone = !m_drainPools;
        m_poolDrainGeneration = 0;
        m_exitLocker = std::make_unique<QEventLoopLocker>();
        Q_EMIT stateChanged(m_state);

        if (m_cancelDirectOperations) {
            m_cancelDirectOperations();
        }
        if (m_beginQueueShutdown) {
            m_beginQueueShutdown();
        }
        if (m_drainPools && m_workerPools) {
            m_poolDrainGeneration = m_workerPools->beginShutdownDrain();
            if (!m_workerPools->hasActiveTasksUpTo(m_poolDrainGeneration)) {
                m_poolDrainDone = true;
            }
        }

        checkDrainCompletion();
        if (m_state == State::Draining) {
            m_watchdog->start();
        }
        return true;
    }

    void notifyQueueShutdownCompleted()
    {
        checkDrainCompletion();
    }

    void notifyQueueShutdownDelayed(const QStringList &diagnostics)
    {
        if (m_state != State::Draining) {
            return;
        }
        QStringList merged = diagnostics;
        const QStringList extra = currentDiagnostics();
        for (const QString &line : extra) {
            if (!merged.contains(line)) {
                merged.append(line);
            }
        }
        Q_EMIT shutdownDelayed(merged);
    }

    void notifyOperationsChanged()
    {
        checkDrainCompletion();
    }

    QStringList currentDiagnostics() const
    {
        QStringList lines;
        if (m_queueDiagnostics) {
            lines.append(m_queueDiagnostics());
        }
        if (m_activeOperationsCount) {
            const int activeOps = m_activeOperationsCount();
            if (activeOps > 0) {
                const int nonKillable = m_nonKillableOperationsCount ? m_nonKillableOperationsCount() : 0;
                if (nonKillable > 0) {
                    lines << QStringLiteral("%1 active direct operation(s) still running (%2 non-killable)")
                                 .arg(activeOps)
                                 .arg(nonKillable);
                } else {
                    lines << QStringLiteral("%1 active direct operation(s) still cancelling").arg(activeOps);
                }
            }
        }
        if (m_drainPools && m_workerPools && !m_poolDrainDone) {
            const int reads = m_workerPools->activeReadTaskCount();
            const int writes = m_workerPools->activeWriteTaskCount();
            if (reads > 0 || writes > 0) {
                lines << QStringLiteral("worker pools draining: reads=%1 writes=%2").arg(reads).arg(writes);
            }
        }
        return lines;
    }

Q_SIGNALS:
    void stateChanged(ShutdownCoordinator::State state);
    void shutdownCompleted();
    void shutdownDelayed(const QStringList &diagnostics);

private:
    void checkDrainCompletion()
    {
        if (m_state != State::Draining) {
            return;
        }
        const bool queueDone = !m_isQueueComplete || m_isQueueComplete();
        const bool opsDone = !m_activeOperationsCount || m_activeOperationsCount() == 0;
        const bool poolsDone = !m_drainPools || m_poolDrainDone
            || !m_workerPools || !m_workerPools->hasActiveTasksUpTo(m_poolDrainGeneration);
        if (!queueDone || !opsDone || !poolsDone) {
            return;
        }

        m_watchdog->stop();
        m_state = State::Completed;
        Q_EMIT stateChanged(m_state);
        Q_EMIT shutdownCompleted();
        m_exitLocker.reset();
    }

    State m_state = State::Idle;
    QTimer *m_watchdog = nullptr;
    std::unique_ptr<QEventLoopLocker> m_exitLocker;
    QPointer<ApplicationWorkerPools> m_workerPools;
    bool m_drainPools = false;
    bool m_poolDrainDone = true;
    quint64 m_poolDrainGeneration = 0;
    std::function<bool()> m_isQueueComplete;
    std::function<void()> m_beginQueueShutdown;
    std::function<QStringList()> m_queueDiagnostics;
    std::function<int()> m_activeOperationsCount;
    std::function<void()> m_cancelDirectOperations;
    std::function<int()> m_nonKillableOperationsCount;
};
