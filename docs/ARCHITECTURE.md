# ARCHITECTURE.md

## Components
### `src/thispc.cpp`
KIO worker backend for `thispc:/`. It enumerates the virtual This PC content and drive/storage information using KDE/Solid/KIO infrastructure.

### `src/thispcview.cpp`
Native Qt/KDE application. `ThisPcWindow` coordinates extracted UI/controllers for address and breadcrumb controls, sidebar/drives, directory views, Split View, operations, properties, search, file actions and Undo/Redo.

The 0.21 refactor extracted the following headers:
- `browsercommon.h`: shared data, URL, MIME and directory helpers.
- `directoryview.h`: item views, filename delegate, tabs and drag/drop support.
- `splitbrowserpane.h`: secondary browser pane.
- `sidebar.h`: places/devices, Quick Access and recent locations.
- `sessionmanager.h`: persistent tab and split state.
- `operationmanager.h`: shared job state, compact history popup and detailed active-operation window.
- `undocontroller.h`: FileUndoManager integration and action state.
- `propertiesdialog.h`: properties, rename and verified permission changes.
- `searchcontroller.h`: asynchronous filename-search jobs and results.
- `fileactions.h`: operation dialogs and KIO create/copy/move/rename/Trash dispatch, with Undo registration.

ThisPcWindow wires the components to navigation and UI. FileActions receives
source/destination snapshots; the window retains pane selection, drag/drop
policy and operation completion/error presentation.

### `src/thispc.json`
KIO worker plugin metadata.

### `data/org.kde.thispcview.desktop`
Desktop entry for the native application.

### `icons/`
Drive/removable-device usage icons used by the This PC presentation.

## KIO-first file operations
Prefer KIO jobs for copy/move/trash/rename/create where possible, so operations integrate with KDE behavior, progress reporting and `KIO::FileUndoManager`.

## Undo/Redo
`UndoController` manages `KIO::FileUndoManager` for `ThisPcWindow`. The 0.16.0 rename bug is an important lesson: record the actual KIO operation in the way FileUndoManager expects rather than inventing metadata that does not match the executed job.

## Operation UI
`OperationManager` owns stable operation IDs and snapshots derived from KJob/KIO signals. The compact custom `Qt::Popup` remains the history and quick-overview surface. A normal nonmodal `OperationWindow` presents active tasks only, opens automatically, groups concurrent operations, sizes to its contents up to a screen-relative cap and closes when no active task remains. It shows structured CopyJob paths, processed/total bytes, current and average speed, ETA and a bounded 120-sample graph. Rebuilds are coalesced and deferred while a pointer interaction is active.

`KIO::CopyJob::suspend()` must not be presented as exact local-transfer pause. KDE's worker connection may buffer work/progress after suspension, producing a large jump on resume. Genuine pause/resume is reserved for the planned 0.23.0 chunked local transfer engine. KIO remains the backend for remote URLs and operations where its semantics are appropriate.

## Future architecture
0.23.0 should introduce the local transfer engine behind a focused interface rather than adding local I/O loops to `ThisPcWindow` or `OperationWindow`.
