/*
 * File-operation dialogs and KIO dispatch, independent of the active pane.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"
#include "batchrename.h"
#include "batchrenamejournal.h"
#include "batchrenamelinear.h"
#include "batchrenamerecovery.h"
#include "localfilecopyjob.h"
#include "localfilemovejob.h"
#include "undocontroller.h"
#include <KIO/EmptyTrashJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/ListJob>
#include <KIO/MkdirJob>
#include <KIO/RenameDialog>
#include <KIO/StoredTransferJob>
#include <KJobUiDelegate>
#include <QApplication>
#include <QFuture>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QClipboard>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProgressDialog>
#include <QSharedPointer>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimer>
#include <functional>
#include <limits>
#include <utility>

class FileActions final : public QObject
{
public:
    using RefreshViews = std::function<void()>;
    using WatchOperation = std::function<void(KJob *, const QString &, bool, const QString &, const RefreshViews &)>;
    struct DirectorySnapshot {
        int error = 0;
        QString errorText;
        QSet<QString> names;
    };
    using DirectorySnapshotCallback = std::function<void(DirectorySnapshot)>;
    using StartDirectorySnapshot =
        std::function<KJob *(const QUrl &, QObject *, DirectorySnapshotCallback)>;

    FileActions(QWidget *parentWidget, UndoController *undoController, WatchOperation watchOperation,
                std::function<void(const QString &)> showRecoveryFence = {},
                StartDirectorySnapshot startDirectorySnapshot = {})
        : QObject(parentWidget)
        , m_parentWidget(parentWidget)
        , m_undoController(undoController)
        , m_watchOperation(std::move(watchOperation))
        , m_showRecoveryFence(std::move(showRecoveryFence))
        , m_startDirectorySnapshot(std::move(startDirectorySnapshot))
    {
    }

    ~FileActions() override
    {
        cancelPendingNewItem();
        if (m_swapWorkerRunning) m_swapFuture.waitForFinished();
        // A batch fence survives between worker invocations. Destruction in
        // that interval cannot silently release it as a verified completion.
        if (m_swapBatchRunning)
            BatchRenameRecoveryGate::instance().finishUnjournaledSwap(true);
    }

    // Arguments are snapshots captured by the initiating pane before any
    // modal dialog can change focus. This class never queries the active pane.
    void pasteClipboardInto(const QUrl &destination)
    {
        if (!mutationAllowed()) return;
        if (!destination.isValid()) {
            return;
        }

        const QMimeData *mime =
            QApplication::clipboard()->mimeData();

        if (!mime || !mime->hasUrls()) {
            return;
        }

        const QList<QUrl> urls = mime->urls();

        if (urls.isEmpty()) {
            return;
        }

        const bool cut =
            mime->data(
                QStringLiteral("application/x-kde-cutselection"))
                == QByteArrayLiteral("1");

        if (!mutationAllowed()) return;

        if (startNativeSingleFileTransfer(
                urls, destination, cut, cut,
                cut
                    ? trLocal("Przenoszenie zakończone", "Move completed")
                    : trLocal("Kopiowanie zakończone", "Copy completed"),
                cut
                    ? trLocal("Przenoszenie", "Moving")
                    : trLocal("Kopiowanie", "Copying"))) {
            return;
        }

        if (!mutationAllowed()) return;

        KIO::CopyJob *job =
            cut
                ? KIO::move(
                    urls,
                    destination,
                    KIO::HideProgressInfo)
                : KIO::copy(
                    urls,
                    destination,
                    KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        if (m_undoController) {
            m_undoController->recordCopyJob(job);
        }

        watchFileOperation(
            job,
            cut
                ? trLocal("Przenoszenie zakończone", "Move completed")
                : trLocal("Kopiowanie zakończone", "Copy completed"),
            cut,
            cut
                ? trLocal("Przenoszenie", "Moving")
                : trLocal("Kopiowanie", "Copying"));
    }

    void copySelectionToDirectory(
        const QList<QUrl> &urls,
        const QString &directory,
        const QString &successMessage)
    {
        if (!mutationAllowed()) return;
        if (urls.isEmpty()
            || directory.isEmpty()) {
            return;
        }

        QDir target(directory);
        if (!target.exists()
            && (!mutationAllowed() || !QDir().mkpath(directory))) {
            QMessageBox::warning(
                m_parentWidget,
                trLocal("Wyślij do", "Send to"),
                trLocal(
                    "Nie udało się utworzyć katalogu docelowego.",
                    "Could not create the destination directory."));
            return;
        }

        if (startNativeSingleFileTransfer(
                urls, QUrl::fromLocalFile(directory), false, false,
                successMessage, trLocal("Kopiowanie", "Copying"))) {
            return;
        }

        if (!mutationAllowed()) return;

        auto *job =
            KIO::copy(
                urls,
                QUrl::fromLocalFile(directory),
                KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        if (m_undoController) {
            m_undoController->recordCopyJob(job);
        }

        watchFileOperation(
            job,
            successMessage,
            false,
            trLocal("Kopiowanie", "Copying"));
    }

    void createNewFile(
        const QUrl &directory,
        const QString &suggestedName,
        const QByteArray &contents)
    {
        if (!mutationAllowed()) return;
        if (!directory.isValid()) {
            return;
        }

        if (!directory.isLocalFile()) {
            requestRemoteNewItemName(
                directory, suggestedName, false,
                [this, directory, contents](const QString &name) {
                    createNewFileWithName(directory, name, contents);
                });
            return;
        }
        cancelPendingNewItem();

        const QString name = requestNewFileName(
            suggestedAvailableName(directory, suggestedName, false));
        if (name.isEmpty()) return;
        createNewFileWithName(directory, name, contents);
    }

    void createFromTemplate(QUrl directory, QUrl source)
    {
        if (!mutationAllowed()) return;
        if (!directory.isValid() || !source.isLocalFile()) return;

        if (!directory.isLocalFile()) {
            requestRemoteNewItemName(
                directory, source.fileName(), false,
                [this, directory, source](const QString &name) {
                    createFromTemplateWithName(directory, source, name);
                });
            return;
        }
        cancelPendingNewItem();

        const QString name = requestNewFileName(
            suggestedAvailableName(directory, source.fileName(), false));
        if (name.isEmpty()) return;
        createFromTemplateWithName(directory, source, name);
    }

    void createNewFolder(const QUrl &directory)
    {
        if (!mutationAllowed()) return;
        if (!directory.isValid()) {
            return;
        }

        if (!directory.isLocalFile()) {
            requestRemoteNewItemName(
                directory, trLocal("Nowy folder", "New folder"), true,
                [this, directory](const QString &name) {
                    createNewFolderWithName(directory, name);
                });
            return;
        }
        cancelPendingNewItem();

        bool ok = false;

        const QString name =
            QInputDialog::getText(
                m_parentWidget,
                trLocal("Nowy folder", "New folder"),
                trLocal("Nazwa folderu:", "Folder name:"),
                QLineEdit::Normal,
                suggestedAvailableName(
                    directory, trLocal("Nowy folder", "New folder"), true),
                &ok)
                .trimmed();

        if (!ok) {
            return;
        }

        if (!validNewName(name)) {
            QMessageBox::warning(
                m_parentWidget,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa folderu jest pusta albo zawiera niedozwolony znak „/”.",
                    "The folder name is empty or contains the invalid “/” character."));
            return;
        }

        createNewFolderWithName(directory, name);
    }

    void renameSelected(const QList<QUrl> &urls, QString oldName)
    {
        if (!mutationAllowed()) return;

        if (urls.size() != 1) {
            return;
        }

        const QUrl source = urls.first();


        if (oldName.isEmpty()) {
            oldName =
                QFileInfo(source.path()).fileName();
        }

        bool ok = false;

        const QString newName =
            QInputDialog::getText(
                m_parentWidget,
                trLocal("Zmień nazwę", "Rename"),
                trLocal("Nowa nazwa:", "New name:"),
                QLineEdit::Normal,
                oldName,
                &ok)
                .trimmed();

        if (!ok || newName == oldName) {
            return;
        }

        if (!validNewName(newName)) {
            QMessageBox::warning(
                m_parentWidget,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa jest pusta albo zawiera niedozwolony znak „/”.",
                    "The name is empty or contains the invalid “/” character."));
            return;
        }

        const QUrl destination =
            siblingUrlWithName(source, newName);

        if (!mutationAllowed()) return;

        KIO::CopyJob *job =
            KIO::moveAs(
                source,
                destination,
                KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        if (m_undoController) {
            m_undoController->recordCopyJob(job);
        }

        watchFileOperation(
            job,
            trLocal("Zmieniono nazwę", "Renamed"),
            false,
            trLocal("Zmiana nazwy", "Renaming"));
    }

    void batchRenameSelected(const QList<QUrl> &urls)
    {
        if (!mutationAllowed()) return;
        if (m_linearBatchRunning) return;
        // Unresolved journal is a safety fence: never silently start another
        // batch that could invalidate the original inode/path evidence.
        const QString pending = BatchRenameRecoveryJournal::pendingJournal();
        if (!pending.isEmpty()) {
            QMessageBox::critical(m_parentWidget,
                trLocal("Nierozwiązany dziennik zmiany nazw", "Unresolved rename journal"),
                trLocal("Poprzedni cykl mógł zostać przerwany. Nie wykonano żadnych zmian. Sprawdź ręcznie pliki i dziennik: ",
                        "An earlier cycle may have been interrupted. No changes were made. Inspect the files and journal manually: ")
                    + pending);
            return;
        }
        const QList<QUrl> snapshot = urls;
        BatchRenameDialog dialog(snapshot, m_parentWidget);
        if (dialog.exec() != QDialog::Accepted) return;
        const BatchRenamePlan plan = dialog.plan();
        if (!plan.isValid()) return;
        const bool undoableSingleCycle = m_undoController
            && batchRenameSingleCycleHistoryEligible(plan);
        const bool undoableLinear = m_undoController
            && batchRenameLinearHistoryEligible(plan);
        const bool undoableSingleSwap = plan.activeCount() == 2
            && plan.executionOrder.isEmpty() && plan.exchangeCycles.isEmpty()
            && plan.atomicSwaps.size() == 1 && m_undoController;
        if (!plan.atomicSwaps.isEmpty() || !plan.exchangeCycles.isEmpty()) {
            const bool multiStage = !plan.exchangeCycles.isEmpty();
            const auto answer = QMessageBox::warning(
                m_parentWidget,
                undoableSingleCycle
                    ? trLocal("Cykl z Undo/Redo (nieatomowy)", "Cycle with non-atomic Undo/Redo")
                    : undoableSingleSwap
                    ? trLocal("Atomowa zamiana z Undo", "Atomic exchange with Undo")
                    : trLocal("Zamiana nazw bez Undo", "Name exchange without Undo"),
                multiStage
                ? trLocal(
                    "Ta partia zawiera cykl co najmniej trzech nazw. "
                    "Każda wymiana jest atomowa, ale cały cykl NIE jest atomowy. "
                    "W razie błędu lub anulowania program spróbuje przywrócić pierwotne nazwy. "
                    "Awaria procesu może pozostawić częściowo zmienione nazwy. "
                    "Program zapisuje trwały dziennik do ręcznej kontroli po awarii, "
                    "ale nie przywraca plików automatycznie. "
                    "Nierozwiązany dziennik blokuje kolejne operacje Batch Rename. "
                    "Dla samodzielnego, ukończonego cyklu Ctrl+Z/Ctrl+Y mogą cofnąć/ponowić całość "
                    "wieloma nieatomowymi wymianami z osobnym dziennikiem. Inne partie nie mają wspólnego Undo. "
                    "Dotychczasowa historia Undo zostanie unieważniona. "
                    "Wykonuj tylko na kopiach. Kontynuować?",

                    "This batch contains a cycle of three or more names. "
                    "Each exchange is atomic, but the whole cycle is NOT atomic. "
                    "On error or cancellation, the program will attempt to restore the original names. "
                    "A process crash may leave partially exchanged names. "
                    "A persistent journal is saved for manual inspection after a crash, "
                    "but automatic recovery is not available. "
                    "An unresolved journal blocks further Batch Rename operations. "
                    "A completed isolated cycle can be undone/redone with Ctrl+Z/Ctrl+Y "
                    "through multiple journaled, non-atomic exchanges. Mixed batches do not have one Undo. "
                    "Previous Undo history will be invalidated. "
                    "Use copies only. Continue?")
                : undoableSingleSwap
                    ? trLocal("Ta partia zamieni dwie nazwy jednym atomowym wywołaniem Linux. "
                              "Po wykonaniu Ctrl+Z i Ctrl+Y mogą odwrócić lub ponowić tę jedną zamianę, "
                              "o ile pliki pozostaną niezmienione. Starsza historia Undo zostanie unieważniona. Kontynuować?",
                              "This batch swaps two names in one atomic Linux syscall. "
                              "Ctrl+Z and Ctrl+Y can undo or redo this exchange while both files remain unchanged. "
                              "Older Undo history will be invalidated. Continue?")
                    : trLocal(
                    "Ta partia zawiera zamianę dwóch istniejących nazw. "
                    "Program spróbuje wykonać ją atomowo w Linux, bez plików tymczasowych. "
                    "Ctrl+Z nie cofnie zamiany, a dotychczasowa historia Undo zostanie unieważniona. "
                    "Kontynuować?",
                    "This batch contains a two-way atomic Linux exchange without temporary files. "
                    "Ctrl+Z cannot undo the exchange and previous Undo history will be invalidated. "
                    "Continue?"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) return;
        }
        if (undoableLinear) {
            const auto answer = QMessageBox::warning(m_parentWidget,
                trLocal("Łańcuch nazw z Undo/Redo", "Rename chain with Undo/Redo"),
                trLocal("Po pełnym sukcesie tej lokalnej partii jedno Ctrl+Z/Ctrl+Y "
                        "cofa/ponawia wszystkie nazwy. Odtwarzanie jest wieloetapowe, "
                        "NIE jest atomowe i używa trwałego dziennika v2. Po awarii procesu "
                        "następny start dokończy wyłącznie ten kwalifikujący się Execute, "
                        "bez nadpisywania; konflikt pozostanie zablokowany do ręcznej kontroli. "
                        "Starsza historia Undo zostanie unieważniona dopiero "
                        "po pomyślnym zakończeniu partii. Program wykonuje całą partię "
                        "bez nadpisywania i bez możliwości anulowania w trakcie; błąd "
                        "powoduje próbę bezpiecznego przywrócenia nazw. "
                        "Testuj na kopiach. Kontynuować?",
                        "After this local batch fully succeeds, one Ctrl+Z/Ctrl+Y "
                        "undoes/redoes all names. Replay uses multiple non-atomic moves "
                        "and a durable v2 journal. After a process crash, the next startup "
                        "finishes only this qualifying forward Execute without overwriting; "
                        "conflicts remain blocked for manual inspection. "
                        "Older Undo history is invalidated only after full success. "
                        "The batch uses no-overwrite moves and cannot be canceled mid-run; "
                        "on failure it attempts verified rollback. Use copies only. Continue?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes) return;
        }
        if (!mutationAllowed()) return;
        // The modal preview may have been open while other programs changed
        // any selected source or target. Abort before touching the first item.
        if (!batchRenamePlanReady(plan)) {
            QMessageBox::warning(m_parentWidget,
                trLocal("Zbiorcza zmiana nazw", "Batch Rename"),
                trLocal("System plików zmienił się od czasu podglądu. Nie zmieniono żadnej nazwy.",
                        "The filesystem changed after the preview. No names were changed."));
            return;
        }

        // Eligible local, acyclic batches must NOT also register their individual
        // KIO moves. Those asynchronous per-item command recordings can reach the
        // KIO undo stack after recordCompletedLinear(), making Ctrl+Z select just
        // the last move. Execute the complete mapping through the existing
        // journaled, no-overwrite native replay, then publish ONE history entry.
        // An interrupted/failed replay attempts a verified rollback; any
        // ambiguous result retains its journal and never publishes an Undo.
        if (undoableLinear) {
            if (!mutationAllowed()) return;
            if (!m_undoController->beginGroupedBatch()) {
                QMessageBox::warning(m_parentWidget,
                    trLocal("Historia niedostępna", "Undo history unavailable"),
                    trLocal("Nie można bezpiecznie rozpocząć partii. Historia jest zajęta lub dziennik wymaga kontroli.",
                            "Cannot safely start the batch. History is busy or a journal needs inspection."));
                return;
            }
            m_linearBatchRunning = true;
            auto *progress = new QProgressDialog(
                trLocal("Bezpieczna zmiana nazw (bez możliwości przerwania)…",
                        "Renaming safely (cannot interrupt in progress)…"),
                QString(), 0, 0, m_parentWidget);
            progress->setWindowTitle(trLocal("Zbiorcza zmiana nazw", "Batch Rename"));
            progress->setWindowModality(Qt::WindowModal);
            progress->setCancelButton(nullptr);
            progress->setMinimumDuration(0);
            progress->show();

            auto *watcher = new QFutureWatcher<BatchRenameCycleReplayResult>(this);
            connect(watcher, &QFutureWatcher<BatchRenameCycleReplayResult>::finished,
                    this, [this, watcher, progress, plan] {
                const auto result = watcher->result();
                watcher->deleteLater();
                progress->close();
                progress->deleteLater();
                bool recorded = false;
                if (result.success && m_undoController)
                    recorded = m_undoController->recordCompletedLinear(plan);
                // A completed but unrecordable mapping, or any uncertain
                // replay, may leave earlier path-based history unsafe.
                if (m_undoController && ((result.success && !recorded) || result.uncertain))
                    m_undoController->invalidateUndoBeforeAtomicSwap();
                m_linearBatchRunning = false;
                if (m_undoController) {
                    m_undoController->endGroupedBatch();
                    m_undoController->refreshAfterAtomicSwap();
                }
                if (result.uncertain) {
                    QMessageBox::critical(m_parentWidget,
                        trLocal("Wymagana ręczna kontrola", "Manual inspection required"),
                        result.error);
                } else if (!result.success) {
                    QMessageBox::warning(m_parentWidget,
                        trLocal("Partia zatrzymana", "Rename batch stopped"), result.error);
                } else if (!recorded) {
                    QMessageBox::warning(m_parentWidget,
                        trLocal("Undo partii niedostępne", "Batch Undo unavailable"),
                        trLocal("Nazwy zmieniono, lecz nie można bezpiecznie zarejestrować wspólnego Undo. Sprawdź pliki.",
                                "Renames completed, but grouped Undo could not be recorded safely. Inspect the files."));
                }
            });
            watcher->setFuture(QtConcurrent::run([plan] {
                return batchRenameExecuteLinearV2(plan);
            }));
            return;
        }

        // Any batch containing a non-journaled exchange must be isolated from
        // its FIRST dispatch (including preliminary KIO moves) until terminal
        // GUI cleanup. Otherwise another action or window close can slip in
        // before the first or between two deferred exchanges.
        if (!plan.atomicSwaps.isEmpty()) {
            if (m_undoController && !m_undoController->beginGroupedBatch()) {
                QMessageBox::warning(m_parentWidget,
                    trLocal("Historia zajęta", "History busy"),
                    trLocal("Nie można bezpiecznie rozpocząć partii nazw.",
                            "Cannot safely start the rename batch."));
                return;
            }
            auto &gate = BatchRenameRecoveryGate::instance();
            if (!gate.beginUnjournaledSwap()) {
                if (m_undoController) m_undoController->endGroupedBatch();
                if (m_showRecoveryFence) m_showRecoveryFence(gate.message());
                return;
            }
            m_swapBatchRunning = true;
        }

        struct State {
            BatchRenamePlan plan;
            int next = 0;
            int nextSwap = 0;
            int nextCycle = 0;
            bool cycleActive = false;
            BatchRenameCycleState cycleState;
            BatchRenameRecoveryJournal cycleJournal;
            bool swapAttempted = false;
            bool swapUncertain = false;
            bool swapFailed = false;
            bool cycleFailed = false;
            QString cycleFailureDetail;
            int completed = 0;
            bool canceled = false;
            QPointer<KIO::CopyJob> current;
            QPointer<QProgressDialog> progress;
            QSharedPointer<std::function<void()>> runner;
        };
        auto state = QSharedPointer<State>::create();
        state->plan = plan;
        state->progress = new QProgressDialog(
            trLocal("Zmienianie nazw…", "Renaming items…"),
            trLocal("Anuluj", "Cancel"), 0, plan.activeCount(), m_parentWidget);
        state->progress->setWindowTitle(trLocal("Zbiorcza zmiana nazw", "Batch Rename"));
        state->progress->setWindowModality(Qt::WindowModal);
        state->progress->setMinimumDuration(0);

        state->runner = QSharedPointer<std::function<void()>>::create();
        *state->runner = [this, state, undoableSingleCycle] {
            // Terminal cleanup clears state->runner to break the runner/state
            // ownership cycle. Keep the currently executing std::function
            // alive until this invocation returns; otherwise clearing it
            // destroys this lambda and its captures mid-call.
            const auto keepRunnerAlive = state->runner;
            // Re-audit on every asynchronous continuation before its next
            // dispatch.  Once this operation has opened its own cycle journal,
            // that journal is expected and the cycle's verified rollback path
            // must remain available.
            // A completed batch has no next mutation to authorize. In
            // particular, Undo may take the swap fence before a deferred
            // completion callback; that must not retroactively cancel Execute.
            const bool allDispatched =
                state->next >= state->plan.executionOrder.size()
                && state->nextSwap >= state->plan.atomicSwaps.size()
                && state->nextCycle >= state->plan.exchangeCycles.size();
            if (!state->canceled && !state->cycleActive && !allDispatched) {
                // Other entry points must remain fenced for the ENTIRE batch
                // containing a swap. Its own continuation still re-audits
                // foreign journals/lock loss rather than bypassing safety.
                auto &gate = BatchRenameRecoveryGate::instance();
                if (m_swapBatchRunning ? !gate.unjournaledSwapContinuationAllowed()
                                       : !mutationAllowed())
                    state->canceled = true;
            }
            // Cancellation between two exchanges of a 3+ cycle must NOT
            // leave an avoidable intermediate permutation. Reverse only
            // verified exchanges, and never overwrite an unknown inode.
            if (state->canceled && state->cycleActive) {
                const auto &cycle = state->plan.exchangeCycles.at(state->nextCycle);
                const int appliedExchanges = state->cycleState.stepsDone;
                QString rollbackError;
                bool restored = !state->cycleState.uncertain;
                // The durable intent must precede EACH rollback exchange.
                // If it cannot be written, leave the cycle and journal intact.
                while (restored && state->cycleState.stepsDone > 0) {
                    if (!state->cycleJournal.checkpoint(
                            QStringLiteral("rollback-intent"), state->cycleState,
                            &rollbackError)) {
                        restored = false;
                        break;
                    }
                    if (!batchRenameCycleExchange(state->plan, cycle, state->cycleState,
                                                  state->cycleState.stepsDone - 1,
                                                  &rollbackError)) {
                        restored = false;
                        break;
                    }
                    --state->cycleState.stepsDone;
                    if (!state->cycleJournal.checkpoint(
                            QStringLiteral("rollback-verified"), state->cycleState,
                            &rollbackError)) {
                        restored = false;
                        break;
                    }
                }
                restored = restored && state->cycleState.stepsDone == 0
                    && batchRenameCycleMatches(state->plan, cycle, state->cycleState);
                if (restored) {
                    restored = state->cycleJournal.finish(
                        QStringLiteral("verified-rollback"), state->cycleState,
                        &rollbackError);
                }
                state->cycleActive = false;
                if (!restored) {
                    state->cycleFailed = true;
                    state->cycleFailureDetail = rollbackError.isEmpty()
                        ? trLocal("Nie można potwierdzić tożsamości wszystkich plików.",
                                  "Not all file identities could be verified.")
                        : rollbackError;
                    QMessageBox::critical(m_parentWidget,
                        trLocal("Cykl wymaga ręcznej kontroli", "Cycle requires manual inspection"),
                        trLocal("Nie udało się bezpiecznie przywrócić całego cyklu. Wstrzymano operacje. NIE usuwaj dziennika przed kontrolą: ",
                                "Could not safely restore the cycle. Operations stopped. DO NOT remove the journal before inspection: ")
                            + state->cycleJournal.path() + QStringLiteral("\n")
                            + state->cycleFailureDetail);
                } else if (appliedExchanges > 0) {
                    QMessageBox::information(m_parentWidget,
                        trLocal("Cykl przywrócony", "Cycle restored"),
                        trLocal("Przywrócono oryginalne nazwy bieżącego cyklu. Inne wcześniej ukończone operacje w tej partii nie zostały cofnięte.",
                                "Original names for the current cycle were restored. Earlier completed operations in this batch were not undone."));
                }
            }
            if (state->canceled || allDispatched) {
                // Capture the outcome before closing QProgressDialog: closing
                // the widget is UI cleanup, not a user cancellation.
                const bool stopped = state->canceled
                    || state->completed != state->plan.activeCount();
                if (state->progress) state->progress->close();
                if (state->swapAttempted && m_undoController)
                    m_undoController->refreshAfterAtomicSwap();
                if (stopped) {
                    QMessageBox::information(
                        m_parentWidget, trLocal("Zbiorcza zmiana nazw", "Batch Rename"),
                        (state->cycleFailed
                            ? trLocal("Operacja zatrzymana. Wymagana ręczna kontrola cyklu! Potwierdzone zmiany innych elementów: ",
                                      "Operation stopped. Manual cycle inspection required! Confirmed changes to other items: ")
                            : state->swapFailed
                                ? trLocal("Operacja zatrzymana. Sprawdź obie nazwy ostatniej zamiany. Potwierdzone zmiany: ",
                                          "Operation stopped. Inspect both names from the last exchange. Confirmed renames: ")
                                : trLocal("Operacja zatrzymana. Ukończone zmiany: ",
                                          "Operation stopped. Completed renames: "))
                            + QString::number(state->completed));
                }
                state->runner.clear();
                if (m_swapBatchRunning) {
                    m_swapBatchRunning = false;
                    BatchRenameRecoveryGate::instance().finishUnjournaledSwap(
                        state->swapUncertain);
                    if (m_undoController) m_undoController->endGroupedBatch();
                }
                return;
            }
            if (state->next >= state->plan.executionOrder.size()
                && state->nextSwap < state->plan.atomicSwaps.size()) {
                const auto pair = state->plan.atomicSwaps.at(state->nextSwap);
                if (!batchRenameSwapReady(state->plan, pair.first, pair.second)) {
                    state->canceled = true;
                    QMessageBox::warning(m_parentWidget,
                        trLocal("Zbiorcza zmiana nazw", "Batch Rename"),
                        trLocal("Zaznaczone pliki zmieniły się przed atomową zamianą. Zatrzymano dalsze operacje.",
                                "Selected files changed before the atomic exchange. Further operations stopped."));
                    (*state->runner)();
                    return;
                }
                // Freeze all cooperating mutations and Undo/Redo before
                // dispatch. Invalidate obsolete path-based history BEFORE the
                // worker can exchange the occupants; never register fake KIO.
                // The whole-batch gate was acquired before the first dispatch.
                // Do not reacquire a fence that this runner already owns.
                if (!m_swapBatchRunning
                    || !BatchRenameRecoveryGate::instance().unjournaledSwapRunning()) {
                    state->canceled = true;
                    (*state->runner)();
                    return;
                }
                m_swapWorkerRunning = true;
                if (m_undoController) m_undoController->invalidateUndoBeforeAtomicSwap();
                state->swapAttempted = true;
                auto *watcher = new QFutureWatcher<BatchRenameSwapWorkerResult>(this);
                connect(watcher, &QFutureWatcher<BatchRenameSwapWorkerResult>::finished,
                        this, [this, watcher, state] {
                    const auto result = watcher->result();
                    watcher->deleteLater();
                    bool historyUntrusted = false;
                    if (!result.success) {
                        state->swapFailed = true;
                        state->canceled = true;
                        QMessageBox::warning(m_parentWidget,
                            trLocal("Błąd atomowej zamiany nazw", "Atomic Exchange Error"),
                            result.error);
                    } else {
                        if (state->plan.activeCount() == 2
                            && state->plan.executionOrder.isEmpty()
                            && state->plan.exchangeCycles.isEmpty()
                            && state->plan.atomicSwaps.size() == 1 && m_undoController
                            && !m_undoController->recordCompletedAtomicSwap(state->plan)) {
                            // Post-exchange mapping or history could not be
                            // proven: freeze mutations for manual inspection.
                            historyUntrusted = true;
                            state->swapFailed = true;
                            state->canceled = true;
                            QMessageBox::warning(m_parentWidget,
                                trLocal("Undo niedostępne", "Undo unavailable"),
                                trLocal("Zamiana się zakończyła, ale nie można bezpiecznie zapisać Undo. Sprawdź obie nazwy.",
                                        "Exchange completed, but a safe Undo record could not be created. Inspect both names."));
                        }
                        ++state->nextSwap;
                        state->completed += 2;
                        if (state->progress) state->progress->setValue(state->completed);
                    }
                    m_swapWorkerRunning = false;
                    state->swapUncertain = result.uncertain || historyUntrusted;
                    // Keep the process-wide mutation/close fence AND grouped
                    // Undo fence through every deferred continuation. The
                    // terminal runner releases both only after GUI cleanup.
                    const bool terminal = state->canceled
                        || (state->next >= state->plan.executionOrder.size()
                            && state->nextSwap >= state->plan.atomicSwaps.size()
                            && state->nextCycle >= state->plan.exchangeCycles.size());
                    if (terminal && state->runner) (*state->runner)();
                    // Only intermediate steps are deferred to yield the GUI.
                    // A Cancel cannot interrupt the exchange already done.
                    if (!terminal) {
                        int yieldDelay = 0;
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
                        bool delayOk = false;
                        const int configured = QString::fromLocal8Bit(
                            qgetenv("THISPC_SWAP_BATCH_GAP_DELAY_MS")).toInt(&delayOk);
                        if (delayOk && configured > 0) yieldDelay = qMin(configured, 5000);
#endif
                        QTimer::singleShot(yieldDelay, this, [state] {
                            if (state->runner) (*state->runner)();
                        });
                    }
                });
                const auto planCopy = state->plan;
                m_swapFuture = QtConcurrent::run([planCopy, pair] {
                    BatchRenameSwapWorkerResult result;
#ifdef THISPC_BATCH_RENAME_TEST_HOOKS
                    bool delayOk = false;
                    const int delay = QString::fromLocal8Bit(qgetenv("THISPC_SWAP_WORKER_DELAY_MS"))
                                          .toInt(&delayOk);
                    if (delayOk && delay > 0) QThread::msleep(unsigned(delay));
#endif
                    result.success = batchRenameAtomicSwap(planCopy, pair.first, pair.second,
                                                           &result.error, &result.uncertain);
                    return result;
                });
                watcher->setFuture(m_swapFuture);
                return;
            }
            if (state->next >= state->plan.executionOrder.size()
                && state->nextSwap >= state->plan.atomicSwaps.size()
                && state->nextCycle < state->plan.exchangeCycles.size()) {
                const auto &cycle = state->plan.exchangeCycles.at(state->nextCycle);
                if (!state->cycleActive) {
                    if (!batchRenameCycleReady(state->plan, cycle)) {
                        state->canceled = true;
                        QMessageBox::warning(m_parentWidget,
                            trLocal("Zbiorcza zmiana nazw", "Batch Rename"),
                            trLocal("Pliki cyklu zmieniły się przed wykonaniem. Niczego w tym cyklu nie zmieniono.",
                                    "Cycle files changed before execution. No names in this cycle were changed."));
                        (*state->runner)();
                        return;
                    }
                    state->cycleState = batchRenameCycleInitialState(cycle);
                    QString journalError;
                    if (!state->cycleJournal.begin(state->plan, cycle, &journalError)) {
                        state->canceled = true;
                        QMessageBox::critical(m_parentWidget,
                            trLocal("Brak bezpiecznego dziennika", "No safe recovery journal"),
                            trLocal("Nie rozpoczęto cyklu. ", "Cycle was not started. ")
                                + journalError);
                        (*state->runner)();
                        return;
                    }
                    state->cycleActive = true;
                }
                // KIO has no Undo command representing multiple native
                // exchanges. Clear obsolete entries BEFORE touching paths.
                if (m_undoController) m_undoController->invalidateUndoBeforeAtomicSwap();
                state->swapAttempted = true;
                QString cycleError;
                if (!state->cycleJournal.checkpoint(
                        QStringLiteral("exchange-intent"), state->cycleState, &cycleError)) {
                    state->canceled = true;
                    QMessageBox::critical(m_parentWidget,
                        trLocal("Błąd dziennika", "Journal error"),
                        trLocal("Nie rozpoczęto następnej wymiany. ",
                                "Next exchange was not started. ") + cycleError);
                    (*state->runner)();
                    return;
                }
                if (!batchRenameCycleAdvance(state->plan, cycle,
                                              state->cycleState, &cycleError)) {
                    state->canceled = true;
                    // The next runner pass attempts rollback of only verified
                    // prior exchanges. A failed identity check fails closed.
                    QMessageBox::warning(m_parentWidget,
                        trLocal("Błąd cyklu nazw", "Name cycle error"), cycleError);
                    (*state->runner)();
                    return;
                }
                // If persistence fails after the syscall, do not perform
                // further exchanges. Retain the journal for manual inspection.
                if (!state->cycleJournal.checkpoint(
                        QStringLiteral("exchange-verified"), state->cycleState,
                        &cycleError)) {
                    state->canceled = true;
                    state->cycleFailed = true;
                    state->cycleFailureDetail = cycleError;
                    state->cycleActive = false;
                    QMessageBox::critical(m_parentWidget,
                        trLocal("Dziennik wymaga kontroli", "Journal requires inspection"),
                        trLocal("Wymiana mogła się udać, ale nie zapisano bezpiecznie wyniku. Nie cofaj automatycznie; sprawdź: ",
                                "The exchange may have succeeded but its result was not durably recorded. Do not roll back automatically; inspect: ")
                            + state->cycleJournal.path() + QStringLiteral("\n") + cycleError);
                    (*state->runner)();
                    return;
                }
                if (batchRenameCycleComplete(state->plan, cycle, state->cycleState)) {
                    state->completed += cycle.size();
                    if (state->progress) state->progress->setValue(state->completed);
                    if (!state->cycleJournal.finish(
                            QStringLiteral("verified-complete"), state->cycleState,
                            &cycleError)) {
                        state->canceled = true;
                        state->cycleFailed = true;
                        state->cycleFailureDetail = cycleError;
                        QMessageBox::critical(m_parentWidget,
                            trLocal("Dziennik wymaga kontroli", "Journal requires inspection"),
                            trLocal("Cykl zakończył się, ale nie udało się zamknąć dziennika. Sprawdź: ",
                                    "Cycle completed but journal cleanup failed. Inspect: ")
                                + state->cycleJournal.path() + QStringLiteral("\n") + cycleError);
                    }
                    if (!state->cycleFailed && undoableSingleCycle && m_undoController
                        && !m_undoController->recordCompletedCycle(state->plan)) {
                        QMessageBox::warning(m_parentWidget,
                            trLocal("Undo niedostępne", "Undo unavailable"),
                            trLocal("Cykl zakończony, ale nie można bezpiecznie zapisać Undo. Sprawdź wszystkie nazwy.",
                                    "Cycle completed, but safe Undo recording failed. Inspect every name."));
                    }
                    ++state->nextCycle;
                    state->cycleActive = false;
                }
                // Handle Cancel between syscalls; no recursive execution of
                // a large cycle and no nested GUI event processing in syscall.
                QTimer::singleShot(0, this, [state] {
                    if (state->runner) (*state->runner)();
                });
                return;
            }
            const BatchRenameEntry entry =
                state->plan.entries.at(state->plan.executionOrder.at(state->next));
            if (!batchRenameEntryReady(entry)) {
                state->canceled = true;
                QMessageBox::warning(
                    m_parentWidget, trLocal("Zbiorcza zmiana nazw", "Batch Rename"),
                    trLocal("System plików zmienił się od czasu podglądu. Pozostałe nazwy nie zostały zmienione.",
                            "The filesystem changed after the preview. Remaining items were not renamed."));
                (*state->runner)();
                return;
            }
            auto *job = KIO::moveAs(entry.source, entry.destination, KIO::HideProgressInfo);
            job->setUiDelegate(nullptr);
            state->current = job;
            if (m_undoController) m_undoController->recordCopyJob(job);
            connect(job, &KJob::result, this, [this, state](KJob *finished) {
                state->current = nullptr;
                if (finished->error() != KJob::NoError) {
                    state->canceled = true;
                    QMessageBox::warning(
                        m_parentWidget, trLocal("Błąd zmiany nazwy", "Rename Error"),
                        finished->errorString());
                } else {
                    ++state->completed;
                    ++state->next;
                    if (state->progress) state->progress->setValue(state->completed);
                }
                (*state->runner)();
            });
            watchFileOperation(job, trLocal("Zmieniono nazwę", "Renamed"), false,
                               trLocal("Zbiorcza zmiana nazw", "Batch renaming"));
        };
        connect(state->progress, &QProgressDialog::canceled, this, [state] {
            // An in-flight KIO move may already have renamed its source when
            // kill() returns an error. Wait for its result instead, so that
            // completion and Undo recording cannot silently become ambiguous.
            // Cancellation stops dispatching *subsequent* entries.
            state->canceled = true;
        });
        (*state->runner)();
    }

    void trashSelected(const QList<QUrl> &urls)
    {
        if (!mutationAllowed()) return;

        if (urls.isEmpty()) {
            return;
        }

        for (const QUrl &url : urls) {
            if (!url.isLocalFile()) {
                QMessageBox::information(
                    m_parentWidget,
                    trLocal("Kosz", "Trash"),
                    trLocal(
                        "W tej wersji usuwanie do Kosza jest dostępne tylko dla lokalnych plików i folderów.",
                        "In this version, moving to Trash is available only for local files and folders."));
                return;
            }
        }

        const QString question =
            isPolish()
                ? QStringLiteral(
                    "Przenieść zaznaczone elementy do Kosza?\n\nLiczba elementów: %1")
                    .arg(urls.size())
                : QStringLiteral(
                    "Move the selected items to Trash?\n\nItems: %1")
                    .arg(urls.size());

        if (QMessageBox::question(
                m_parentWidget,
                trLocal("Przenieś do Kosza", "Move to Trash"),
                question,
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }

        if (!mutationAllowed()) return;

        KIO::CopyJob *job =
            KIO::trash(
                urls,
                KIO::HideProgressInfo);

        job->setUiDelegate(nullptr);
        if (m_undoController) {
            m_undoController->recordTrashJob(
                urls,
                job);
        }

        watchFileOperation(
            job,
            trLocal(
                "Przeniesiono do Kosza",
                "Moved to Trash"),
            false,
            trLocal("Przenoszenie do Kosza", "Moving to Trash"));
    }

    static bool isTrashRoot(const QUrl &directory)
    {
        return directory.isValid() && directory.scheme() == QStringLiteral("trash")
            && directory.authority().isEmpty()
            && (directory.path().isEmpty() || directory.path() == QStringLiteral("/"))
            && !directory.hasQuery() && !directory.hasFragment();
    }

    bool canEmptyTrash(const QUrl &directory) const
    {
        return isTrashRoot(directory) && !m_confirmingEmptyTrash && !m_emptyTrashJob;
    }

    void emptyTrash(QUrl directory, const RefreshViews &refreshViews)
    {
        if (!mutationAllowed()) return;
        if (!canEmptyTrash(directory)) return;

        QScopedValueRollback<bool> confirming(m_confirmingEmptyTrash, true);
        if (QMessageBox::warning(
                m_parentWidget,
                trLocal("Opróżnij kosz", "Empty Trash"),
                trLocal(
                    "Trwale usunąć wszystkie elementy z Kosza?\n\nTej operacji nie można cofnąć.",
                    "Permanently delete all items from Trash?\n\nThis operation cannot be undone."),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            return;
        }

        if (!mutationAllowed()) return;

        auto *job = KIO::emptyTrash();
        job->setUiDelegate(nullptr);
        m_emptyTrashJob = job;
        connect(job, &KJob::result, this, [this] { m_emptyTrashJob = nullptr; });
        // Emptying Trash is irreversible; do not record it as an undoable job.
        watchFileOperation(job,
            trLocal("Kosz został opróżniony", "Trash emptied"), false,
            trLocal("Opróżnianie kosza", "Emptying Trash"), refreshViews);
    }

    void transfer(const QList<QUrl> &urls, const QUrl &destination, Qt::DropAction action)
    {
        if (!mutationAllowed()) return;
        if (urls.isEmpty() || !destination.isValid()) return;
        if (!mutationAllowed()) return;
        const bool move = action == Qt::MoveAction;
        if (startNativeSingleFileTransfer(
                urls, destination, move, false,
                move
                    ? trLocal("Przenoszenie zakończone", "Move completed")
                    : trLocal("Kopiowanie zakończone", "Copy completed"),
                move
                    ? trLocal("Przenoszenie", "Moving")
                    : trLocal("Kopiowanie", "Copying"))) {
            return;
        }
        if (!mutationAllowed()) return;
        KIO::CopyJob *job =
            action == Qt::MoveAction
                ? KIO::move(
                    urls,
                    destination,
                    KIO::HideProgressInfo)
                : KIO::copy(
                    urls,
                    destination,
                    KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        if (m_undoController) {
            m_undoController->recordCopyJob(job);
        }

        watchFileOperation(
            job,
            action == Qt::MoveAction
                ? trLocal("Przenoszenie zakończone", "Move completed")
                : trLocal("Kopiowanie zakończone", "Copy completed"),
            false,
            action == Qt::MoveAction
                ? trLocal("Przenoszenie", "Moving")
                : trLocal("Kopiowanie", "Copying"));
    }

private:
    void cancelPendingNewItem()
    {
        ++m_newItemRequestSerial;
        if (!m_newItemListJob) return;
        disconnect(m_newItemListJob, nullptr, this, nullptr);
        m_newItemListJob->kill(KJob::Quietly);
        m_newItemListJob = nullptr;
    }

    KJob *startDirectorySnapshot(
        const QUrl &directory, const DirectorySnapshotCallback &callback)
    {
        if (m_startDirectorySnapshot) {
            return m_startDirectorySnapshot(directory, this, callback);
        }

        auto names = QSharedPointer<QSet<QString>>::create();
        KIO::ListJob *job = KIO::listDir(directory, KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        connect(job, &KIO::ListJob::entries, this,
                [names](KIO::Job *, const KIO::UDSEntryList &entries) {
            for (const KIO::UDSEntry &entry : entries) {
                const QString name = entry.stringValue(KIO::UDSEntry::UDS_NAME);
                if (!name.isEmpty()) names->insert(name);
            }
        });
        connect(job, &KJob::result, this,
                [names, callback](KJob *finished) {
            callback({finished->error(), finished->errorString(), *names});
        });
        return job;
    }

    static QString suggestedAvailableName(
        const QSet<QString> &names, const QString &requestedName, bool directoryEntry)
    {
        const QString initial = requestedName.isEmpty()
            ? trLocal("Nowy plik", "New file") : requestedName;
        if (!names.contains(initial)) return initial;
        for (int number = 1; number < std::numeric_limits<int>::max(); ++number) {
            const QString candidate = numberedName(initial, number, directoryEntry);
            if (!names.contains(candidate)) return candidate;
        }
        return initial;
    }

    void showRemoteListingError(const DirectorySnapshot &snapshot)
    {
        QString detail = snapshot.errorText.trimmed();
        if (!detail.isEmpty()) detail.prepend(QStringLiteral("\n\n"));
        QMessageBox::warning(
            m_parentWidget,
            trLocal("Nie można sprawdzić nazwy", "Could not check the name"),
            trLocal(
                "Nie udało się bezpiecznie odczytać zdalnego katalogu. Nie utworzono żadnego elementu.",
                "The remote directory could not be read safely. No item was created.")
                + detail);
    }

    void requestRemoteNewItemName(
        const QUrl &directory, const QString &requestedName, bool directoryEntry,
        std::function<void(const QString &)> create)
    {
        cancelPendingNewItem();
        const quint64 serial = m_newItemRequestSerial;
        const QPointer<FileActions> guard(this);
        m_newItemListJob = startDirectorySnapshot(
            directory,
            [guard, serial, directory, requestedName, directoryEntry,
             create = std::move(create)](DirectorySnapshot snapshot) mutable {
                FileActions *self = guard.data();
                if (!self || serial != self->m_newItemRequestSerial) return;
                self->m_newItemListJob = nullptr;
                if (snapshot.error) {
                    self->showRemoteListingError(snapshot);
                    return;
                }

                const QString suggestion = suggestedAvailableName(
                    snapshot.names, requestedName, directoryEntry);
                bool ok = false;
                const QString name = QInputDialog::getText(
                    self->m_parentWidget,
                    directoryEntry
                        ? trLocal("Nowy folder", "New folder")
                        : trLocal("Nowy plik", "New file"),
                    directoryEntry
                        ? trLocal("Nazwa folderu:", "Folder name:")
                        : trLocal("Nazwa pliku:", "File name:"),
                    QLineEdit::Normal, suggestion, &ok).trimmed();
                if (!ok) return;
                if (!validNewName(name)) {
                    QMessageBox::warning(
                        self->m_parentWidget,
                        trLocal("Nieprawidłowa nazwa", "Invalid name"),
                        directoryEntry
                            ? trLocal(
                                "Nazwa folderu jest pusta albo zawiera niedozwolony znak „/”.",
                                "The folder name is empty or contains the invalid “/” character.")
                            : trLocal(
                                "Nazwa pliku jest pusta albo zawiera niedozwolony znak „/”.",
                                "The file name is empty or contains the invalid “/” character."));
                    return;
                }
                self->verifyRemoteNameBeforeCreate(
                    directory, name, directoryEntry, serial, std::move(create));
            });
    }

    void verifyRemoteNameBeforeCreate(
        const QUrl &directory, const QString &name, bool directoryEntry,
        quint64 serial, std::function<void(const QString &)> create)
    {
        const QPointer<FileActions> guard(this);
        m_newItemListJob = startDirectorySnapshot(
            directory,
            [guard, serial, name, directoryEntry,
             create = std::move(create)](DirectorySnapshot snapshot) mutable {
                FileActions *self = guard.data();
                if (!self || serial != self->m_newItemRequestSerial) return;
                self->m_newItemListJob = nullptr;
                if (snapshot.error) {
                    self->showRemoteListingError(snapshot);
                    return;
                }
                if (snapshot.names.contains(name)) {
                    const QString next = suggestedAvailableName(
                        snapshot.names, name, directoryEntry);
                    QMessageBox::warning(
                        self->m_parentWidget,
                        trLocal("Nazwa jest już zajęta", "Name already exists"),
                        trLocal(
                            "Ta nazwa została zajęta przed utworzeniem elementu. Niczego nie nadpisano. Spróbuj ponownie; następna wolna propozycja to: %1",
                            "That name was taken before the item could be created. Nothing was overwritten. Try again; the next available suggestion is: %1")
                            .arg(next));
                    return;
                }
                if (!self->mutationAllowed()) return;
                create(name);
            });
    }

    void createNewFileWithName(
        const QUrl &directory, const QString &name, const QByteArray &contents)
    {
        if (!mutationAllowed()) return;
        const QUrl destination = childUrlWithName(directory, name);
        if (directory.isLocalFile()
            && !ensureNewDestinationAvailable(destination, false)) return;

        // KIO::DefaultFlags deliberately excludes Overwrite and Resume. A
        // protocol that honors KIO's contract must fail a post-check race.
        KIO::StoredTransferJob *job = KIO::storedPut(
            contents, destination, -1, KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        if (m_undoController) m_undoController->recordPutJob(destination, job);
        watchFileOperation(job,
            trLocal("Utworzono plik", "File created"), false,
            trLocal("Tworzenie pliku", "Creating file"));
    }

    void createFromTemplateWithName(
        const QUrl &directory, const QUrl &source, const QString &name)
    {
        if (!mutationAllowed()) return;
        // Recheck after the modal dialog: a stale entry must not turn into
        // a recursive folder copy or create a symbolic link as a document.
        const QFileInfo info(source.toLocalFile());
        if (info.isSymLink() || (info.exists() && !info.isFile())) {
            QMessageBox::warning(m_parentWidget,
                trLocal("Szablony", "Templates"),
                trLocal("Szablon nie jest zwykłym plikiem.", "The template is not a regular file."));
            return;
        }

        const QUrl destination = childUrlWithName(directory, name);
        if (directory.isLocalFile()
            && !ensureNewDestinationAvailable(destination, false)) return;
        if (!mutationAllowed()) return;
        KIO::CopyJob *job = KIO::copyAs(source, destination, KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        if (m_undoController) m_undoController->recordCopyJob(job);
        watchFileOperation(job,
            trLocal("Utworzono plik", "File created"), false,
            trLocal("Tworzenie pliku", "Creating file"));
    }

    void createNewFolderWithName(const QUrl &directory, const QString &name)
    {
        if (!mutationAllowed()) return;
        const QUrl destination = childUrlWithName(directory, name);
        if (directory.isLocalFile()
            && !ensureNewDestinationAvailable(destination, true)) return;

        KIO::MkdirJob *job = KIO::mkdir(destination);
        job->setUiDelegate(nullptr);
        if (m_undoController) m_undoController->recordMkdirJob(destination, job);
        watchFileOperation(job,
            trLocal("Utworzono folder", "Folder created"), false,
            trLocal("Tworzenie folderu", "Creating folder"));
    }

    static bool localEntryExists(const QUrl &url)
    {
        if (!url.isLocalFile()) return false;
        const QFileInfo info(url.toLocalFile());
        return info.exists() || info.isSymLink();
    }

    static QString numberedName(const QString &name, int number, bool directory)
    {
        if (directory) return QStringLiteral("%1 (%2)").arg(name).arg(number);

        // A leading dot alone does not introduce an extension.  Preserve a
        // complete multi-part suffix such as .tar.gz.
        const int dot = name.indexOf(u'.', name.startsWith(u'.') ? 1 : 0);
        if (dot < 0) return QStringLiteral("%1 (%2)").arg(name).arg(number);
        const QString base = name.left(dot);
        const QString suffix = name.mid(dot + 1);
        return QStringLiteral("%1 (%2).%3")
            .arg(base).arg(number).arg(suffix);
    }

    static QString suggestedAvailableName(
        const QUrl &directory, const QString &requestedName, bool directoryEntry)
    {
        const QString initial = requestedName.isEmpty()
            ? trLocal("Nowy plik", "New file") : requestedName;
        if (!directory.isLocalFile()
            || !localEntryExists(childUrlWithName(directory, initial))) {
            return initial;
        }
        for (int number = 1; number < std::numeric_limits<int>::max(); ++number) {
            const QString candidate = numberedName(initial, number, directoryEntry);
            if (!localEntryExists(childUrlWithName(directory, candidate))) return candidate;
        }
        return initial;
    }

    bool ensureNewDestinationAvailable(const QUrl &destination, bool directoryEntry)
    {
        if (!localEntryExists(destination)) return true;
        const QString next = suggestedAvailableName(
            destination.adjusted(QUrl::RemoveFilename),
            destination.fileName(), directoryEntry);
        QMessageBox::warning(
            m_parentWidget,
            trLocal("Nazwa jest już zajęta", "Name already exists"),
            trLocal(
                "Ta nazwa została zajęta przed utworzeniem elementu. Niczego nie nadpisano. Spróbuj ponownie; następna wolna propozycja to: %1",
                "That name was taken before the item could be created. Nothing was overwritten. Try again; the next available suggestion is: %1")
                .arg(next));
        return false;
    }

    bool mutationAllowed()
    {
        auto &gate = BatchRenameRecoveryGate::instance();
        gate.refresh();
        if (!gate.mutationsBlocked()) return true;
        if (m_showRecoveryFence) m_showRecoveryFence(gate.message());
        return false;
    }

    QString requestNewFileName(const QString &suggestedName)
    {
        bool ok = false;
        const QString initial = suggestedName.isEmpty()
            ? trLocal("Nowy plik", "New file") : suggestedName;
        const QString name = QInputDialog::getText(
            m_parentWidget,
            trLocal("Nowy plik", "New file"),
            trLocal("Nazwa pliku:", "File name:"),
            QLineEdit::Normal, initial, &ok).trimmed();
        if (!ok) return {};
        if (!validNewName(name)) {
            QMessageBox::warning(
                m_parentWidget,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa pliku jest pusta albo zawiera niedozwolony znak „/”.",
                    "The file name is empty or contains the invalid “/” character."));
            return {};
        }
        return name;
    }

    bool startNativeSingleFileTransfer(
        const QList<QUrl> &sources,
        const QUrl &destinationDirectory,
        bool move,
        bool clearClipboard,
        const QString &successMessage,
        const QString &title)
    {
        if (!mutationAllowed()) return true;
        if (sources.isEmpty() || !destinationDirectory.isLocalFile()) {
            return false;
        }
        for (const QUrl &source : sources) if (!source.isLocalFile()) return false;
        const QFileInfo sourceInfo(sources.first().toLocalFile());
        const QFileInfo destinationInfo(destinationDirectory.toLocalFile());
        if (!destinationInfo.exists() || !destinationInfo.isDir()) return false;
        if (sources.size() > 1 || sourceInfo.isDir() || sourceInfo.isSymLink()) {
            if (!mutationAllowed()) return true;
            auto *job = new LocalTransferJob(sources, destinationDirectory, move, this);
            job->setFallbackFactory([this, sources, destinationDirectory, move] {
                if (!mutationAllowed()) return static_cast<KIO::CopyJob *>(nullptr);
                auto *fallback = move
                    ? KIO::move(sources, destinationDirectory, KIO::HideProgressInfo)
                    : KIO::copy(sources, destinationDirectory, KIO::HideProgressInfo);
                configureInteractiveCopyJob(fallback);
                if (m_undoController) m_undoController->recordCopyJob(fallback);
                return fallback;
            });
            if (m_undoController) m_undoController->recordNativeTransfer(job, move);
            watchFileOperation(job, successMessage, clearClipboard, title);
            job->start();
            return true;
        }
        if (!sourceInfo.exists() || !sourceInfo.isFile()
            || sourceInfo.isSymLink()
            || !destinationInfo.exists() || !destinationInfo.isDir()) {
            return false;
        }
        QUrl destination = childUrlWithName(
            destinationDirectory, sourceInfo.fileName());
        bool overwrite = false;
        LocalFileIdentity originalDestination;
        while (true) {
            const QFileInfo targetInfo(destination.toLocalFile());
            if (!targetInfo.exists() && !targetInfo.isSymLink()) break;
            if (!targetInfo.isFile() || targetInfo.isSymLink()
                || LocalFileIdentity::read(sourceInfo.absoluteFilePath())
                    .sameFile(LocalFileIdentity::read(destination.toLocalFile()))) {
                // Keep KIO's handling of type conflicts and self-copy, also
                // when a renamed destination introduces such a conflict.
                if (!mutationAllowed()) return true;
                auto *job = move
                    ? KIO::moveAs(sources.first(), destination, KIO::HideProgressInfo)
                    : KIO::copyAs(sources.first(), destination, KIO::HideProgressInfo);
                configureInteractiveCopyJob(job);
                if (m_undoController) m_undoController->recordCopyJob(job);
                watchFileOperation(job, successMessage, clearClipboard, title);
                return true;
            }
            originalDestination = LocalFileIdentity::read(destination.toLocalFile());
            KIO::RenameDialog dialog(
                m_parentWidget, title, sources.first(), destination,
                KIO::RenameDialog_Overwrite | KIO::RenameDialog_Skip
                    | KIO::RenameDialog_MultipleItems,
                sourceInfo.size(), targetInfo.size(),
                sourceInfo.birthTime(), targetInfo.birthTime(),
                sourceInfo.lastModified(), targetInfo.lastModified());
            const int decision = dialog.exec();
            if (!mutationAllowed()) return true;
            if (decision == KIO::Result_Overwrite || decision == KIO::Result_OverwriteAll
                || (decision == KIO::Result_OverwriteWhenOlder
                    && sourceInfo.lastModified() > targetInfo.lastModified())) {
                overwrite = true;
                break;
            }
            if (decision == KIO::Result_Rename || decision == KIO::Result_AutoRename) {
                destination = decision == KIO::Result_Rename
                    ? dialog.newDestUrl() : dialog.autoDestUrl();
                if (destination.isLocalFile() && !destination.fileName().isEmpty()) continue;
            }
            // Skip and Cancel do not dispatch a job, clear cut data, or
            // add an Undo command. The existing files remain untouched.
            return true;
        }

        if (!mutationAllowed()) return true;
        KIO::Job *job = move
            ? static_cast<KIO::Job *>(new LocalFileMoveJob(
                  sources.first(), destination, this,
                  LocalFileMoveStrategy::Automatic, 256 * 1024, 0, {}, overwrite,
                  originalDestination))
            : static_cast<KIO::Job *>(new LocalFileCopyJob(
                  sources.first(), destination, this, 256 * 1024, 0, overwrite,
                  false, originalDestination));
        if (m_undoController) {
            m_undoController->recordNativeTransfer(
                job, move);
        }
        watchFileOperation(job, successMessage, clearClipboard, title);
        job->start();
        return true;
    }

    void configureInteractiveCopyJob(KIO::CopyJob *job)
    {
        if (!job) {
            return;
        }

        // KIO's WidgetsAskUserActionHandler is attached through the default
        // widgets UI delegate.  We keep automatic error/warning handling
        // disabled because watchFileOperation() already owns our error UI;
        // conflict/skip/rename questions remain fully interactive.
        if (KJobUiDelegate *delegate =
                KIO::createDefaultJobUiDelegate(
                    KJobUiDelegate::AutoHandlingDisabled,
                    m_parentWidget)) {
            job->setUiDelegate(delegate);
        }
    }

    void watchFileOperation(KJob *job, const QString &message, bool clearClipboard, const QString &title,
                            const RefreshViews &refreshViews = {})
    {
        m_watchOperation(job, message, clearClipboard, title, refreshViews);
    }

    QWidget *m_parentWidget = nullptr;
    UndoController *m_undoController = nullptr;
    WatchOperation m_watchOperation;
    std::function<void(const QString &)> m_showRecoveryFence;
    StartDirectorySnapshot m_startDirectorySnapshot;
    QPointer<KJob> m_newItemListJob;
    quint64 m_newItemRequestSerial = 0;
    bool m_confirmingEmptyTrash = false;
    QPointer<KJob> m_emptyTrashJob;
    QFuture<BatchRenameSwapWorkerResult> m_swapFuture;
    bool m_swapWorkerRunning = false;
    bool m_swapBatchRunning = false;
    bool m_linearBatchRunning = false;
};
