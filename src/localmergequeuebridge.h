#ifndef THISPC_LOCALMERGEQUEUEBRIDGE_H
#define THISPC_LOCALMERGEQUEUEBRIDGE_H

#include "localmergeorchestrator.h"
#include "transferqueuecontroller.h"
#include "undocontroller.h"

#include <KIO/Global>
#include <KJob>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <functional>
#include <sys/stat.h>

class LocalMergeDevSandbox
{
public:
    static QString devTestRootFromEnv()
    {
        return qEnvironmentVariable("THISPC_FOLDER_MERGE_DEV_ROOT");
    }

    static bool isDestinationAllowed(const QString &destPath,
                                     const QString &devRootPath,
                                     QString *reasonOut = nullptr)
    {
        auto reject = [reasonOut](const QString &r) {
            if (reasonOut) *reasonOut = r;
            return false;
        };
        if (devRootPath.isEmpty()) {
            return reject(QStringLiteral("THISPC_FOLDER_MERGE_DEV_ROOT is not set"));
        }
        if (destPath.isEmpty() || destPath.contains(QLatin1String("/../")) || destPath.endsWith(QLatin1String("/.."))) {
            return reject(QStringLiteral("Destination path is empty or contains '..'"));
        }
        QFileInfo rootInfo(devRootPath);
        if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink()) {
            return reject(QStringLiteral("Dev root must be an existing non-symlink directory"));
        }
        const QString canonRoot = rootInfo.canonicalFilePath();
        const QString homeCanon = QFileInfo(QDir::homePath()).canonicalFilePath();
        if (canonRoot.isEmpty() || canonRoot == QLatin1String("/") || canonRoot == homeCanon) {
            return reject(QStringLiteral("Dev root cannot be '/' or HOME"));
        }

        struct stat rootSt {};
        if (::lstat(QFile::encodeName(canonRoot).constData(), &rootSt) != 0 || !S_ISDIR(rootSt.st_mode)) {
            return reject(QStringLiteral("Cannot stat dev root"));
        }

        // Resolve deepest existing ancestor without trusting symlinks that escape canonRoot
        const QString resolvedDest = LocalMergeReservationRegistry::resolvePathForReservation(destPath);
        if (resolvedDest.isEmpty()) {
            return reject(QStringLiteral("Cannot resolve destination path"));
        }
        const QString rootSlash = canonRoot.endsWith(QLatin1Char('/')) ? canonRoot : (canonRoot + QLatin1Char('/'));
        if (resolvedDest != canonRoot && !resolvedDest.startsWith(rootSlash)) {
            return reject(QStringLiteral("Destination escapes isolated dev root: %1").arg(resolvedDest));
        }

        // Verify (st_dev, st_ino) ancestry on existing directory chain
        QString probe = QDir::cleanPath(destPath);
        while (!probe.isEmpty() && !QFileInfo::exists(probe)) {
            const QString parent = QFileInfo(probe).path();
            if (parent == probe) break;
            probe = parent;
        }
        bool matchedRootInode = false;
        while (!probe.isEmpty()) {
            struct stat st {};
            if (::lstat(QFile::encodeName(probe).constData(), &st) != 0) {
                return reject(QStringLiteral("lstat failed on destination ancestor: %1").arg(probe));
            }
            if (S_ISLNK(st.st_mode)) {
                return reject(QStringLiteral("Symlink component blocked in dev sandbox: %1").arg(probe));
            }
            if (static_cast<quint64>(st.st_dev) == static_cast<quint64>(rootSt.st_dev)
                && static_cast<quint64>(st.st_ino) == static_cast<quint64>(rootSt.st_ino)) {
                matchedRootInode = true;
                break;
            }
            if (probe == QLatin1String("/")) {
                break;
            }
            const QString parent = QFileInfo(probe).path();
            if (parent == probe) break;
            probe = parent;
        }
        if (!matchedRootInode) {
            return reject(QStringLiteral("Destination inode ancestry does not match isolated dev root"));
        }
        return true;
    }
};

struct LocalMergeQueueRequest {
    QList<QUrl> sources;
    QUrl destinationDir;
    QString title;
    FileOperationContext context;
    TransferPriority priority = TransferPriority::Normal;
    LocalMergeBackendCapabilities capabilities;
    LocalMergeOrchestrator::FolderPromptFn folderPrompt;
    LocalMergeOrchestrator::FilePromptFn filePrompt;
    LocalMergeOrchestrator::AsyncFolderPromptFn asyncFolderPrompt;
    LocalMergeOrchestrator::AsyncFilePromptFn asyncFilePrompt;
    std::function<void(LocalMergeExecutionJob *)> executionJobConfigurator;
    QPointer<UndoController> undoController;
    bool forceRecoveryRequired = false;
    int maxReplanRounds = 8;
};

inline bool localMergeApplyUndoFenceIfNeeded(UndoController *undo, const LocalMergeOrchestratorResult &result)
{
    if (!undo) {
        return false;
    }
    const bool wroteAnything = (result.progress.filesCreated > 0
                                || result.progress.directoriesCreated > 0
                                || result.progress.replacedCount > 0);
    if (wroteAnything
        || result.undoSummary.requiresIrreversibleSerial
        || result.progress.replacedCount > 0
        || result.finalState == LocalMergeOrchestratorState::FailedPartialSuccess
        || result.finalState == LocalMergeOrchestratorState::FailedUncertainDurability
        || result.finalState == LocalMergeOrchestratorState::RecoveryRequired
        || (result.finalState == LocalMergeOrchestratorState::Cancelled && result.progress.partialSuccess)) {
        undo->fenceIrreversibleOperation();
        return true;
    }
    return false;
}

class LocalMergeQueueJob final : public KJob
{
public:
    explicit LocalMergeQueueJob(LocalMergeQueueRequest request,
                                QPointer<TransferQueueController> queueController = nullptr,
                                quint64 queueTaskId = 0,
                                QObject *parent = nullptr)
        : KJob(parent)
        , m_request(std::move(request))
        , m_queueController(queueController)
        , m_queueTaskId(queueTaskId)
    {
        setCapabilities(KJob::Killable);
        setAutoDelete(true);
        setProperty("thispcMergeInDecisionPhase", true);
    }

    ~LocalMergeQueueJob() override = default;

    static qulonglong computeWindowSpeedBytesPerSec(quint64 dBytes, qint64 dtMs, qint64 minWindowMs = 250)
    {
        if (dtMs < minWindowMs || dtMs <= 0 || dBytes == 0) {
            return 0;
        }
        return static_cast<qulonglong>((dBytes * 1000ULL) / static_cast<quint64>(dtMs));
    }

    void setQueueTaskId(quint64 taskId) { m_queueTaskId = taskId; }
    quint64 queueTaskId() const { return m_queueTaskId; }

    void setStateCallback(std::function<void(LocalMergeOrchestratorState)> cb) { m_onState = std::move(cb); }
    void setProgressCallback(std::function<void(const LocalMergeOrchestratorProgress &)> cb) { m_onProgress = std::move(cb); }
    void setResultCallback(std::function<void(const LocalMergeOrchestratorResult &)> cb) { m_onResult = std::move(cb); }

    bool hasOrchestratorResult() const { return m_hasResult; }
    const LocalMergeOrchestratorResult &orchestratorResult() const { return m_result; }
    LocalMergeOrchestrator *orchestrator() const { return m_orchestrator.data(); }

    bool requestCancelAsync()
    {
        if (m_resultEmitted) {
            return false;
        }
        m_cancelRequested = true;
        if (m_orchestrator) {
            m_orchestrator->cancel();
            return true;
        }
        LocalMergeOrchestratorResult cancelled;
        cancelled.finalState = LocalMergeOrchestratorState::Cancelled;
        cancelled.progress.state = LocalMergeOrchestratorState::Cancelled;
        cancelled.progress.retryAllowed = true;
        cancelled.diagnostic = QStringLiteral("Cancelled before start");
        handleOrchestratorFinished(cancelled);
        return true;
    }

    void start() override
    {
        if (m_started || m_resultEmitted) {
            setProperty("thispcMergeDuplicateStartBlocked", true);
            return;
        }
        m_started = true;

        if (!m_queueController || m_queueTaskId == 0) {
            LocalMergeOrchestratorResult failed;
            failed.finalState = LocalMergeOrchestratorState::FailedNoChanges;
            failed.progress.state = LocalMergeOrchestratorState::FailedNoChanges;
            failed.progress.retryAllowed = false;
            failed.diagnostic = QStringLiteral("Queue controller or live task ID missing for Folder Merge");
            handleOrchestratorFinished(failed);
            return;
        }

        auto *orch = new LocalMergeOrchestrator(
            m_request.sources,
            m_request.destinationDir,
            nullptr,  // Shared reservation is managed by TransferQueueController::m_registry
            m_queueTaskId,
            m_request.capabilities,
            this);
        m_orchestrator = orch;

        if (m_request.folderPrompt) {
            orch->setFolderPrompt(m_request.folderPrompt);
        }
        if (m_request.filePrompt) {
            orch->setFilePrompt(m_request.filePrompt);
        }
        if (m_request.asyncFolderPrompt) {
            orch->setAsyncFolderPrompt(m_request.asyncFolderPrompt);
        }
        if (m_request.asyncFilePrompt) {
            orch->setAsyncFilePrompt(m_request.asyncFilePrompt);
        }
        if (m_request.executionJobConfigurator) {
            orch->setExecutionJobConfigurator(m_request.executionJobConfigurator);
        }
        orch->setForceRecoveryRequired(m_request.forceRecoveryRequired);
        orch->setMaxReplanRounds(m_request.maxReplanRounds);

        // Bind dynamic Rename targets into the shared TransferReservationRegistry
        QPointer<TransferQueueController> qc = m_queueController;
        const quint64 taskId = m_queueTaskId;
        orch->setDynamicTargetReservationFn([qc, taskId](const QUrl &targetUrl, QString *reasonOut) -> bool {
            if (!qc || taskId == 0) {
                if (reasonOut) {
                    *reasonOut = QStringLiteral("Queue controller or task ID unavailable for dynamic target reservation: %1")
                                     .arg(targetUrl.toLocalFile());
                }
                return false;
            }
            if (!qc->tryReserveAdditionalTarget(taskId, targetUrl)) {
                if (reasonOut) {
                    *reasonOut = QStringLiteral("Renamed target conflicts with active queue reservation: %1")
                                     .arg(targetUrl.toLocalFile());
                }
                return false;
            }
            return true;
        });

        // Stage 3B.3B-2 Undo Fence Timing: raise Undo barrier BEFORE any Overwrite starts
        QPointer<UndoController> undoCtrl = m_request.undoController;
        orch->setPreIrreversibleWriteHook([undoCtrl]() {
            if (undoCtrl) {
                undoCtrl->fenceIrreversibleOperation();
            }
        });

        QPointer<LocalMergeQueueJob> self(this);
        orch->setStateChangedCallback([self](LocalMergeOrchestratorState s) {
            if (!self) return;
            const bool inDecision = (s == LocalMergeOrchestratorState::Queued
                                     || s == LocalMergeOrchestratorState::Planning
                                     || s == LocalMergeOrchestratorState::AwaitingUserDecision
                                     || s == LocalMergeOrchestratorState::Revalidating
                                     || s == LocalMergeOrchestratorState::NeedsReplan);
            self->setProperty("thispcMergeInDecisionPhase", inDecision);
            const QString srcText = self->m_request.sources.isEmpty()
                ? QString()
                : self->m_request.sources.first().toLocalFile();
            const QString dstText = self->m_request.destinationDir.toLocalFile();
            QString phaseLabel;
            switch (s) {
            case LocalMergeOrchestratorState::Planning: phaseLabel = QStringLiteral("Planowanie scalania"); break;
            case LocalMergeOrchestratorState::AwaitingUserDecision: phaseLabel = QStringLiteral("Oczekiwanie na decyzję"); break;
            case LocalMergeOrchestratorState::Revalidating: phaseLabel = QStringLiteral("Rewalidacja celów"); break;
            case LocalMergeOrchestratorState::Executing: phaseLabel = QStringLiteral("Scalanie folderów"); break;
            case LocalMergeOrchestratorState::NeedsReplan: phaseLabel = QStringLiteral("Ponowne planowanie"); break;
            case LocalMergeOrchestratorState::Cancelling: phaseLabel = QStringLiteral("Anulowanie scalania"); break;
            default: phaseLabel = localMergeStateName(s); break;
            }
            if (s == LocalMergeOrchestratorState::Executing) {
                self->m_transferTimer.start();
                self->m_lastSpeedSampleMs = 0;
                self->m_lastSpeedSampleBytes = self->m_lastReportedBytes;
            } else if (inDecision) {
                self->emitSpeed(0);
            }
            Q_EMIT self->description(
                self.data(),
                phaseLabel,
                qMakePair(QStringLiteral("Źródło"), srcText),
                qMakePair(QStringLiteral("Cel"), dstText));
            if (self->m_onState) {
                self->m_onState(s);
            }
        });
        orch->setProgressChangedCallback([self](const LocalMergeOrchestratorProgress &p) {
            if (!self || self->m_resultEmitted) return;
            self->m_lastReportedBytes = p.bytesProcessed;
            if (p.bytesPlanned > 0) {
                self->setTotalAmount(KJob::Bytes, p.bytesPlanned);
                self->setProcessedAmount(KJob::Bytes, qMin(p.bytesProcessed, p.bytesPlanned));
                const unsigned long pct = static_cast<unsigned long>(
                    qMin<quint64>(100, (p.bytesProcessed * 100ULL) / p.bytesPlanned));
                self->setPercent(pct);
            }
            if (p.state == LocalMergeOrchestratorState::Executing && self->m_transferTimer.isValid()) {
                const qint64 nowMs = self->m_transferTimer.elapsed();
                const qint64 dtMs = nowMs - self->m_lastSpeedSampleMs;
                if (dtMs >= 250 && p.bytesProcessed > self->m_lastSpeedSampleBytes) {
                    const quint64 dBytes = p.bytesProcessed - self->m_lastSpeedSampleBytes;
                    const qulonglong speed = computeWindowSpeedBytesPerSec(dBytes, dtMs, 250);
                    if (speed > 0) {
                        self->emitSpeed(static_cast<unsigned long>(
                            qMin<qulonglong>(speed, static_cast<qulonglong>(std::numeric_limits<unsigned long>::max()))));
                    }
                    self->m_lastSpeedSampleMs = nowMs;
                    self->m_lastSpeedSampleBytes = p.bytesProcessed;
                }
            }
            if (self->m_onProgress) {
                self->m_onProgress(p);
            }
        });
        orch->setFinishedCallback([self](const LocalMergeOrchestratorResult &res) {
            if (!self) return;
            self->handleOrchestratorFinished(res);
        });

        if (m_cancelRequested) {
            orch->cancel();
            return;
        }
        orch->start();
    }

protected:
    bool doKill() override
    {
        if (m_resultEmitted) {
            return false;
        }
        m_cancelRequested = true;
        if (m_orchestrator) {
            m_orchestrator->cancel();
            if (!m_resultEmitted && m_orchestrator) {
                const LocalMergeOrchestratorResult snap = m_orchestrator->snapshotResultForCancel();
                if (snap.progress.partialSuccess) {
                    setProperty("thispcBatchPartiallyCompleted", true);
                    setProperty("thispcBatchPublishedCount",
                                snap.progress.filesCreated + snap.progress.replacedCount);
                    setProperty("thispcMergeRetryBlocked", true);
                    setErrorText(QStringLiteral("Anulowano po częściowym scaleniu (utworzono: %1, zastąpiono: %2)")
                                     .arg(snap.progress.filesCreated)
                                     .arg(snap.progress.replacedCount));
                }
                if (m_request.undoController) {
                    localMergeApplyUndoFenceIfNeeded(m_request.undoController.data(), snap);
                }
            }
            return true;
        }
        // Killed before start() ran
        LocalMergeOrchestratorResult cancelled;
        cancelled.finalState = LocalMergeOrchestratorState::Cancelled;
        cancelled.progress.state = LocalMergeOrchestratorState::Cancelled;
        cancelled.progress.retryAllowed = true;
        cancelled.diagnostic = QStringLiteral("Cancelled before start");
        handleOrchestratorFinished(cancelled);
        return true;
    }

private:
    void handleOrchestratorFinished(const LocalMergeOrchestratorResult &res)
    {
        if (m_resultEmitted) {
            return;
        }
        m_resultEmitted = true;
        m_hasResult = true;
        m_result = res;
        setProperty("thispcMergeInDecisionPhase", false);
        emitSpeed(0);

        if (res.progress.bytesPlanned > 0) {
            setTotalAmount(KJob::Bytes, res.progress.bytesPlanned);
            setProcessedAmount(KJob::Bytes, res.progress.bytesProcessed);
        }

        setProperty("thispcMergeState", localMergeStateName(res.finalState));
        setProperty("thispcMergeFilesCreated", res.progress.filesCreated);
        setProperty("thispcMergeReplacedCount", res.progress.replacedCount);
        setProperty("thispcMergeSkippedCount", res.progress.skippedCount + res.progress.skippedSubtreeCount);
        setProperty("thispcMergeRequiresIrreversibleSerial", res.undoSummary.requiresIrreversibleSerial);

        if (m_request.undoController) {
            localMergeApplyUndoFenceIfNeeded(m_request.undoController.data(), res);
        }

        if (m_onResult) {
            m_onResult(res);
        }

        switch (res.finalState) {
        case LocalMergeOrchestratorState::Completed:
            setError(KJob::NoError);
            break;
        case LocalMergeOrchestratorState::CompletedWithSkippedConflicts:
            setError(KJob::NoError);
            setProperty("thispcMergeHistoryNote",
                        QStringLiteral("Scalanie zakończone z pominiętymi konfliktami (%1)")
                            .arg(res.progress.skippedCount + res.progress.skippedSubtreeCount));
            break;
        case LocalMergeOrchestratorState::Cancelled:
            setError(KJob::KilledJobError);
            if (res.progress.partialSuccess) {
                setProperty("thispcBatchPartiallyCompleted", true);
                setProperty("thispcBatchPublishedCount",
                            res.progress.filesCreated + res.progress.replacedCount);
                setProperty("thispcMergeRetryBlocked", true);
                setErrorText(QStringLiteral("Anulowano po częściowym scaleniu (utworzono: %1, zastąpiono: %2)")
                                 .arg(res.progress.filesCreated)
                                 .arg(res.progress.replacedCount));
            } else {
                setErrorText(res.diagnostic.isEmpty() ? QStringLiteral("Anulowano przed zmianami") : res.diagnostic);
            }
            break;
        case LocalMergeOrchestratorState::FailedNoChanges:
            setError(KIO::ERR_CANNOT_WRITE);
            setErrorText(res.diagnostic.isEmpty() ? QStringLiteral("Scalanie nie powiodło się przed zapisem") : res.diagnostic);
            break;
        case LocalMergeOrchestratorState::FailedPartialSuccess:
            setError(KIO::ERR_CANNOT_WRITE);
            setProperty("thispcBatchPartiallyCompleted", true);
            setProperty("thispcBatchPublishedCount",
                        res.progress.filesCreated + res.progress.replacedCount);
            setProperty("thispcMergeRetryBlocked", true);
            setErrorText(QStringLiteral("Częściowy sukces scalania zakończony błędem (ponowienie zablokowane): %1")
                             .arg(res.diagnostic));
            break;
        case LocalMergeOrchestratorState::FailedUncertainDurability:
            setError(KIO::ERR_CANNOT_WRITE);
            setProperty("thispcPartialMoveOutcome", true);
            setProperty("thispcBatchPartiallyCompleted", true);
            setProperty("thispcMergeRetryBlocked", true);
            setErrorText(QStringLiteral("Niepewna trwałość zapisu po błędzie I/O (wymagana weryfikacja ręczna): %1")
                             .arg(res.diagnostic));
            break;
        case LocalMergeOrchestratorState::RecoveryRequired:
            setError(KIO::ERR_CANNOT_WRITE);
            setProperty("thispcBatchPartiallyCompleted", true);
            setProperty("thispcMergeRetryBlocked", true);
            setProperty("thispcMergeRecoveryRequired", true);
            setErrorText(QStringLiteral("Wykryto niedokończoną transakcję scalania — wymagane odzyskiwanie: %1")
                             .arg(res.diagnostic));
            break;
        default:
            setError(KIO::ERR_ABORTED);
            setErrorText(res.diagnostic);
            break;
        }

        emitResult();
    }

    LocalMergeQueueRequest m_request;
    QPointer<TransferQueueController> m_queueController;
    quint64 m_queueTaskId = 0;
    QPointer<LocalMergeOrchestrator> m_orchestrator;

    std::function<void(LocalMergeOrchestratorState)> m_onState;
    std::function<void(const LocalMergeOrchestratorProgress &)> m_onProgress;
    std::function<void(const LocalMergeOrchestratorResult &)> m_onResult;

    bool m_started = false;
    bool m_cancelRequested = false;
    bool m_resultEmitted = false;
    bool m_hasResult = false;
    LocalMergeOrchestratorResult m_result;
    QElapsedTimer m_transferTimer;
    qint64 m_lastSpeedSampleMs = 0;
    quint64 m_lastSpeedSampleBytes = 0;
    quint64 m_lastReportedBytes = 0;
};

class LocalMergeQueueBridge
{
public:
    // Explicitly disabled by default; production Copy/Paste/DnD must NOT route here in 3B.3B-1.
    static bool isDeveloperFeatureEnabled()
    {
        return qEnvironmentVariable("THISPC_ENABLE_FOLDER_MERGE_QUEUE_DEV") == QLatin1String("1");
    }

    static TransferEnqueueResult enqueueMerge(
        TransferQueueController *controller,
        LocalMergeQueueRequest request,
        std::function<void(LocalMergeQueueJob *)> onJobCreated = {})
    {
        if (!controller) {
            TransferEnqueueResult rej;
            rej.rejection = TransferEnqueueRejection::InvalidRequest;
            rej.diagnostic = QStringLiteral("TransferQueueController is null");
            return rej;
        }

        auto sharedReq = std::make_shared<LocalMergeQueueRequest>(std::move(request));
        QPointer<TransferQueueController> qc(controller);

        TransferTaskDescriptor desc;
        desc.title = sharedReq->title.isEmpty() ? QStringLiteral("Folder Merge") : sharedReq->title;
        desc.type = TransferOperationType::Copy;
        desc.backend = TransferBackendKind::LocalNative;
        desc.sources = sharedReq->sources;
        desc.destination = sharedReq->destinationDir;
        desc.context = sharedReq->context;
        desc.priority = sharedReq->priority;
        desc.capabilities.canCancelRunning = true;
        desc.capabilities.canPause = false;
        desc.capabilities.canResume = false;
        desc.capabilities.supportsThrottling = false;
        desc.capabilities.cancelOutcome = TransferCancelOutcome::PartialResultsMayRemain;
        desc.startMode = TransferJobStartMode::ControllerStarts;
        desc.isFolderMerge = true;

        desc.preflight = [sharedReq]() -> QString {
            const QString destPath = sharedReq->destinationDir.toLocalFile();
            const LocalMergeRecoveryReport rep = LocalMergeRecoveryInspector::inspectDirectory(destPath, false);
            if (rep.anyIncompleteFound || rep.anyManualInterventionRequired) {
                return QStringLiteral("RecoveryRequired: incomplete merge transaction in destination");
            }
            return {};
        };

        desc.jobFactory = [qc, sharedReq, onJobCreated]() -> KJob * {
            auto *job = new LocalMergeQueueJob(*sharedReq, qc, 0);
            if (onJobCreated) {
                onJobCreated(job);
            }
            return job;
        };

        desc.configureJob = [](KJob *job, quint64 liveTaskId) {
            if (auto *mergeJob = dynamic_cast<LocalMergeQueueJob *>(job)) {
                mergeJob->setQueueTaskId(liveTaskId);
            }
        };

        desc.cancelHandler = [](KJob *job) -> bool {
            if (auto *mergeJob = dynamic_cast<LocalMergeQueueJob *>(job)) {
                return mergeJob->requestCancelAsync();
            }
            return false;
        };

        return controller->enqueue(std::move(desc));
    }

    // Stage 3B.3B-2: strictly gated developer sandbox entry point for controlled GUI testing.
    static TransferEnqueueResult enqueueDevSandboxedMerge(
        TransferQueueController *controller,
        const QString &devRootPath,
        LocalMergeQueueRequest request,
        std::function<void(LocalMergeQueueJob *)> onJobCreated = {})
    {
        TransferEnqueueResult rej;
        rej.rejection = TransferEnqueueRejection::InvalidRequest;
        if (!isDeveloperFeatureEnabled()) {
            rej.diagnostic = QStringLiteral("Developer Folder Merge gate (THISPC_ENABLE_FOLDER_MERGE_QUEUE_DEV=1) is disabled");
            return rej;
        }
        if (!request.destinationDir.isValid() || !request.destinationDir.isLocalFile()) {
            rej.diagnostic = QStringLiteral("Developer Folder Merge requires a local destination directory");
            return rej;
        }
        QString sandboxReason;
        if (!LocalMergeDevSandbox::isDestinationAllowed(request.destinationDir.toLocalFile(), devRootPath, &sandboxReason)) {
            rej.diagnostic = sandboxReason;
            return rej;
        }
        for (const QUrl &src : std::as_const(request.sources)) {
            if (!src.isValid() || !src.isLocalFile()) {
                rej.diagnostic = QStringLiteral("Developer Folder Merge requires local sources");
                return rej;
            }
        }
        return enqueueMerge(controller, std::move(request), std::move(onJobCreated));
    }

    // Heap-allocated, non-blocking (QDialog::open) GUI folder conflict prompt.
    // Ownership: parented to parentWidget (if alive), tracked via QPointer, explicitly
    // deleted via deleteLater() after decision capture on finished() or on orchestrator cancel.
    // Unexpected destruction without finished() fails closed with Abort.
    static LocalMergeOrchestrator::AsyncFolderPromptFn createAsyncGuiFolderPrompt(QPointer<QWidget> parentWidget)
    {
        return [parentWidget](const LocalMergePlanEntry &entry,
                              qsizetype remainingFolderConflicts,
                              LocalMergeConflictProgress progress,
                              std::function<void(const LocalFolderConflictDecision &)> complete)
            -> LocalMergeOrchestrator::AsyncPromptCancelFn {
            auto *dlg = new LocalFolderMergeConflictDialog(
                entry,
                QSet<QString>{},
                false,
                QLocale(),
                parentWidget.data(),
                remainingFolderConflicts,
                progress);
            dlg->setWindowModality(Qt::ApplicationModal);

            QPointer<LocalFolderMergeConflictDialog> dlgPtr(dlg);
            auto settled = std::make_shared<bool>(false);
            auto sharedComplete = std::make_shared<std::function<void(const LocalFolderConflictDecision &)>>(
                std::move(complete));

            QObject::connect(dlg, &QDialog::finished, dlg, [dlgPtr, settled, sharedComplete](int) {
                if (*settled) {
                    return;
                }
                *settled = true;
                LocalFolderConflictDecision dec;
                if (dlgPtr) {
                    dec = dlgPtr->decision();
                    dlgPtr->deleteLater();
                } else {
                    dec.accepted = false;
                    dec.action = LocalMergeApprovedAction::Abort;
                }
                if (sharedComplete && *sharedComplete) {
                    (*sharedComplete)(dec);
                }
            });

            QObject::connect(dlg, &QObject::destroyed, [settled, sharedComplete]() {
                if (*settled) {
                    return;
                }
                *settled = true;
                LocalFolderConflictDecision abortDec;
                abortDec.accepted = false;
                abortDec.action = LocalMergeApprovedAction::Abort;
                if (sharedComplete && *sharedComplete) {
                    (*sharedComplete)(abortDec);
                }
            });

            // Use non-blocking show() after setWindowModality(Qt::ApplicationModal)
            // because QDialog::open() would downgrade modality to Qt::WindowModal.
            dlg->show();
            QTimer::singleShot(0, dlg, [dlgPtr] {
                if (!dlgPtr) {
                    return;
                }
                dlgPtr->raise();
                dlgPtr->activateWindow();
            });

            return [dlgPtr, settled]() {
                *settled = true;
                if (dlgPtr) {
                    dlgPtr->hide();
                    dlgPtr->reject();
                    dlgPtr->deleteLater();
                }
            };
        };
    }

    // Heap-allocated, non-blocking (QDialog::open) GUI file conflict prompt.
    // Ownership: parented to parentWidget (if alive), tracked via QPointer, explicitly
    // deleted via deleteLater() after decision capture on finished() or on orchestrator cancel.
    // Unexpected destruction without finished() fails closed with Abort.
    static LocalMergeOrchestrator::AsyncFilePromptFn createAsyncGuiFilePrompt(QPointer<QWidget> parentWidget)
    {
        return [parentWidget](const LocalMergePlanEntry &entry,
                              const LocalMergeBackendCapabilities &capabilities,
                              qsizetype remainingExactFileConflicts,
                              LocalMergeConflictProgress progress,
                              std::function<void(const LocalFileConflictDecision &)> complete)
            -> LocalMergeOrchestrator::AsyncPromptCancelFn {
            auto *dlg = new LocalFileConflictResolutionDialog(
                entry,
                capabilities,
                remainingExactFileConflicts,
                QSet<QString>{},
                QLocale(),
                parentWidget.data(),
                progress);
            dlg->setWindowModality(Qt::ApplicationModal);

            QPointer<LocalFileConflictResolutionDialog> dlgPtr(dlg);
            auto settled = std::make_shared<bool>(false);
            auto sharedComplete = std::make_shared<std::function<void(const LocalFileConflictDecision &)>>(
                std::move(complete));

            QObject::connect(dlg, &QDialog::finished, dlg, [dlgPtr, settled, sharedComplete](int) {
                if (*settled) {
                    return;
                }
                *settled = true;
                LocalFileConflictDecision dec;
                if (dlgPtr) {
                    dec = dlgPtr->decision();
                    dlgPtr->deleteLater();
                } else {
                    dec.accepted = false;
                    dec.action = LocalMergeApprovedAction::Abort;
                }
                if (sharedComplete && *sharedComplete) {
                    (*sharedComplete)(dec);
                }
            });

            QObject::connect(dlg, &QObject::destroyed, [settled, sharedComplete]() {
                if (*settled) {
                    return;
                }
                *settled = true;
                LocalFileConflictDecision abortDec;
                abortDec.accepted = false;
                abortDec.action = LocalMergeApprovedAction::Abort;
                if (sharedComplete && *sharedComplete) {
                    (*sharedComplete)(abortDec);
                }
            });

            // Use non-blocking show() after setWindowModality(Qt::ApplicationModal)
            // because QDialog::open() would downgrade modality to Qt::WindowModal.
            dlg->show();
            QTimer::singleShot(0, dlg, [dlgPtr] {
                if (!dlgPtr) {
                    return;
                }
                dlgPtr->raise();
                dlgPtr->activateWindow();
            });

            return [dlgPtr, settled]() {
                *settled = true;
                if (dlgPtr) {
                    dlgPtr->hide();
                    dlgPtr->reject();
                    dlgPtr->deleteLater();
                }
            };
        };
    }

    // Fail-closed Undo fence: raise irreversible barrier whenever Folder Merge mutates the
    // destination or terminates in a partial/uncertain/recovery state.
    static bool applyUndoFenceIfNeeded(UndoController *undo, const LocalMergeOrchestratorResult &result)
    {
        return localMergeApplyUndoFenceIfNeeded(undo, result);
    }

    static QString formatHistorySummary(const LocalMergeOrchestratorResult &result)
    {
        switch (result.finalState) {
        case LocalMergeOrchestratorState::Completed:
            return QStringLiteral("Scalanie ukończone (utworzono plików: %1, katalogów: %2)")
                .arg(result.progress.filesCreated)
                .arg(result.progress.directoriesCreated);
        case LocalMergeOrchestratorState::CompletedWithSkippedConflicts:
            return QStringLiteral("Scalanie ukończone z pominiętymi konfliktami (pominięto: %1)")
                .arg(result.progress.skippedCount + result.progress.skippedSubtreeCount);
        case LocalMergeOrchestratorState::Cancelled:
            return result.progress.partialSuccess
                ? QStringLiteral("Anulowano po częściowym scaleniu (utworzono: %1, zastąpiono: %2)")
                      .arg(result.progress.filesCreated)
                      .arg(result.progress.replacedCount)
                : QStringLiteral("Anulowano przed wprowadzeniem zmian");
        case LocalMergeOrchestratorState::FailedNoChanges:
            return QStringLiteral("Błąd scalania bez zmian w celu");
        case LocalMergeOrchestratorState::FailedPartialSuccess:
            return QStringLiteral("Częściowy sukces scalania przerwany błędem (ponowienie zablokowane)");
        case LocalMergeOrchestratorState::FailedUncertainDurability:
            return QStringLiteral("Niepewna trwałość zapisu — wymagana weryfikacja ręczna");
        case LocalMergeOrchestratorState::RecoveryRequired:
            return QStringLiteral("Wymagane odzyskiwanie niedokończonej transakcji");
        case LocalMergeOrchestratorState::NeedsReplan:
            return QStringLiteral("Wymagane ponowne planowanie po wykryciu kolizji");
        default:
            return localMergeStateName(result.finalState);
        }
    }

    static QString formatNotificationSummary(const LocalMergeOrchestratorResult &result)
    {
        return formatHistorySummary(result);
    }
};

#endif // THISPC_LOCALMERGEQUEUEBRIDGE_H
