# ThisPC 0.46.0 — Stage 1A Internal Checkpoint Record
# (Higiena cyklu życia, bezpieczne zamykanie i własność wątków)

- **Status:** `COMPLETED & OWNER-ACCEPTED` (internal development checkpoint; **NOT** a public release)
- **Baseline commit:** `1f6871107b052e29dad2de9fbbf611f313a1c4b3` (`main...origin/main`)
- **Installed production baseline:** ThisPC `0.45.2` (`v0.45.2`, untouched)
- **Proposed checkpoint commit message:** `feat(thispc): complete 0.46.0 Stage 1A lifecycle hardening`

---

## 1. Scope & Implemented Changes (Stage 1A)

1. **`AUD-A-07` — Explicit `ShutdownCoordinator` (`src/applicationlifecycle.h`, `src/thispcview.cpp`):**
   - Added pure Qt 6 Core `ShutdownCoordinator` with explicit state machine (`Idle` → `Confirming` → `Draining` → `Completed`), `QEventLoopLocker` lifetime management, `TransferQueueController::shutdownCompleted()` / `shutdownDelayed()` wiring, and `OperationManager` integration.
   - Removed hidden reliance on `KJob`'s internal `QEventLoopLocker` for shutdown timing without removing `KJob` prematurely (scheduled for Stage 2).
2. **`AUD-A-08` — `ChecksumJob` Thread Leak & Cancellation Race (`src/checksumjob.h`):**
   - Replaced cross-thread `QPointer<ChecksumWorker>` cancellation with `std::shared_ptr<std::atomic<bool>>` cancel token.
   - Decoupled `QThread` and `ChecksumWorker` cleanup from `ChecksumJob` lifetime (`worker->finished` → `thread->quit` & `deleteLater`, `thread->finished` → `deleteLater`).
3. **`AUD-A-09` & `AUD-A-10` — Managed `ApplicationWorkerPools` (`src/applicationlifecycle.h`, `src/metadataprovider.h`, `src/propertiesxattrs.h`, `src/propertiesxattrwriter.h`):**
   - Replaced detached `new QThread` in `MetadataProvider` and function-local `static QThreadPool`s in `PropertiesXattrProvider` / `PropertiesXattrMutationController` with bounded (`maxThreadCount=2`), application-owned `QThreadPool`s drained before `~QCoreApplication()`.
4. **`AUD-A-11` — Asynchronous Storage Analysis Context Menus (`src/storagescandialog.h`, `src/storagetreemapwidget.h`):**
   - Replaced all 4 blocking stack `QMenu::exec()` calls (`m_filesTableView`, `m_dirsTableView`, `m_dupTreeView`, `StorageTreemapCanvas`) with heap `QMenu` (`Qt::WA_DeleteOnClose`, `popup()`) and immutable path/value snapshots.
5. **`AUD-A-13` (partial) — `closeEvent()` Confirmation Reentrancy Guard (`src/thispcview.cpp`):**
   - Added `ShutdownCoordinator::ConfirmationScope` preventing overlapping close-confirmation dialogs.

---

## 2. Independent Audit Findings & R1/R2 Remediation

An independent read-only Orchestrator audit of initial Stage 1A identified 7 findings (`AUD-1A-01`–`AUD-1A-07`), all resolved in **Stage 1A-R1** and verified by an independent Orchestrator delta audit (`PASS`):

- **`AUD-1A-01` (HIGH):** Removed `QThreadPool::clear()` on `QtConcurrent::run()` pools; switched to cooperative per-generation cancellation via `QPromise<ResultT>` and `readCancelToken`, ensuring every `QFuture` reaches a terminal state, `QFutureWatcher`s clean up, cancelled reads perform no I/O and publish no bogus results, and accepted xattr writes complete.
- **`AUD-1A-02` (HIGH):** Added main-window registration (`registerWindow` / `unregisterWindow` / `hasOtherActiveWindows`), per-generation `beginShutdownDrain()` / `drainCompleted(generation)` so closing one window never poisons surviving windows or races a concurrent drain, and visible error reporting in `PropertiesXattrWidget::mutate()` when `submit()` returns `false`.
- **`AUD-1A-03` (HIGH):** Separated `OperationManager::cancelDirectOperations()` from `TransferQueueController::beginShutdown()`, added `nonKillableActiveCount()`, included `queuedCount()` in close confirmation, added a non-destructive diagnostic watchdog across all 3 subsystems, kept the closing window visible with status feedback during async draining, and closed `OperationWindow` on completion.
- **`AUD-1A-04` (MEDIUM):** Unified `ApplicationWorkerPools::SharedState` under a single `std::mutex` state machine with safe `owner = nullptr` teardown after `waitForDone()`.
- **`AUD-1A-05` (MEDIUM):** Added `m_runId` generation checks, `QList<QPointer<QThread>> m_threads` tracking for rapid restarts from `stateChanged()`, cancellation check before `::open()`, and idle-cancel separation (`m_runId == 0` vs `m_runId > 0`).
- **`AUD-1A-06` (LOW):** Added `QPointer<QMenu> m_activeContextMenu` across all 4 Storage Analysis context menus to close any active popup before opening another.
- **`AUD-1A-07` (MEDIUM):** Revalidated `BatchRenameRecoveryGate` and `PropertiesLifecycle::deferCloseFor(this)` after modal `QMessageBox::question()` acceptance, and added `showEvent()` reset (`Completed` → `Idle`).

**Stage 1A-R2** resolved the remaining `AUD-1A-R1-NOTE-01` (LOW) from the R1 delta audit:
- Added `isShutdownDraining()`, `ensureNotShuttingDown()`, and `applyShutdownInteractionState()` in `ThisPcWindow` (`src/thispcview.cpp`) to disable `centralWidget()` and mutating actions/shortcuts/context menus/Drag & Drop during `Draining`/`Completed`, while preserving shutdown status, operation progress, other active windows, and restoration on `showEvent()`.

---

## 3. Accepted Verification & Test Evidence

- **Isolated Stage 1A-R2 Build (`/home/sebastianh/Pobrane/Testy/ThisPC-0.46.0/Stage1A/build`):** `thispc-view`, `thispc`, `thispc-view-stage3c1-test` PASS (`logs/r2-build.log`).
- **Full Automated Regression (`tests/run-pane-actions.py --all`, ASan + UBSan, `detect_leaks=0`):** **90/90 PASS** (`logs/r2-asan-all-90-suites.log`), including isolated `batch_rename` (`1789` assertions PASS without touching `repo/build/`).
- **Targeted Sanitizer Suites (ASan + UBSan + LSan `detect_leaks=1`):** **11/11 PASS, 0 B leaks** (`logs/r2-asan-lsan-11-suites.log`):
  - `lifecycle_hygiene`: **190/190 checks PASS**, including **5/5 real `QApplication::exec()` subprocesses** (`last-window-non-kjob`, `multi-window`, `non-killable-direct-op`, `about-to-quit-drain`, `draining-mutation-guard`).
  - `checksums` (114), `hash_utilities` (170), `duplicate_finder` (100), `metadata` (59), `properties_xattrs` (95), `properties_xattr_edit` (379), `properties_lifecycle` (124), `storage_scan` (22438), `storage_analysis` (97), `storage_treemap` (179).
- **Owner Manual GUI Acceptance (KDE Plasma / Wayland):** **10/10 PASS** (8/8 Stage 1A + 2/2 Stage 1A-R2; no lingering `thispc-view` process after R2-B).

---

## 4. Remaining Known Issues & Stage Assignment

- **Unresolved pre-existing LSan ownership in `transfer_queue` (2832 B / 18 allocations `Indirect leak`) and `operations` (11784 B / 108 allocations `Indirect leak`):** Assigned to **Stage 2** (`KJob` / transfer-queue migration). Not suppressed.
- **Intermittent pre-existing `trash` test flake (`tests/empty-trash.cpp:211`):** Observed once under load during R1 intermediate testing; passed cleanly in isolation and in final R1/R2 90/90 runs. Assigned to **future investigation** (no Trash/KIO refactor in Stage 1A).
- **Historical `directory-refresh` pending contracts (`DR-058`, `DR-059`, `DR-062`):** Pre-existing since `v0.41.2` (154 GREEN, 3 RED pending, 2 NOT RUN interactive boundary).
- **`PreviewPane` / PDF worker-pool concurrency (remainder of `AUD-A-13`):** Intentionally deferred to **Stage 5A**.
