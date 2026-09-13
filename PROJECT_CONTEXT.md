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
- future rich transfer window, previews, search, device/network integration, etc.

## Last confirmed stable version
**0.21.0** is the current user-confirmed stable release candidate on `main`.

User-confirmed working behavior includes:
- navigation and address/breadcrumb controls;
- common home folders and Windows-labeled drives;
- file/folder operations through KIO;
- context menus, view modes and sorting;
- search, hidden files, thumbnails;
- command toolbar and New/Open With/Properties;
- editable permissions and admin fallback via `admin://`;
- tabs, new-window behavior and reopen closed tab;
- Split View with independent right pane state;
- operation manager popup on the right side of the application;
- dynamic operation list height with scrollbar at the cap;
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
- `operationmanager.h` — KIO job tracking and operation popup;
- `undocontroller.h` — KIO Undo/Redo integration;
- `propertiesdialog.h` — Properties, permissions, admin/NTFS behavior;
- `searchcontroller.h` — search lifecycle, filters and result integration;
- `fileactions.h` — create/copy/move/rename/trash/conflict actions;
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


## Current state — 0.21.0

0.18.0 conflict handling is user-confirmed. 0.19.x adds persistent session restore and the stable full-name/IconMode layout. 0.20.0 adds persistent Quick Access / Favorites and Recent locations.

0.21.0 completes the architecture refactor and stabilization milestone. Major responsibilities were extracted from `src/thispcview.cpp` into dedicated modules without changing the user-facing behavior or settings format. The final automated suite passes 577 assertions, including real KIO operations, conflicts, Trash and Undo/Redo, and the user completed full manual KDE/CachyOS acceptance without regressions.

The next roadmap milestone is 0.22.0 — Advanced transfer window.
