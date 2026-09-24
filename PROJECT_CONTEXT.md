# PROJECT_CONTEXT.md

## What this project is
`kio-thispc` began as a KDE KIO worker exposing a Windows-like **This PC / Ten komputer** location via `thispc:/`. It then grew a native Qt/KDE application, `thispc-view`, which uses KIO and provides a lightweight Windows-Explorer-inspired file manager for KDE Plasma 6.

The user actively runs and tests every release on a real KDE system. Screenshots and compiler logs are the source of truth for UI/runtime behavior.

## Product direction
The goal is not to replace KDE with Windows UI. The goal is a familiar Explorer-like workflow while using native KDE/Qt/KIO infrastructure:
- This PC home screen with common folders and drives;
- sidebar with Places, Remote, Devices;
- tabs and Split View;
- native file operations and properties;
- operation progress UI;
- Undo/Redo;
- rich transfer UI, native local transfer engine, previews, search, device/network integration, etc.

## Current release candidate — 0.31.0
**0.31.0 — Split View Synchronization** is functionally complete and
has passed manual KDE/CachyOS acceptance. Stage 1 (Read-only Compare),
Stage 2 (Sync Plan Preview) and Stage 3 (Safe Execution & Deterministic Cancel)
are complete and verified. The source tree is being prepared as a release candidate.

Accepted 0.31.0 scope:
- Read-only pane comparison (Stage 1) for left/right Split View folders (Same, Only left, Only right, Changed);
- Unicode/case-sensitive name matching, non-recursive direct child scan, blocked for `thispc:/` and `thispcsearch:/`;
- Sync plan preview (Stage 2) for Left → Right and Right → Left directions (NoAction, CopyFile, UpdateFile, Conflict, Unsupported);
- Safe asynchronous execution (Stage 3) via `LocalFileCopyJob` with zero deletions, no recursive directory sync, conflicts/unsupported untouched, and per-file atomic preflight;
- Deterministic Cancel lifecycle eliminating counter race conditions (UI report strictly matches disk state, verified with 20,000 files cancel audit);
- Automatic re-comparison after execution completion.
- per-folder view mode/settings persisted by normalized URL and shared across the
  main pane, Split View, tabs and restored sessions;
- reorganized Explorer-like View menu with the existing working display options;
- four icon sizes (96/64/48/32 px) with stable fixed-grid geometry;
- Compact view with dense column layout and normal selection, activation, DnD
  and file actions;
- Group by Type, Date and Size in Icons/List/Details/Compact, including Search,
  Split View, tabs and KIO-backed locations;
- current sorting preserved inside groups, with non-file group headers kept out
  of file actions, selection, clipboard, DnD, Preview and Quick Look;
- Date grouping uses local-calendar buckets and midnight refresh; Size grouping
  uses the non-recursive size supplied by KIO and has separate folder/unknown
  categories;
- selection-border rendering is consistent for short and expanded names;
- a stale-hover rendering bug that looked like double selection was fixed; the
  actual `QItemSelectionModel` had only one selected item, and manual retesting
  confirmed normal single-select plus Ctrl/Shift multi-select behavior.

The icon-size slider and broader keyboard-navigation work remain explicitly
outside 0.30.0 and stay on the post-release backlog.

User-confirmed working behavior includes:
- navigation and address/breadcrumb controls;
- common home folders and Windows-labeled drives;
- file/folder operations through KIO;
- context menus, view modes and sorting;
- search, hidden files, thumbnails;
- command toolbar and New/Open With/Properties;
- editable permissions and admin fallback via `admin://`;
- tabs, new-window behavior and reopen closed tab;
- Split View with equal left/right pane behavior, independent state and per-pane breadcrumbs;
- active-pane routing for shared toolbar, sidebar, View, Sort, navigation and Search controls;
- sidebar Drag & Drop, vertical scrolling, persisted resizable width and long-label ellipsis/tooltips;
- independent Search state and full `thispc:/` card presentation in both panes;
- operation manager popup on the right side of the application;
- automatic detailed window for active operations with graph, speed, ETA and prominent progress;
- multiple simultaneous operations combined in one dynamically sized window;
- automatic detailed-window close after the last active operation;
- dynamic operation list height with scrollbar at the cap;
- native local file/tree copy and move with exact Pause/Resume and safe partial publication;
- native local conflicts, directories, multiple sources and symbolic links with preserved metadata;
- native/KIO ordered Undo/Redo, including verified cross-filesystem moves and tree history;
- visible application version in the bottom-right status area;
- Undo/Redo through `KIO::FileUndoManager`, including fixed rename Undo in 0.16.0.1.

## Important historical milestones
### 0.12.x — Properties / permissions
Editable General + Permissions UI was introduced. Permission changes are performed through KIO and read back to verify the result. `admin://` + kio-admin/PolicyKit support was added.

### NTFS discovery
The application must not assume NTFS/fuseblk permissions are inherently uneditable. The correct policy is **attempt + verify**.

The user configured true NTFS-3G permissions on C/D/G using `.NTFS-3G/UserMapping` and `permissions` mount behavior. On C and G, actual `chmod` changes were verified by the user. Existing Windows ACL/security descriptors may still result in items such as `root:root`; do not mass-change them.

### 0.13.x — Tabs
Multiple tabs, tab history/state, reorder/close/duplicate/reopen, Ctrl+T/W/Shift+T, Ctrl+Tab, folder/background open-in-new-tab, sidebar new-tab/new-window behavior.

### 0.14.x — Split View
Two-pane browsing with F3/F6 workflow, independent address/navigation, drag/drop plumbing between panes, view modes/sorting/thumbnails, active-pane highlighting and per-tab split state.

### 0.15.x — Operation manager
A bottom operation panel was rejected visually. It evolved into a compact top-right operation popup. `QMenu` sizing proved unreliable, so 0.15.3 switched to a custom `Qt::Popup`/`QFrame`. In 0.15.4 the popup was shifted mostly outside the application's right edge and the version label was made palette-readable. User confirmed the result looked good and worked.

### 0.16.x — Undo/Redo
Implemented with `KIO::FileUndoManager`. Initial manual rename recording failed: Undo consumed Rename without restoring the old name, then the next Undo tried to remove the previously created folder. 0.16.0.1 fixed rename by using `KIO::moveAs()` + `recordCopyJob()`. User confirmed rename Undo/Redo and Trash restoration work.

## Historical development note: 0.17 Drag & Drop
The user requested complete Explorer-like Drag & Drop. Several 0.17 hotfixes were attempted after 0.16.0.1.

Observed state by 0.17.0.4:
- dragging a file can start;
- hovering/dropping on a tab works sufficiently to demonstrate a real drag is active;
- dropping on the open directory's main file viewport is rejected with the prohibited cursor;
- therefore the source side is no longer the main problem; acceptance of the directory viewport/background is.

Do not continue stacking ad-hoc event filters on the failed branch. Start from stable `main`, reproduce locally, inspect Qt event flow, and implement the smallest correct fix.

See:
- `TASK_0.17_DRAG_DROP.md`
- `handoff/failed-0.17.0.4.patch`
- `handoff/screenshots/`
- branch `reference/failed-0.17.0.4`

## Main source layout today
The 0.21.0 refactor split the former monolithic `src/thispcview.cpp` into focused modules while keeping `ThisPcWindow` as the coordinator:
- `browsercommon.h` — shared types/helpers;
- `directoryview.h` — directory widgets, filename delegate and shared view behavior;
- `splitbrowserpane.h` — secondary pane;
- `sidebar.h` — Places/Remote/Devices, Quick Access and Recent;
- `sessionmanager.h` — persisted tabs/history/split state;
- `operationmanager.h` — KIO/native job tracking, compact history and detailed operation window;
- `undocontroller.h` — KIO Undo/Redo integration;
- `propertiesdialog.h` — Properties, permissions, admin/NTFS behavior;
- `searchcontroller.h` — search lifecycle, filters and result integration;
- `fileactions.h` — create/copy/move/rename/trash/conflict actions and native/KIO routing;
- `localfilecopyjob.h` — chunked native local single-file copy;
- `localfilemovejob.h` — native local move and cross-filesystem move semantics;
- `localfileoverwrite.h` — safe native overwrite publication/recovery;
- `localtransferplan.h` — immutable asynchronous plans for files, trees and symbolic links;
- `localtransferjob.h` — aggregate execution for directories and multiple sources;
- `localtreehistory.h` — safe native tree Undo/Redo;
- `thispcview.cpp` — main window orchestration and remaining UI/navigation logic.

The refactor reduced `src/thispcview.cpp` from roughly 14,178 to roughly 7,250 lines and is intended to make later roadmap features safer to add.

## User workflow
Typical installation/test cycle:
```bash
cd ~/Pobrane
rm -rf kio-thispc
unzip kio-thispc-X.Y.Z.zip
cd kio-thispc
chmod +x install.sh
./install.sh
thispc-view
```

For Codex, prefer working directly in a Git checkout and running `./scripts/build.sh` after edits instead of repeatedly creating ZIPs.


## Current state — 0.31.0

0.18.0 conflict handling is user-confirmed. 0.19.x adds persistent session restore
and the stable full-name/IconMode layout. 0.20.0 adds persistent Quick Access /
Favorites and Recent locations.

0.21.0 completes the architecture refactor and stabilization milestone. Major
responsibilities were extracted from `src/thispcview.cpp` into dedicated modules
without changing the user-facing behavior or settings format.

0.22.0 adds the user-confirmed advanced transfer window while retaining the
compact popup as operation history. 0.23.0 delivers the native local transfer
engine with exact Pause/Resume for supported local transfers, safe partial
publication, conflict handling and native Undo/Redo. KIO remains in use for
remote URLs and unsupported/unresolved local cases.

0.24.0 completes Sidebar & Split View UX. Both panes are equal peers, shared
controls route to the active pane, sidebar DnD uses the existing FileActions
path, and Search plus `thispc:/` presentation work independently in both panes.

0.26.0 Preview Pane and 0.27.0 Quick Look are user-confirmed. 0.28.0 Batch Rename
was released as commit `c361c08` and tagged `v0.28.0`; its production automatic
recovery remains deliberately limited to qualifying local-linear Execute/Undo/Redo.
Swap/cycle recovery, equivalent KIO guarantees and power-loss durability remain
future work.

0.29.0 Intelligent New Item Naming was released as commit `e56acee`, tagged
`v0.29.0`, installed and manually confirmed. Local naming and remote KIO
preflight use first-free suggestions and no-overwrite final operations within
the documented backend/protocol limits.

0.30.0 Grouping and per-folder view settings was released and user-confirmed,
including Icons/List/Details/Compact, per-folder settings, Type/Date/Size grouping,
and hover/selection fixes.

0.31.0 Split View Synchronization is complete and user-confirmed:
- Stage 1 (Read-only compare): compares direct children between left and right
  panes (Same, Only left, Only right, Changed), case-sensitive/Unicode matching,
  asynchronous listing with generation tracking, blocked for virtual locations;
- Stage 2 (Sync plan preview): deterministic plan calculation for Left → Right
  and Right → Left (NoAction, CopyFile, UpdateFile, Conflict, Unsupported),
  zero deletions, no recursive directory diffing, read-only preview;
- Stage 3 (Safe execution & deterministic cancel): execution of CopyFile and
  UpdateFile using `LocalFileCopyJob`, zero deletions, directories/conflicts/
  unsupported untouched, atomic preflight revalidation before each file copy,
  and deterministic Cancel lifecycle waiting for active worker thread completion
  to guarantee strict UI-to-filesystem counter equality (verified on 20,000 files);
- Automatic re-comparison after sync execution.

Manual acceptance for 0.31.0 Stage 1, Stage 2 and Stage 3 has passed with 100% PASS.
Automated regression: 25 test suites / 6618 assertions PASS.
The working tree is prepared for the 0.31.0 release.
