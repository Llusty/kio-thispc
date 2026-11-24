# ThisPC 0.46.0 — Stage 2A.2-1 & R1: `KJobTransferAdapter` Foundation

## Scope

Stage 2A.2-1 introduces the isolated KDE compatibility adapter `KJobTransferAdapter` bridging existing `KJob` / `KIO::Job` operations to the Qt6 Core-only `PortableTransferJob` contract established in Stage 2A.1.

- **Portable core boundary preserved:** `PortableTransferJob` and `PortableTransferCleanupHandle` remain 100% Qt6 Core-only (zero KF6/KIO/GIO/QtWidgets dependencies).
- **No production queue migration:** `TransferQueueController`, `TransferTaskDescriptor`, `FileActions`, `LocalMergeQueueBridge`, `TransferQueueUI`, `OperationManager`, and `Local*Job` classes remain unchanged in this step.

## Files

- `src/portabletransferjob.h` / `src/portabletransferjob.cpp` — Minimal backwards-compatible Qt6 Core-only additions for R1: `PortableTransferCleanupHandle`, `cleanupHandle()`, `hasActiveBackendBeforeStart()`, and `syncSuspendedState()`.
- `src/kjobtransferadapter.h` / `src/kjobtransferadapter.cpp` — Exclusive ownership validation, `KJobCleanupRetainer` detached teardown, start-mode & pre-start cancellation handling, progress/error/cancellation mapping, conservative KIO pause exclusion, fail-closed `TransferOutcomeFlags` and `thispcMergeInDecisionPhase` event filtering.
- `tests/kjob-transfer-adapter-test.cpp` — Isolated unit, real-worker-thread, and KIO compatibility test suite (`thispc-kjob-transfer-adapter-test`).
- `CMakeLists.txt` — Registers `kjobtransferadapter.{h,cpp}` and `thispc-kjob-transfer-adapter-test` strictly inside the KDE-compatible branch (`THISPC_BUILD_PORTABLE_CORE_ONLY=OFF`).
- `tests/run-pane-actions.py` — Adds `kjobtransferadapter.cpp` to the global AUTOMOC source list so `pane-test` links cleanly without duplicate symbols.

## Key Invariants (including R1 Remediation)

1. **Exclusive ownership & non-blocking detached cleanup (`AUD-2A2-01`):**
   - `KJobTransferAdapter::create()` rejects null, externally parented, cross-thread, already-wrapped, and already-errored `KJob` instances.
   - In the normal post-completion path (`m_wrappedFinished && m_inWrappedCallbackDepth == 0`), `~KJobTransferAdapter()` synchronously destroys the already-finished `KJob` and marks `cleanupHandle()` complete *before* `QObject::destroyed(adapter)` fires.
   - When destroyed during an active wrapped-job callback (`m_inWrappedCallbackDepth > 0`) or while a backend worker is still active, `~KJobTransferAdapter()` never blocks the GUI thread and never deletes `KJob` on its own active call stack; instead it hands exclusive ownership and `cleanupHandle()` to `KJobCleanupRetainer` (parented to `QCoreApplication::instance()` on the current thread), which waits for `KJob::result` even if cancellation is unsupported/refused (`AUD-2A2-R1-01`), completes `cleanupHandle()` **only after `QObject::destroyed(rawJob)` has actually fired**, and avoids synchronous GUI-blocking destruction inside `aboutToQuit` (`AUD-2A2-R1-02`).
   - **Mandatory Stage 2A.2-2 Queue Requirement:** `TransferQueueController` must observe `job->cleanupHandle()` (`isCleanupComplete()` / `cleanupCompleted()`) for `m_awaitingDestruction` / `isShutdownComplete()` rather than relying solely on `QObject::destroyed(adapter)`.
2. **Start modes & pre-start cancellation (`AUD-2A2-02`):**
   - `ControllerStarts` invokes `KJob::start()` at most once; pre-start cancellation finishes immediately.
   - `FactoryStarts` never calls `start()` a second time, buffers pre-start terminal results, and overrides `hasActiveBackendBeforeStart()` so `requestCancel()` before `start()` dispatches real backend cancellation and stays `Cancelling` until `KJob::result` arrives.
3. **Conservative exact pause (`AUD-2A2-03`):** `KIO::CopyJob`, `KIO::FileCopyJob`, `KIO::SimpleJob`, `KIO::DeleteJob`, and `KioStagedTransferJob` are never marked `Suspendable`; `doSuspend()` / `doResume()` recheck live `KJob::Suspendable` capability (protecting `LocalTransferJob` KIO fallback), and `KJob::suspended`/`resumed` signals remain synchronized.
4. **Fail-closed decision phase & event filter (`AUD-2A2-04`):** Invalid/non-bool values or removal of `thispcMergeInDecisionPhase` while active fail closed (`true`); `eventFilter()` returns `false` on reentrant adapter destruction so unrelated event delivery continues.
5. **Mapping consistency (`AUD-2A2-05`):** `KIO::ERR_CANNOT_WRITE` maps to `TransferErrorCode::CannotWrite`; `RecoveryRequired`/`FailedPartialSuccess`/`FailedUncertainDurability` merge states force retry blocking; late progress after `m_wrappedFinished` is suppressed and byte-derived percent stays authoritative; `setBandwidthLimiter()` verifies both parameter types consistently.
