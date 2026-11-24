/*
 * Portable Qt6 Core-only transfer job contract (0.46.0 Stage 2A.1).
 *
 * Replaces mandatory KJob / KIO::Job inheritance for the portable core layer.
 * Strictly depends only on Qt6::Core and TransferBandwidthLimiter.
 *
 * Ownership & lifecycle contract:
 *  - Before completion: owned by its QObject parent (if any) or by the caller
 *    observing it via QPointer<PortableTransferJob>.
 *  - No hidden QEventLoopLocker: process shutdown lifetime is managed solely
 *    by ShutdownCoordinator (Stage 1A).
 *  - Cancellation is cooperative and asynchronous: requestCancel() transitions
 *    an active job to TransferJobState::Cancelling and delegates to doCancel().
 *    It NEVER emits finished() before the subclass has completed cleanup and
 *    unwound any worker thread.
 *  - Terminal state is emitted at most once via finishCompleted(),
 *    finishCancelled(), or finishFailed(). Duplicate calls are ignored.
 *  - Deferred destruction: when autoDelete() is true (default), deleteLater()
 *    is scheduled AFTER finished(this) returns so signal receivers may safely
 *    inspect errorCode(), errorText(), and outcomeFlags() without dangling
 *    pointers or immediate self-deletion inside callbacks.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <QFlags>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class TransferBandwidthLimiter;

// Lightweight Qt6 Core-only handle tracking actual backend/worker cleanup
// completion independently of PortableTransferJob destruction (Stage 2A.2-1-R1).
class PortableTransferCleanupHandle final : public QObject
{
    Q_OBJECT

public:
    explicit PortableTransferCleanupHandle(QObject *parent = nullptr);
    ~PortableTransferCleanupHandle() override;

    bool isCleanupComplete() const { return m_complete; }
    void markCleanupComplete();

Q_SIGNALS:
    void cleanupCompleted();

private:
    bool m_complete = false;
};

enum class TransferJobCapability : unsigned int {
    None             = 0u,
    Killable         = 1u << 0,
    Suspendable      = 1u << 1,
    AsyncCancel      = 1u << 2,
    BandwidthLimited = 1u << 3,
};
Q_DECLARE_FLAGS(TransferJobCapabilities, TransferJobCapability)
Q_DECLARE_OPERATORS_FOR_FLAGS(TransferJobCapabilities)

enum class TransferErrorCode {
    NoError = 0,
    Cancelled,
    DoesNotExist,
    AlreadyExists,
    CannotRead,
    CannotWrite,
    CannotOpenForReading,
    CannotOpenForWriting,
    CannotMkdir,
    CannotRename,
    CannotChmod,
    DiskFull,
    IsDirectory,
    UnsupportedAction,
    UnsupportedProtocol,
    BackendError,
    InternalError,
};

enum class TransferJobState {
    NotStarted,
    Running,
    Paused,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

inline bool isTerminalTransferJobState(TransferJobState state)
{
    return state == TransferJobState::Completed
        || state == TransferJobState::Cancelled
        || state == TransferJobState::Failed;
}

struct TransferOutcomeFlags {
    bool destinationPublished = false;
    bool destinationReplaced = false;
    bool directoryDurabilityConfirmed = false;
    bool partialMoveOutcome = false;
    int batchPublishedCount = 0;
    bool batchPartiallyCompleted = false;
    bool mergeRetryBlocked = false;
    bool mergeRecoveryRequired = false;
    QString mergeState;
    int mergeSkippedCount = 0;
    int mergeReplacedCount = 0;
    QString mergeHistoryNote;
    int systemError = 0;
    int backendErrorCode = 0;

    bool hasPartialPublication() const
    {
        return partialMoveOutcome
            || batchPublishedCount > 0
            || batchPartiallyCompleted
            || mergeReplacedCount > 0;
    }

    bool blocksSafeRetry() const
    {
        return hasPartialPublication()
            || mergeRetryBlocked
            || mergeRecoveryRequired
            || destinationPublished
            || destinationReplaced;
    }
};

class PortableTransferJob : public QObject
{
    Q_OBJECT

public:
    explicit PortableTransferJob(QObject *parent = nullptr);
    ~PortableTransferJob() override;

    TransferJobState state() const { return m_state; }
    bool isTerminal() const { return isTerminalTransferJobState(m_state); }
    bool isStarted() const { return m_state != TransferJobState::NotStarted; }
    bool isCancelRequested() const { return m_cancelRequested; }
    bool isSuspended() const { return m_state == TransferJobState::Paused; }
    bool isInDecisionPhase() const { return m_inDecisionPhase; }

    TransferJobCapabilities capabilities() const { return m_capabilities; }
    bool hasCapability(TransferJobCapability capability) const
    {
        return m_capabilities.testFlag(capability);
    }

    TransferErrorCode errorCode() const { return m_errorCode; }
    QString errorText() const { return m_errorText; }
    const TransferOutcomeFlags &outcomeFlags() const { return m_outcomeFlags; }

    qulonglong processedBytes() const { return m_processedBytes; }
    qulonglong totalBytes() const { return m_totalBytes; }
    qulonglong processedFiles() const { return m_processedFiles; }
    qulonglong totalFiles() const { return m_totalFiles; }
    unsigned long percent() const { return m_percent; }
    unsigned long speedBytesPerSecond() const { return m_speedBytesPerSecond; }

    QUrl currentSourceUrl() const { return m_currentSourceUrl; }
    QUrl currentDestinationUrl() const { return m_currentDestinationUrl; }

    bool autoDelete() const { return m_autoDelete; }
    void setAutoDelete(bool enabled) { m_autoDelete = enabled; }

    std::shared_ptr<PortableTransferCleanupHandle> cleanupHandle() const { return m_cleanupHandle; }
    bool isBackendCleanupComplete() const
    {
        return m_cleanupHandle ? m_cleanupHandle->isCleanupComplete() : true;
    }

    void start();
    bool requestCancel();
    bool suspend();
    bool resume();

    virtual void setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter,
                                     quint64 taskId = 0);

Q_SIGNALS:
    void stateChanged(PortableTransferJob *job, TransferJobState state);
    void finished(PortableTransferJob *job);
    void suspendedChanged(PortableTransferJob *job, bool suspended);
    void decisionPhaseChanged(PortableTransferJob *job, bool inDecisionPhase);
    void percentChanged(PortableTransferJob *job, unsigned long percent);
    void processedBytesChanged(PortableTransferJob *job, qulonglong bytes);
    void totalBytesChanged(PortableTransferJob *job, qulonglong bytes);
    void processedFilesChanged(PortableTransferJob *job, qulonglong files);
    void totalFilesChanged(PortableTransferJob *job, qulonglong files);
    void speedChanged(PortableTransferJob *job, unsigned long bytesPerSecond);
    void currentItemChanged(PortableTransferJob *job,
                            const QUrl &source,
                            const QUrl &destination);

protected:
    virtual void doStart() = 0;
    virtual bool doCancel();
    virtual bool doSuspend();
    virtual bool doResume();
    virtual bool hasActiveBackendBeforeStart() const;

    void setCapabilities(TransferJobCapabilities capabilities);
    void setTotalBytes(qulonglong bytes);
    void setProcessedBytes(qulonglong bytes);
    void setTotalFiles(qulonglong files);
    void setProcessedFiles(qulonglong files);
    void setPercent(unsigned long percent);
    void emitSpeed(unsigned long bytesPerSecond);
    void setCurrentItem(const QUrl &source, const QUrl &destination);
    void setInDecisionPhase(bool inDecisionPhase);
    void syncSuspendedState(bool suspended);
    void setCleanupDetached(bool detached) { m_cleanupDetached = detached; }
    void markBackendCleanupComplete();

    void setOutcomeFlags(const TransferOutcomeFlags &flags);

    void finishCompleted();
    void finishCancelled(const QString &errorText = QString());
    void finishFailed(TransferErrorCode code, const QString &errorText);

private:
    void transitionState(TransferJobState nextState);
    void finalizeTerminal(TransferJobState terminalState,
                          TransferErrorCode code,
                          const QString &errorText);
    void recomputePercentFromBytes();

    TransferJobState m_state = TransferJobState::NotStarted;
    TransferJobCapabilities m_capabilities = TransferJobCapability::None;
    TransferErrorCode m_errorCode = TransferErrorCode::NoError;
    QString m_errorText;
    TransferOutcomeFlags m_outcomeFlags;

    qulonglong m_processedBytes = 0;
    qulonglong m_totalBytes = 0;
    qulonglong m_processedFiles = 0;
    qulonglong m_totalFiles = 0;
    unsigned long m_percent = 0;
    unsigned long m_speedBytesPerSecond = 0;

    QUrl m_currentSourceUrl;
    QUrl m_currentDestinationUrl;

    bool m_cancelRequested = false;
    bool m_inDecisionPhase = false;
    bool m_autoDelete = true;
    bool m_finalizing = false;
    bool m_deleteScheduled = false;
    bool m_cleanupDetached = false;
    std::shared_ptr<PortableTransferCleanupHandle> m_cleanupHandle;
};

Q_DECLARE_METATYPE(TransferJobCapability)
Q_DECLARE_METATYPE(TransferJobCapabilities)
Q_DECLARE_METATYPE(TransferErrorCode)
Q_DECLARE_METATYPE(TransferJobState)
Q_DECLARE_METATYPE(TransferOutcomeFlags)
