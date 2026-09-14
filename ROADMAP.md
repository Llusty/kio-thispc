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

## Completed releases

### 0.19.0.4 — Session restore + full names ✅ user-confirmed
- restore tabs, active tab, locations and primary navigation history;
- restore Split View per tab, including right-pane location/view/sort state;
- restore splitter widths;
- option in View to reopen the previous session;
- selected icon expands to show its full wrapped name;
- toolbar toggle `Full names` / `Pełne nazwy` persists across launches;
- fixed rigid IconMode grid and bounded multi-line filename layout;
- selected full-name callout no longer changes row geometry.

### 0.20.0 — Quick Access / Favorites / Recent ✅ user-confirmed
- pin/unpin folders;
- drag reorder with persistent ordering;
- favorites / Quick Access in sidebar;
- persistent recent locations;
- optional recent files deferred.

### 0.21.0 — Architecture refactor + stabilization ✅ user-confirmed
- extracted `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController` and `FileActions`;
- moved shared browser helpers/types to `browsercommon.h`;
- reduced duplication between primary and split panes;
- reduced `src/thispcview.cpp` from roughly 14,178 to roughly 7,250 lines;
- added/expanded regression tests for pane actions, tab Drag & Drop, Properties, search and file actions;
- 577 automated assertions pass;
- full manual KDE/CachyOS acceptance passed without regressions.

### 0.22.0 — Advanced transfer window ✅ user-confirmed
- normal movable/minimizable window opens automatically for active operations;
- current file, source/destination summary, processed/total size, prominent percentage, current/average speed and ETA;
- bounded speed-over-time graph;
- multiple simultaneous operations in one dynamically sized window;
- responsive expandable details and per-operation cancellation;
- automatic close after the last active operation finishes;
- compact popup remains the operation-history view;
- misleading KIO-based pause was rejected during manual acceptance and removed.

### 0.23.0 — Native local transfer engine ✅ user-confirmed
- chunked local copy with real pause/resume at a known byte offset;
- cross-device move built on verified copy followed by source removal;
- safe partial-file handling and cleanup/recovery after cancellation or failure;
- conflicts, overwrite/rename/skip decisions, directories, links, permissions and timestamps;
- disk-full, disconnect and I/O error handling;
- progress integration with the 0.22.0 detailed window and compact history;
- Undo/Redo integration where the completed operation is safely reversible;
- keep KIO for remote URLs and operations where it remains the appropriate backend;
- integrity and interruption tests before replacing any existing local KIO path;
- full automated regression: **842/842 assertions**;
- full manual KDE/CachyOS acceptance passed, including conflicts, trees, symlinks, metadata, Pause/Resume, Undo/Redo, disk-full, device-loss and SHA-256 integrity.

## Planned releases

### 0.24.0 — Resizable and scrollable sidebar
- expanding Quick Access, Recent, Places, Remote or Devices does not change the application window height;
- the sidebar gains its own vertical scrollbar whenever expanded content does not fit;
- the divider between the sidebar and file view can be dragged to change sidebar width;
- the chosen sidebar width persists across launches.

### 0.25.0 — Archives + richer New menu
Archives:
- native KDE/Ark-oriented integration;
- ZIP / 7z / tar.gz where backend support exists;
- Extract Here / Extract To / Add to archive;
- CLI fallback only where justified.

New:
- folder, empty file, text, Markdown;
- user templates.

### 0.26.0 — Preview pane
- images, PDF, text/Markdown, JSON/XML;
- audio/video metadata;
- EXIF;
- archive contents;
- folder summary;
- toggle shortcut such as Alt+P.

### 0.27.0 — Quick Look on Space
Large temporary preview without opening the associated app.

### 0.28.0 — Batch rename
- prefix/suffix;
- numbering;
- replace text;
- extension/case changes;
- regex;
- preview before apply;
- Undo support.

### 0.29.0 — Advanced search
- type/name/extension/date/size filters;
- files-only/folders-only;
- saved searches;
- Baloo acceleration when available, current search fallback otherwise.

### 0.30.0 — Grouping and per-folder view settings
- group by type/date/size;
- Today/Yesterday/This week/etc.;
- remember icon/list/details mode per folder.

### 0.31.0 — Split View synchronization
- compare left/right;
- same / only-left / only-right / changed;
- preview synchronization plan;
- copy differences left/right;
- safe sync execution.

### 0.32.0 — File/folder comparison
- external Meld/KDiff3 integration first;
- folder difference view;
- optional hashes for stronger comparisons.

### 0.33.0 — Drives and devices
- mount/unmount/eject;
- removable media;
- MTP;
- ISO mount/unmount;
- filesystem/mount details;
- sensible SMART integration where available.

### 0.34.0 — Network
- SMB, SFTP, FTP, WebDAV via KIO where appropriate;
- saved remote locations;
- network discovery where reliable.

### 0.35.0 — Disk usage analyzer
- biggest directories/files;
- background scan;
- top-N views;
- optional treemap later.

### 0.36.0 — Duplicates + checksums
- SHA-256 / SHA-1 / MD5 utilities;
- duplicate discovery by size then hash;
- safe review before removal/move.

### 0.37.0 — Advanced Properties / ACL
- POSIX ACL;
- owner/group/inode/filesystem/mount;
- atime/mtime/ctime;
- MIME/checksum;
- EXIF/media metadata;
- continue verified NTFS behavior rather than blanket assumptions.

### 0.38.0 — Administrator fallback for failed operations
When a normal operation receives permission denied, offer a targeted `admin://` retry instead of requiring an entire window to run elevated.

### 0.39.0 — Transfer queue/control
- serial vs parallel;
- concurrency limit;
- priorities/order;
- queue-wide control and priorities on top of backends that genuinely support them.

### 0.40.0 — Notifications + operation history
- Plasma notification for long/background completions;
- recent operation log with source/destination/result;
- retry where meaningful.

### 0.41.0 — Plugin / Service Action architecture
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
