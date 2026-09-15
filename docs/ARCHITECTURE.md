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

Split View uses one shared command/navigation toolbar whose actions resolve
through the active `PaneContext`. The sidebar follows the same active-pane
routing. Each `QSplitter` child owns its address section directly above its
browser content, so both addresses remain visible and track the exact width of
their respective panes. The secondary pane keeps its independent URL/history,
view, sort and Search state without presenting a second navigation toolbar.
Each pane owns an asynchronous `SearchController`; the shared Search controls
bind to the active pane and Search URLs remain in that pane's history/session
state. Both panes use the same builder and drive inventory for the `thispc:/`
folder-and-drive card presentation.

The sidebar and the complete file-view area are children of an outer splitter.
The sidebar content lives in its own vertical scroll area, while the inner
splitter remains solely responsible for the two browser panes. Sidebar width
is persisted independently from the per-pane split state.

### `src/thispc.json`
KIO worker plugin metadata.

### `data/org.kde.thispcview.desktop`
Desktop entry for the native application.

### `icons/`
Drive/removable-device usage icons used by the This PC presentation.

## File operation backends
Supported local copy/move operations use the native transfer engine when its exact semantics are available. KIO remains the backend for remote URLs, Trash, rename/create and local cases that are intentionally unsupported or unresolved by the native planner.

`FileActions` owns the routing decision. Native and KIO commands share KDE command serial ordering so Undo/Redo remains coherent across both backends.

## Undo/Redo
`UndoController` manages `KIO::FileUndoManager` for `ThisPcWindow`. The 0.16.0 rename bug is an important lesson: record the actual KIO operation in the way FileUndoManager expects rather than inventing metadata that does not match the executed job.

## Operation UI
`OperationManager` owns stable operation IDs and snapshots derived from KJob/KIO signals. The compact custom `Qt::Popup` remains the history and quick-overview surface. A normal nonmodal `OperationWindow` presents active tasks only, opens automatically, groups concurrent operations, sizes to its contents up to a screen-relative cap and closes when no active task remains. It shows structured CopyJob paths, processed/total bytes, current and average speed, ETA and a bounded 120-sample graph with a vertical speed scale. Rebuilds are coalesced and deferred while a pointer interaction is active.

`KIO::CopyJob::suspend()` must not be presented as exact local-transfer pause. KDE workers may buffer work/progress after suspension. `OperationManager` therefore exposes Pause only for jobs that advertise genuine suspendability; KIO fallback jobs do not receive that capability.

## Native local transfer engine
0.23.0 implements the local engine behind focused modules rather than embedding local I/O loops in `ThisPcWindow` or `OperationWindow`:

- `LocalFileCopyJob` performs bounded-chunk copy with exact Pause/Resume and `.thispc-part` publication;
- `LocalFileMoveJob` handles local moves and verified cross-filesystem move semantics;
- `LocalTransferPlan` builds immutable plans for regular files, directories and symbolic links;
- `LocalTransferJob` executes trees and multiple sources with aggregate progress/current-file reporting;
- `LocalTreeHistory` provides validated native tree Undo/Redo;
- native overwrite support retains/replaces existing targets safely before cleanup.

The native planner/executor preserves supported permissions, timestamps and literal symlink targets. Remote URLs and unsupported local conflicts/cases continue through KIO.
