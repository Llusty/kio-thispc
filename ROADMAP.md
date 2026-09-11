# ROADMAP.md

This ordering prioritizes core file-manager correctness, safety and maintainability before advanced convenience features.

## Completed baseline
- **0.16.0.1 — Undo / Redo** ✅ user-confirmed
  - copy/move/rename/trash/new folder/new file where supported by KIO;
  - Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z;
  - rename hotfix using `KIO::moveAs()` + `recordCopyJob()`.
- **0.17.0.5 — Full Drag & Drop + Split View parity** ✅ user-confirmed
  - background/folder/tab drops, primary ↔ split, Ctrl=Copy, Shift=Move;
  - tab hover activation;
  - active-pane shortcuts/context menus;
  - cycle protection and Undo integration.

### 0.18.0 — Copy/move conflict handling ✅ user-confirmed
- interactive KIO/KIOWidgets conflict UI for clipboard, Drag & Drop and Send To;
- overwrite / skip / rename or suggested new name;
- remembered decisions for multiple conflicts;
- source/destination size and timestamp comparison;
- nested conflicts handled by `KIO::CopyJob`;
- clean cancellation without a second error dialog.

## Current release candidate
### 0.19.0 — Session restore + full names
- restore tabs, active tab, locations and primary navigation history;
- restore Split View per tab, including right-pane location/view/sort state;
- restore splitter widths;
- option in View to reopen the previous session;
- selected icon expands to show its full wrapped name;
- toolbar toggle `Full names` / `Pełne nazwy` persists across launches.

## Planned releases

### 0.20.0 — Quick Access / Favorites / Recent
- pin/unpin folders;
- drag reorder;
- favorites in sidebar;
- recent locations and optionally recent files.

### 0.21.0 — Architecture refactor + stabilization
Split the monolithic view into maintainable components such as:
- `ThisPcWindow`
- `DirectoryView`
- `SplitBrowserPane`
- `Sidebar`
- `OperationManager`
- `OperationPopup`
- `UndoController`
- `PropertiesDialog`
- `SearchController`
- `SessionManager`
- `FileActions`

Goal: reduce duplication between primary/split panes and make subsequent features safer.

### 0.22.0 — Advanced transfer window
A normal movable/minimizable top-level window, inspired by Windows file operation details:
- current file;
- source/destination;
- total/processed size;
- percentage;
- current and average speed;
- ETA;
- speed-over-time graph/histogram;
- multiple simultaneous operations;
- cancellation;
- expandable/collapsible details;
- existing compact popup remains as quick overview and can open the detailed window.

### 0.23.0 — Archives + richer New menu
Archives:
- native KDE/Ark-oriented integration;
- ZIP / 7z / tar.gz where backend support exists;
- Extract Here / Extract To / Add to archive;
- CLI fallback only where justified.

New:
- folder, empty file, text, Markdown;
- user templates.

### 0.24.0 — Preview pane
- images, PDF, text/Markdown, JSON/XML;
- audio/video metadata;
- EXIF;
- archive contents;
- folder summary;
- toggle shortcut such as Alt+P.

### 0.25.0 — Quick Look on Space
Large temporary preview without opening the associated app.

### 0.26.0 — Batch rename
- prefix/suffix;
- numbering;
- replace text;
- extension/case changes;
- regex;
- preview before apply;
- Undo support.

### 0.27.0 — Advanced search
- type/name/extension/date/size filters;
- files-only/folders-only;
- saved searches;
- Baloo acceleration when available, current search fallback otherwise.

### 0.28.0 — Grouping and per-folder view settings
- group by type/date/size;
- Today/Yesterday/This week/etc.;
- remember icon/list/details mode per folder.

### 0.29.0 — Split View synchronization
- compare left/right;
- same / only-left / only-right / changed;
- preview synchronization plan;
- copy differences left/right;
- safe sync execution.

### 0.30.0 — File/folder comparison
- external Meld/KDiff3 integration first;
- folder difference view;
- optional hashes for stronger comparisons.

### 0.31.0 — Drives and devices
- mount/unmount/eject;
- removable media;
- MTP;
- ISO mount/unmount;
- filesystem/mount details;
- sensible SMART integration where available.

### 0.32.0 — Network
- SMB, SFTP, FTP, WebDAV via KIO where appropriate;
- saved remote locations;
- network discovery where reliable.

### 0.33.0 — Disk usage analyzer
- biggest directories/files;
- background scan;
- top-N views;
- optional treemap later.

### 0.34.0 — Duplicates + checksums
- SHA-256 / SHA-1 / MD5 utilities;
- duplicate discovery by size then hash;
- safe review before removal/move.

### 0.35.0 — Advanced Properties / ACL
- POSIX ACL;
- owner/group/inode/filesystem/mount;
- atime/mtime/ctime;
- MIME/checksum;
- EXIF/media metadata;
- continue verified NTFS behavior rather than blanket assumptions.

### 0.36.0 — Administrator fallback for failed operations
When a normal operation receives permission denied, offer a targeted `admin://` retry instead of requiring an entire window to run elevated.

### 0.37.0 — Transfer queue/control
- serial vs parallel;
- concurrency limit;
- priorities/order;
- pause/resume only where the backend genuinely supports it.

### 0.38.0 — Notifications + operation history
- Plasma notification for long/background completions;
- recent operation log with source/destination/result;
- retry where meaningful.

### 0.39.0 — Plugin / Service Action architecture
Allow new context-menu actions and integrations without editing the core window source.

## Before 1.0
Stability-focused cycle covering:
- large files/directories;
- tens of thousands of entries;
- Btrfs/ext4/NTFS;
- local and remote KIO;
- Split View parity;
- Undo/Redo conflicts;
- device hotplug;
- crash recovery;
- memory/resource leaks;
- keyboard accessibility audit;
- regression tests for file-operation safety.
