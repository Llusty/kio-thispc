/*
 * KDE compatibility adapter bridging KJob / KIO::Job to PortableTransferJob
 * (0.46.0 Stage 2A.2-1 / R1).
 *
 * Lives strictly in the KDE compatibility layer. NEVER add this file to
 * THISPC_PORTABLE_CORE_SOURCES or expose KJob/KIO types in portable headers.
 *
 * Ownership & lifetime contract (Exclusive Ownership + Retainer Teardown):
 *  - KJobTransferAdapter::create() validates that the wrapped KJob has no
 *    external QObject parent, lives on the current thread, has not already
 *    been wrapped, and has not already failed before handoff.
 *  - On acceptance, the adapter disables KJob auto-deletion
 *    (wrappedJob->setAutoDelete(false)) and becomes the sole owner of the
 *    wrapped job.
 *  - Normal teardown (m_wrappedFinished && m_inWrappedCallbackDepth == 0):
 *    ~KJobTransferAdapter() disconnects observers, synchronously deletes the
 *    already-finished wrapped KJob (non-blocking), and marks cleanupHandle()
 *    complete BEFORE ~QObject() emits destroyed(adapter).
 *  - Detached teardown (m_inWrappedCallbackDepth > 0 OR active wrapped job
 *    !m_wrappedFinished && m_wrappedStarted):
 *    ~KJobTransferAdapter() NEVER blocks the GUI thread with synchronous
 *    worker-thread deletion and NEVER deletes a KJob on its own active stack
 *    frame. Instead, it transfers exclusive ownership of the wrapped KJob and
 *    the PortableTransferCleanupHandle to an internal retainer that dispatches
 *    cooperative cancellation (if active), schedules rawJob->deleteLater()
 *    once the KJob finishes, and marks cleanupHandle() complete ONLY after
 *    QObject::destroyed(rawJob) has actually fired.
 *  - Future Stage 2A.2-2 queue integration MUST track job->cleanupHandle()
 *    (isCleanupComplete() / cleanupCompleted()) in addition to adapter
 *    destruction so shutdown never completes while a detached KJob is alive.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "portabletransferjob.h"

#include <KJob>

#include <QPair>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

enum class KJobStartMode {
    ControllerStarts,
    FactoryStarts,
};

struct KJobTransferAdapterOptions {
    KJobStartMode startMode = KJobStartMode::ControllerStarts;

    // Exact pause/resume is opt-in and verified against concrete job behavior.
    // Upstream KIO jobs (CopyJob, FileCopyJob, SimpleJob, DeleteJob) and
    // KioStagedTransferJob are NEVER marked Suspendable even if true.
    bool allowExactPause = false;

    // Optional asynchronous cancellation callback (e.g. LocalMergeQueueJob).
    std::function<bool(KJob *)> cancelHandler;
};

class KJobTransferAdapter : public PortableTransferJob
{
    Q_OBJECT

public:
    // Validates ownership preconditions and returns a new adapter on success,
    // or nullptr with a diagnostic in rejectReason on failure.
    static KJobTransferAdapter *create(KJob *wrappedJob,
                                       const KJobTransferAdapterOptions &options = {},
                                       QObject *parent = nullptr,
                                       QString *rejectReason = nullptr);

    ~KJobTransferAdapter() override;

    KJob *wrappedJob() const { return m_job.data(); }
    KJobStartMode startMode() const { return m_options.startMode; }

    // Diagnostic/shutdown counter of detached KJob retainers whose wrapped
    // KJob has not yet been destroyed.
    static int pendingDetachedCleanupCount();

    QString lastDescriptionTitle() const { return m_lastDescriptionTitle; }
    QPair<QString, QString> lastDescriptionField1() const { return m_lastField1; }
    QPair<QString, QString> lastDescriptionField2() const { return m_lastField2; }

    void setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter,
                             quint64 taskId = 0) override;

    static TransferErrorCode mapKJobError(int kjobError);
    static TransferOutcomeFlags extractOutcomeFlags(const KJob *job);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    void doStart() override;
    bool doCancel() override;
    bool doSuspend() override;
    bool doResume() override;
    bool hasActiveBackendBeforeStart() const override;

private:
    KJobTransferAdapter(KJob *wrappedJob,
                        const KJobTransferAdapterOptions &options,
                        TransferJobCapabilities resolvedCapabilities,
                        QObject *parent);

    static bool isUnsafeKioPauseJob(const KJob *job);
    static bool supportsBandwidthLimiter(const KJob *job);
    static TransferJobCapabilities resolveCapabilities(const KJob *job,
                                                       const KJobTransferAdapterOptions &options);

    void connectWrappedJob();
    void syncDecisionPhaseFromJob();
    void syncCurrentItemFromJob();
    void deliverTerminalOutcome(int kjobError,
                                const QString &kjobErrorText,
                                const TransferOutcomeFlags &flags);

    void onWrappedResult(KJob *job);
    void onWrappedDestroyed();

    QPointer<KJob> m_job;
    KJobTransferAdapterOptions m_options;
    std::shared_ptr<TransferBandwidthLimiter> m_bandwidthLimiter;

    bool m_wrappedStarted = false;
    bool m_wrappedFinished = false;
    bool m_cancelDispatched = false;
    bool m_inAdapterSuspendResume = false;
    int m_inWrappedCallbackDepth = 0;

    bool m_hasBufferedResult = false;
    int m_bufferedError = 0;
    QString m_bufferedErrorText;
    TransferOutcomeFlags m_bufferedOutcomeFlags;

    QString m_lastDescriptionTitle;
    QPair<QString, QString> m_lastField1;
    QPair<QString, QString> m_lastField2;
};
