# ThisPC 0.46.0 — Stage 2A.1: Portable Transfer Contracts & Header Decoupling

**Status:** `STAGE 2A.1 — IMPLEMENTED / AUTOMATED PASS / AWAITING INDEPENDENT AUDIT` (NOT yet finally accepted, NOT committed/pushed, NOT a public release).
**Starting baseline:** Stage 1B checkpoint commit `9ffa95e46cbc2bb0ba83df1787139529bea66cad`.
**Installed production release (unchanged):** ThisPC `0.45.2` (`/usr/bin/thispc-view`).

---

## 1. Adopted Contract Design (`PortableTransferJob`)

Added `src/portabletransferjob.h` and `src/portabletransferjob.cpp` to `thispc-core` (strictly Qt6::Core + C++20; zero KF6, KJob, KIO, GIO, or QtWidgets dependencies; Qt 6.4.2 compatible):

- **Value types & enums:**
  - `TransferJobCapability` (`None`, `Killable`, `Suspendable`, `AsyncCancel`, `BandwidthLimited`) + `TransferJobCapabilities` (`Q_DECLARE_FLAGS`).
  - `TransferErrorCode` (`NoError`, `Cancelled`, `DoesNotExist`, `AlreadyExists`, `CannotRead`, `CannotWrite`, `CannotOpenForReading`, `CannotOpenForWriting`, `CannotMkdir`, `CannotRename`, `CannotChmod`, `DiskFull`, `IsDirectory`, `UnsupportedAction`, `UnsupportedProtocol`, `BackendError`, `InternalError`).
  - `TransferJobState` (`NotStarted`, `Running`, `Paused`, `Cancelling`, `Completed`, `Cancelled`, `Failed`) + `isTerminalTransferJobState()`.
  - `TransferOutcomeFlags` (`destinationPublished`, `destinationReplaced`, `directoryDurabilityConfirmed`, `partialMoveOutcome`, `batchPublishedCount`, `batchPartiallyCompleted`, `mergeRetryBlocked`, `mergeRecoveryRequired`, `mergeState`, `mergeSkippedCount`, `mergeReplacedCount`, `mergeHistoryNote`, `systemError`, `backendErrorCode`, `hasPartialPublication()`, fail-closed `blocksSafeRetry()`).
  - Live transient decision-phase state kept distinct from terminal `TransferOutcomeFlags`: `isInDecisionPhase()`, protected `setInDecisionPhase(bool)`, signal `decisionPhaseChanged(PortableTransferJob *, bool)`, automatically cleared to `false` during terminalization before `finished()`.
- **Ownership, reentrancy & cancellation guarantees (hardened in Stage 2A.1-R1):**
  - Owned by its `QObject` parent or observed via `QPointer<PortableTransferJob>`.
  - Zero hidden `QEventLoopLocker` (application shutdown lifetime remains governed solely by Stage 1A `ShutdownCoordinator`).
  - `start()` guards `transitionState(Running)` with `QPointer<PortableTransferJob> self(this)` and invokes `doStart()` only if `self && m_state == Running`.
  - Cooperative asynchronous cancellation: `requestCancel()` transitions active jobs (`Running` or `Paused`) to `Cancelling` and delegates to `doCancel()`; terminal `finished()` is emitted only after subclass cleanup completes (`finishCompleted()`, `finishCancelled()`, `finishFailed()`).
  - `finalizeTerminal()` sets `m_finalizing = true` and `m_state = terminalState` before emitting any signal, guards all signals (`decisionPhaseChanged`, `stateChanged`, `finished`) with `QPointer<PortableTransferJob> self(this)`, ignores nested/duplicate terminal calls, and schedules `deleteLater()` at most once (`m_deleteScheduled`) *after* `finished(this)` returns (never if a subscriber synchronously destroyed `this`).
  - Post-terminal immutability: `setOutcomeFlags()`, `setInDecisionPhase()`, progress/speed setters, and post-start `setCapabilities()` are ignored once locked/terminal; unprotected `mutableOutcomeFlags()` was removed.

---

## 2. Extracted Helpers (`src/portablecommon.h`) & Dead-Include Cleanup

- **Extracted to `src/portablecommon.h` (pure Qt6::Core, in `thispc-core`):**
  `siblingUrlWithName()`, `validNewName()`, `isPolish()`, `trLocal()`, `formatFileSize()`, `normalizedUrl()`, `sameLocation()`, `parentDirectoryUrl()`, `FileOperationType`, `FileOperationContext`.
- **Backwards compatibility:** `src/browsercommon.h` includes `"portablecommon.h"` and removes the duplicated definitions (zero ODR violations, unchanged behavior).
- **Updated direct includes:** `src/transfertaskdescriptor.h` and `src/transferhistorymanager.h` switched from `browsercommon.h` to `portablecommon.h`.
- **Verified dead includes removed:**
  - `src/localbtrfsmovecommon.h`: removed unused `#include "browsercommon.h"`.
  - `src/localdirectorymovejob.h`: removed unused `#include "browsercommon.h"`.
  - `src/localbatchmovejob.h`: removed unused `#include "browsercommon.h"`.
  - `src/transferqueuecontroller.h`: removed unused `#include "kiostagedtransferjob.h"`.

---

## 3. Build, Qt 6.4.2, Dependency Gate & Sanitizer Evidence

Evidence logs stored in `/home/sebastianh/Pobrane/Testy/ThisPC-0.46.0/Stage2/Stage2A1/logs/`:

- **Compiled `thispc-core` manifest:** `sources=37`, `POSITION_INDEPENDENT_CODE=ON`, 45 direct/transitive header files scanned, 0 violations.
- **CachyOS core-only (`build-core`, Qt 6.12.0):** **3/3 CTest PASS** (`thispc-core-smoke`, `thispc-portable-transfer-job-test` 115 checks, `thispc-core-deps-gate`). `readelf -d` confirms 0 KF6/GIO/QtWidgets dependencies.
- **CachyOS core-only ASan/UBSan/LSan (`build-sanitizers`, `detect_leaks=1`):** **3/3 CTest PASS, 0 B leaks** (`cachyos-core-and-asan-lsan.log`).
- **Real Ubuntu 24.04.5 LTS / Qt 6.4.2 (`qt6-base-dev 6.4.2+dfsg-21.1build5`, `g++ 13.3.0`, `cmake 3.28.3`):** **3/3 CTest PASS** (`thispc-core-smoke` Qt 6.4.2, `thispc-portable-transfer-job-test` 115 checks, `thispc-core-deps-gate` PASS; `readelf -d` lists only `libQt6Core.so.6`, `libstdc++`, `libm`, `libgcc_s`, `libc`) (`ubuntu-qt642-core.log`).
- **CachyOS KDE-compat (`build-kde`, `THISPC_BUILD_PORTABLE_CORE_ONLY=OFF`):** `thispc`, `thispc-view`, `thispc-view-stage3c1-test`, `thispc-merge-preview`, `thispc-core-smoke`, `thispc-portable-transfer-job-test` built cleanly; **4/4 CTest PASS** (`cachyos-kde-compat.log`).
- **Targeted regressions (`targeted-regressions-asan.log`):**
  - ASan/UBSan (`detect_leaks=0`): **9/9 suites PASS** (`transfer_queue`, `history`, `local_transfer`, `local_move`, `local_dir_move`, `local_batch_move`, `local_merge_dialogs`, `actions`, `lifecycle_hygiene`).
  - ASan/UBSan/LSan (`detect_leaks=1`): **6/6 suites PASS, 0 B leaks** (`history` 2008, `local_transfer` 154, `local_move` 490, `local_dir_move` 367, `local_batch_move` 330, `lifecycle_hygiene` 190 + 5 real `QApplication::exec()` subprocesses).
- **Stage 2A.1-R1 Remediation Evidence (`AUD-2A1-01`, `AUD-2A1-02`, `AUD-2A1-03`):**
  - `r1-cachyos-core.log`: **3/3 CTest PASS** (`thispc-portable-transfer-job-test` **177 checks passed**, `thispc-core-deps-gate` PASS, `sources=37`, 0 violations).
  - `r1-sanitizers.log` (ASan + UBSan + LSan `detect_leaks=1`): **3/3 CTest PASS, 0 B leaks** (including synchronous `delete` reentrancy tests).
  - `r1-ubuntu-qt642.log` (Ubuntu 24.04.5 LTS / `qt6-base-dev 6.4.2+dfsg-21.1build5`): **3/3 CTest PASS** (`177 checks passed`, `readelf -d` 0 KF6/GIO/QtWidgets).
  - `r1-kde-compat.log`: **4/4 CTest PASS** (all KDE targets rebuilt and linked cleanly).
- **Stage 2A.1-R2 Final Signal-Reentrancy Hardening (`requestCancel()`, `suspend()`/`resume()`, `setTotalBytes()`/`setProcessedBytes()`):**
  - `r2-cachyos-core.log`: **3/3 CTest PASS** (`thispc-portable-transfer-job-test` **207 checks passed**, `thispc-core-deps-gate` PASS, `sources=37`, 0 violations).
  - `r2-sanitizers.log` (ASan + UBSan + LSan `detect_leaks=1`): **3/3 CTest PASS, 0 B leaks** (including synchronous `delete` inside `stateChanged(Cancelling|Paused|Running)`, `totalBytesChanged`, and `processedBytesChanged`).
  - `r2-ubuntu-qt642.log` (Ubuntu 24.04.5 LTS / `qt6-base-dev 6.4.2+dfsg-21.1build5`): **3/3 CTest PASS** (`207 checks passed`, `readelf -d` 0 KF6/GIO/QtWidgets).
  - `r2-kde-compat.log`: **4/4 CTest PASS**.
- **Canonical gates:** **G2** remains **PASS**; **G1**, **G3**, **G4** remain **PENDING**.

---

## 4. Remaining Limitations & Explicitly Deferred Work

- Existing production native jobs (`LocalFileCopyJob`, `LocalFileMoveJob`, etc.) and `TransferQueueController` still use `KJob`/`KIO::Job` in Stage 2A.1; pre-existing LSan leaks in `transfer_queue` and `operations` are unchanged and deferred as scheduled.
- **Stage 2A.2:** `KJobTransferAdapter`, migrating `TransferQueueController` / `TransferTaskDescriptor` to `PortableTransferJob`, converting live retry metadata.
- **Stage 2A.3:** `TransferHistoryManager` worker-pool integration (`ApplicationWorkerPools`) and full `transfer_queue` LSan leak remediation.
- **Stage 2B:** Local transfer engine and Folder Merge migration to `PortableTransferJob`.
- **Stage 2C:** `OperationTracker` extraction, `OperationManager`/`FileActions`/`UndoController` integration, and final Stage 2 acceptance.
