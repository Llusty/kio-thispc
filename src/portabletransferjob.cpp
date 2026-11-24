/*
 * Portable Qt6 Core-only transfer job contract implementation (0.46.0 Stage 2A.1).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "portabletransferjob.h"

#include <QPointer>

PortableTransferCleanupHandle::PortableTransferCleanupHandle(QObject *parent)
    : QObject(parent)
{
}

PortableTransferCleanupHandle::~PortableTransferCleanupHandle() = default;

void PortableTransferCleanupHandle::markCleanupComplete()
{
    if (m_complete) {
        return;
    }
    m_complete = true;
    Q_EMIT cleanupCompleted();
}

PortableTransferJob::PortableTransferJob(QObject *parent)
    : QObject(parent)
    , m_cleanupHandle(std::make_shared<PortableTransferCleanupHandle>())
{
}

PortableTransferJob::~PortableTransferJob()
{
    if (!m_cleanupDetached && m_cleanupHandle) {
        m_cleanupHandle->markCleanupComplete();
    }
}

void PortableTransferJob::start()
{
    if (m_state != TransferJobState::NotStarted) {
        return;
    }
    QPointer<PortableTransferJob> self(this);
    transitionState(TransferJobState::Running);
    if (!self || self->m_state != TransferJobState::Running) {
        return;
    }
    doStart();
}

bool PortableTransferJob::requestCancel()
{
    if (isTerminal()) {
        return false;
    }
    if (!hasCapability(TransferJobCapability::Killable)) {
        return false;
    }
    if (m_state == TransferJobState::Cancelling) {
        return true;
    }
    if (m_state == TransferJobState::NotStarted && !hasActiveBackendBeforeStart()) {
        m_cancelRequested = true;
        finishCancelled();
        return true;
    }

    const TransferJobState previousState = m_state;
    m_cancelRequested = true;
    QPointer<PortableTransferJob> self(this);
    transitionState(TransferJobState::Cancelling);
    if (!self || self->m_state != TransferJobState::Cancelling) {
        return true;
    }

    const bool accepted = doCancel();
    if (!self || self->isTerminal()) {
        return true;
    }
    if (!accepted) {
        if (self->m_state == TransferJobState::Cancelling) {
            m_cancelRequested = false;
            transitionState(previousState);
        }
        return false;
    }
    return true;
}

bool PortableTransferJob::suspend()
{
    if (m_state != TransferJobState::Running) {
        return false;
    }
    if (!hasCapability(TransferJobCapability::Suspendable)) {
        return false;
    }
    QPointer<PortableTransferJob> self(this);
    if (!doSuspend()) {
        return false;
    }
    if (!self || self->m_state != TransferJobState::Running) {
        return false;
    }
    transitionState(TransferJobState::Paused);
    if (!self || self->m_state != TransferJobState::Paused) {
        return false;
    }
    Q_EMIT suspendedChanged(this, true);
    return true;
}

bool PortableTransferJob::resume()
{
    if (m_state != TransferJobState::Paused) {
        return false;
    }
    if (!hasCapability(TransferJobCapability::Suspendable)) {
        return false;
    }
    QPointer<PortableTransferJob> self(this);
    if (!doResume()) {
        return false;
    }
    if (!self || self->m_state != TransferJobState::Paused) {
        return false;
    }
    transitionState(TransferJobState::Running);
    if (!self || self->m_state != TransferJobState::Running) {
        return false;
    }
    Q_EMIT suspendedChanged(this, false);
    return true;
}

void PortableTransferJob::setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter,
                                              quint64 taskId)
{
    Q_UNUSED(limiter);
    Q_UNUSED(taskId);
}

bool PortableTransferJob::doCancel()
{
    return false;
}

bool PortableTransferJob::doSuspend()
{
    return false;
}

bool PortableTransferJob::doResume()
{
    return false;
}

bool PortableTransferJob::hasActiveBackendBeforeStart() const
{
    return false;
}

void PortableTransferJob::syncSuspendedState(bool suspended)
{
    if (isTerminal()) {
        return;
    }
    if (suspended && m_state == TransferJobState::Running) {
        QPointer<PortableTransferJob> self(this);
        transitionState(TransferJobState::Paused);
        if (self && self->m_state == TransferJobState::Paused) {
            Q_EMIT suspendedChanged(this, true);
        }
    } else if (!suspended && m_state == TransferJobState::Paused) {
        QPointer<PortableTransferJob> self(this);
        transitionState(TransferJobState::Running);
        if (self && self->m_state == TransferJobState::Running) {
            Q_EMIT suspendedChanged(this, false);
        }
    }
}

void PortableTransferJob::markBackendCleanupComplete()
{
    if (m_cleanupHandle) {
        m_cleanupHandle->markCleanupComplete();
    }
}

void PortableTransferJob::setCapabilities(TransferJobCapabilities capabilities)
{
    if (isStarted() || isTerminal()) {
        return;
    }
    m_capabilities = capabilities;
}

void PortableTransferJob::setTotalBytes(qulonglong bytes)
{
    if (isTerminal() || m_totalBytes == bytes) {
        return;
    }
    m_totalBytes = bytes;
    QPointer<PortableTransferJob> self(this);
    Q_EMIT totalBytesChanged(this, m_totalBytes);
    if (!self || self->isTerminal()) {
        return;
    }
    recomputePercentFromBytes();
}

void PortableTransferJob::setProcessedBytes(qulonglong bytes)
{
    if (isTerminal() || m_processedBytes == bytes) {
        return;
    }
    m_processedBytes = bytes;
    QPointer<PortableTransferJob> self(this);
    Q_EMIT processedBytesChanged(this, m_processedBytes);
    if (!self || self->isTerminal()) {
        return;
    }
    recomputePercentFromBytes();
}

void PortableTransferJob::setTotalFiles(qulonglong files)
{
    if (isTerminal() || m_totalFiles == files) {
        return;
    }
    m_totalFiles = files;
    Q_EMIT totalFilesChanged(this, m_totalFiles);
}

void PortableTransferJob::setProcessedFiles(qulonglong files)
{
    if (isTerminal() || m_processedFiles == files) {
        return;
    }
    m_processedFiles = files;
    Q_EMIT processedFilesChanged(this, m_processedFiles);
}

void PortableTransferJob::setPercent(unsigned long percent)
{
    const unsigned long clamped = percent > 100UL ? 100UL : percent;
    if (isTerminal() || m_percent == clamped) {
        return;
    }
    m_percent = clamped;
    Q_EMIT percentChanged(this, m_percent);
}

void PortableTransferJob::emitSpeed(unsigned long bytesPerSecond)
{
    if (isTerminal()) {
        return;
    }
    m_speedBytesPerSecond = bytesPerSecond;
    Q_EMIT speedChanged(this, m_speedBytesPerSecond);
}

void PortableTransferJob::setCurrentItem(const QUrl &source, const QUrl &destination)
{
    if (isTerminal()) {
        return;
    }
    if (m_currentSourceUrl == source && m_currentDestinationUrl == destination) {
        return;
    }
    m_currentSourceUrl = source;
    m_currentDestinationUrl = destination;
    Q_EMIT currentItemChanged(this, m_currentSourceUrl, m_currentDestinationUrl);
}

void PortableTransferJob::setInDecisionPhase(bool inDecisionPhase)
{
    if (isTerminal() || m_inDecisionPhase == inDecisionPhase) {
        return;
    }
    m_inDecisionPhase = inDecisionPhase;
    Q_EMIT decisionPhaseChanged(this, m_inDecisionPhase);
}

void PortableTransferJob::setOutcomeFlags(const TransferOutcomeFlags &flags)
{
    if (isTerminal()) {
        return;
    }
    m_outcomeFlags = flags;
}

void PortableTransferJob::finishCompleted()
{
    finalizeTerminal(TransferJobState::Completed, TransferErrorCode::NoError, QString());
}

void PortableTransferJob::finishCancelled(const QString &errorText)
{
    m_cancelRequested = true;
    finalizeTerminal(TransferJobState::Cancelled, TransferErrorCode::Cancelled, errorText);
}

void PortableTransferJob::finishFailed(TransferErrorCode code, const QString &errorText)
{
    const TransferErrorCode effectiveCode = (code == TransferErrorCode::NoError)
        ? TransferErrorCode::InternalError
        : code;
    finalizeTerminal(TransferJobState::Failed, effectiveCode, errorText);
}

void PortableTransferJob::transitionState(TransferJobState nextState)
{
    if (m_state == nextState) {
        return;
    }
    m_state = nextState;
    Q_EMIT stateChanged(this, m_state);
}

void PortableTransferJob::finalizeTerminal(TransferJobState terminalState,
                                           TransferErrorCode code,
                                           const QString &errorText)
{
    if (isTerminal() || m_finalizing) {
        return;
    }
    m_finalizing = true;
    QPointer<PortableTransferJob> self(this);

    m_errorCode = code;
    m_errorText = errorText;
    m_state = terminalState;

    if (m_inDecisionPhase) {
        m_inDecisionPhase = false;
        Q_EMIT decisionPhaseChanged(this, false);
        if (!self) {
            return;
        }
    }

    Q_EMIT stateChanged(this, m_state);
    if (!self) {
        return;
    }

    Q_EMIT finished(this);
    if (self && self->m_autoDelete && !self->m_deleteScheduled) {
        self->m_deleteScheduled = true;
        self->deleteLater();
    }
}

void PortableTransferJob::recomputePercentFromBytes()
{
    if (m_totalBytes == 0) {
        return;
    }
    const qulonglong clampedProcessed = (m_processedBytes > m_totalBytes)
        ? m_totalBytes
        : m_processedBytes;
    const unsigned long pct = static_cast<unsigned long>(
        (clampedProcessed * 100ULL) / m_totalBytes);
    setPercent(pct);
}
