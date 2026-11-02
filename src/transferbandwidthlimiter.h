/*
 * Bandwidth limiter for the 0.44 transfer queue (Stage 6).
 *
 * Implements a thread-safe Shared Token Bucket rate limiter.
 * Coordinates global bandwidth across all throttled transfer tasks.
 *
 * SCOPE AND CAPABILITIES
 *   - Global byte budget per second (Shared Token Bucket).
 *   - Monotonic clock with support for mock/virtual clock injection.
 *   - Strict burst clamping: no unbounded token accumulation during idleness or pause.
 *   - Safe for multithreaded worker threads (e.g. LocalFileCopyWorker) and GUI/event-driven jobs.
 *   - Fair, starvation-free token distribution among active tasks.
 *   - Safe cancellation: removes pending requests immediately upon task cancellation or Cancel All.
 *   - Zero CPU spin: sleeps on wait condition or timer only for the required replenishment duration.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWaitCondition>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>

enum class BandwidthUnit {
    KiB_s,
    MiB_s
};

inline QString bandwidthUnitName(BandwidthUnit unit)
{
    switch (unit) {
    case BandwidthUnit::KiB_s: return QStringLiteral("KiB/s");
    case BandwidthUnit::MiB_s: return QStringLiteral("MiB/s");
    }
    return QStringLiteral("MiB/s");
}

inline BandwidthUnit parseBandwidthUnit(const QString &raw, BandwidthUnit fallback = BandwidthUnit::MiB_s)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.compare(QStringLiteral("KiB/s"), Qt::CaseInsensitive) == 0 ||
        trimmed.compare(QStringLiteral("KB/s"), Qt::CaseInsensitive) == 0 ||
        trimmed.compare(QStringLiteral("KiB"), Qt::CaseInsensitive) == 0) {
        return BandwidthUnit::KiB_s;
    }
    if (trimmed.compare(QStringLiteral("MiB/s"), Qt::CaseInsensitive) == 0 ||
        trimmed.compare(QStringLiteral("MB/s"), Qt::CaseInsensitive) == 0 ||
        trimmed.compare(QStringLiteral("MiB"), Qt::CaseInsensitive) == 0) {
        return BandwidthUnit::MiB_s;
    }
    return fallback;
}

inline quint64 bandwidthToBytesPerSec(int value, BandwidthUnit unit)
{
    if (value <= 0) return 0;
    const quint64 base = static_cast<quint64>(value);
    switch (unit) {
    case BandwidthUnit::KiB_s:
        return base * 1024ULL;
    case BandwidthUnit::MiB_s:
        return base * 1024ULL * 1024ULL;
    }
    return base * 1024ULL * 1024ULL;
}

inline int bytesPerSecToUnitValue(quint64 bytesPerSec, BandwidthUnit &unit)
{
    if (bytesPerSec == 0) {
        unit = BandwidthUnit::MiB_s;
        return 0;
    }
    if (bytesPerSec % (1024ULL * 1024ULL) == 0) {
        unit = BandwidthUnit::MiB_s;
        return static_cast<int>(bytesPerSec / (1024ULL * 1024ULL));
    }
    unit = BandwidthUnit::KiB_s;
    return static_cast<int>(std::max<quint64>(1, bytesPerSec / 1024ULL));
}

class TransferBandwidthLimiter : public QObject
{
    Q_OBJECT

public:
    using ClockFn = std::function<qint64()>;
    using CancelCheckFn = std::function<bool()>;
    using TokenGrantCallback = std::function<void(qint64 grantedBytes)>;

    static constexpr quint64 MinBandwidthLimitBytes = 1024ULL;              // 1 KiB/s
    static constexpr quint64 MaxBandwidthLimitBytes = 10ULL * 1024 * 1024 * 1024; // 10 GiB/s
    static constexpr qint64 MinBurstBytes = 64 * 1024;                    // 64 KiB
    static constexpr qint64 MaxBurstBytes = 2 * 1024 * 1024;              // 2 MiB

    struct PendingRequest {
        quint64 taskId = 0;
        qint64 requestedBytes = 0;
        TokenGrantCallback callback;
    };

    explicit TransferBandwidthLimiter(QObject *parent = nullptr)
        : QObject(parent)
        , m_clock([]() -> qint64 {
            static const auto epoch = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - epoch).count();
        })
    {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, [this] {
            onTimerFired();
        });
    }

    ~TransferBandwidthLimiter() override
    {
        QMutexLocker locker(&m_mutex);
        m_pendingRequests.clear();
        m_waitCondition.wakeAll();
        if (m_timer) {
            m_timer->stop();
            delete m_timer;
            m_timer = nullptr;
        }
    }

    // --- Configuration ---

    void setClock(ClockFn clock)
    {
        QMutexLocker locker(&m_mutex);
        m_clock = std::move(clock);
        m_lastRefillMs = m_clock ? m_clock() : 0;
    }

    bool isEnabled() const
    {
        QMutexLocker locker(&m_mutex);
        return m_enabled;
    }

    quint64 bytesPerSecond() const
    {
        QMutexLocker locker(&m_mutex);
        return m_bytesPerSec;
    }

    void setLimit(bool enabled, quint64 bytesPerSec)
    {
        QList<PendingRequest> toInvoke;
        {
            QMutexLocker locker(&m_mutex);
            const bool wasEnabled = m_enabled;
            const quint64 oldRate = m_bytesPerSec;

            if (!enabled || bytesPerSec == 0) {
                m_enabled = false;
                m_bytesPerSec = 0;
                m_tokens = 0;
                // Wake up all waiting requests immediately: unlimited mode
                toInvoke = std::move(m_pendingRequests);
                m_waitCondition.wakeAll();
            } else {
                m_enabled = true;
                m_bytesPerSec = std::clamp(bytesPerSec, MinBandwidthLimitBytes, MaxBandwidthLimitBytes);
                m_maxBurst = std::clamp<qint64>(static_cast<qint64>(m_bytesPerSec), MinBurstBytes, MaxBurstBytes);

                const qint64 now = nowMs();
                if (!wasEnabled || m_lastRefillMs == 0) {
                    m_tokens = static_cast<double>(m_maxBurst);
                    m_lastRefillMs = now;
                } else {
                    refillLocked(now);
                    m_tokens = std::min(m_tokens, static_cast<double>(m_maxBurst));
                }
            }

            if (wasEnabled != m_enabled || oldRate != m_bytesPerSec) {
                scheduleTimerLocked();
            }
        }

        // Grant pending requests if now unlimited
        for (const auto &req : toInvoke) {
            if (req.callback) {
                req.callback(req.requestedBytes);
            }
        }

        Q_EMIT limitChanged(m_enabled, m_bytesPerSec);
    }

    // --- Synchronous Token Acquisition (for worker threads) ---
    // Blocks the caller thread until tokens are granted, or until isCancelled() returns true.
    // Returns the number of granted bytes (0 if cancelled/stopped).
    qint64 acquire(quint64 taskId, qint64 bytes, CancelCheckFn isCancelled = nullptr)
    {
        if (bytes <= 0) return 0;

        QMutexLocker locker(&m_mutex);
        while (true) {
            if (isCancelled && isCancelled()) {
                return 0;
            }

            if (!m_enabled || m_bytesPerSec == 0) {
                recordTransferredLocked(taskId, bytes);
                return bytes;
            }

            const qint64 now = nowMs();
            refillLocked(now);

            // If tokens are available and no pending async request has priority
            if (m_tokens >= static_cast<double>(bytes) && m_pendingRequests.isEmpty()) {
                m_tokens -= static_cast<double>(bytes);
                recordTransferredLocked(taskId, bytes);
                return bytes;
            }

            // Calculate deficit wait time
            const double needed = static_cast<double>(bytes) - m_tokens;
            qint64 waitMs = 10;
            if (needed > 0 && m_bytesPerSec > 0) {
                waitMs = static_cast<qint64>(std::ceil((needed * 1000.0) / static_cast<double>(m_bytesPerSec)));
            }
            waitMs = std::clamp<qint64>(waitMs, 5, 200);

            m_waitCondition.wait(&m_mutex, static_cast<unsigned long>(waitMs));
        }
    }

    // --- Asynchronous Token Request (for event-driven/GUI thread) ---
    // If tokens are available, callback is invoked immediately.
    // If not, request is queued and invoked once tokens replenish.
    void requestTokens(quint64 taskId, qint64 bytes, TokenGrantCallback callback)
    {
        if (!callback) return;
        if (bytes <= 0) {
            callback(0);
            return;
        }

        bool invokeNow = false;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_enabled || m_bytesPerSec == 0) {
                recordTransferredLocked(taskId, bytes);
                invokeNow = true;
            } else {
                const qint64 now = nowMs();
                refillLocked(now);

                if (m_pendingRequests.isEmpty() && m_tokens >= static_cast<double>(bytes)) {
                    m_tokens -= static_cast<double>(bytes);
                    recordTransferredLocked(taskId, bytes);
                    invokeNow = true;
                } else {
                    PendingRequest req;
                    req.taskId = taskId;
                    req.requestedBytes = bytes;
                    req.callback = std::move(callback);
                    m_pendingRequests.push_back(std::move(req));
                    scheduleTimerLocked();
                    return;
                }
            }
        }

        if (invokeNow) {
            callback(bytes);
        }
    }

    // --- Cancellation and Cleanup ---

    void cancelTask(quint64 taskId)
    {
        QMutexLocker locker(&m_mutex);
        m_pendingRequests.removeIf([taskId](const PendingRequest &req) {
            return req.taskId == taskId;
        });
        m_taskTransferredBytes.remove(taskId);
        m_waitCondition.wakeAll();
        scheduleTimerLocked();
    }

    void cancelAll()
    {
        QMutexLocker locker(&m_mutex);
        m_pendingRequests.clear();
        m_waitCondition.wakeAll();
        if (m_timer && m_timer->isActive()) {
            m_timer->stop();
        }
    }

    // Force replenishment evaluation (useful for tests with virtual clocks)
    void processPending()
    {
        QList<std::pair<TokenGrantCallback, qint64>> toInvoke;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_enabled || m_bytesPerSec == 0) {
                for (auto &req : m_pendingRequests) {
                    if (req.callback) {
                        recordTransferredLocked(req.taskId, req.requestedBytes);
                        toInvoke.push_back({std::move(req.callback), req.requestedBytes});
                    }
                }
                m_pendingRequests.clear();
            } else {
                const qint64 now = nowMs();
                refillLocked(now);

                while (!m_pendingRequests.isEmpty()) {
                    auto &front = m_pendingRequests.front();
                    if (m_tokens >= static_cast<double>(front.requestedBytes)) {
                        m_tokens -= static_cast<double>(front.requestedBytes);
                        recordTransferredLocked(front.taskId, front.requestedBytes);
                        toInvoke.push_back({std::move(front.callback), front.requestedBytes});
                        m_pendingRequests.pop_front();
                    } else {
                        break;
                    }
                }
            }
            m_waitCondition.wakeAll();
            scheduleTimerLocked();
        }

        for (const auto &item : toInvoke) {
            item.first(item.second);
        }
    }

    // --- Statistics ---

    quint64 totalBytesTransferred() const
    {
        QMutexLocker locker(&m_mutex);
        return m_totalBytesTransferred;
    }

    quint64 bytesTransferredForTask(quint64 taskId) const
    {
        QMutexLocker locker(&m_mutex);
        return m_taskTransferredBytes.value(taskId, 0ULL);
    }

    int pendingRequestsCount() const
    {
        QMutexLocker locker(&m_mutex);
        return static_cast<int>(m_pendingRequests.size());
    }

    double availableTokens() const
    {
        QMutexLocker locker(&m_mutex);
        return m_tokens;
    }

    void resetStats()
    {
        QMutexLocker locker(&m_mutex);
        m_totalBytesTransferred = 0;
        m_taskTransferredBytes.clear();
    }

Q_SIGNALS:
    void limitChanged(bool enabled, quint64 bytesPerSec);

private:
    qint64 nowMs() const
    {
        return m_clock ? m_clock() : 0;
    }

    void refillLocked(qint64 now)
    {
        if (!m_enabled || m_bytesPerSec == 0) return;
        if (m_lastRefillMs == 0) {
            m_lastRefillMs = now;
            return;
        }

        const qint64 elapsedMs = now - m_lastRefillMs;
        if (elapsedMs <= 0) return;

        const double addedTokens = (static_cast<double>(elapsedMs) * static_cast<double>(m_bytesPerSec)) / 1000.0;
        m_tokens = std::min(static_cast<double>(m_maxBurst), m_tokens + addedTokens);
        m_lastRefillMs = now;
    }

    void scheduleTimerLocked()
    {
        if (!m_enabled || m_bytesPerSec == 0 || m_pendingRequests.isEmpty()) {
            if (m_timer && m_timer->isActive()) {
                m_timer->stop();
            }
            return;
        }

        const auto &front = m_pendingRequests.front();
        const double needed = static_cast<double>(front.requestedBytes) - m_tokens;
        int delayMs = 10;
        if (needed > 0 && m_bytesPerSec > 0) {
            delayMs = static_cast<int>(std::ceil((needed * 1000.0) / static_cast<double>(m_bytesPerSec)));
        }
        delayMs = std::clamp(delayMs, 5, 500);

        if (m_timer) {
            m_timer->start(delayMs);
        }
    }

    void onTimerFired()
    {
        processPending();
    }

    void recordTransferredLocked(quint64 taskId, qint64 bytes)
    {
        if (bytes <= 0) return;
        m_totalBytesTransferred += static_cast<quint64>(bytes);
        if (taskId != 0) {
            m_taskTransferredBytes[taskId] += static_cast<quint64>(bytes);
        }
    }

    mutable QMutex m_mutex;
    QWaitCondition m_waitCondition;
    ClockFn m_clock;

    bool m_enabled = false;
    quint64 m_bytesPerSec = 0;
    qint64 m_maxBurst = MaxBurstBytes;
    double m_tokens = 0.0;
    qint64 m_lastRefillMs = 0;

    QList<PendingRequest> m_pendingRequests;
    QTimer *m_timer = nullptr;

    quint64 m_totalBytesTransferred = 0;
    QHash<quint64, quint64> m_taskTransferredBytes;
};

Q_DECLARE_METATYPE(std::shared_ptr<TransferBandwidthLimiter>)
