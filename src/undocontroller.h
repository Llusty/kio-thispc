/*
 * Undo/redo controller for file operations.
 *
 * Extracted during the 0.21.0 architecture refactor so ThisPcWindow no
 * longer owns KIO::FileUndoManager state or signal handling.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "batchrename.h"
#include "batchrenamejournal.h"
#include "batchrenamelinear.h"
#include "batchrenamerecovery.h"
#include "localfilemovejob.h"
#include "localtreehistory.h"

#include <KIO/CopyJob>
#include <KIO/FileUndoManager>
#include <KIO/Job>

#include <QAction>
#include <QFuture>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QMessageBox>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <functional>
#include <optional>
#include <utility>

class UndoController final : public QObject
{
public:
    using RefreshViews = std::function<void(bool preserveStatusMessage)>;
    using UpdateFileActions = std::function<void()>;
    using ShowStatus = std::function<void(const QString &, int)>;

    explicit UndoController(
        QWidget *parentWidget,
        RefreshViews refreshViews,
        UpdateFileActions updateFileActions,
        ShowStatus showStatus,
        QObject *parent = nullptr)
        : QObject(parent)
        , m_parentWidget(parentWidget)
        , m_refreshViews(std::move(refreshViews))
        , m_updateFileActions(std::move(updateFileActions))
        , m_showStatus(std::move(showStatus))
        , m_manager(KIO::FileUndoManager::self())
        , m_nativeHistory(new LocalMoveHistory(this))
        , m_treeHistory(new LocalTreeHistory(this))
    {
        connect(m_nativeHistory, &LocalMoveHistory::availabilityChanged,
                this, [this](bool, bool) { updateActions(); });
        connect(m_treeHistory, &LocalTreeHistory::availabilityChanged,
                this, [this] { updateActions(); });
        if (!m_manager) {
            return;
        }

        if (m_manager->uiInterface()) {
            m_manager->uiInterface()->setParentWidget(m_parentWidget);
            // thispc-view has its own status/operation UI; avoid a second
            // generic KIO progress dialog during undo/redo.
            m_manager->uiInterface()->setShowProgressInfo(false);
        }

        connect(
            m_manager,
            &KIO::FileUndoManager::undoAvailable,
            this,
            [this](bool) { updateActions(); });
        connect(
            m_manager,
            &KIO::FileUndoManager::redoAvailable,
            this,
            [this](bool) { updateActions(); });
        connect(
            m_manager,
            &KIO::FileUndoManager::undoTextChanged,
            this,
            [this](const QString &) { updateActions(); });
        connect(
            m_manager,
            &KIO::FileUndoManager::redoTextChanged,
            this,
            [this](const QString &) { updateActions(); });
        connect(
            m_manager,
            &KIO::FileUndoManager::jobRecordingFinished,
            this,
            [this](KIO::FileUndoManager::CommandType) {
                updateActions();
            });
        connect(
            m_manager,
            &KIO::FileUndoManager::undoJobFinished,
            this,
            [this] {
                const int completedMode = m_mode;
                m_mode = 0;
                m_busy = false;
                if (completedMode == 1) {
                    m_redoKinds.push_back(false);
                } else if (completedMode == 2 && !m_redoKinds.isEmpty()) {
                    m_redoKinds.removeLast();
                }

                if (m_refreshViews) {
                    m_refreshViews(false);
                }
                if (m_updateFileActions) {
                    m_updateFileActions();
                }
                updateActions();

                if (completedMode == 1) {
                    this->showStatus(
                        trLocal(
                            "Cofanie zakończone",
                            "Undo completed"),
                        4000);
                } else if (completedMode == 2) {
                    this->showStatus(
                        trLocal(
                            "Ponawianie zakończone",
                            "Redo completed"),
                        4000);
                }
            });
    }

    ~UndoController() override
    {
        // Normal window close is refused during the non-journaled exchange.
        // If a caller destroys the controller directly, never let its worker
        // outlive the process object or leave the process fence open.
        if (m_swapWorkerRunning) {
            m_swapFuture.waitForFinished();
            BatchRenameRecoveryGate::instance().finishUnjournaledSwap(true);
        }
    }

    void setActions(QAction *undoAction, QAction *redoAction)
    {
        m_undoAction = undoAction;
        m_redoAction = redoAction;
        updateActions();
    }

    // Keep normal KIO Undo/Redo from interleaving with a journaled initial
    // batch. The batch publishes its single history record only after success.
    bool beginGroupedBatch()
    {
        if (recoveryBlocked()) return false;
        if (!m_manager || m_busy || m_groupedBatchRunning
            || !BatchRenameRecoveryJournal::pendingJournal().isEmpty()) return false;
        m_groupedBatchRunning = true;
        updateActions();
        return true;
    }

    void endGroupedBatch()
    {
        m_groupedBatchRunning = false;
        updateActions();
    }

    void undo()
    {
        if (recoveryBlocked()) return;
        if (m_busy || m_groupedBatchRunning) {
            return;
        }

        const QString pending = BatchRenameRecoveryJournal::pendingJournal();
        if (!pending.isEmpty()) {
            showStatus(trLocal("Dziennik wymaga kontroli przed Undo: ",
                               "Inspect unresolved journal before Undo: ") + pending, 12000);
            return;
        }
        const bool kioAvailable = kioUndoAvailable();
        const bool nativeAvailable = nativeUndoAvailable();
        const bool treeAvailable = treeUndoAvailable();
        if (linearUndoIsNewest()) {
            replayRecordedLinear(true);
            return;
        }
        if (cycleUndoIsNewest()) {
            replayRecordedCycle(true);
            return;
        }
        if (swapUndoIsNewest()) {
            replayRecordedSwap(true);
            return;
        }
        if (treeAvailable
            && (!nativeAvailable || m_treeHistory->undoSerial() > m_nativeHistory->undoSerial())
            && (!kioAvailable || m_treeHistory->undoSerial() > m_manager->currentCommandSerialNumber())) {
            startNativeHistoryJob(m_treeHistory->undo(), 1, 2);
            return;
        }
        if (!kioAvailable && !nativeAvailable) return;
        const bool useNative = nativeAvailable
            && (!kioAvailable
                || m_nativeHistory->undoSerial()
                    > m_manager->currentCommandSerialNumber());

        if (useNative) {
            startNativeHistoryJob(m_nativeHistory->undo(), 1);
            return;
        }

        prepareUiInterface();
        m_busy = true;
        m_mode = 1;
        updateActions();
        showStatus(
            trLocal("Cofanie operacji…", "Undoing operation…"),
            0);
        m_manager->undo();
    }

    void redo()
    {
        if (recoveryBlocked()) return;
        if (m_busy || m_groupedBatchRunning) {
            return;
        }

        const QString pending = BatchRenameRecoveryJournal::pendingJournal();
        if (!pending.isEmpty()) {
            showStatus(trLocal("Dziennik wymaga kontroli przed Redo: ",
                               "Inspect unresolved journal before Redo: ") + pending, 12000);
            return;
        }
        const bool kioAvailable = kioRedoAvailable();
        const bool nativeAvailable = nativeRedoAvailable();
        const bool treeAvailable = treeRedoAvailable();
        if (linearRedoIsNext()) {
            replayRecordedLinear(false);
            return;
        }
        if (cycleRedoIsNext()) {
            replayRecordedCycle(false);
            return;
        }
        if (swapRedoIsNext()) {
            replayRecordedSwap(false);
            return;
        }
        if (treeAvailable && ((!kioAvailable && !nativeAvailable)
                || (!m_redoKinds.isEmpty() && m_redoKinds.constLast() == 2))) {
            startNativeHistoryJob(m_treeHistory->redo(), 2, 2);
            return;
        }
        if (!kioAvailable && !nativeAvailable) return;
        const bool useNative = nativeAvailable
            && (!kioAvailable
                || (!m_redoKinds.isEmpty() && m_redoKinds.constLast() == 1));
        if (useNative) {
            startNativeHistoryJob(m_nativeHistory->redo(), 2);
            return;
        }

        prepareUiInterface();
        m_busy = true;
        m_mode = 2;
        updateActions();
        showStatus(
            trLocal("Ponawianie operacji…", "Redoing operation…"),
            0);
        m_manager->redo();
    }

    // A complete batch consisting of exactly ONE two-way exchange can be
    // replayed as one atomic Undo/Redo action. Mixed batches, multiple swaps,
    // and cycles of 3+ are deliberately excluded from this history.
    bool recordCompletedAtomicSwap(const BatchRenamePlan &plan)
    {
        if (!m_manager || !plan.isValid() || plan.activeCount() != 2
            || !plan.executionOrder.isEmpty() || !plan.exchangeCycles.isEmpty()
            || plan.atomicSwaps.size() != 1) return false;
        const auto pair = plan.atomicSwaps.first();
        const auto &left = plan.entries.at(pair.first);
        const auto &right = plan.entries.at(pair.second);
        if (!batchRenameSnapshotAt(left, right.source)
            || !batchRenameSnapshotAt(right, left.source)) return false;
#ifndef Q_OS_WIN
        if (left.sourceDevice == right.sourceDevice
            && left.sourceInode == right.sourceInode) return false;
#endif
        m_cycle.reset();
        m_linear.reset();
        m_swap = SwapRecord{left, right, m_manager->newCommandSerialNumber(), false};
        m_redoKinds.clear();
        updateActions();
        return true;
    }

    // A single completed cycle (3+ entries) is one *user-visible* history
    // action, but its replay is N-1 syscalls, NOT a filesystem transaction.
    // The worker below journals every syscall and fails closed on ambiguity.
    bool recordCompletedCycle(const BatchRenamePlan &plan)
    {
        if (!m_manager || !batchRenameSingleCycleHistoryEligible(plan)
            || !BatchRenameRecoveryJournal::pendingJournal().isEmpty()) return false;
        const auto &cycle = plan.exchangeCycles.first();
        const auto finalState = batchRenameCycleFinalState(cycle);
        if (!batchRenameCycleComplete(plan, cycle, finalState)
            || !batchRenameCycleSnapshotsMatch(plan, cycle, finalState)) return false;
        m_swap.reset();
        m_linear.reset();
        m_cycle = CycleRecord{plan, m_manager->newCommandSerialNumber(), false};
        m_redoKinds.clear();
        updateActions();
        return true;
    }

    // Called only after a successful, journaled native initial batch. Never
    // register per-item KIO move commands for this eligible batch: a late KIO
    // recording can overtake the grouped command and make Ctrl+Z undo one item.
    bool recordCompletedLinear(const BatchRenamePlan &plan)
    {
        if (!m_manager || !batchRenameLinearHistoryEligible(plan)
            || !BatchRenameRecoveryJournal::pendingJournal().isEmpty()
            || !batchRenameLinearMappingMatches(plan, true, 0)) return false;
        // Previous path-based KIO/native commands may now address a different
        // occupant; fence them just as the existing swap/cycle path does.
        m_irreversibleSerial = qMax(m_irreversibleSerial,
                                     m_manager->newCommandSerialNumber());
        m_swap.reset();
        m_cycle.reset();
        m_linear = LinearRecord{plan, m_manager->newCommandSerialNumber(), false};
        m_redoKinds.clear();
        updateActions();
        return true;
    }

    // A renameat2 exchange is not a KIO::CopyJob. Fence older commands
    // whose recorded paths might refer to different inodes. For a pure
    // single-pair batch, a separate native record is added after success.
    void invalidateUndoBeforeAtomicSwap()
    {
        if (!m_manager) return;
        m_irreversibleSerial = qMax(m_irreversibleSerial,
                                     m_manager->newCommandSerialNumber());
        m_swap.reset();
        m_cycle.reset();
        m_linear.reset();
        m_redoKinds.clear();
        updateActions();
    }

    void refreshAfterAtomicSwap()
    {
        if (m_refreshViews) m_refreshViews(false);
        if (m_updateFileActions) m_updateFileActions();
        updateActions();
    }

    void recordCopyJob(KIO::CopyJob *job)
    {
        if (job && m_manager) {
            discardSwapRedo();
            discardCycleRedo();
            discardLinearRedo();
            m_redoKinds.clear();
            m_manager->recordCopyJob(job);
        }
    }

    void recordNativeTransfer(
        KIO::Job *job,
        bool move)
    {
        if (!job || !m_manager) return;
        discardSwapRedo();
        discardCycleRedo();
        discardLinearRedo();
        m_redoKinds.clear();
        if (auto *tree = qobject_cast<LocalTransferJob *>(job)) {
            const quint64 serial = m_manager->newCommandSerialNumber();
            connect(tree, &KJob::result, this, [this, tree, serial] {
                m_treeHistory->recordCompleted(tree, serial);
                updateActions();
            });
            return;
        }
        if (job->property("thispcOverwritesDestination").toBool()) {
            const quint64 serial = m_manager->newCommandSerialNumber();
            connect(job, &KJob::result, this, [this, serial](KJob *completed) {
                if (completed->error() != KJob::NoError
                    && !completed->property("thispcDestinationReplaced").toBool()) return;
                // We do not retain backups after success. Earlier commands
                // might refer to the replaced path and are no longer safe.
                m_irreversibleSerial = qMax(m_irreversibleSerial, serial);
                m_redoKinds.clear();
                updateActions();
            });
            return;
        }
        if (move) {
            const quint64 serial = m_manager->newCommandSerialNumber();
            if (auto *moveJob = qobject_cast<LocalFileMoveJob *>(job)) {
                m_nativeHistory->recordWhenCompleted(moveJob, serial);
            }
            return;
        }
        const quint64 serial = m_manager->newCommandSerialNumber();
        if (auto *copyJob = qobject_cast<LocalFileCopyJob *>(job)) {
            m_nativeHistory->recordWhenCompleted(copyJob, serial);
        }
    }

    void recordPutJob(const QUrl &destination, KIO::Job *job)
    {
        recordJob(
            KIO::FileUndoManager::Put,
            {},
            destination,
            job);
    }

    void recordMkdirJob(const QUrl &destination, KIO::Job *job)
    {
        recordJob(
            KIO::FileUndoManager::Mkdir,
            {},
            destination,
            job);
    }

    void recordTrashJob(const QList<QUrl> &sources, KIO::Job *job)
    {
        recordJob(
            KIO::FileUndoManager::Trash,
            sources,
            QUrl(QStringLiteral("trash:/")),
            job);
    }

private:
    bool recoveryBlocked()
    {
        auto &gate = BatchRenameRecoveryGate::instance();
        gate.refresh();
        if (!gate.mutationsBlocked()) return false;
        showStatus(gate.message(), 0);
        return true;
    }

    struct LinearRecord {
        BatchRenamePlan plan;
        quint64 serial = 0;
        bool undone = false;
    };

    bool linearUndoAvailable() const
    {
        return m_linear && !m_linear->undone && m_linear->serial > m_irreversibleSerial;
    }

    bool linearRedoAvailable() const
    {
        return m_linear && m_linear->undone && m_linear->serial > m_irreversibleSerial;
    }

    bool linearUndoIsNewest() const
    {
        return linearUndoAvailable()
            && (!kioUndoAvailable()
                || m_linear->serial > m_manager->currentCommandSerialNumber())
            && (!nativeUndoAvailable() || m_linear->serial > m_nativeHistory->undoSerial())
            && (!treeUndoAvailable() || m_linear->serial > m_treeHistory->undoSerial());
    }

    bool linearRedoIsNext() const
    {
        return linearRedoAvailable()
            && ((!kioRedoAvailable() && !nativeRedoAvailable() && !treeRedoAvailable())
                || (!m_redoKinds.isEmpty() && m_redoKinds.constLast() == 5));
    }

    void discardLinearRedo()
    {
        if (m_linear && m_linear->undone) m_linear.reset();
    }

    void replayRecordedLinear(bool undo)
    {
        if (!(undo ? linearUndoAvailable() : linearRedoAvailable())) return;
        const QString pending = BatchRenameRecoveryJournal::pendingJournal();
        if (!pending.isEmpty()) {
            showStatus(trLocal("Nierozwiązany dziennik blokuje Undo/Redo: ",
                               "Unresolved journal blocks Undo/Redo: ") + pending, 12000);
            return;
        }
        m_busy = true;
        updateActions();
        showStatus(undo ? trLocal("Cofanie partii nazw…", "Undoing rename batch…")
                        : trLocal("Ponawianie partii nazw…", "Redoing rename batch…"), 0);
        const auto plan = m_linear->plan;
        const quint64 serial = m_linear->serial;
        auto *watcher = new QFutureWatcher<BatchRenameCycleReplayResult>(this);
        connect(watcher, &QFutureWatcher<BatchRenameCycleReplayResult>::finished,
                this, [this, watcher, undo, serial] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (result.success && m_linear && m_linear->serial == serial) {
                m_linear->undone = undo;
                if (undo) m_redoKinds.push_back(5);
                else if (!m_redoKinds.isEmpty()) m_redoKinds.removeLast();
                showStatus(undo ? trLocal("Cofnięto partię nazw", "Rename batch undone")
                                : trLocal("Ponowiono partię nazw", "Rename batch redone"), 4000);
            } else {
                if (result.success || result.uncertain) {
                    // Unknown mapping or concurrent history mutation: never
                    // offer another replay on an untrusted sequence of paths.
                    m_irreversibleSerial = qMax(m_irreversibleSerial, serial);
                    m_linear.reset();
                    m_swap.reset();
                    m_cycle.reset();
                    m_redoKinds.clear();
                    if (result.uncertain) {
                        QMessageBox::critical(m_parentWidget,
                            trLocal("Wymagana ręczna kontrola", "Manual inspection required"),
                            result.error);
                    }
                }
                if (!result.success) showStatus(result.error, 12000);
            }
            // A clean preflight refusal deliberately remains retryable. Keep
            // its actionable status text visible while the resulting async
            // directory listing finishes; ordinary refreshes stay unchanged.
            if (m_refreshViews) m_refreshViews(!result.success && !result.uncertain);
            if (m_updateFileActions) m_updateFileActions();
            m_busy = false;
            updateActions();
        });
        watcher->setFuture(QtConcurrent::run([plan, undo] {
            // Only the already-qualified local, same-directory, acyclic plan
            // reaches this path. Persist its exact Undo/Redo direction so
            // startup can finish policy B after a process crash.
            return batchRenameReplayLinearHistoryV2(plan, undo);
        }));
    }

    struct CycleRecord {
        BatchRenamePlan plan;
        quint64 serial = 0;
        bool undone = false;
    };

    bool cycleUndoAvailable() const
    {
        return m_cycle.has_value() && !m_cycle->undone
            && m_cycle->serial > m_irreversibleSerial;
    }

    bool cycleRedoAvailable() const
    {
        return m_cycle.has_value() && m_cycle->undone
            && m_cycle->serial > m_irreversibleSerial;
    }

    bool cycleUndoIsNewest() const
    {
        return cycleUndoAvailable()
            && (!kioUndoAvailable()
                || m_cycle->serial > m_manager->currentCommandSerialNumber())
            && (!nativeUndoAvailable() || m_cycle->serial > m_nativeHistory->undoSerial())
            && (!treeUndoAvailable() || m_cycle->serial > m_treeHistory->undoSerial());
    }

    bool cycleRedoIsNext() const
    {
        return cycleRedoAvailable()
            && ((!kioRedoAvailable() && !nativeRedoAvailable() && !treeRedoAvailable())
                || (!m_redoKinds.isEmpty() && m_redoKinds.constLast() == 4));
    }

    void discardCycleRedo()
    {
        if (m_cycle && m_cycle->undone) m_cycle.reset();
    }

    void replayRecordedCycle(bool undo)
    {
        if (!(undo ? cycleUndoAvailable() : cycleRedoAvailable())) return;
        const QString pending = BatchRenameRecoveryJournal::pendingJournal();
        if (!pending.isEmpty()) {
            showStatus(trLocal("Nierozwiązany dziennik blokuje Undo/Redo: ",
                               "Unresolved journal blocks Undo/Redo: ") + pending, 12000);
            return;
        }
        m_busy = true;
        updateActions();
        showStatus(undo ? trLocal("Cofanie cyklu nazw…", "Undoing name cycle…")
                        : trLocal("Ponawianie cyklu nazw…", "Redoing name cycle…"), 0);
        const auto plan = m_cycle->plan;
        const quint64 serial = m_cycle->serial;
        auto *watcher = new QFutureWatcher<BatchRenameCycleReplayResult>(this);
        connect(watcher, &QFutureWatcher<BatchRenameCycleReplayResult>::finished,
                this, [this, watcher, undo, serial] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (result.success && m_cycle && m_cycle->serial == serial) {
                m_cycle->undone = undo;
                if (undo) m_redoKinds.push_back(4);
                else if (!m_redoKinds.isEmpty()) m_redoKinds.removeLast();
                showStatus(undo ? trLocal("Cofnięto cykl nazw", "Name cycle undone")
                                : trLocal("Ponowiono cykl nazw", "Name cycle redone"), 4000);
            } else {
                if (result.success) {
                    // Another command changed the history while the worker was
                    // running. Never claim the original serial is replayable.
                    m_irreversibleSerial = qMax(m_irreversibleSerial, serial);
                    m_cycle.reset();
                    m_redoKinds.clear();
                    showStatus(trLocal("Cykl zakończony, ale historia zmieniła się w trakcie. Undo/Redo zablokowane.",
                                       "Cycle finished while history changed. Undo/Redo is disabled."), 12000);
                }
                if (result.uncertain) {
                    // No further history may operate on paths with an unknown
                    // permutation. The journal is left for manual inspection.
                    m_irreversibleSerial = qMax(m_irreversibleSerial, serial);
                    m_swap.reset();
                    m_cycle.reset();
                    m_redoKinds.clear();
                    QMessageBox::critical(m_parentWidget,
                        trLocal("Wymagana ręczna kontrola", "Manual inspection required"),
                        result.error);
                }
                if (!result.success) showStatus(result.error, 12000);
            }
            if (m_refreshViews) m_refreshViews(false);
            if (m_updateFileActions) m_updateFileActions();
            m_busy = false;
            updateActions();
        });
        watcher->setFuture(QtConcurrent::run([plan, undo] {
            return batchRenameReplaySingleCycle(plan, undo);
        }));
    }

    struct SwapRecord {
        BatchRenameEntry left;
        BatchRenameEntry right;
        quint64 serial = 0;
        bool undone = false;
    };

    bool swapUndoIsNewest() const
    {
        return swapUndoAvailable()
            && (!kioUndoAvailable()
                || m_swap->serial > m_manager->currentCommandSerialNumber())
            && (!nativeUndoAvailable() || m_swap->serial > m_nativeHistory->undoSerial())
            && (!treeUndoAvailable() || m_swap->serial > m_treeHistory->undoSerial());
    }

    bool swapRedoIsNext() const
    {
        return swapRedoAvailable()
            && ((!kioRedoAvailable() && !nativeRedoAvailable() && !treeRedoAvailable())
                || (!m_redoKinds.isEmpty() && m_redoKinds.constLast() == 3));
    }

    bool swapUndoAvailable() const
    {
        return m_swap.has_value() && !m_swap->undone
            && m_swap->serial > m_irreversibleSerial;
    }

    bool swapRedoAvailable() const
    {
        return m_swap.has_value() && m_swap->undone
            && m_swap->serial > m_irreversibleSerial;
    }

    void discardSwapRedo()
    {
        if (m_swap && m_swap->undone) m_swap.reset();
    }

    void replayRecordedSwap(bool undo)
    {
        if (!(undo ? swapUndoAvailable() : swapRedoAvailable())) return;
        if (recoveryBlocked()) return;
        if (!BatchRenameRecoveryJournal::pendingJournal().isEmpty()) {
            showStatus(trLocal("Nierozwiązany dziennik blokuje cofanie/ponawianie zamiany.",
                               "An unresolved journal blocks swap Undo/Redo."), 10000);
            return;
        }
        auto &gate = BatchRenameRecoveryGate::instance();
        if (!gate.beginUnjournaledSwap()) {
            showStatus(gate.message(), 0);
            return;
        }
        m_swapWorkerRunning = true;
        m_busy = true;
        updateActions();
        showStatus(undo ? trLocal("Cofanie zamiany nazw…", "Undoing name exchange…")
                        : trLocal("Ponawianie zamiany nazw…", "Redoing name exchange…"), 0);
        const BatchRenameEntry left = m_swap->left;
        const BatchRenameEntry right = m_swap->right;
        const quint64 serial = m_swap->serial;
        auto *watcher = new QFutureWatcher<BatchRenameSwapWorkerResult>(this);
        connect(watcher, &QFutureWatcher<BatchRenameSwapWorkerResult>::finished,
                this, [this, watcher, undo, serial] {
            const auto result = watcher->result();
            watcher->deleteLater();
            const bool recordValid = m_swap && m_swap->serial == serial;
            if (result.success && recordValid) {
                m_swap->undone = undo;
                if (undo) m_redoKinds.push_back(3);
                else if (!m_redoKinds.isEmpty()) m_redoKinds.removeLast();
                showStatus(undo ? trLocal("Cofnięto zamianę nazw", "Name exchange undone")
                                : trLocal("Ponowiono zamianę nazw", "Name exchange redone"), 4000);
            } else {
                if (result.uncertain || (result.success && !recordValid)) {
                    // Never expose a replayable history after an untrusted
                    // postcheck or unexpected concurrent history mutation.
                    m_irreversibleSerial = qMax(m_irreversibleSerial, serial);
                    m_swap.reset();
                    m_redoKinds.clear();
                    if (result.uncertain)
                        QMessageBox::critical(m_parentWidget,
                            trLocal("Wymagana ręczna kontrola", "Manual inspection required"),
                            result.error);
                }
                showStatus(result.success
                    ? trLocal("Zamiana zakończona, lecz historia zmieniła się w trakcie. Undo/Redo zablokowane.",
                              "Exchange completed, but history changed; Undo/Redo is disabled.")
                    : result.error, 12000);
            }
            m_swapWorkerRunning = false;
            BatchRenameRecoveryGate::instance().finishUnjournaledSwap(result.uncertain
                || (result.success && !recordValid));
            m_busy = false;
            if (m_refreshViews) m_refreshViews(!result.success && !result.uncertain);
            if (m_updateFileActions) m_updateFileActions();
            updateActions();
        });
        m_swapFuture = QtConcurrent::run([left, right, undo] {
            BatchRenameSwapWorkerResult result;
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
            bool delayOk = false;
            const int delay = QString::fromLocal8Bit(qgetenv("THISPC_SWAP_WORKER_DELAY_MS"))
                                  .toInt(&delayOk);
            if (delayOk && delay > 0) QThread::msleep(unsigned(delay));
#endif
            result.success = batchRenameRecordedSwap(
                undo ? right : left, left.source,
                undo ? left : right, right.source,
                &result.error, &result.uncertain);
            return result;
        });
        watcher->setFuture(m_swapFuture);
    }

    bool treeUndoAvailable() const
    {
        return m_treeHistory->canUndo() && m_treeHistory->undoSerial() > m_irreversibleSerial;
    }

    bool treeRedoAvailable() const
    {
        return m_treeHistory->canRedo() && m_treeHistory->redoSerial() > m_irreversibleSerial;
    }

    bool nativeUndoAvailable() const
    {
        return m_nativeHistory->canUndo()
            && m_nativeHistory->undoSerial() > m_irreversibleSerial;
    }

    bool kioUndoAvailable() const
    {
        return m_manager && m_manager->isUndoAvailable()
            && m_manager->currentCommandSerialNumber() > m_irreversibleSerial;
    }

    bool nativeRedoAvailable() const
    {
        return m_nativeHistory->canRedo()
            && m_nativeHistory->redoSerial() > m_irreversibleSerial;
    }

    bool kioRedoAvailable() const
    {
        return m_manager && m_manager->isRedoAvailable()
            && (!m_irreversibleSerial || !m_redoKinds.isEmpty());
    }

    void startNativeHistoryJob(KJob *job, int mode, int kind = 1)
    {
        if (!job) return;
        m_busy = true;
        m_mode = mode;
        updateActions();
        showStatus(
            mode == 1
                ? trLocal("Cofanie operacji…", "Undoing operation…")
                : trLocal("Ponawianie operacji…", "Redoing operation…"),
            0);
        connect(job, &KJob::result, this, [this, mode, kind](KJob *completed) {
            m_busy = false;
            m_mode = 0;
            if (completed->error() == KJob::NoError) {
                if (mode == 1) m_redoKinds.push_back(kind);
                else if (!m_redoKinds.isEmpty()) m_redoKinds.removeLast();
                showStatus(
                    mode == 1
                        ? trLocal("Cofanie zakończone", "Undo completed")
                        : trLocal("Ponawianie zakończone", "Redo completed"),
                    4000);
            } else {
                showStatus(completed->errorString(), 7000);
            }
            if (m_refreshViews) m_refreshViews(false);
            if (m_updateFileActions) m_updateFileActions();
            updateActions();
        });
    }

    void prepareUiInterface()
    {
        if (m_manager && m_manager->uiInterface()) {
            m_manager->uiInterface()->setParentWidget(m_parentWidget);
        }
    }

    void showStatus(const QString &message, int timeout)
    {
        if (m_showStatus) {
            m_showStatus(message, timeout);
        }
    }

    void updateActions()
    {
        const bool managerAvailable = m_manager != nullptr;
        auto &recoveryGate = BatchRenameRecoveryGate::instance();
        recoveryGate.refresh();
        const bool historySafe = !m_busy && !m_groupedBatchRunning
            && !recoveryGate.mutationsBlocked();
        const bool canUndo =
            historySafe
            && (kioUndoAvailable() || nativeUndoAvailable() || treeUndoAvailable()
                || swapUndoAvailable() || cycleUndoAvailable() || linearUndoAvailable());
        const bool canRedo =
            historySafe
            && (kioRedoAvailable() || nativeRedoAvailable() || treeRedoAvailable()
                || swapRedoAvailable() || cycleRedoAvailable() || linearRedoAvailable());

        if (m_undoAction) {
            m_undoAction->setEnabled(canUndo);

            QString text =
                managerAvailable
                    ? m_manager->undoText().trimmed()
                    : QString();
            if (swapUndoIsNewest()) {
                text = trLocal("Cofnij zamianę nazw", "Undo name exchange");
            }
            if (cycleUndoIsNewest()) {
                text = trLocal("Cofnij cykl nazw", "Undo name cycle");
            }
            if (linearUndoIsNewest()) {
                text = trLocal("Cofnij partię nazw", "Undo rename batch");
            }
            if (text.isEmpty()) {
                text = trLocal("Cofnij", "Undo");
            }
            m_undoAction->setToolTip(
                QStringLiteral("%1 (%2)")
                    .arg(
                        text,
                        QStringLiteral("Ctrl+Z")));
        }

        if (m_redoAction) {
            m_redoAction->setEnabled(canRedo);

            QString text =
                managerAvailable
                    ? m_manager->redoText().trimmed()
                    : QString();
            if (swapRedoIsNext()) {
                text = trLocal("Ponów zamianę nazw", "Redo name exchange");
            }
            if (cycleRedoIsNext()) {
                text = trLocal("Ponów cykl nazw", "Redo name cycle");
            }
            if (linearRedoIsNext()) {
                text = trLocal("Ponów partię nazw", "Redo rename batch");
            }
            if (text.isEmpty()) {
                text = trLocal("Ponów", "Redo");
            }
            m_redoAction->setToolTip(
                QStringLiteral("%1 (Ctrl+Y / Ctrl+Shift+Z)")
                    .arg(text));
        }
    }

    void recordJob(
        KIO::FileUndoManager::CommandType type,
        const QList<QUrl> &sources,
        const QUrl &destination,
        KIO::Job *job)
    {
        if (job && m_manager) {
            discardSwapRedo();
            discardCycleRedo();
            discardLinearRedo();
            m_redoKinds.clear();
            m_manager->recordJob(
                type,
                sources,
                destination,
                job);
        }
    }

    QWidget *m_parentWidget = nullptr;
    RefreshViews m_refreshViews;
    UpdateFileActions m_updateFileActions;
    ShowStatus m_showStatus;

    KIO::FileUndoManager *m_manager = nullptr;
    LocalMoveHistory *m_nativeHistory = nullptr;
    LocalTreeHistory *m_treeHistory = nullptr;
    std::optional<SwapRecord> m_swap;
    std::optional<CycleRecord> m_cycle;
    std::optional<LinearRecord> m_linear;
    QList<int> m_redoKinds;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QFuture<BatchRenameSwapWorkerResult> m_swapFuture;
    bool m_swapWorkerRunning = false;
    bool m_busy = false;
    bool m_groupedBatchRunning = false;
    quint64 m_irreversibleSerial = 0;
    int m_mode = 0;
};
