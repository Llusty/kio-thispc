[English](README.md) | [Polski](README.pl.md)

# kio-thispc (This PC) 0.40.0

ThisPC is a native KDE/Qt file manager focused on a Windows Explorer-like workflow, dual-pane operation, KIO integration, safe local file operations, and native KDE/Linux device handling.

- **Platform:** Linux / KDE Plasma 6
- **Frameworks:** Qt 6 / KDE Frameworks 6
- **License:** MIT License

---

## 0.40.0 — Advanced Properties Follow-up

- Properties now detects backend capabilities and retains a no-follow identity snapshot, rejecting writes after an item disappears, moves, is replaced, or loses its mount.
- Extended Attributes shows bounded text/binary previews and supports immediate local user.* add/edit/remove through verified file descriptors. These operations are outside Apply/Cancel/Undo. security.*, trusted.* and system.* remain protected.
- Numeric POSIX permissions (0000–0777) synchronize with permission checkboxes, preserve existing special bits, and refresh ACL state using normal kernel mask semantics.
- Hidden presentation distinguishes KIO visibility, Unix dot-name semantics, and unverified native flags. Safe Unix dot-name changes use the shared rename path; NTFS native Hidden is not emulated or written.
- Primary, Split and Search share Properties behavior, refresh, target-loss guards and safe close sequencing. ACL remains unavailable for links after rename, including broken links.
- Manual Stage 1–6 acceptance is prior user-confirmed evidence. Live ext4 and remote KIO remain NOT TESTED; deterministic policies are covered automatically. Final release verification is recorded in [the release verification](docs/PROPERTIES_RELEASE_040.md); the pre-release audit remains historical.

Excluded: SELinux labels, chattr/immutable, Linux file capabilities, larger ACL redesign, admin:// retry.

---

## 0.39.0 — Native File Integration & Rich Previews

- Local files now resolve to proper `file://` URLs and launch through the native KDE/KIO Open/Execute workflow, including local executables and Windows binaries handled by their system associations.
- Image and video thumbnails, native Windows executable icons, and folder content previews use the KDE KIO preview infrastructure with standard MIME-icon fallback.
- **View → Show Previews** is stored per location in `DirectoryViewProfile` and applies consistently to Primary, Split, and search views.
- `PreviewController` schedules previews asynchronously in bounded batches, caches results, prioritizes visible items in large directories, and rejects stale results with generation tokens.
- Remote previews stay disabled. F5 invalidates preview state, and preview jobs are cancelled safely during navigation and shutdown.
- Manual acceptance Stages 1–7: FULL MANUAL PASS. Final release regression is recorded in the 0.39.0 changelog.

---

## 0.38.0 — Storage Tools

- **Storage Scan Core:** High-performance, asynchronous background directory scanner (`StorageScanJob`, `StorageScanWorker`) with live counters, cancel support, mount boundary isolation (`st_dev`), strict symlink rejection, hardlink deduplication, and aggregated error reporting.
- **Largest Files & Directories (Top-N):** Disk space analyzer displaying the largest items with configurable limits (Top 25/50/100/All), logical vs. allocated sorting, subfolder drill-down, and "Show in folder" navigation.
- **Hash Utilities:** Standalone modeless checksum dialog (`HashUtilitiesDialog`) for SHA-256, SHA-1, and MD5 using cancelable chunked streaming workers, live progress, clipboard copy, and concurrent modification detection.
- **Duplicate Finder:** Fast duplicate detection (`DuplicateFinderJob`) using size-first candidate grouping, physical identity deduplication, and sequential SHA-256 validation only for colliding byte blocks.
- **Safe Review & Actions:** Interactive duplicate cleanup controller (`DuplicateActionController`) integrated with native `FileActions` and `OperationManager`. Supports Trash and Move with confirmation dialogs, preflight snapshot revalidation, `KIO::FileUndoManager` Undo, and exclusion of modified files.
- **Interactive Treemap ("Mapa zajętości"):** Pure in-memory Squarified Treemap layout engine (`StorageTreemapLayout`, `StorageTreemapWidget`) visualizing directory space without rescanning or MIME probing. Features canonical physical ownership for allocated space, logical extent mode, drill-down, hover tooltips, and keyboard navigation.
- Final automated regression: 52 suites / 32282+ assertions PASS; build, CLI `thispc-view 0.38.0`, and manual acceptance Stages 1–6 PASS.

---

## 0.37.0 — Properties

- **Comprehensive Properties 2.0:** Asynchronous data-model backed properties for files, directories, and filesystem volumes.
- **General Info:** Full path and location details, logical vs. allocated disk size, inode number, owner/group, timestamps (including `statx` birth time), and symlink target resolution.
- **Drive Properties:** Modeless filesystem volume inspector showing filesystem type, mount options, capacity/used/free space bars, UUID, parent disk bus hierarchy (NVMe/SATA/USB), and safe device removal indicators.
- **POSIX & Native ACL Editor:** Standard permission bits with live write verification plus dedicated `libacl` table editor supporting named users/groups, mask computation, effective permissions, and default directory ACL inheritance.
- **SHA-256 Checksums:** Dedicated checksum calculator computing hashes asynchronously in chunks with an animated progress bar, cancel/restart, clipboard copy, and file modification detection.
- **KDE Metadata:** Lazy, read-only metadata inspection powered by KF6 `KFileMetaData` for images (EXIF), audio tags, video streams, and office/PDF documents.
- **Modeless Multi-Window:** File, folder, and drive properties dialogs operate modelessly; multiple dialogs can stay open simultaneously across Primary and Split panes; safe target identity check (`st_dev`/`st_ino`) prevents saving into deleted or replaced files.
- Final automated regression: 46 suites / 9690 assertions PASS; build, CLI `thispc-view 0.37.0`, and manual acceptance Stages 1–6 PASS.

---

## 0.36.0 — Network & Remote Locations

- Manual SMB, SFTP, FTP, WebDAV, and WebDAVS connections through native KIO, plus Saved Remote Locations and success-confirmed Recent/Reconnect workflows.
- Transactional remote session and history restore, including persistent independent Split history.
- Optional native `remote:/` and `smb:/` discovery; manual connections do not depend on discovery.
- Passwords are never stored by ThisPC in QSettings; credentials remain with KIO/KWallet.
- Final automated regression: 39 suites / 9075 assertions PASS; build, CLI `thispc-view 0.36.0`, and manual acceptance Stages 1–5 PASS. Live discovered SMB server path: NOT TESTED (no discoverable LAN services available).

---

## 0.35.0 — Drives & Devices

- **Stage 1:** Dynamic drive and storage device discovery backed by KDE Solid; event-driven `SolidDeviceMonitor` with 250 ms debouncing reacts to hotplug and mount changes without periodic polling or flickering "Refreshing…" status.
- **Stage 2:** Detection and presentation of unmounted removable volumes on the `thispc:/` home view with a dedicated `drive-removable-media` icon and "Unmounted" status label; `DeviceMountController` enables on-demand mounting (click or Enter) with immediate navigation into the mounted folder; the sidebar displays mounted volumes only.
- **Stage 3:** Full asynchronous lifecycle management for removable devices via `DeviceRemovalController`:
  - **Unmount:** Performs a clean filesystem-only unmount using native asynchronous QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount`, keeping device power on and retaining volume visibility;
  - **Safely Remove:** Coordinates unmounting of all mounted partitions on the physical drive, followed by asynchronous `org.freedesktop.UDisks2.Drive.PowerOff`;
  - **Safely Remove for unmounted drives:** Works reliably for volumes that are already unmounted;
  - **Eject:** Ejects optical media ("Eject") on hardware with `canEject` capability;
  - **Pane Redirection:** Safely redirects Primary and Split panes away from unmounted or detached mountpoints back to `thispc:/`, without disturbing unaffected panes.
- **Final Automated Regression:** 35 test suites / 8562 assertions PASS; build, CLI `thispc-view 0.35.0`, and manual acceptance PASS.

---

## 0.31.0 — Split View Synchronization

- Safe, non-invasive folder comparison and synchronization in Split View mode (`Ctrl+Alt+C`, Tools / View menu).
- **Stage 1 (Compare Panes):** Asynchronous comparison of direct child items in left and right panes with classification (Same, Only Left, Only Right, Changed) using KIO metadata (size, mtime, entry type) with case-preserving Unicode name matching. Virtual paths (`thispc:/`, `thispcsearch:/`) are safely blocked.
- **Stage 2 (Plan Preview):** Deterministic sync plan generator for Left → Right and Right → Left directions with action classes: No Action, Copy, Update, Conflict, Unsupported.
- **Stage 3 (Safe Execution):** Asynchronous file copying and updating (`LocalFileCopyJob`) with zero deletion operations (no Delete/Mirror), directory recursion skipping, and protection of conflict and unsupported entries.
- **File Operation Safety:** Atomic preflight immediately before mutating each file (path validation, permissions, rejecting `.` and `..`).
- **Deterministic Cancel Lifecycle:** Race-condition-free accounting (no off-by-one errors), waiting for terminal signals from active worker threads, and 100% UI report agreement with physical filesystem state (verified via 20,000-file audit).
- **Automatic Re-comparison:** Immediate re-comparison of panes once synchronization finishes.

---

## 0.30.0 — Grouping and Per-Folder View Settings

- View mode (Icons/List/Details/Compact) is remembered per normalized folder URL and restored across Primary pane, Split View, tabs, and session restore.
- Settings cover local and remote KIO URLs without scanning folder contents or relying on extra metadata.
- **View Menu:** Groups Icons/List/Details/Compact; the **Show** submenu provides working toggles for hidden items, thumbnails, preview pane, and full names.
- **Icon Size Submenu:** Provides working levels: Extra Large (96 px), Large (64 px), Medium (48 px), and Small (32 px), saved per folder.
- **Compact View:** Dense 20 px rows in top-to-bottom filled columns, name eliding, native Qt directional navigation, and dual-pane support.
- **Grouping by Type, Date, and Size:** Works in Icons, List, Details, and Compact across both panes, tabs, and Search/KIO results.
- **Presentation Headers:** Group headers do not participate in selection, file actions, clipboard, DnD, Preview, or Quick Look. Sorting inside groups preserves active key and direction.
- **Calendar & Size Rules:** Date grouping uses local calendar boundaries (Today, Yesterday, This week, etc.); size grouping uses existing KIO size metadata without recursive scans; directories and missing sizes form distinct deterministic groups.
- Visual selection outline and stale hover fixes in grouped view.

---

## 0.29.0 — Intelligent New Item Naming

- New files, folders, and items created from templates suggest the first available name before displaying an editable dialog, automatically filling numbering gaps.
- Numbers are inserted before full extensions; handles hidden files, multiple dots, Unicode, manual editing, and active Split View pane.
- Second preflight check after the dialog prevents collisions. Creation operations never request `Overwrite` or `Resume`, preserving Undo/Redo registration.
- For remote KIO, listings are asynchronous and fail-closed. Not an atomic reservation: ultimate protection relies on worker/protocol no-overwrite guarantees. Details in `RELEASE_NOTES_0.29.0.md`.

---

## 0.28.0 — Batch Rename

- Pre-execution preview: prefix/suffix, numbering, text replacement, case/extension changes, and regular expressions.
- Safe collision preflight and inode-tracking, active Split View pane support, and unified Undo/Redo for qualifying local linear batches.
- Production auto-recovery v2 handles qualifying local linear Execute/Undo/Redo, resuming recorded direction after process crashes.
- Swaps and cycles execute correctly with session Undo/Redo; automatic crash recovery for cycles remains disabled.
- KIO fallback does not carry local-linear guarantees; details in `RELEASE_NOTES_0.28.0.md`.

---

## 0.27.0 — Quick Look on Space

- Large, temporary preview opened with Space in the active file view without launching associated applications.
- Space or Esc closes Quick Look; focus stays in the file view, allowing arrow keys to change selection.
- Preview tracks the active Split View pane and current single selection.
- Search, address bar, inline rename, popups, and dialogs retain standard Space key behavior.
- Shares Preview Pane backend, formats, limits, and asynchronous stale-result protection; `Alt+P` toggles independently.
- Full regression: 22 test suites / 3598 assertions PASS; all 9 manual KDE/CachyOS acceptance cases confirmed.

---

## 0.26.0 — Preview Pane

- Preview panel toggled via `Alt+P`.
- Images with proportional scaling to available space.
- Plain text, Markdown, JSON, and XML.
- Asynchronous first-page PDF preview using QtPdf with opaque white background compositing.
- Audio and video metadata via TagLib.
- Image EXIF metadata via Exiv2 without replacing the image.
- Safe archive manifest preview for ZIP, 7z, tar, and tar.gz via libarchive without extracting.
- Directory summaries and Split View breadcrumb fixes.
- Full regression: 20 test suites PASS, Preview Pane 106 assertions PASS.

---

## 0.25.0 — Archives & Richer New Menu

- **New Menu:** Markdown and user templates; Empty Trash with confirmation.
- **Archive Extraction:** Extract Here / Extract To for ZIP, 7z, tar, and tar.gz; content validation, process isolation (Landlock ABI 3+), and publication without overwriting.
- **Archive Creation:** Create ZIP, 7z, and tar.gz with multi-item selection, progress reporting, error handling, and cancellation.
- Split View width protection for long paths.
- Automated regression: 20 test suites, 3470 assertions PASS; Stage 3 manually accepted.
- Details in `docs/ARCHIVE_STAGE2.md` and `docs/ARCHIVE_STAGE3.md`.

---

## 0.24.0 — Sidebar & Split View UX

- Symmetrical Split View panes with independent breadcrumbs and shared toolbar/sidebar routed to the active pane.
- Sidebar Drag & Drop into Places, devices, and Quick Access destinations via `FileActions`.
- Vertically scrollable, resizable sidebar (205–480 px) with persistent width and right-aligned text ellipsis with tooltips.
- Independent Search state, queries, filters, progress, and jobs per pane.
- Full This PC (`thispc:/`) cards presentation in both panes.
- Full regression: 1022/1022 assertions PASS; manual KDE/CachyOS acceptance PASS.

---

## 0.23.0 — Native Local Transfer Engine

- Chunked local copy/move engine with exact Pause/Resume at known byte offsets; KIO remains the backend for remote and unsupported cases.
- Safe publication via `.thispc-part` files, clean cancellation cleanup, and controlled handling of write errors, disk-full conditions, and device disconnection.
- Native conflict resolution for single files: **Overwrite / Rename / Skip**.
- Directory trees, multi-source operations, and symlinks with total and per-file progress reporting.
- Preserves permissions, nanosecond timestamps, and literal symlink targets.
- Verified cross-filesystem moves, safe Undo/Redo for files and directory trees, and unified command ordering with KIO history.
- Operation window supports Pause/Resume, vertical speed graph scaling, and persistent stopped-job visibility.
- Full regression: 842/842 assertions PASS; manual KDE/CachyOS acceptance confirmed.

---

## 0.22.0 — Advanced Transfer Window

- Active operations automatically open a non-blocking detail window.
- Displays current file, source/destination summary, processed/total size, large progress percentage, current/average speed, ETA, and speed graph (120 samples).
- Multiple simultaneous operations merge into a single dynamically resizing window.
- Expandable/collapsible details with per-operation cancellation.
- Operation history is preserved in the compact toolbar popup.

---

## 0.21.0 — Architecture Refactor & Stabilization

- Deconstructed monolithic `src/thispcview.cpp` into modular components without changing user behavior.
- Extracted: `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController`, and `FileActions`.
- Shared types and helpers relocated to `browsercommon.h`, reducing duplication between panes.
- Reduced `src/thispcview.cpp` from ~14,178 to ~7,250 lines.
- Full regression: 577 automated assertions PASS; full manual KDE/CachyOS acceptance without regressions.

---

## 0.20.0 — Quick Access / Favorites / Recent

- **Quick Access** section in the sidebar.
- Pin and unpin folders via context menus.
- Drag & Drop reordering of pinned items with persistent order stored in `QSettings`.
- **Recent** section tracking visited locations across sessions.

---

## 0.19.0.4 — Icon View Filename Layout Fix

- Rigid, uniform tile grid in IconMode (`setGridSize` + `setUniformItemSizes(true)`) — selecting an item never shifts neighboring rows.
- Custom text layout via `QTextLayout` with `WrapAtWordBoundaryOrAnywhere` — long filenames without whitespace wrap properly without clipping.
- Normal mode: maximum 2 lines of text with ellipsis `…` on the second line.
- **Full Names** mode: uniform tile height with up to 4 lines of text.
- Selected item full name callout rendered directly at viewport level in `paintEvent` without separate `QLabel` widgets.
- Callouts respect viewport boundaries and do not intercept mouse events.

---

## 0.19.0.3 — Session Restore & Full Names

- Remembers and restores open tabs, active tab, and Back/Forward history.
- Split View state preserved per tab: location, view mode, sort settings, and pane enablement.
- Splitter proportions restored from saved geometry.
- Full Names toggle button on the action bar.
- View menu option: **Restore Previous Session**.

---

## 0.18.0 — Intelligent Conflict Handling

- Interactive KIOWidgets conflict handling for copy and move operations.
- Avoids aborting entire operations on existing files or directories.
- Options: Overwrite, Skip, Rename / Suggest Name, and "Apply to All".
- Source and destination sizes and timestamps displayed in dialogs.
- Unified across clipboard paste, Drag & Drop, Send To, and inline rename.

---

## 0.17.0.5 — Full Drag & Drop

This version continues functional 0.17 milestones developed on top of stable 0.16.0.1, intended for KDE Plasma testing before final 0.17.0.

- Drag & Drop to directory background and folders across Icons, List, and Details views.
- Drag & Drop between Primary and Split panes.
- Tab drop support with ~650 ms hover activation.
- `Ctrl+drag` forces Copy; `Shift+drag` forces Move.
- Default drag without modifiers suggests Move on the same filesystem and Copy between different storage devices.
- Prevents copying/moving folders into themselves or descendants.
- Registered with `KIO::FileUndoManager`.

---

## Undo / Redo

File operations executed through `thispc-view` support native Undo/Redo backed by `KIO::FileUndoManager`:
- Copying and moving;
- Renaming;
- Moving to Trash and restoring on Undo;
- Folder and file creation.

History is preserved during the active session. When undoing copies, KIO warns if files were modified after the operation.

### Controls
- **Undo:** `Ctrl+Z`
- **Redo:** `Ctrl+Y` or `Ctrl+Shift+Z`

Toolbar buttons activate dynamically based on operation availability, displaying descriptions in tooltips. Directory panes refresh automatically after Undo/Redo.

Features from 0.15.4 — compact operations popup, dynamic height, task cancellation, version number display, and NTFS/fuseblk fixes — remain active.

---

## Requirements

- Linux with KDE Plasma 6
- Qt 6 (6.5+)
- KDE Frameworks 6 (6.17+ required for native `FileUndoManager::redo()`)
- CMake and Extra CMake Modules (ECM)
- LibArchive, ZLIB, TagLib, Exiv2
- libacl development headers and pkg-config (Arch: `acl`, `pkgconf`; Debian/Ubuntu: `libacl1-dev`, `pkg-config`)

---

## Installation

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.40.0.tar.gz
cd kio-thispc-0.40.0
chmod +x install.sh
./install.sh
```

Run:
```bash
thispc-view
```

---

## Uninstallation

`./uninstall.sh` requires the original `build/install_manifest.txt`. Keep the installed build directory: without its manifest the script refuses to remove files.

---

## Hotfix History

- **0.16.0.1:** Fixed rename Undo by registering `KIO::moveAs()` through `FileUndoManager::recordCopyJob()`, allowing Undo to restore the previous name accurately.
- **0.19.0.4:** Fixed icon view filename layout (rigid grid, `QTextLayout`, viewport callout).

---

## License

ThisPC is licensed under the MIT License.
Copyright (c) 2026 Sebastian Harasim.
See `LICENSE` for details.
