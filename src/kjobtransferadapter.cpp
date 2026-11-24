/*
 * KDE compatibility adapter bridging KJob / KIO::Job to PortableTransferJob
 * (0.46.0 Stage 2A.2-1 / R1).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "kjobtransferadapter.h"

#include "localfilecopyjob.h"

#include <KIO/CopyJob>
#include <KIO/DeleteJob>
#include <KIO/FileCopyJob>
#include <KIO/Global>
#include <KIO/SimpleJob>

#include <QByteArray>
#include <QCoreApplication>
#include <QDynamicPropertyChangeEvent>
#include <QEvent>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <QThread>
#include <QVariant>

#include <atomic>
#include <cstring>
#include <limits>

namespace {

constexpr const char *kWrappedMarkerProperty = "thispcKJobAdapterWrapped";

std::atomic<int> g_pendingDetachedCleanups{0};

bool isSupportedTaskIdType(const QByteArray &typeName)
{
    return typeName == QByteArrayLiteral("quint64")
        || typeName == QByteArrayLiteral("qulonglong")
        || typeName == QByteArrayLiteral("unsigned long long");
}

bool hasDynamicProperty(const QObject *obj, const char *name)
{
    if (!obj || !name) {
        return false;
    }
    const QList<QByteArray> names = obj->dynamicPropertyNames();
    return names.contains(QByteArray(name));
}

// Reads a safety-critical boolean dynamic property with fail-closed semantics:
//  - Absent property -> false (no fault).
//  - Present valid bool -> exact boolean value (no fault).
//  - Present with non-bool or invalid QVariant -> sets fault = true.
bool readSafetyBoolProperty(const QObject *obj, const char *name, bool &fault)
{
    if (!hasDynamicProperty(obj, name)) {
        return false;
    }
    const QVariant value = obj->property(name);
    if (!value.isValid() || value.metaType().id() != QMetaType::Bool) {
        fault = true;
        return false;
    }
    return value.toBool();
}

// Reads a safety-critical non-negative integer dynamic property with fail-closed semantics:
//  - Absent property -> 0 (no fault).
//  - Present valid integer >= 0 -> exact int value (no fault).
//  - Present with non-integer, negative, or invalid QVariant -> sets fault = true.
int readSafetyNonNegativeIntProperty(const QObject *obj, const char *name, bool &fault)
{
    if (!hasDynamicProperty(obj, name)) {
        return 0;
    }
    const QVariant value = obj->property(name);
    const int tid = value.metaType().id();
    const bool isIntegerType = (tid == QMetaType::Int
                                || tid == QMetaType::UInt
                                || tid == QMetaType::LongLong
                                || tid == QMetaType::ULongLong
                                || tid == QMetaType::Short
                                || tid == QMetaType::UShort);
    if (!value.isValid() || !isIntegerType) {
        fault = true;
        return 0;
    }
    bool ok = false;
    const qlonglong parsed = value.toLongLong(&ok);
    if (!ok || parsed < 0 || parsed > static_cast<qlonglong>(std::numeric_limits<int>::max())) {
        fault = true;
        return 0;
    }
    return static_cast<int>(parsed);
}

int readOptionalIntProperty(const QObject *obj, const char *name)
{
    if (!hasDynamicProperty(obj, name)) {
        return 0;
    }
    const QVariant value = obj->property(name);
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok ? parsed : 0;
}

QString readOptionalStringProperty(const QObject *obj, const char *name, bool &fault, bool safetyCritical)
{
    if (!hasDynamicProperty(obj, name)) {
        return {};
    }
    const QVariant value = obj->property(name);
    if (!value.isValid() || value.metaType().id() != QMetaType::QString) {
        if (safetyCritical) {
            fault = true;
        }
        return {};
    }
    return value.toString();
}

// Retains exclusive ownership of a wrapped KJob when KJobTransferAdapter is
// destroyed during a wrapped-job callback or while the backend is still active.
// Guarantees:
//  - No GUI-thread blocking on active worker teardown.
//  - No UAF on an active KJob call stack (uses deleteLater after unwind).
//  - cleanupHandle->markCleanupComplete() fires ONLY after QObject::destroyed(rawJob).
//  - On QCoreApplication::aboutToQuit, performs final teardown so no worker
//    thread or KJob is abandoned when the event loop exits.
class KJobCleanupRetainer final : public QObject
{
public:
    KJobCleanupRetainer(KJob *rawJob,
                        bool alreadyFinished,
                        bool alreadyStarted,
                        bool cancelAlreadyDispatched,
                        std::function<bool(KJob *)> cancelHandler,
                        std::shared_ptr<TransferBandwidthLimiter> limiter,
                        std::shared_ptr<PortableTransferCleanupHandle> cleanupHandle)
        : QObject(retainerParent())
        , m_job(rawJob)
        , m_cancelHandler(std::move(cancelHandler))
        , m_limiter(std::move(limiter))
        , m_cleanupHandle(std::move(cleanupHandle))
        , m_jobFinished(alreadyFinished || !alreadyStarted)
        , m_cancelDispatched(cancelAlreadyDispatched)
    {
        g_pendingDetachedCleanups.fetch_add(1, std::memory_order_acq_rel);

        if (!m_job) {
            completeAndDispose();
            return;
        }

        m_job->setAutoDelete(false);

        connect(m_job.data(), &QObject::destroyed, this, [this]() {
            m_job.clear();
            completeAndDispose();
        });

        connect(m_job.data(), &KJob::result, this, [this](KJob *) {
            m_jobFinished = true;
            if (m_job && !m_deleteScheduled) {
                m_deleteScheduled = true;
                m_job->deleteLater();
            }
        });

        if (QCoreApplication *app = QCoreApplication::instance()) {
            connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
                if (!m_job) {
                    return;
                }
                if (m_jobFinished) {
                    if (!m_deleteScheduled) {
                        m_deleteScheduled = true;
                        m_job->deleteLater();
                    }
                    return;
                }
                // AUD-2A2-R1-02: Never forcibly delete an active, uncooperative
                // backend synchronously inside aboutToQuit (avoids blocking the
                // GUI). Dispatch cooperative cancellation once if not yet sent.
                dispatchCancelOnce();
            });
        }

        if (m_jobFinished) {
            // Safe to schedule deferred deletion immediately; active stack frames
            // unwind first, then rawJob is deleted and destroyed() completes cleanup.
            m_deleteScheduled = true;
            m_job->deleteLater();
            return;
        }

        // AUD-2A2-R1-01: Backend is still active. Dispatch cooperative
        // cancellation once, and NEVER call m_job->deleteLater() if cancel is
        // unsupported or refused — wait for KJob::result instead so an active
        // backend is never destroyed mid-operation or blocks the GUI thread.
        dispatchCancelOnce();
    }

    ~KJobCleanupRetainer() override
    {
        if (m_job) {
            KJob *jobToDelete = m_job.data();
            m_job.clear();
            disconnect(jobToDelete, nullptr, this, nullptr);
            if (!m_jobFinished && jobToDelete->capabilities().testFlag(KJob::Killable)) {
                jobToDelete->kill(KJob::Quietly);
            }
            delete jobToDelete;
        }
        if (!m_completed) {
            completeAccounting();
        }
    }

private:
    static QObject *retainerParent()
    {
        QCoreApplication *app = QCoreApplication::instance();
        if (app && app->thread() == QThread::currentThread()) {
            return app;
        }
        return nullptr;
    }

    void dispatchCancelOnce()
    {
        if (!m_job || m_jobFinished || m_cancelDispatched) {
            return;
        }
        m_cancelDispatched = true;
        if (m_cancelHandler) {
            m_cancelHandler(m_job.data());
        } else if (auto *copyJob = qobject_cast<LocalFileCopyJob *>(m_job.data())) {
            copyJob->requestCancelAsync();
        } else if (m_job->capabilities().testFlag(KJob::Killable)) {
            m_job->kill(KJob::EmitResult);
        }
    }

    void completeAccounting()
    {
        if (m_completed) {
            return;
        }
        m_completed = true;
        g_pendingDetachedCleanups.fetch_sub(1, std::memory_order_acq_rel);
        if (m_cleanupHandle) {
            m_cleanupHandle->markCleanupComplete();
        }
    }

    void completeAndDispose()
    {
        completeAccounting();
        deleteLater();
    }

    QPointer<KJob> m_job;
    std::function<bool(KJob *)> m_cancelHandler;
    std::shared_ptr<TransferBandwidthLimiter> m_limiter;
    std::shared_ptr<PortableTransferCleanupHandle> m_cleanupHandle;
    bool m_jobFinished = false;
    bool m_cancelDispatched = false;
    bool m_deleteScheduled = false;
    bool m_completed = false;
};

} // namespace

int KJobTransferAdapter::pendingDetachedCleanupCount()
{
    return g_pendingDetachedCleanups.load(std::memory_order_acquire);
}

bool KJobTransferAdapter::isUnsafeKioPauseJob(const KJob *job)
{
    if (!job) {
        return true;
    }
    if (qobject_cast<const KIO::CopyJob *>(job) != nullptr
        || qobject_cast<const KIO::FileCopyJob *>(job) != nullptr
        || qobject_cast<const KIO::SimpleJob *>(job) != nullptr
        || qobject_cast<const KIO::DeleteJob *>(job) != nullptr) {
        return true;
    }
    if (job->property("thispcStagedTransfer").toBool()) {
        return true;
    }
    if (const QMetaObject *mo = job->metaObject()) {
        if (std::strcmp(mo->className(), "KioStagedTransferJob") == 0) {
            return true;
        }
    }
    return false;
}

KJobTransferAdapter *KJobTransferAdapter::create(KJob *wrappedJob,
                                                 const KJobTransferAdapterOptions &options,
                                                 QObject *parent,
                                                 QString *rejectReason)
{
    auto reject = [&](const QString &reason) -> KJobTransferAdapter * {
        if (rejectReason) {
            *rejectReason = reason;
        }
        return nullptr;
    };

    if (!wrappedJob) {
        return reject(QStringLiteral("Wrapped KJob pointer is null"));
    }

    QThread *currentThread = QThread::currentThread();
    if (wrappedJob->thread() != currentThread) {
        return reject(QStringLiteral("Wrapped KJob belongs to a different thread; exclusive ownership rejected"));
    }
    if (parent && parent->thread() != currentThread) {
        return reject(QStringLiteral("Adapter parent belongs to a different thread"));
    }
    if (wrappedJob->parent() != nullptr) {
        return reject(QStringLiteral("Wrapped KJob already has an external QObject parent; exclusive ownership cannot be established"));
    }
    if (wrappedJob->property(kWrappedMarkerProperty).toBool()) {
        return reject(QStringLiteral("Wrapped KJob is already owned by another KJobTransferAdapter"));
    }
    if (wrappedJob->error() != 0) {
        return reject(QStringLiteral("Wrapped KJob already holds a terminal error before adapter creation; result may have been missed"));
    }

    const TransferJobCapabilities caps = resolveCapabilities(wrappedJob, options);
    return new KJobTransferAdapter(wrappedJob, options, caps, parent);
}

KJobTransferAdapter::KJobTransferAdapter(KJob *wrappedJob,
                                         const KJobTransferAdapterOptions &options,
                                         TransferJobCapabilities resolvedCapabilities,
                                         QObject *parent)
    : PortableTransferJob(parent)
    , m_job(wrappedJob)
    , m_options(options)
    , m_wrappedStarted(options.startMode == KJobStartMode::FactoryStarts)
{
    setCapabilities(resolvedCapabilities);

    // Establish exclusive ownership over the wrapped KJob.
    m_job->setAutoDelete(false);
    m_job->setProperty(kWrappedMarkerProperty, true);
    m_job->installEventFilter(this);

    connectWrappedJob();
    syncDecisionPhaseFromJob();
    syncCurrentItemFromJob();
}

KJobTransferAdapter::~KJobTransferAdapter()
{
    if (!m_job) {
        markBackendCleanupComplete();
        return;
    }

    m_job->removeEventFilter(this);
    disconnect(m_job.data(), nullptr, this, nullptr);

    KJob *rawJob = m_job.data();
    m_job.clear();

    const bool backendStillActive = !m_wrappedFinished && m_wrappedStarted;
    if (m_inWrappedCallbackDepth > 0 || backendStillActive) {
        // AUD-2A2-01: Never block the GUI thread on an active worker destructor
        // and never delete rawJob while its stack frame is active. Hand off
        // exclusive ownership and cleanupHandle() to KJobCleanupRetainer so
        // cleanupCompleted() fires ONLY after rawJob is actually destroyed.
        setCleanupDetached(true);
        new KJobCleanupRetainer(rawJob,
                                m_wrappedFinished,
                                m_wrappedStarted,
                                m_cancelDispatched,
                                m_options.cancelHandler,
                                m_bandwidthLimiter,
                                cleanupHandle());
        return;
    }

    // Normal post-completion (or never-started) path outside callbacks:
    // rawJob has no active worker thread, so synchronous deletion is non-blocking
    // and guarantees ~KJob() completes BEFORE cleanupCompleted() and destroyed(adapter).
    delete rawJob;
    markBackendCleanupComplete();
}

bool KJobTransferAdapter::supportsBandwidthLimiter(const KJob *job)
{
    if (!job) {
        return false;
    }
    if (qobject_cast<const LocalFileCopyJob *>(job) != nullptr) {
        return true;
    }
    const QMetaObject *mo = job->metaObject();
    if (!mo) {
        return false;
    }
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod method = mo->method(i);
        if (method.name() != QByteArrayLiteral("setBandwidthLimiter")) {
            continue;
        }
        if (method.parameterCount() == 2
            && method.parameterTypeName(0) == QByteArrayLiteral("std::shared_ptr<TransferBandwidthLimiter>")
            && isSupportedTaskIdType(method.parameterTypeName(1))) {
            return true;
        }
        if (method.parameterCount() == 1
            && method.parameterTypeName(0) == QByteArrayLiteral("std::shared_ptr<TransferBandwidthLimiter>")) {
            return true;
        }
    }
    return false;
}

TransferJobCapabilities KJobTransferAdapter::resolveCapabilities(const KJob *job,
                                                                 const KJobTransferAdapterOptions &options)
{
    TransferJobCapabilities caps = TransferJobCapability::None;
    if (!job) {
        return caps;
    }

    const bool hasCustomCancel = static_cast<bool>(options.cancelHandler);
    const bool isLocalCopyJob = qobject_cast<const LocalFileCopyJob *>(job) != nullptr;
    if (hasCustomCancel || isLocalCopyJob || job->capabilities().testFlag(KJob::Killable)) {
        caps |= TransferJobCapability::Killable;
    }
    if (hasCustomCancel || isLocalCopyJob) {
        caps |= TransferJobCapability::AsyncCancel;
    }

    // AUD-2A2-03: Exact pause/resume is strictly opt-in and never advertised
    // for upstream KIO transfer jobs or KioStagedTransferJob.
    if (options.allowExactPause
        && !isUnsafeKioPauseJob(job)
        && job->capabilities().testFlag(KJob::Suspendable)) {
        caps |= TransferJobCapability::Suspendable;
    }

    if (supportsBandwidthLimiter(job)) {
        caps |= TransferJobCapability::BandwidthLimited;
    }

    return caps;
}

void KJobTransferAdapter::connectWrappedJob()
{
    if (!m_job) {
        return;
    }

    connect(m_job.data(), &KJob::result, this, &KJobTransferAdapter::onWrappedResult);
    connect(m_job.data(), &QObject::destroyed, this, &KJobTransferAdapter::onWrappedDestroyed);

    connect(m_job.data(), &KJob::suspended, this, [this](KJob *) {
        if (isTerminal() || m_wrappedFinished || m_inAdapterSuspendResume) {
            return;
        }
        if (!hasCapability(TransferJobCapability::Suspendable)
            || !m_job
            || !m_job->capabilities().testFlag(KJob::Suspendable)
            || isUnsafeKioPauseJob(m_job.data())) {
            return;
        }
        QPointer<KJobTransferAdapter> self(this);
        ++m_inWrappedCallbackDepth;
        syncSuspendedState(true);
        if (self) {
            --self->m_inWrappedCallbackDepth;
        }
    });

    connect(m_job.data(), &KJob::resumed, this, [this](KJob *) {
        if (isTerminal() || m_wrappedFinished || m_inAdapterSuspendResume) {
            return;
        }
        QPointer<KJobTransferAdapter> self(this);
        ++m_inWrappedCallbackDepth;
        syncSuspendedState(false);
        if (self) {
            --self->m_inWrappedCallbackDepth;
        }
    });

    connect(m_job.data(), &KJob::totalAmountChanged, this,
            [this](KJob *, KJob::Unit unit, qulonglong amount) {
                if (isTerminal() || m_wrappedFinished) {
                    return;
                }
                QPointer<KJobTransferAdapter> self(this);
                ++m_inWrappedCallbackDepth;
                if (unit == KJob::Bytes) {
                    setTotalBytes(amount);
                } else if (unit == KJob::Files) {
                    setTotalFiles(amount);
                }
                if (self) {
                    --self->m_inWrappedCallbackDepth;
                }
            });

    connect(m_job.data(), &KJob::processedAmountChanged, this,
            [this](KJob *, KJob::Unit unit, qulonglong amount) {
                if (isTerminal() || m_wrappedFinished) {
                    return;
                }
                QPointer<KJobTransferAdapter> self(this);
                ++m_inWrappedCallbackDepth;
                if (unit == KJob::Bytes) {
                    setProcessedBytes(amount);
                } else if (unit == KJob::Files) {
                    setProcessedFiles(amount);
                }
                if (self) {
                    --self->m_inWrappedCallbackDepth;
                }
            });

    connect(m_job.data(), &KJob::percentChanged, this,
            [this](KJob *, unsigned long percent) {
                if (isTerminal() || m_wrappedFinished) {
                    return;
                }
                // AUD-2A2-05: When byte totals are known, keep byte-derived
                // percentage authoritative; only use raw KJob percentChanged
                // when totalBytes() == 0.
                if (totalBytes() > 0) {
                    return;
                }
                QPointer<KJobTransferAdapter> self(this);
                ++m_inWrappedCallbackDepth;
                setPercent(percent);
                if (self) {
                    --self->m_inWrappedCallbackDepth;
                }
            });

    connect(m_job.data(), &KJob::speed, this,
            [this](KJob *, unsigned long bytesPerSecond) {
                if (isTerminal() || m_wrappedFinished) {
                    return;
                }
                QPointer<KJobTransferAdapter> self(this);
                ++m_inWrappedCallbackDepth;
                emitSpeed(bytesPerSecond);
                if (self) {
                    --self->m_inWrappedCallbackDepth;
                }
            });

    connect(m_job.data(), &KJob::description, this,
            [this](KJob *,
                   const QString &title,
                   const QPair<QString, QString> &field1,
                   const QPair<QString, QString> &field2) {
                if (isTerminal() || m_wrappedFinished) {
                    return;
                }
                QPointer<KJobTransferAdapter> self(this);
                ++m_inWrappedCallbackDepth;
                m_lastDescriptionTitle = title;
                m_lastField1 = field1;
                m_lastField2 = field2;
                syncCurrentItemFromJob();
                if (self) {
                    --self->m_inWrappedCallbackDepth;
                }
            });
}

void KJobTransferAdapter::setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter,
                                              quint64 taskId)
{
    if (!hasCapability(TransferJobCapability::BandwidthLimited) || !m_job || isTerminal()) {
        return;
    }

    m_bandwidthLimiter = limiter;

    if (auto *copyJob = qobject_cast<LocalFileCopyJob *>(m_job.data())) {
        copyJob->setBandwidthLimiter(m_bandwidthLimiter, taskId);
        return;
    }

    const QMetaObject *mo = m_job->metaObject();
    if (!mo) {
        return;
    }
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod method = mo->method(i);
        if (method.name() != QByteArrayLiteral("setBandwidthLimiter")) {
            continue;
        }
        if (method.parameterCount() == 2
            && method.parameterTypeName(0) == QByteArrayLiteral("std::shared_ptr<TransferBandwidthLimiter>")
            && isSupportedTaskIdType(method.parameterTypeName(1))) {
            method.invoke(m_job.data(),
                          Qt::DirectConnection,
                          Q_ARG(std::shared_ptr<TransferBandwidthLimiter>, m_bandwidthLimiter),
                          Q_ARG(quint64, taskId));
            return;
        }
        if (method.parameterCount() == 1
            && method.parameterTypeName(0) == QByteArrayLiteral("std::shared_ptr<TransferBandwidthLimiter>")) {
            method.invoke(m_job.data(),
                          Qt::DirectConnection,
                          Q_ARG(std::shared_ptr<TransferBandwidthLimiter>, m_bandwidthLimiter));
            return;
        }
    }
}

TransferErrorCode KJobTransferAdapter::mapKJobError(int kjobError)
{
    if (kjobError == 0) {
        return TransferErrorCode::NoError;
    }
    if (kjobError == KJob::KilledJobError || kjobError == KIO::ERR_USER_CANCELED) {
        return TransferErrorCode::Cancelled;
    }
    switch (kjobError) {
    case KIO::ERR_DOES_NOT_EXIST:
        return TransferErrorCode::DoesNotExist;
    case KIO::ERR_FILE_ALREADY_EXIST:
    case KIO::ERR_DIR_ALREADY_EXIST:
        return TransferErrorCode::AlreadyExists;
    case KIO::ERR_CANNOT_READ:
    case KIO::ERR_ACCESS_DENIED:
        return TransferErrorCode::CannotRead;
    case KIO::ERR_WRITE_ACCESS_DENIED:
    case KIO::ERR_CANNOT_WRITE:
        return TransferErrorCode::CannotWrite;
    case KIO::ERR_CANNOT_OPEN_FOR_READING:
        return TransferErrorCode::CannotOpenForReading;
    case KIO::ERR_CANNOT_OPEN_FOR_WRITING:
        return TransferErrorCode::CannotOpenForWriting;
    case KIO::ERR_CANNOT_MKDIR:
        return TransferErrorCode::CannotMkdir;
    case KIO::ERR_CANNOT_RENAME:
    case KIO::ERR_CANNOT_RENAME_PARTIAL:
        return TransferErrorCode::CannotRename;
    case KIO::ERR_CANNOT_CHMOD:
    case KIO::ERR_CANNOT_CHOWN:
        return TransferErrorCode::CannotChmod;
    case KIO::ERR_DISK_FULL:
    case KIO::ERR_OUT_OF_MEMORY:
        return TransferErrorCode::DiskFull;
    case KIO::ERR_IS_DIRECTORY:
    case KIO::ERR_IS_FILE:
        return TransferErrorCode::IsDirectory;
    case KIO::ERR_UNSUPPORTED_ACTION:
        return TransferErrorCode::UnsupportedAction;
    case KIO::ERR_UNSUPPORTED_PROTOCOL:
        return TransferErrorCode::UnsupportedProtocol;
    default:
        return TransferErrorCode::BackendError;
    }
}

TransferOutcomeFlags KJobTransferAdapter::extractOutcomeFlags(const KJob *job)
{
    TransferOutcomeFlags flags;
    if (!job) {
        flags.mergeRetryBlocked = true;
        return flags;
    }

    bool safetyFault = false;
    bool recoveryFault = false;

    flags.destinationPublished = readSafetyBoolProperty(job, "thispcDestinationPublished", safetyFault);
    flags.destinationReplaced = readSafetyBoolProperty(job, "thispcDestinationReplaced", safetyFault);
    flags.directoryDurabilityConfirmed = readSafetyBoolProperty(job, "thispcDirectoryDurabilityConfirmed", safetyFault);
    flags.partialMoveOutcome = readSafetyBoolProperty(job, "thispcPartialMoveOutcome", safetyFault);
    flags.batchPublishedCount = readSafetyNonNegativeIntProperty(job, "thispcBatchPublishedCount", safetyFault);
    flags.batchPartiallyCompleted = readSafetyBoolProperty(job, "thispcBatchPartiallyCompleted", safetyFault);
    flags.mergeRetryBlocked = readSafetyBoolProperty(job, "thispcMergeRetryBlocked", safetyFault);
    flags.mergeRecoveryRequired = readSafetyBoolProperty(job, "thispcMergeRecoveryRequired", recoveryFault);
    flags.mergeState = readOptionalStringProperty(job, "thispcMergeState", safetyFault, true);
    flags.mergeSkippedCount = readSafetyNonNegativeIntProperty(job, "thispcMergeSkippedCount", safetyFault);
    flags.mergeReplacedCount = readSafetyNonNegativeIntProperty(job, "thispcMergeReplacedCount", safetyFault);
    flags.mergeHistoryNote = readOptionalStringProperty(job, "thispcMergeHistoryNote", safetyFault, false);
    flags.systemError = readOptionalIntProperty(job, "thispcSystemError");
    flags.backendErrorCode = job->error();

    // AUD-2A2-05: Fail-closed cross-check against producer merge-state strings
    // even if a boolean flag was omitted or inconsistent.
    if (flags.mergeState == QStringLiteral("RecoveryRequired")) {
        flags.mergeRecoveryRequired = true;
        flags.mergeRetryBlocked = true;
    } else if (flags.mergeState == QStringLiteral("FailedPartialSuccess")) {
        flags.batchPartiallyCompleted = true;
        flags.mergeRetryBlocked = true;
    } else if (flags.mergeState == QStringLiteral("FailedUncertainDurability")) {
        flags.partialMoveOutcome = true;
        flags.mergeRetryBlocked = true;
    }

    // Fail-closed invariant: any corrupted or type-invalid safety-critical
    // property blocks safe retry rather than silently appearing safe.
    if (recoveryFault) {
        flags.mergeRecoveryRequired = true;
        flags.mergeRetryBlocked = true;
    }
    if (safetyFault) {
        flags.mergeRetryBlocked = true;
    }

    return flags;
}

bool KJobTransferAdapter::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_job.data()
        && event
        && event->type() == QEvent::DynamicPropertyChange
        && !isTerminal()
        && !m_wrappedFinished) {
        const auto *propEvent = static_cast<const QDynamicPropertyChangeEvent *>(event);
        const QByteArray name = propEvent->propertyName();
        if (name == QByteArrayLiteral("thispcMergeInDecisionPhase")
            || name == QByteArrayLiteral("thispcCurrentSourceUrl")
            || name == QByteArrayLiteral("thispcCurrentDestinationUrl")) {
            QPointer<KJobTransferAdapter> self(this);
            ++m_inWrappedCallbackDepth;
            if (name == QByteArrayLiteral("thispcMergeInDecisionPhase")) {
                syncDecisionPhaseFromJob();
            } else {
                syncCurrentItemFromJob();
            }
            if (!self) {
                // AUD-2A2-04: Return false so we never suppress unrelated
                // dynamic-property delivery on watched after adapter teardown.
                return false;
            }
            --self->m_inWrappedCallbackDepth;
        }
    }
    return PortableTransferJob::eventFilter(watched, event);
}

void KJobTransferAdapter::syncDecisionPhaseFromJob()
{
    if (!m_job || isTerminal()) {
        return;
    }
    if (!hasDynamicProperty(m_job.data(), "thispcMergeInDecisionPhase")) {
        // Property removed while decision phase is active: retain true (fail-closed).
        // If genuinely absent from the beginning (isInDecisionPhase() == false), stay false.
        return;
    }
    const QVariant value = m_job->property("thispcMergeInDecisionPhase");
    if (value.isValid() && value.metaType().id() == QMetaType::Bool) {
        setInDecisionPhase(value.toBool());
        return;
    }
    // AUD-2A2-04: Invalid or unexpectedly typed present property fails closed.
    setInDecisionPhase(true);
}

void KJobTransferAdapter::syncCurrentItemFromJob()
{
    if (!m_job || isTerminal()) {
        return;
    }
    const QUrl src = m_job->property("thispcCurrentSourceUrl").value<QUrl>();
    const QUrl dst = m_job->property("thispcCurrentDestinationUrl").value<QUrl>();
    if (src.isValid() || dst.isValid()) {
        setCurrentItem(src, dst);
    }
}

bool KJobTransferAdapter::hasActiveBackendBeforeStart() const
{
    if (m_options.startMode != KJobStartMode::FactoryStarts) {
        return false;
    }
    return m_hasBufferedResult || (!m_wrappedFinished && m_job != nullptr);
}

void KJobTransferAdapter::doStart()
{
    if (m_hasBufferedResult) {
        m_hasBufferedResult = false;
        deliverTerminalOutcome(m_bufferedError, m_bufferedErrorText, m_bufferedOutcomeFlags);
        return;
    }

    if (!m_job) {
        TransferOutcomeFlags flags;
        flags.mergeRetryBlocked = true;
        setOutcomeFlags(flags);
        finishFailed(TransferErrorCode::InternalError,
                     QStringLiteral("Wrapped KJob was destroyed before start()"));
        return;
    }

    if (m_options.startMode == KJobStartMode::FactoryStarts) {
        // Already started by its factory (e.g. KIO::copy/move). Never start twice.
        return;
    }

    if (m_wrappedStarted) {
        return;
    }
    m_wrappedStarted = true;
    m_job->start();
}

bool KJobTransferAdapter::doCancel()
{
    if (m_hasBufferedResult) {
        m_hasBufferedResult = false;
        deliverTerminalOutcome(m_bufferedError, m_bufferedErrorText, m_bufferedOutcomeFlags);
        return true;
    }

    if (!m_job || m_wrappedFinished) {
        return false;
    }
    if (m_cancelDispatched) {
        return true;
    }
    m_cancelDispatched = true;

    QPointer<KJobTransferAdapter> self(this);
    bool accepted = false;

    if (m_options.cancelHandler) {
        accepted = m_options.cancelHandler(m_job.data());
    } else if (auto *copyJob = qobject_cast<LocalFileCopyJob *>(m_job.data())) {
        accepted = copyJob->requestCancelAsync();
    } else if (m_job->capabilities().testFlag(KJob::Killable)) {
        accepted = m_job->kill(KJob::EmitResult);
    }

    if (!self) {
        return true;
    }
    if (!accepted && !self->isTerminal()) {
        m_cancelDispatched = false;
    }
    return accepted;
}

bool KJobTransferAdapter::doSuspend()
{
    if (!m_job
        || !hasCapability(TransferJobCapability::Suspendable)
        || m_wrappedFinished
        || !m_job->capabilities().testFlag(KJob::Suspendable)
        || isUnsafeKioPauseJob(m_job.data())) {
        return false;
    }
    QPointer<KJobTransferAdapter> self(this);
    m_inAdapterSuspendResume = true;
    const bool ok = m_job->suspend();
    if (self) {
        self->m_inAdapterSuspendResume = false;
    }
    return ok;
}

bool KJobTransferAdapter::doResume()
{
    // AUD-2A2-R1-03: Do not re-check KJob::Suspendable when resuming an already
    // paused job so a mid-pause capability change cannot wedge the job in Paused.
    if (!m_job
        || !hasCapability(TransferJobCapability::Suspendable)
        || m_wrappedFinished
        || isUnsafeKioPauseJob(m_job.data())) {
        return false;
    }
    QPointer<KJobTransferAdapter> self(this);
    m_inAdapterSuspendResume = true;
    const bool ok = m_job->resume();
    if (self) {
        self->m_inAdapterSuspendResume = false;
    }
    return ok;
}

void KJobTransferAdapter::deliverTerminalOutcome(int kjobError,
                                                 const QString &kjobErrorText,
                                                 const TransferOutcomeFlags &flags)
{
    if (isTerminal()) {
        return;
    }

    setOutcomeFlags(flags);

    if (kjobError == 0) {
        finishCompleted();
        return;
    }

    const TransferErrorCode mapped = mapKJobError(kjobError);
    if (mapped == TransferErrorCode::Cancelled) {
        finishCancelled(kjobErrorText);
        return;
    }

    finishFailed(mapped, kjobErrorText);
}

void KJobTransferAdapter::onWrappedResult(KJob *job)
{
    if (m_wrappedFinished || isTerminal()) {
        return;
    }
    m_wrappedFinished = true;

    const KJob *sourceJob = job ? job : m_job.data();
    const int err = sourceJob ? sourceJob->error() : KJob::UserDefinedError;
    const QString errText = sourceJob ? sourceJob->errorText() : QStringLiteral("Wrapped KJob missing on result");
    const TransferOutcomeFlags flags = extractOutcomeFlags(sourceJob);

    if (!isStarted()) {
        // FactoryStarts job completed after adapter construction but before
        // PortableTransferJob::start() or requestCancel() was called. Buffer
        // the exact snapshot so start() / requestCancel() delivers it cleanly.
        m_hasBufferedResult = true;
        m_bufferedError = err;
        m_bufferedErrorText = errText;
        m_bufferedOutcomeFlags = flags;
        return;
    }

    QPointer<KJobTransferAdapter> self(this);
    ++m_inWrappedCallbackDepth;
    deliverTerminalOutcome(err, errText, flags);
    if (self) {
        --self->m_inWrappedCallbackDepth;
    }
}

void KJobTransferAdapter::onWrappedDestroyed()
{
    m_job.clear();
    if (m_wrappedFinished || isTerminal()) {
        return;
    }

    TransferOutcomeFlags flags;
    flags.mergeRetryBlocked = true;

    if (!isStarted()) {
        m_hasBufferedResult = true;
        m_bufferedError = KJob::UserDefinedError;
        m_bufferedErrorText = QStringLiteral("Wrapped KJob was unexpectedly destroyed before start()");
        m_bufferedOutcomeFlags = flags;
        return;
    }

    setOutcomeFlags(flags);
    finishFailed(TransferErrorCode::InternalError,
                 QStringLiteral("Wrapped KJob was unexpectedly destroyed before emitting result"));
}
