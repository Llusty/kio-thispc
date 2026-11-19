#ifndef THISPC_LOCALMERGEORCHESTRATOR_H
#define THISPC_LOCALMERGEORCHESTRATOR_H

#include "localmergedialogs.h"
#include "localmergeexecutionjob.h"
#include "localmergeplan.h"
#include "localmergetransaction.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>

#include <sys/stat.h>

enum class LocalMergeOrchestratorState {
    Queued,
    Planning,
    AwaitingUserDecision,
    Revalidating,
    Executing,
    NeedsReplan,
    Cancelling,
    Completed,
    CompletedWithSkippedConflicts,
    FailedNoChanges,
    FailedPartialSuccess,
    FailedUncertainDurability,
    RecoveryRequired,
    Cancelled
};

inline bool localMergeStateIsTerminal(LocalMergeOrchestratorState state)
{
    switch (state) {
    case LocalMergeOrchestratorState::Completed:
    case LocalMergeOrchestratorState::CompletedWithSkippedConflicts:
    case LocalMergeOrchestratorState::FailedNoChanges:
    case LocalMergeOrchestratorState::FailedPartialSuccess:
    case LocalMergeOrchestratorState::FailedUncertainDurability:
    case LocalMergeOrchestratorState::RecoveryRequired:
    case LocalMergeOrchestratorState::Cancelled:
        return true;
    default:
        return false;
    }
}

inline QString localMergeStateName(LocalMergeOrchestratorState state)
{
    switch (state) {
    case LocalMergeOrchestratorState::Queued: return QStringLiteral("Queued");
    case LocalMergeOrchestratorState::Planning: return QStringLiteral("Planning");
    case LocalMergeOrchestratorState::AwaitingUserDecision: return QStringLiteral("AwaitingUserDecision");
    case LocalMergeOrchestratorState::Revalidating: return QStringLiteral("Revalidating");
    case LocalMergeOrchestratorState::Executing: return QStringLiteral("Executing");
    case LocalMergeOrchestratorState::NeedsReplan: return QStringLiteral("NeedsReplan");
    case LocalMergeOrchestratorState::Cancelling: return QStringLiteral("Cancelling");
    case LocalMergeOrchestratorState::Completed: return QStringLiteral("Completed");
    case LocalMergeOrchestratorState::CompletedWithSkippedConflicts: return QStringLiteral("CompletedWithSkippedConflicts");
    case LocalMergeOrchestratorState::FailedNoChanges: return QStringLiteral("FailedNoChanges");
    case LocalMergeOrchestratorState::FailedPartialSuccess: return QStringLiteral("FailedPartialSuccess");
    case LocalMergeOrchestratorState::FailedUncertainDurability: return QStringLiteral("FailedUncertainDurability");
    case LocalMergeOrchestratorState::RecoveryRequired: return QStringLiteral("RecoveryRequired");
    case LocalMergeOrchestratorState::Cancelled: return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

struct LocalMergeReservationScope {
    quint64 holderId = 0;
    QStringList sourcePaths;
    QStringList destinationPaths;
    QStringList transactionParents;
};

class LocalMergeReservationRegistry
{
public:
    struct Identity {
        quint64 dev = 0;
        quint64 ino = 0;
        bool valid = false;
    };

    struct Conflict {
        quint64 holderId = 0;
        QString reason;
    };

    static QString resolvePathForReservation(const QString &rawPath)
    {
        if (rawPath.isEmpty()) {
            return {};
        }
        const QString cleaned = QDir::cleanPath(rawPath);
        QFileInfo info(cleaned);
        if (info.exists() || info.isSymLink()) {
            const QString canon = info.canonicalFilePath();
            if (!canon.isEmpty()) {
                return canon;
            }
        }
        // Walk up to deepest existing ancestor to resolve symlinks on parent
        QStringList tailParts;
        QString probe = cleaned;
        while (!probe.isEmpty() && probe != QLatin1String("/")) {
            QFileInfo probeInfo(probe);
            if (probeInfo.exists()) {
                const QString canon = probeInfo.canonicalFilePath();
                if (!canon.isEmpty()) {
                    QString joined = canon;
                    for (auto it = tailParts.rbegin(); it != tailParts.rend(); ++it) {
                        if (!joined.endsWith(QLatin1Char('/'))) {
                            joined.append(QLatin1Char('/'));
                        }
                        joined.append(*it);
                    }
                    return QDir::cleanPath(joined);
                }
                break;
            }
            const QString fileName = probeInfo.fileName();
            if (fileName.isEmpty()) {
                break;
            }
            tailParts.append(fileName);
            const QString parent = probeInfo.path();
            if (parent == probe) {
                break;
            }
            probe = parent;
        }
        return cleaned;
    }

    static Identity statIdentity(const QString &path)
    {
        struct stat st {};
        if (::stat(QFile::encodeName(path).constData(), &st) == 0) {
            return Identity{static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino), true};
        }
        return {};
    }

    static bool pathsOverlap(const QString &rawA, const QString &rawB)
    {
        const QString a = resolvePathForReservation(rawA);
        const QString b = resolvePathForReservation(rawB);
        if (a.isEmpty() || b.isEmpty()) {
            return false;
        }
        if (a == b) {
            return true;
        }
        const QString aSlash = a.endsWith(QLatin1Char('/')) ? a : (a + QLatin1Char('/'));
        const QString bSlash = b.endsWith(QLatin1Char('/')) ? b : (b + QLatin1Char('/'));
        if (a.startsWith(bSlash) || b.startsWith(aSlash)) {
            return true;
        }
        const Identity idA = statIdentity(a);
        const Identity idB = statIdentity(b);
        if (idA.valid && idB.valid && idA.dev == idB.dev && idA.ino == idB.ino) {
            return true;
        }
        return false;
    }

    std::optional<Conflict> findConflict(const LocalMergeReservationScope &request) const
    {
        QList<quint64> ids = m_scopes.keys();
        std::sort(ids.begin(), ids.end());
        for (quint64 id : std::as_const(ids)) {
            if (id == request.holderId) {
                continue;
            }
            const LocalMergeReservationScope &held = m_scopes.value(id);
            // Destination vs Destination
            for (const QString &reqDst : request.destinationPaths) {
                for (const QString &heldDst : held.destinationPaths) {
                    if (pathsOverlap(reqDst, heldDst)) {
                        return Conflict{id, QStringLiteral("Destination tree conflict: %1 vs %2").arg(reqDst, heldDst)};
                    }
                }
                // Destination vs Held Source (reading while another writes)
                for (const QString &heldSrc : held.sourcePaths) {
                    if (pathsOverlap(reqDst, heldSrc)) {
                        return Conflict{id, QStringLiteral("Destination overlaps active source: %1 vs %2").arg(reqDst, heldSrc)};
                    }
                }
            }
            // Source vs Held Destination
            for (const QString &reqSrc : request.sourcePaths) {
                for (const QString &heldDst : held.destinationPaths) {
                    if (pathsOverlap(reqSrc, heldDst)) {
                        return Conflict{id, QStringLiteral("Source overlaps active destination: %1 vs %2").arg(reqSrc, heldDst)};
                    }
                }
            }
            // Transaction parent scopes (.thispc-tx.*)
            for (const QString &reqTx : request.transactionParents) {
                for (const QString &heldTx : held.transactionParents) {
                    if (pathsOverlap(reqTx, heldTx)) {
                        return Conflict{id, QStringLiteral("Transaction scope conflict: %1 vs %2").arg(reqTx, heldTx)};
                    }
                }
            }
        }
        return std::nullopt;
    }

    bool tryAcquire(const LocalMergeReservationScope &scope, QString *reasonOut = nullptr)
    {
        if (scope.holderId == 0 || m_scopes.contains(scope.holderId)) {
            if (reasonOut) {
                *reasonOut = QStringLiteral("Invalid or duplicate reservation holder");
            }
            return false;
        }
        if (const auto conflict = findConflict(scope)) {
            if (reasonOut) {
                *reasonOut = conflict->reason;
            }
            return false;
        }
        m_scopes.insert(scope.holderId, scope);
        return true;
    }

    bool updateScope(const LocalMergeReservationScope &scope, QString *reasonOut = nullptr)
    {
        if (scope.holderId == 0 || !m_scopes.contains(scope.holderId)) {
            return false;
        }
        if (const auto conflict = findConflict(scope)) {
            if (reasonOut) {
                *reasonOut = conflict->reason;
            }
            return false;
        }
        m_scopes.insert(scope.holderId, scope);
        return true;
    }

    bool release(quint64 holderId)
    {
        return m_scopes.remove(holderId) > 0;
    }

    bool holds(quint64 holderId) const
    {
        return m_scopes.contains(holderId);
    }

    int holderCount() const
    {
        return static_cast<int>(m_scopes.size());
    }

private:
    QHash<quint64, LocalMergeReservationScope> m_scopes;
};

struct LocalMergeOrchestratorProgress {
    LocalMergeOrchestratorState state = LocalMergeOrchestratorState::Queued;
    QString phase = QStringLiteral("queued");
    int filesPlanned = 0;
    int filesCreated = 0;
    int directoriesCreated = 0;
    int existingDirectoriesMerged = 0;
    int conflictsCount = 0;
    int skippedCount = 0;
    int replacedCount = 0;
    int skippedSubtreeCount = 0;
    int errorCount = 0;
    quint64 bytesPlanned = 0;
    quint64 bytesProcessed = 0;
    bool partialSuccess = false;
    bool manualInterventionRequired = false;
    bool retryAllowed = false;
};

struct LocalMergeOrchestratorUndoSummary {
    bool reversible = false;
    bool requiresIrreversibleSerial = false;
    bool existingDirectoriesNeverDelete = true;
    QStringList createdFiles;
    QStringList createdDirectories;
    QString undoBlockedReason;
};

struct LocalMergeOrchestratorResult {
    LocalMergeOrchestratorState finalState = LocalMergeOrchestratorState::Queued;
    LocalMergeOrchestratorProgress progress;
    LocalMergeOrchestratorUndoSummary undoSummary;
    LocalMergeRecoveryReport recoveryReport;
    LocalMergeExecutionReport executionReport;
    QString diagnostic;
    int replanRounds = 0;
    bool duplicateExecutionBlocked = false;
    QStringList externallyModifiedCompletedEntries;
    bool freshDecisionRequired = false;
};

class LocalMergeOrchestrator : public QObject
{
public:
    using FolderPromptFn = LocalMergeConflictResolver::FolderPromptFn;
    using FilePromptFn = LocalMergeConflictResolver::FilePromptFn;

    LocalMergeOrchestrator(const QList<QUrl> &sources,
                           const QUrl &destinationDir,
                           LocalMergeReservationRegistry *registry = nullptr,
                           quint64 holderId = 1,
                           const LocalMergeBackendCapabilities &capabilities = {},
                           QObject *parent = nullptr)
        : QObject(parent)
        , m_sources(sources)
        , m_destinationDir(destinationDir)
        , m_registry(registry)
        , m_holderId(holderId)
        , m_capabilities(capabilities)
    {
        m_progress.state = LocalMergeOrchestratorState::Queued;
        m_progress.phase = QStringLiteral("queued");
    }

    ~LocalMergeOrchestrator() override
    {
        releaseReservation();
    }

    using DynamicTargetReservationFn = std::function<bool(const QUrl &targetUrl, QString *reasonOut)>;
    using PreIrreversibleWriteHook = std::function<void()>;

    void setFolderPrompt(FolderPromptFn fn) { m_folderPrompt = std::move(fn); }
    void setFilePrompt(FilePromptFn fn) { m_filePrompt = std::move(fn); }
    void setStateChangedCallback(std::function<void(LocalMergeOrchestratorState)> cb) { m_onStateChanged = std::move(cb); }
    void setProgressChangedCallback(std::function<void(const LocalMergeOrchestratorProgress &)> cb) { m_onProgressChanged = std::move(cb); }
    void setFinishedCallback(std::function<void(const LocalMergeOrchestratorResult &)> cb) { m_onFinished = std::move(cb); }
    void setExecutionJobConfigurator(std::function<void(LocalMergeExecutionJob *)> cb) { m_jobConfigurator = std::move(cb); }
    void setDynamicTargetReservationFn(DynamicTargetReservationFn fn) { m_dynamicTargetReservationFn = std::move(fn); }
    void setPreIrreversibleWriteHook(PreIrreversibleWriteHook fn) { m_preIrreversibleWriteHook = std::move(fn); }
    void setForceRecoveryRequired(bool force) { m_forceRecoveryRequired = force; }
    void setMaxReplanRounds(int maxRounds) { m_maxReplanRounds = qMax(1, maxRounds); }

    LocalMergeOrchestratorState state() const { return m_state; }
    const LocalMergeOrchestratorProgress &progress() const { return m_progress; }
    const LocalMergeOrchestratorResult &result() const { return m_result; }
    bool isFinished() const { return localMergeStateIsTerminal(m_state); }
    quint64 holderId() const { return m_holderId; }
    const QList<LocalMergeOrchestratorState> &stateHistory() const { return m_stateHistory; }

    void start()
    {
        if (m_started || localMergeStateIsTerminal(m_state)) {
            m_result.duplicateExecutionBlocked = true;
            return;
        }
        m_started = true;
        QPointer<LocalMergeOrchestrator> self(this);
        QTimer::singleShot(0, this, [self] {
            if (!self) {
                return;
            }
            self->beginPipeline();
        });
    }

    bool executePreparedPlan()
    {
        if (m_hasCompletedExecution || localMergeStateIsTerminal(m_state)) {
            m_result.duplicateExecutionBlocked = true;
            return false;
        }
        return true;
    }

    void cancel()
    {
        if (localMergeStateIsTerminal(m_state) || m_cancelRequested) {
            return;
        }
        m_cancelRequested = true;
        setState(LocalMergeOrchestratorState::Cancelling, QStringLiteral("cancelling"));
        if (m_activePlanJob) {
            if (!m_activePlanJob->hasFinished()) {
                m_activePlanJob->requestCancelAsync();
                return;
            }
            m_activePlanJob = nullptr;
        }
        if (m_activeExecJob) {
            m_activeExecJob->requestSafeCancel();
            return;
        }
        finishCancelled(QStringLiteral("Cancelled before execution"));
    }

    LocalMergeOrchestratorResult snapshotResultForCancel() const
    {
        LocalMergeOrchestratorResult snap = m_result;
        snap.finalState = LocalMergeOrchestratorState::Cancelled;
        snap.progress = m_progress;
        snap.progress.state = LocalMergeOrchestratorState::Cancelled;
        int curFiles = 0;
        int curDirs = 0;
        int curReplaced = 0;
        if (m_activeExecJob) {
            const LocalMergeExecutionReport &rep = m_activeExecJob->report();
            curFiles = static_cast<int>(rep.filesCreated.size() + rep.symlinksCreated.size());
            curDirs = static_cast<int>(rep.directoriesCreated.size());
            curReplaced = static_cast<int>(rep.filesOverwritten.size());
        }
        snap.progress.filesCreated += curFiles;
        snap.progress.directoriesCreated += curDirs;
        snap.progress.replacedCount += curReplaced;
        const bool wroteAnything = (snap.progress.filesCreated > 0
                                    || snap.progress.directoriesCreated > 0
                                    || snap.progress.replacedCount > 0);
        snap.progress.partialSuccess = wroteAnything;
        snap.undoSummary.requiresIrreversibleSerial = wroteAnything;
        return snap;
    }

private:
    static QString entryKey(const LocalMergePlanEntry &entry)
    {
        return entry.sourcePath + QStringLiteral("\n->\n") + entry.destinationPath;
    }

    static QByteArray computeFileSha256(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return {};
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        char buf[65536];
        while (!f.atEnd()) {
            const qint64 n = f.read(buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            hash.addData(QByteArrayView(buf, n));
        }
        return hash.result();
    }

    LocalMergeReservationScope buildInitialScope() const
    {
        LocalMergeReservationScope scope;
        scope.holderId = m_holderId;
        for (const QUrl &src : m_sources) {
            if (src.isLocalFile()) {
                scope.sourcePaths.append(src.toLocalFile());
            }
        }
        const QString destPath = m_destinationDir.toLocalFile();
        if (!destPath.isEmpty()) {
            scope.destinationPaths.append(destPath);
            scope.transactionParents.append(destPath);
        }
        return scope;
    }

    void releaseReservation()
    {
        if (m_registry && m_reservationAcquired) {
            m_registry->release(m_holderId);
            m_reservationAcquired = false;
        }
    }

    void setState(LocalMergeOrchestratorState next, const QString &phase)
    {
        if (m_state != next) {
            m_stateHistory.append(next);
        }
        m_state = next;
        m_progress.state = next;
        m_progress.phase = phase;
        if (m_onStateChanged) {
            m_onStateChanged(next);
        }
        if (m_onProgressChanged) {
            m_onProgressChanged(m_progress);
        }
    }

    void emitProgress()
    {
        if (m_onProgressChanged) {
            m_onProgressChanged(m_progress);
        }
    }

    void finalizeUndoSummary()
    {
        LocalMergeOrchestratorUndoSummary undo;
        undo.existingDirectoriesNeverDelete = true;
        undo.createdFiles = m_cumulativeCreatedFiles;
        undo.createdDirectories = m_cumulativeCreatedDirs;

        if (m_progress.replacedCount > 0) {
            undo.reversible = false;
            undo.requiresIrreversibleSerial = true;
            undo.undoBlockedReason = QStringLiteral("Overwrite replaced existing file(s)");
        } else if (m_state == LocalMergeOrchestratorState::FailedUncertainDurability) {
            undo.reversible = false;
            undo.requiresIrreversibleSerial = true;
            undo.undoBlockedReason = QStringLiteral("Uncertain filesystem durability after I/O error");
        } else if (m_state == LocalMergeOrchestratorState::RecoveryRequired) {
            undo.reversible = false;
            undo.requiresIrreversibleSerial = true;
            undo.undoBlockedReason = QStringLiteral("Incomplete transaction requires manual recovery");
        } else if (m_progress.partialSuccess && (m_state == LocalMergeOrchestratorState::FailedPartialSuccess
                                               || m_state == LocalMergeOrchestratorState::Cancelled)) {
            undo.reversible = false;
            undo.requiresIrreversibleSerial = true;
            undo.undoBlockedReason = QStringLiteral("Partial merge requires irreversible serial fence");
        } else if (m_state == LocalMergeOrchestratorState::Completed
                   || m_state == LocalMergeOrchestratorState::CompletedWithSkippedConflicts) {
            undo.reversible = true;
            undo.requiresIrreversibleSerial = false;
        } else {
            undo.reversible = false;
            undo.requiresIrreversibleSerial = false;
        }
        m_result.undoSummary = undo;
    }

    void finishTerminal(LocalMergeOrchestratorState terminalState, const QString &diagnostic = QString())
    {
        releaseReservation();
        m_hasCompletedExecution = true;
        const bool wroteAnything = (m_progress.filesCreated > 0
                                    || m_progress.directoriesCreated > 0
                                    || m_progress.replacedCount > 0);
        const bool isCompletedOutcome = (terminalState == LocalMergeOrchestratorState::Completed
                                         || terminalState == LocalMergeOrchestratorState::CompletedWithSkippedConflicts);
        m_progress.partialSuccess = !isCompletedOutcome && wroteAnything;
        setState(terminalState, QStringLiteral("terminal"));
        finalizeUndoSummary();
        m_result.finalState = terminalState;
        m_result.progress = m_progress;
        m_result.diagnostic = diagnostic;
        m_result.replanRounds = m_replanRounds;
        if (m_onFinished) {
            m_onFinished(m_result);
        }
    }

    void finishCancelled(const QString &reason)
    {
        m_progress.retryAllowed = !(m_progress.filesCreated > 0
                                    || m_progress.directoriesCreated > 0
                                    || m_progress.replacedCount > 0);
        finishTerminal(LocalMergeOrchestratorState::Cancelled, reason);
    }

    void beginPipeline()
    {
        if (m_cancelRequested) {
            finishCancelled(QStringLiteral("Cancelled while queued"));
            return;
        }

        if (m_registry) {
            QString conflictReason;
            if (!m_registry->tryAcquire(buildInitialScope(), &conflictReason)) {
                m_progress.errorCount += 1;
                m_progress.retryAllowed = true;
                finishTerminal(LocalMergeOrchestratorState::FailedNoChanges,
                               conflictReason.isEmpty() ? QStringLiteral("Destination/source reservation conflict") : conflictReason);
                return;
            }
            m_reservationAcquired = true;
        }

        // Pre-flight recovery inspection before planning/mutating
        const QString destPath = m_destinationDir.toLocalFile();
        m_result.recoveryReport = LocalMergeRecoveryInspector::inspectDirectory(destPath, false);
        if (m_forceRecoveryRequired
            || m_result.recoveryReport.anyIncompleteFound
            || m_result.recoveryReport.anyManualInterventionRequired) {
            m_progress.manualInterventionRequired = true;
            m_progress.retryAllowed = false;
            m_progress.errorCount += 1;
            QString diag;
            if (!m_result.recoveryReport.findings.isEmpty()) {
                diag = m_result.recoveryReport.findings.first().diagnostic;
            }
            if (diag.isEmpty()) {
                diag = QStringLiteral("Incomplete merge transaction detected in destination");
            }
            finishTerminal(LocalMergeOrchestratorState::RecoveryRequired, diag);
            return;
        }

        startPlanningRound();
    }

    void startPlanningRound()
    {
        if (m_cancelRequested) {
            finishCancelled(QStringLiteral("Cancelled before planning"));
            return;
        }
        setState(LocalMergeOrchestratorState::Planning, QStringLiteral("planning"));

        auto *planJob = new LocalMergePlanJob(m_sources, m_destinationDir, this);
        planJob->setAutoDelete(false);
        m_activePlanJob = planJob;
        QPointer<LocalMergeOrchestrator> self(this);
        connect(planJob, &KJob::result, this, [self, planJob](KJob *) {
            if (!self) {
                return;
            }
            self->m_activePlanJob = nullptr;
            const auto err = planJob->planError();
            LocalMergePlan plan = planJob->plan();
            const QString errStr = planJob->errorString();
            planJob->deleteLater();

            if (self->m_cancelRequested) {
                self->finishCancelled(QStringLiteral("Cancelled during planning"));
                return;
            }
            if (err != LocalMergePlanError::None) {
                self->m_progress.errorCount += 1;
                const bool hasPartial = (self->m_progress.filesCreated > 0
                                         || self->m_progress.directoriesCreated > 0
                                         || self->m_progress.replacedCount > 0);
                self->m_progress.retryAllowed = !hasPartial;
                self->finishTerminal(hasPartial ? LocalMergeOrchestratorState::FailedPartialSuccess
                                                : LocalMergeOrchestratorState::FailedNoChanges,
                                     errStr.isEmpty() ? QStringLiteral("Planning failed") : errStr);
                return;
            }

            self->onPlanReady(std::move(plan));
        });
        planJob->start();
    }

    void onPlanReady(LocalMergePlan plan)
    {
        // Stage 3B.3B-1 Scoped Crash Recovery Gate: inspect every unique existing target directory
        // in the plan scope without following symlinks and without scanning unrelated disk trees.
        {
            QSet<QPair<quint64, quint64>> visitedDirIds;
            auto checkDirRecovery = [&](const QString &dirPath) -> bool {
                if (dirPath.isEmpty()) {
                    return true;
                }
                struct stat st {};
                if (::lstat(QFile::encodeName(dirPath).constData(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                    return true;  // Missing or symlink: do not follow symlinks
                }
                const QPair<quint64, quint64> id(static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino));
                if (visitedDirIds.contains(id)) {
                    return true;
                }
                visitedDirIds.insert(id);
                const LocalMergeRecoveryReport subRep = LocalMergeRecoveryInspector::inspectDirectory(dirPath, false);
                if (subRep.anyIncompleteFound || subRep.anyManualInterventionRequired) {
                    m_result.recoveryReport = subRep;
                    m_progress.manualInterventionRequired = true;
                    m_progress.retryAllowed = false;
                    m_progress.errorCount += 1;
                    QString diag = subRep.findings.isEmpty() ? QString() : subRep.findings.first().diagnostic;
                    if (diag.isEmpty()) {
                        diag = QStringLiteral("Incomplete merge transaction detected in target subtree: %1").arg(dirPath);
                    }
                    finishTerminal(LocalMergeOrchestratorState::RecoveryRequired, diag);
                    return false;
                }
                return true;
            };

            if (!checkDirRecovery(m_destinationDir.toLocalFile())) {
                return;
            }
            for (const auto &entry : std::as_const(plan.entries)) {
                if (entry.destinationKind == LocalMergeObjectKind::Directory) {
                    if (!checkDirRecovery(entry.destinationPath)) {
                        return;
                    }
                }
                const QString parentDir = QFileInfo(entry.destinationPath).path();
                if (!checkDirRecovery(parentDir)) {
                    return;
                }
            }
        }

        if (plan.hasBlockingErrors) {
            m_progress.errorCount += 1;
            const bool hasPartial = (m_progress.filesCreated > 0
                                     || m_progress.directoriesCreated > 0
                                     || m_progress.replacedCount > 0);
            m_progress.retryAllowed = !hasPartial;
            const QString reason = !plan.blockingReason.isEmpty()
                ? plan.blockingReason
                : QStringLiteral("Merge plan contains blocking errors");
            finishTerminal(hasPartial ? LocalMergeOrchestratorState::FailedPartialSuccess
                                      : LocalMergeOrchestratorState::FailedNoChanges,
                           reason);
            return;
        }

        m_revalidatedPreApprovedDirs.clear();
        if (m_replanRounds == 0) {
            m_progress.filesPlanned = 0;
            m_progress.bytesPlanned = 0;
            m_progress.conflictsCount = 0;
            m_progress.existingDirectoriesMerged = 0;
            for (const auto &entry : std::as_const(plan.entries)) {
                if (entry.sourceKind == LocalMergeObjectKind::RegularFile
                    || entry.sourceKind == LocalMergeObjectKind::SymbolicLink) {
                    m_progress.filesPlanned += 1;
                    if (entry.sourceKind == LocalMergeObjectKind::RegularFile) {
                        m_progress.bytesPlanned += static_cast<quint64>(entry.size);
                    }
                }
                if (entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate) {
                    m_progress.existingDirectoriesMerged += 1;
                }
                if (entry.classification == LocalMergeEntryClassification::FileConflict
                    || entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate
                    || entry.classification == LocalMergeEntryClassification::TypeConflictFileOverDir
                    || entry.classification == LocalMergeEntryClassification::TypeConflictDirOverFile
                    || entry.classification == LocalMergeEntryClassification::DestinationSymlinkConflict
                    || entry.classification == LocalMergeEntryClassification::BrokenSymlink
                    || entry.classification == LocalMergeEntryClassification::UnsupportedSpecial
                    || entry.classification == LocalMergeEntryClassification::CaseInsensitiveCollision) {
                    m_progress.conflictsCount += 1;
                }
            }
        } else {
            // 1. Revalidate previously completed/created/renamed directory contexts against live FS.
            const QString cleanDestRoot = QDir::cleanPath(m_destinationDir.toLocalFile());
            const QString destRootSlash = cleanDestRoot.endsWith(QLatin1Char('/'))
                ? cleanDestRoot
                : (cleanDestRoot + QLatin1Char('/'));
            QStringList invalidDirKeys;
            for (auto it = m_completedDirectories.constBegin(); it != m_completedDirectories.constEnd(); ++it) {
                const CompletedDirectoryContext &ctx = it.value();
                bool ok = true;
                LocalMergeExecDetail::UniqueFd srcFd = LocalMergeExecDetail::openDirectoryNoSymlinks(ctx.cleanSourcePath);
                LocalMergeExecDetail::UniqueFd dstFd = LocalMergeExecDetail::openDirectoryNoSymlinks(ctx.effectiveDestinationPath);
                if (!srcFd.valid()
                    || !LocalMergeExecDetail::verifyFdIdentity(srcFd.fd, ctx.sourceStat, ctx.sourceStatValid)
                    || !dstFd.valid()
                    || !LocalMergeExecDetail::verifyFdIdentity(dstFd.fd, ctx.destStat, ctx.destStatValid)) {
                    ok = false;
                }
                if (ok && ctx.effectiveDestinationPath != cleanDestRoot
                    && !ctx.effectiveDestinationPath.startsWith(destRootSlash)) {
                    ok = false;
                }
                if (!ok) {
                    invalidDirKeys.append(it.key());
                    m_result.externallyModifiedCompletedEntries.append(ctx.effectiveDestinationPath);
                    m_result.freshDecisionRequired = true;
                }
            }
            for (const QString &k : std::as_const(invalidDirKeys)) {
                m_completedDirectories.remove(k);
                QStringList recordKeysToDrop;
                for (auto rit = m_completedEntryRecords.constBegin(); rit != m_completedEntryRecords.constEnd(); ++rit) {
                    if (rit.key().startsWith(k + QLatin1Char('\n'))
                        || rit.key().startsWith(k + QLatin1Char('/'))) {
                        recordKeysToDrop.append(rit.key());
                    }
                }
                for (const QString &rk : recordKeysToDrop) {
                    m_completedEntryRecords.remove(rk);
                }
            }

            // Collect revalidated Rename directory contexts sorted longest-prefix first for descendant remapping
            QList<CompletedDirectoryContext> renamedDirs;
            for (const auto &ctx : std::as_const(m_completedDirectories)) {
                if (ctx.action == LocalMergeApprovedAction::Rename) {
                    renamedDirs.append(ctx);
                }
                LocalMergeAnchoredDirectory anchored;
                anchored.relativePath = ctx.effectiveRelativePath;
                anchored.sourcePath = ctx.cleanSourcePath;
                anchored.destinationPath = ctx.effectiveDestinationPath;
                anchored.sourceRootPath = ctx.sourceRootPath;
                anchored.expectedSourceStat = ctx.sourceStat;
                anchored.expectedSourceValid = ctx.sourceStatValid;
                anchored.expectedDestStat = ctx.destStat;
                anchored.expectedDestValid = ctx.destStatValid;
                m_revalidatedPreApprovedDirs.append(anchored);
            }
            std::sort(renamedDirs.begin(), renamedDirs.end(),
                      [](const CompletedDirectoryContext &a, const CompletedDirectoryContext &b) {
                          return a.cleanSourcePath.size() > b.cleanSourcePath.size();
                      });

            auto remapUnderRenamedDir = [&](LocalMergePlanEntry &entry) {
                if (renamedDirs.isEmpty()) return;
                const QString cleanEntrySrc = QDir::cleanPath(entry.sourcePath);
                for (const auto &rdir : std::as_const(renamedDirs)) {
                    if (cleanEntrySrc == rdir.cleanSourcePath) {
                        entry.relativePath = rdir.effectiveRelativePath;
                        entry.destinationPath = rdir.effectiveDestinationPath;
                        return;
                    }
                    if (cleanEntrySrc.startsWith(rdir.cleanSourcePath + QLatin1Char('/'))) {
                        const QString tail = cleanEntrySrc.mid(rdir.cleanSourcePath.size() + 1);
                        entry.relativePath = rdir.effectiveRelativePath + QLatin1Char('/') + tail;
                        entry.destinationPath = QDir::cleanPath(rdir.effectiveDestinationPath + QLatin1Char('/') + tail);
                        // Re-inspect live destination under renamed subtree if not yet completed
                        if (!m_completedEntryRecords.contains(entryKey(entry))) {
                            struct stat liveDst {};
                            bool liveDstValid = false;
                            bool liveDstBroken = false;
                            int liveErr = 0;
                            bool liveUnreadable = false;
                            entry.destinationKind = LocalMergeDetail::classifyFromLstat(
                                entry.destinationPath, &liveDst, &liveDstValid,
                                &liveDstBroken, &liveErr, &liveUnreadable);
                            entry.destinationStat = liveDst;
                            entry.destinationStatValid = liveDstValid;
                            entry.destinationIsBrokenSymlink = liveDstBroken;
                            entry.destinationSize = (entry.destinationKind == LocalMergeObjectKind::RegularFile && liveDstValid)
                                ? static_cast<qulonglong>(liveDst.st_size)
                                : 0;
                            if (entry.destinationKind == LocalMergeObjectKind::Missing) {
                                entry.classification = (entry.sourceKind == LocalMergeObjectKind::SymbolicLink && entry.sourceIsBrokenSymlink)
                                    ? LocalMergeEntryClassification::BrokenSymlink
                                    : LocalMergeEntryClassification::CreateNew;
                                entry.pendingDecision = LocalMergePendingDecision::NoneNeeded;
                            } else if (entry.sourceKind == LocalMergeObjectKind::Directory
                                       && entry.destinationKind == LocalMergeObjectKind::Directory) {
                                entry.classification = LocalMergeEntryClassification::MergeDirectoryCandidate;
                                entry.caseStatus = LocalMergeCaseStatus::ExactNameMatch;
                                entry.pendingDecision = LocalMergePendingDecision::AwaitUserFolderDecision;
                            } else if (entry.sourceKind == LocalMergeObjectKind::RegularFile
                                       && entry.destinationKind == LocalMergeObjectKind::RegularFile) {
                                entry.classification = LocalMergeEntryClassification::FileConflict;
                                entry.caseStatus = LocalMergeCaseStatus::ExactNameMatch;
                                entry.pendingDecision = LocalMergePendingDecision::AwaitUserFileDecision;
                            } else if (entry.destinationKind == LocalMergeObjectKind::Unreadable) {
                                entry.classification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
                                entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
                            } else if (entry.destinationKind == LocalMergeObjectKind::Special) {
                                entry.classification = LocalMergeEntryClassification::UnsupportedSpecial;
                                entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
                            } else if (entry.destinationKind == LocalMergeObjectKind::SymbolicLink) {
                                entry.classification = liveDstBroken
                                    ? LocalMergeEntryClassification::BrokenSymlink
                                    : LocalMergeEntryClassification::DestinationSymlinkConflict;
                                entry.pendingDecision = LocalMergePendingDecision::AwaitUserAmbiguousDecision;
                            } else if ((entry.sourceKind == LocalMergeObjectKind::RegularFile
                                        || entry.sourceKind == LocalMergeObjectKind::SymbolicLink)
                                       && entry.destinationKind == LocalMergeObjectKind::Directory) {
                                entry.classification = LocalMergeEntryClassification::TypeConflictFileOverDir;
                                entry.pendingDecision = LocalMergePendingDecision::AwaitUserAmbiguousDecision;
                            } else if (entry.sourceKind == LocalMergeObjectKind::Directory
                                       && entry.destinationKind == LocalMergeObjectKind::RegularFile) {
                                entry.classification = LocalMergeEntryClassification::TypeConflictDirOverFile;
                                entry.pendingDecision = LocalMergePendingDecision::AwaitUserAmbiguousDecision;
                            } else if (entry.sourceKind == LocalMergeObjectKind::SymbolicLink
                                       && entry.destinationKind == LocalMergeObjectKind::RegularFile) {
                                entry.classification = LocalMergeEntryClassification::AmbiguousOrUnreadableMetadata;
                                entry.pendingDecision = LocalMergePendingDecision::RefusedBlocked;
                            }
                        }
                        return;
                    }
                }
            };

            // Filter out entries already completed in prior rounds using stable source + published
            // destination identity (dev, ino, size, SHA-256 / symlink target) so completed Overwrites and
            // transfers are never re-prompted or re-executed, AND any external modification is detected!
            QList<LocalMergePlanEntry> remaining;
            remaining.reserve(plan.entries.size());
            for (LocalMergePlanEntry &entry : plan.entries) {
                remapUnderRenamedDir(entry);
                const QString key = entryKey(entry);
                if (m_completedEntryRecords.contains(key)) {
                    const CompletedEntryRecord &rec = m_completedEntryRecords.value(key);
                    const bool sourceMatches = (!rec.sourceStatValid || !entry.sourceStatValid)
                        || (rec.sourceDev == static_cast<quint64>(entry.sourceStat.st_dev)
                            && rec.sourceIno == static_cast<quint64>(entry.sourceStat.st_ino));
                    bool destMatches = true;
                    if (rec.action == LocalMergeApprovedAction::Overwrite
                        || rec.action == LocalMergeApprovedAction::Create
                        || rec.action == LocalMergeApprovedAction::Rename
                        || rec.action == LocalMergeApprovedAction::MergeDirectory) {
                        struct stat dstSt {};
                        if (::lstat(QFile::encodeName(rec.effectiveDestinationPath).constData(), &dstSt) != 0) {
                            destMatches = false;
                        } else if (rec.destStatValid) {
                            const bool statMatch = (static_cast<quint64>(dstSt.st_dev) == rec.destDev
                                                    && static_cast<quint64>(dstSt.st_ino) == rec.destIno);
                            destMatches = S_ISDIR(dstSt.st_mode)
                                ? (statMatch && S_ISDIR(dstSt.st_mode))
                                : (statMatch && static_cast<qint64>(dstSt.st_size) == rec.destSize);
                            if (destMatches && !rec.destSha256.isEmpty() && S_ISREG(dstSt.st_mode)) {
                                destMatches = (computeFileSha256(rec.effectiveDestinationPath) == rec.destSha256);
                            }
                            if (destMatches && !rec.destSymlinkTarget.isEmpty() && S_ISLNK(dstSt.st_mode)) {
                                destMatches = (readLocalLink(rec.effectiveDestinationPath) == rec.destSymlinkTarget);
                            }
                        }
                    }
                    if (sourceMatches && destMatches) {
                        continue;
                    }
                    // External modification or inode replacement detected on a previously completed entry!
                    m_result.externallyModifiedCompletedEntries.append(rec.effectiveDestinationPath);
                    m_result.freshDecisionRequired = true;
                    m_completedEntryRecords.remove(key);
                    m_completedDirectories.remove(QDir::cleanPath(entry.sourcePath));
                    m_entriesRequiringFreshDecision.insert(key);
                    entry.diagnostic = QStringLiteral("Previously completed destination changed externally; fresh user decision required");
                }
                remaining.append(entry);
            }
            plan.entries = remaining;
        }

        emitProgress();
        setState(LocalMergeOrchestratorState::AwaitingUserDecision, QStringLiteral("resolving"));

        m_pendingPlan = std::move(plan);
        QPointer<LocalMergeOrchestrator> self(this);
        QTimer::singleShot(0, this, [self] {
            if (!self) {
                return;
            }
            self->resolveConflictsOnMainLoop();
        });
    }

    void resolveConflictsOnMainLoop()
    {
        if (m_cancelRequested) {
            finishCancelled(QStringLiteral("Cancelled while awaiting decision"));
            return;
        }

        FolderPromptFn safeFolderPrompt = m_folderPrompt;
        if (!safeFolderPrompt) {
            safeFolderPrompt = [](const LocalMergePlanEntry &entry, qsizetype, LocalMergeConflictProgress) {
                LocalFolderConflictDecision d;
                if (entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate) {
                    d.accepted = true;
                    d.action = LocalMergeApprovedAction::MergeDirectory;
                } else {
                    d.accepted = false;
                    d.action = LocalMergeApprovedAction::Abort;
                }
                return d;
            };
        }
        FilePromptFn safeFilePrompt = m_filePrompt;
        if (!safeFilePrompt) {
            safeFilePrompt = [](const LocalMergePlanEntry &, const LocalMergeBackendCapabilities &, qsizetype, LocalMergeConflictProgress) {
                LocalFileConflictDecision d;
                d.accepted = false;
                d.action = LocalMergeApprovedAction::Abort;
                return d;
            };
        }

        LocalMergePlan originalPlan = m_pendingPlan;
        m_roundOriginalRelBySourcePath.clear();
        for (const auto &e : std::as_const(originalPlan.entries)) {
            if (e.sourceKind == LocalMergeObjectKind::Directory) {
                m_roundOriginalRelBySourcePath.insert(QDir::cleanPath(e.sourcePath), e.relativePath);
            }
        }
        const auto res = LocalMergeConflictResolver::resolve(
            m_pendingPlan,
            m_capabilities,
            safeFolderPrompt,
            safeFilePrompt);

        if (m_cancelRequested || res.cancelled) {
            finishCancelled(QStringLiteral("Cancelled by user during conflict resolution"));
            return;
        }

        setState(LocalMergeOrchestratorState::Revalidating, QStringLiteral("revalidating"));

        // Bind and revalidate all approved decisions onto originalPlan using Stage 2B/3B.2 helpers
        approveAdditiveMergePlan(originalPlan);
        for (qsizetype i = 0; i < m_pendingPlan.entries.size() && i < originalPlan.entries.size(); ++i) {
            const LocalMergePlanEntry &resolved = m_pendingPlan.entries.at(i);
            const LocalMergePlanEntry &orig = originalPlan.entries.at(i);
            if (resolved.approvedAction == LocalMergeApprovedAction::Create
                && orig.sourceKind == LocalMergeObjectKind::SymbolicLink) {
                if (orig.approvedAction != LocalMergeApprovedAction::Create) {
                    approveSymlinkCreate(originalPlan, orig.relativePath);
                }
            } else if (resolved.approvedAction == LocalMergeApprovedAction::Skip) {
                if (orig.sourceKind == LocalMergeObjectKind::Directory) {
                    approveDirectorySkip(originalPlan, orig.relativePath);
                } else if (orig.classification == LocalMergeEntryClassification::FileConflict) {
                    approveFileSkip(originalPlan, orig.relativePath);
                } else {
                    originalPlan.entries[i].approvedAction = LocalMergeApprovedAction::Skip;
                    originalPlan.entries[i].decisionBinding.bound = true;
                    originalPlan.entries[i].decisionBinding.expectedClassification = orig.classification;
                    originalPlan.entries[i].decisionBinding.expectedSourceStat = orig.sourceStat;
                    originalPlan.entries[i].decisionBinding.expectedSourceValid = orig.sourceStatValid;
                    originalPlan.entries[i].decisionBinding.expectedDestinationStat = orig.destinationStat;
                    originalPlan.entries[i].decisionBinding.expectedDestinationValid = orig.destinationStatValid;
                }
            } else if (resolved.approvedAction == LocalMergeApprovedAction::Overwrite) {
                QString rejectReason;
                if (!approveFileOverwrite(originalPlan, orig.relativePath,
                                          QStringLiteral("Explicitly approved overwrite"),
                                          &rejectReason,
                                          resolved.decisionBinding.policyScope)) {
                    m_progress.errorCount += 1;
                    const bool hasPartial = (m_progress.filesCreated > 0
                                             || m_progress.directoriesCreated > 0
                                             || m_progress.replacedCount > 0);
                    m_progress.retryAllowed = !hasPartial;
                    finishTerminal(hasPartial ? LocalMergeOrchestratorState::FailedPartialSuccess
                                              : LocalMergeOrchestratorState::FailedNoChanges,
                                   rejectReason);
                    return;
                }
            } else if (resolved.approvedAction == LocalMergeApprovedAction::Rename) {
                QString rejectReason;
                if (orig.sourceKind == LocalMergeObjectKind::Directory) {
                    bool needsReplan = false;
                    if (!approveDirectoryRenameAndRevalidate(originalPlan, orig.relativePath,
                                                             resolved.decisionBinding.targetFileName,
                                                             &rejectReason, &needsReplan)) {
                        if (needsReplan && m_replanRounds < m_maxReplanRounds) {
                            m_replanRounds += 1;
                            setState(LocalMergeOrchestratorState::NeedsReplan, QStringLiteral("replanning"));
                            QPointer<LocalMergeOrchestrator> self(this);
                            QTimer::singleShot(0, this, [self] {
                                if (self) self->startPlanningRound();
                            });
                            return;
                        }
                        m_progress.errorCount += 1;
                        const bool hasPartial = (m_progress.filesCreated > 0
                                                 || m_progress.directoriesCreated > 0
                                                 || m_progress.replacedCount > 0);
                        m_progress.retryAllowed = !hasPartial;
                        finishTerminal(hasPartial ? LocalMergeOrchestratorState::FailedPartialSuccess
                                                  : LocalMergeOrchestratorState::FailedNoChanges,
                                       rejectReason);
                        return;
                    }
                } else {
                    if (!approveFileRename(originalPlan, orig.relativePath,
                                           resolved.decisionBinding.targetFileName,
                                           &rejectReason)) {
                        m_progress.errorCount += 1;
                        const bool hasPartial = (m_progress.filesCreated > 0
                                                 || m_progress.directoriesCreated > 0
                                                 || m_progress.replacedCount > 0);
                        m_progress.retryAllowed = !hasPartial;
                        finishTerminal(hasPartial ? LocalMergeOrchestratorState::FailedPartialSuccess
                                                  : LocalMergeOrchestratorState::FailedNoChanges,
                                       rejectReason);
                        return;
                    }
                }
            }
        }
        m_pendingPlan = std::move(originalPlan);

        // Recompute bytesPlanned for approved actions so Skipped files do not skew totalBytes/progress
        quint64 approvedBytes = 0;
        for (const auto &entry : std::as_const(m_pendingPlan.entries)) {
            if (entry.sourceKind == LocalMergeObjectKind::RegularFile
                && (entry.approvedAction == LocalMergeApprovedAction::Create
                    || entry.approvedAction == LocalMergeApprovedAction::Overwrite
                    || entry.approvedAction == LocalMergeApprovedAction::Rename)) {
                approvedBytes += static_cast<quint64>(entry.size);
            }
        }
        if (m_replanRounds == 0) {
            m_progress.bytesPlanned = approvedBytes;
        }
        emitProgress();

        // Update reservation scope with any renamed destination paths
        if (m_registry && m_reservationAcquired) {
            LocalMergeReservationScope updated = buildInitialScope();
            for (const auto &entry : std::as_const(m_pendingPlan.entries)) {
                if (entry.approvedAction == LocalMergeApprovedAction::Rename
                    && !entry.decisionBinding.targetFileName.isEmpty()) {
                    const QString renamedPath = QDir(QFileInfo(entry.destinationPath).path())
                                                    .filePath(entry.decisionBinding.targetFileName);
                    updated.destinationPaths.append(renamedPath);
                }
            }
            QString conflictReason;
            if (!m_registry->updateScope(updated, &conflictReason)) {
                m_progress.errorCount += 1;
                const bool hasPartial = (m_progress.filesCreated > 0
                                         || m_progress.directoriesCreated > 0
                                         || m_progress.replacedCount > 0);
                m_progress.retryAllowed = !hasPartial;
                finishTerminal(hasPartial ? LocalMergeOrchestratorState::FailedPartialSuccess
                                          : LocalMergeOrchestratorState::FailedNoChanges,
                               conflictReason);
                return;
            }
        }

        // Reserve any dynamically renamed targets in the shared TransferReservationRegistry
        if (m_dynamicTargetReservationFn) {
            for (const auto &entry : std::as_const(m_pendingPlan.entries)) {
                if (entry.approvedAction == LocalMergeApprovedAction::Rename
                    && !entry.decisionBinding.targetFileName.isEmpty()) {
                    const QString renamedPath = QDir(QFileInfo(entry.destinationPath).path())
                                                    .filePath(entry.decisionBinding.targetFileName);
                    QString reason;
                    if (!m_dynamicTargetReservationFn(QUrl::fromLocalFile(renamedPath), &reason)) {
                        m_progress.errorCount += 1;
                        const bool hasPartial = (m_progress.filesCreated > 0
                                                 || m_progress.directoriesCreated > 0
                                                 || m_progress.replacedCount > 0);
                        m_progress.retryAllowed = !hasPartial;
                        finishTerminal(hasPartial ? LocalMergeOrchestratorState::FailedPartialSuccess
                                                  : LocalMergeOrchestratorState::FailedNoChanges,
                                       reason.isEmpty()
                                           ? QStringLiteral("Shared reservation conflict on renamed target: %1").arg(renamedPath)
                                           : reason);
                        return;
                    }
                }
            }
        }

        startExecutionRound();
    }

    void startExecutionRound()
    {
        if (m_cancelRequested) {
            finishCancelled(QStringLiteral("Cancelled before execution"));
            return;
        }
        if (m_hasCompletedExecution) {
            m_result.duplicateExecutionBlocked = true;
            return;
        }

        // Check if any entry actually needs execution (or all skipped)
        bool hasActionableEntries = false;
        for (const auto &entry : std::as_const(m_pendingPlan.entries)) {
            if (!m_completedEntryRecords.contains(entryKey(entry))) {
                hasActionableEntries = true;
                break;
            }
        }
        if (!hasActionableEntries) {
            const auto terminal = (m_progress.skippedCount > 0 || m_progress.skippedSubtreeCount > 0)
                ? LocalMergeOrchestratorState::CompletedWithSkippedConflicts
                : LocalMergeOrchestratorState::Completed;
            finishTerminal(terminal);
            return;
        }

        // Stage 3B.3B-2 Undo Fence Timing: raise Undo barrier BEFORE any Overwrite or
        // non-reversible write begins execution!
        if (m_preIrreversibleWriteHook) {
            bool hasOverwrite = false;
            for (const auto &entry : std::as_const(m_pendingPlan.entries)) {
                if (entry.approvedAction == LocalMergeApprovedAction::Overwrite) {
                    hasOverwrite = true;
                    break;
                }
            }
            if (hasOverwrite) {
                m_preIrreversibleWriteHook();
            }
        }

        setState(LocalMergeOrchestratorState::Executing, QStringLiteral("executing"));
        auto *execJob = new LocalMergeExecutionJob(m_pendingPlan, this);
        if (!m_revalidatedPreApprovedDirs.isEmpty()) {
            execJob->setPreviouslyCompletedDirectories(m_revalidatedPreApprovedDirs);
        }
        if (m_jobConfigurator) {
            m_jobConfigurator(execJob);
        }
        m_activeExecJob = execJob;

        QPointer<LocalMergeOrchestrator> self(this);
        connect(execJob, &KJob::processedAmountChanged, this,
                [self](KJob *, KJob::Unit unit, qulonglong amount) {
                    if (!self || unit != KJob::Bytes) {
                        return;
                    }
                    self->m_progress.bytesProcessed = self->m_baseBytesProcessed + amount;
                    self->emitProgress();
                });

        connect(execJob, &KJob::result, this, [self, execJob](KJob *) {
            if (!self) {
                return;
            }
            self->m_activeExecJob = nullptr;
            const LocalMergeExecutionReport rep = execJob->report();
            execJob->deleteLater();
            self->onExecutionRoundFinished(rep);
        });
        execJob->start();
    }

    void onExecutionRoundFinished(const LocalMergeExecutionReport &rep)
    {
        m_result.executionReport = rep;

        // Accumulate actually-completed entries so NeedsReplan never re-runs them!
        auto containsRelOrPath = [](const QStringList &list,
                                    const LocalMergePlanEntry &entry,
                                    const QString &effectiveRel,
                                    const QString &effectiveDst) {
            return list.contains(effectiveDst)
                || list.contains(entry.destinationPath)
                || (!effectiveRel.isEmpty() && list.contains(effectiveRel))
                || (!entry.relativePath.isEmpty() && list.contains(entry.relativePath));
        };

        for (const auto &entry : std::as_const(m_pendingPlan.entries)) {
            const bool isRenamed = (entry.approvedAction == LocalMergeApprovedAction::Rename
                                    && !entry.decisionBinding.targetFileName.isEmpty());
            const QString effectiveDst = isRenamed
                ? QDir(QFileInfo(entry.destinationPath).path()).filePath(entry.decisionBinding.targetFileName)
                : entry.destinationPath;
            QString effectiveRel = entry.relativePath;
            if (isRenamed) {
                const qsizetype slash = entry.relativePath.lastIndexOf(QLatin1Char('/'));
                effectiveRel = (slash > 0)
                    ? (entry.relativePath.left(slash + 1) + entry.decisionBinding.targetFileName)
                    : entry.decisionBinding.targetFileName;
            }

            bool inSkippedEntries = false;
            for (const auto &sk : std::as_const(rep.skippedEntries)) {
                if (sk.relativePath == entry.relativePath
                    || sk.relativePath == effectiveRel
                    || sk.relativePath == entry.destinationPath) {
                    inSkippedEntries = true;
                    break;
                }
            }

            const bool wasExecuted = containsRelOrPath(rep.filesCreated, entry, effectiveRel, effectiveDst)
                || containsRelOrPath(rep.filesRenamed, entry, effectiveRel, effectiveDst)
                || containsRelOrPath(rep.symlinksCreated, entry, effectiveRel, effectiveDst)
                || containsRelOrPath(rep.directoriesCreated, entry, effectiveRel, effectiveDst)
                || containsRelOrPath(rep.existingDirectoriesUsed, entry, effectiveRel, effectiveDst)
                || containsRelOrPath(rep.filesOverwritten, entry, effectiveRel, effectiveDst)
                || inSkippedEntries
                || containsRelOrPath(rep.skippedSubtreeEntries, entry, effectiveRel, effectiveDst);

            const bool needsReplan = containsRelOrPath(rep.entriesNeedingReplan, entry, effectiveRel, effectiveDst);
            const bool failed = containsRelOrPath(rep.failedOperations, entry, effectiveRel, effectiveDst);
            if (wasExecuted && !needsReplan && !failed) {
                CompletedEntryRecord rec;
                rec.sourceStatValid = entry.sourceStatValid;
                if (entry.sourceStatValid) {
                    rec.sourceDev = static_cast<quint64>(entry.sourceStat.st_dev);
                    rec.sourceIno = static_cast<quint64>(entry.sourceStat.st_ino);
                }
                rec.effectiveDestinationPath = effectiveDst;
                rec.action = entry.approvedAction;
                struct stat dstSt {};
                if (::lstat(QFile::encodeName(effectiveDst).constData(), &dstSt) == 0) {
                    rec.destStatValid = true;
                    rec.destDev = static_cast<quint64>(dstSt.st_dev);
                    rec.destIno = static_cast<quint64>(dstSt.st_ino);
                    rec.destSize = static_cast<qint64>(dstSt.st_size);
                    if (S_ISREG(dstSt.st_mode)) {
                        rec.destSha256 = computeFileSha256(effectiveDst);
                    } else if (S_ISLNK(dstSt.st_mode)) {
                        rec.destSymlinkTarget = readLocalLink(effectiveDst);
                    }
                }
                m_completedEntryRecords.insert(entryKey(entry), rec);

                if (entry.sourceKind == LocalMergeObjectKind::Directory
                    && (containsRelOrPath(rep.directoriesCreated, entry, effectiveRel, effectiveDst)
                        || containsRelOrPath(rep.existingDirectoriesUsed, entry, effectiveRel, effectiveDst))) {
                    CompletedDirectoryContext dirCtx;
                    dirCtx.cleanSourcePath = QDir::cleanPath(entry.sourcePath);
                    dirCtx.sourceRootPath = entry.sourceRootPath;
                    dirCtx.originalRelativePath = m_roundOriginalRelBySourcePath.value(
                        dirCtx.cleanSourcePath, entry.relativePath);
                    dirCtx.effectiveRelativePath = effectiveRel;
                    dirCtx.effectiveDestinationPath = QDir::cleanPath(effectiveDst);
                    dirCtx.action = entry.approvedAction;
                    dirCtx.sourceStat = entry.sourceStat;
                    dirCtx.sourceStatValid = entry.sourceStatValid;
                    LocalMergeExecDetail::UniqueFd dstDirFd = LocalMergeExecDetail::openDirectoryNoSymlinks(dirCtx.effectiveDestinationPath);
                    if (dstDirFd.valid() && ::fstat(dstDirFd.fd, &dirCtx.destStat) == 0) {
                        dirCtx.destStatValid = true;
                    }
                    m_completedDirectories.insert(dirCtx.cleanSourcePath, dirCtx);
                }
            }
        }

        m_progress.filesCreated += rep.filesCreated.size() + rep.symlinksCreated.size();
        m_progress.directoriesCreated += rep.directoriesCreated.size();
        m_progress.replacedCount += rep.filesOverwritten.size();
        m_progress.skippedCount += rep.skippedEntries.size();
        m_progress.skippedSubtreeCount += static_cast<int>(rep.skippedSubtreeCount);
        m_progress.errorCount += rep.failedOperations.size();
        m_baseBytesProcessed += rep.bytesProcessed;
        m_progress.bytesProcessed = m_baseBytesProcessed;
        m_cumulativeCreatedFiles.append(rep.filesCreated);
        m_cumulativeCreatedFiles.append(rep.symlinksCreated);
        m_cumulativeCreatedDirs.append(rep.directoriesCreated);

        const QString reportErr = rep.failedOperations.isEmpty()
            ? QString()
            : rep.failedOperations.join(QStringLiteral("; "));

        if (m_cancelRequested
            || rep.outcome == LocalMergeExecutionOutcome::CancelledNoChanges
            || rep.outcome == LocalMergeExecutionOutcome::CancelledPartialSuccess) {
            finishCancelled(reportErr.isEmpty() ? QStringLiteral("Cancelled during execution") : reportErr);
            return;
        }

        if (rep.outcome == LocalMergeExecutionOutcome::NeedsReplan) {
            if (m_replanRounds >= m_maxReplanRounds) {
                m_progress.manualInterventionRequired = true;
                m_progress.retryAllowed = false;
                finishTerminal(LocalMergeOrchestratorState::FailedPartialSuccess,
                               QStringLiteral("Maximum replan rounds exceeded"));
                return;
            }
            m_replanRounds += 1;
            setState(LocalMergeOrchestratorState::NeedsReplan, QStringLiteral("replanning"));
            QPointer<LocalMergeOrchestrator> self(this);
            QTimer::singleShot(0, this, [self] {
                if (!self) {
                    return;
                }
                self->startPlanningRound();
            });
            return;
        }

        if (!rep.objectsBlocked.isEmpty()) {
            const bool hasPartial = (m_progress.filesCreated > 0
                                     || m_progress.directoriesCreated > 0
                                     || m_progress.replacedCount > 0);
            m_progress.errorCount += rep.objectsBlocked.size();
            m_progress.retryAllowed = false;
            const QString blockedDiag = QStringLiteral("Blocked unapproved merge entries: %1")
                                            .arg(rep.objectsBlocked.join(QStringLiteral(", ")));
            finishTerminal(hasPartial ? LocalMergeOrchestratorState::FailedPartialSuccess
                                      : LocalMergeOrchestratorState::FailedNoChanges,
                           blockedDiag);
            return;
        }

        if (rep.outcome == LocalMergeExecutionOutcome::Succeeded
            || rep.outcome == LocalMergeExecutionOutcome::SucceededWithSkippedConflicts) {
            m_progress.retryAllowed = false;
            const auto terminal = (m_progress.skippedCount > 0 || m_progress.skippedSubtreeCount > 0
                                   || rep.outcome == LocalMergeExecutionOutcome::SucceededWithSkippedConflicts)
                ? LocalMergeOrchestratorState::CompletedWithSkippedConflicts
                : LocalMergeOrchestratorState::Completed;
            finishTerminal(terminal);
            return;
        }

        // Failed
        if (rep.uncertainDurability || rep.outcome == LocalMergeExecutionOutcome::FailedUncertainDurability) {
            m_progress.manualInterventionRequired = true;
            m_progress.retryAllowed = false;
            finishTerminal(LocalMergeOrchestratorState::FailedUncertainDurability, reportErr);
            return;
        }

        const bool hasPartial = (m_progress.filesCreated > 0
                                 || m_progress.directoriesCreated > 0
                                 || m_progress.replacedCount > 0);
        if (hasPartial || rep.partialSuccess || rep.outcome == LocalMergeExecutionOutcome::FailedPartialSuccess) {
            m_progress.manualInterventionRequired = rep.manualInterventionRequired;
            m_progress.retryAllowed = false;
            finishTerminal(LocalMergeOrchestratorState::FailedPartialSuccess, reportErr);
            return;
        }

        m_progress.retryAllowed = true;
        finishTerminal(LocalMergeOrchestratorState::FailedNoChanges, reportErr);
    }

    QList<QUrl> m_sources;
    QUrl m_destinationDir;
    LocalMergeReservationRegistry *m_registry = nullptr;
    quint64 m_holderId = 1;
    LocalMergeBackendCapabilities m_capabilities;

    FolderPromptFn m_folderPrompt;
    FilePromptFn m_filePrompt;
    std::function<void(LocalMergeOrchestratorState)> m_onStateChanged;
    std::function<void(const LocalMergeOrchestratorProgress &)> m_onProgressChanged;
    std::function<void(const LocalMergeOrchestratorResult &)> m_onFinished;
    std::function<void(LocalMergeExecutionJob *)> m_jobConfigurator;

    bool m_forceRecoveryRequired = false;
    int m_maxReplanRounds = 8;
    bool m_started = false;
    bool m_cancelRequested = false;
    bool m_reservationAcquired = false;
    bool m_hasCompletedExecution = false;
    int m_replanRounds = 0;

    LocalMergeOrchestratorState m_state = LocalMergeOrchestratorState::Queued;
    QList<LocalMergeOrchestratorState> m_stateHistory{LocalMergeOrchestratorState::Queued};
    LocalMergeOrchestratorProgress m_progress;
    LocalMergeOrchestratorResult m_result;

    QPointer<LocalMergePlanJob> m_activePlanJob;
    QPointer<LocalMergeExecutionJob> m_activeExecJob;
    LocalMergePlan m_pendingPlan;

    struct CompletedEntryRecord {
        quint64 sourceDev = 0;
        quint64 sourceIno = 0;
        bool sourceStatValid = false;
        QString effectiveDestinationPath;
        quint64 destDev = 0;
        quint64 destIno = 0;
        qint64 destSize = 0;
        bool destStatValid = false;
        QByteArray destSha256;
        QByteArray destSymlinkTarget;
        LocalMergeApprovedAction action = LocalMergeApprovedAction::Unapproved;
    };

    struct CompletedDirectoryContext {
        QString cleanSourcePath;
        QString sourceRootPath;
        QString originalRelativePath;
        QString effectiveRelativePath;
        QString effectiveDestinationPath;
        LocalMergeApprovedAction action = LocalMergeApprovedAction::Unapproved;
        struct stat sourceStat {};
        bool sourceStatValid = false;
        struct stat destStat {};
        bool destStatValid = false;
    };

    DynamicTargetReservationFn m_dynamicTargetReservationFn;
    PreIrreversibleWriteHook m_preIrreversibleWriteHook;
    QHash<QString, CompletedEntryRecord> m_completedEntryRecords;
    QHash<QString, CompletedDirectoryContext> m_completedDirectories;
    QHash<QString, QString> m_roundOriginalRelBySourcePath;
    QList<LocalMergeAnchoredDirectory> m_revalidatedPreApprovedDirs;
    QSet<QString> m_entriesRequiringFreshDecision;
    quint64 m_baseBytesProcessed = 0;
    QStringList m_cumulativeCreatedFiles;
    QStringList m_cumulativeCreatedDirs;
};

#endif // THISPC_LOCALMERGEORCHESTRATOR_H
