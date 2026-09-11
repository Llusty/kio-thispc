# TASK_0.17_DRAG_DROP.md

## Goal
Implement reliable, Windows-Explorer-like Drag & Drop without regressing the user-confirmed 0.16.0.1 baseline.

## Current verified symptoms
The failed 0.17.0.4 experiment demonstrates:
- a file can be picked up and a real drag starts;
- dragging onto a tab is recognized;
- the open directory's main background/file viewport shows the **prohibited/no-drop cursor** and rejects the drop;
- therefore solve destination acceptance/event routing rather than repeatedly rewriting the drag source.

Reference screenshots:
- `handoff/screenshots/drag-to-tab-works-0.17.0.2.png`
- `handoff/screenshots/drop-on-directory-rejected-0.17.0.4.png`

## Failed attempts to learn from
The reference branch/patch contains experiments including:
1. custom `startDrag()` in list/tree widgets;
2. manual mouse press/move threshold using `QApplication::startDragDistance()`;
3. custom MIME marker `application/x-thispc-drag` alongside URL data;
4. `viewport()->setAcceptDrops(true)`;
5. a viewport `eventFilter()` attempt;
6. tab/sidebar/drive/drop-target plumbing.

One automated edit in 0.17.0.3 accidentally inserted a QListWidget viewport event filter into `BreadcrumbFrame` and failed compilation. 0.17.0.4 moved it back, but the runtime directory-background drop was still rejected.

Do not assume any of those experiments are conceptually correct merely because they compile.

## Investigation plan
Before changing code, reproduce the issue and instrument the event flow temporarily if needed.
Check at minimum:
- actual runtime class receiving `QEvent::DragEnter`, `DragMove`, `Drop` and `DragLeave`;
- whether `QAbstractItemView` or its viewport accepts URLs;
- `dragDropMode`, `acceptDrops`, `dragEnabled`, `defaultDropAction`, `dropIndicatorShown`;
- supported drag/drop actions;
- whether item/model flags prevent dropping on viewport/background;
- whether the event is accepted, ignored, or overwritten by a base-class handler after custom handling;
- distinction between dropping on a folder item and on empty background;
- Qt6 `QDropEvent::position()` coordinate system for the actual receiver;
- whether Split View uses a separate view instance requiring shared logic.

Prefer a reusable destination handler over duplicated ad-hoc filters.

## Required behavior for 0.17.0
### Source
- LMB press + movement beyond system drag threshold starts dragging selected items.
- Normal click still selects.
- Multi-selection remains usable.
- Standard `text/uri-list`/Qt URL MIME interoperability with Dolphin and Plasma.

### Destination
- Drop on empty area of a directory view -> current directory.
- Drop directly on a directory item -> that directory.
- Drop primary -> split and split -> primary.
- Drop on a tab activates/targets that tab after a sensible hover delay.
- Sidebar folder/drive destinations should work where writable.
- Trash accepts appropriate drops using KIO Trash semantics.

### Action choice
Desired Explorer-like policy:
- same filesystem, no modifier -> move;
- different filesystem, no modifier -> copy;
- Ctrl -> copy;
- Shift -> move;
- link modifier only if implementation is safe and KIO supports the source/destination.

If KDE's standard DnD action-selection UI is more reliable/idiomatic than custom modifier logic, prefer correctness and consistency with KDE, document the choice, and preserve explicit Ctrl/Shift where practical.

### Safety
- reject drop into itself;
- reject moving a directory into one of its descendants;
- reject nonsensical/non-writable special destinations cleanly;
- do not delete source data until the move operation succeeds;
- resulting KIO CopyJob/MoveJob should integrate with operation progress and Undo where supported.

## Acceptance test matrix
At minimum test with temporary files/folders:
1. current folder -> child folder;
2. tab A -> hover tab B -> empty background;
3. primary pane -> Split View;
4. Split View -> primary pane;
5. same filesystem default action;
6. cross-filesystem default action;
7. Ctrl+drag;
8. Shift+drag;
9. Dolphin -> thispc-view;
10. thispc-view -> Dolphin or Plasma desktop;
11. folder into itself/descendant is rejected;
12. Undo after a DnD copy/move where FileUndoManager supports it.

Do not release until the directory-background case works; that is the exact regression the user reported.

## Current implementation status — 0.17.0.5 test build
User testing under Plasma has already confirmed:
- directory background Drop works;
- primary <-> Split View Drop works;
- Split View keyboard actions/context menus now follow the active pane;
- tab hover activation and direct Drop on tabs work.

0.17.0.5 completes the interrupted action-policy stage:
- Ctrl forces Copy;
- Shift forces Move;
- cursor/default action is Move for the same local storage and Copy for a different/unknown storage;
- an unmodified Drop still shows the explicit Copy/Move menu;
- directory self/descendant cycles are blocked before KIO dispatch.

Still planned before final 0.17.0:
- writable sidebar/drive Drop targets;
- Trash Drop using KIO Trash semantics;
- final Dolphin/Plasma interoperability pass;
- manual Undo verification for real DnD Copy/Move combinations.
