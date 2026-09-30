[English](ROADMAP.md) | [Polski](ROADMAP.pl.md)

# Roadmap

This ordering prioritizes core file-manager correctness, safety, and maintainability before advanced convenience features.

---

## Completed Baseline

- **0.16.0.1 — Undo / Redo** ✅ user-confirmed
  - Copy, move, rename, Trash, new folder, and new file where supported by KIO;
  - `Ctrl+Z`, `Ctrl+Y`, `Ctrl+Shift+Z`;
  - Rename hotfix using `KIO::moveAs()` + `recordCopyJob()`.
- **0.17.0.5 — Full Drag & Drop + Split View Parity** ✅ user-confirmed
  - Background, folder, and tab drops; primary ↔ split; `Ctrl`=Copy, `Shift`=Move;
  - Tab hover activation;
  - Active-pane shortcuts and context menus;
  - Cycle protection and Undo integration.
- **0.18.0 — Copy/Move Conflict Handling** ✅ user-confirmed
  - Interactive KIO/KIOWidgets conflict UI for clipboard, Drag & Drop, and Send To;
  - Overwrite / skip / rename or suggested new name;
  - Remembered decisions for multiple conflicts;
  - Source/destination size and timestamp comparison;
  - Nested conflicts handled by `KIO::CopyJob`;
  - Clean cancellation without secondary error dialogs.

---

## Completed Releases

### 0.19.0.4 — Session Restore + Full Names ✅ user-confirmed
- Restore tabs, active tab, locations, and primary navigation history;
- Restore Split View per tab, including right-pane location/view/sort state;
- Restore splitter widths;
- View menu option to reopen previous session;
- Selected icon expands to show its full wrapped name;
- Toolbar toggle `Full names` persists across launches;
- Fixed rigid IconMode grid and bounded multi-line filename layout;
- Selected full-name callout no longer shifts neighboring row geometry.

### 0.20.0 — Quick Access / Favorites / Recent ✅ user-confirmed
- Pin/unpin folders;
- Drag reordering with persistent order;
- Favorites / Quick Access in sidebar;
- Persistent recent locations;
- Optional recent files deferred.

### 0.21.0 — Architecture Refactor + Stabilization ✅ user-confirmed
- Extracted `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController`, and `FileActions`;
- Relocated shared browser helpers and types to `browsercommon.h`;
- Reduced duplication between primary and split panes;
- Reduced `src/thispcview.cpp` from ~14,178 to ~7,250 lines;
- Added and expanded regression tests for pane actions, tab Drag & Drop, Properties, search, and file actions;
- 577 automated assertions PASS;
- Full manual KDE/CachyOS acceptance passed without regressions.

### 0.22.0 — Advanced Transfer Window ✅ user-confirmed
- Standard movable/minimizable window opens automatically for active operations;
- Displays current file, source/destination summary, processed/total size, prominent percentage, current/average speed, and ETA;
- Bounded speed-over-time graph;
- Multiple simultaneous operations in a single dynamically sized window;
- Responsive expandable details and per-operation cancellation;
- Automatic close after the last active operation completes;
- Compact popup remains the operation-history view;
- Misleading KIO-based pause was rejected during manual acceptance and removed.

### 0.23.0 — Native Local Transfer Engine ✅ user-confirmed
- Chunked local copy with real pause/resume at known byte offsets;
- Cross-device move built on verified copy followed by source removal;
- Safe partial-file handling and cleanup/recovery after cancellation or failure;
- Conflict handling (overwrite/rename/skip), directories, links, permissions, and timestamps;
- Disk-full, disconnect, and I/O error handling;
- Progress integration with 0.22.0 detailed window and compact history;
- Undo/Redo integration where completed operations are safely reversible;
- Retains KIO for remote URLs and operations where appropriate;
- Full automated regression: **842/842 assertions PASS**;
- Full manual KDE/CachyOS acceptance confirmed.

### 0.24.0 — Sidebar & Split View UX ✅ user-confirmed
- Symmetrical Split View panes with aligned per-pane breadcrumbs and a shared toolbar/sidebar routed to the active pane;
- Sidebar Drag & Drop into supported Places, devices, and Quick Access destinations;
- Vertically scrollable, resizable sidebar with persistent user-selected width and long-label ellipsis/tooltips;
- Independent Search state, results, filters, progress, and jobs for both panes;
- Full `thispc:/` / This PC card presentation in both panes;
- Full automated regression: **1022/1022 assertions PASS**;
- Full Stage 1–4 manual KDE/CachyOS acceptance confirmed.

### 0.25.0 — Archives + Richer New Menu ✅ user-confirmed
- **Archives:** Native KDE/Ark-oriented integration; ZIP / 7z / tar.gz formats; Extract Here / Extract To / Add to archive; Landlock ABI 3+ process confinement.
- **New Menu:** Folder, empty file, text, Markdown; user templates; Empty Trash with confirmation dialog.
- Split View width protection for long paths.
- Stage 3 automated regression: 20 suites / 3470 assertions PASS; manual acceptance confirmed.

### 0.26.0 — Preview Pane ✅ user-confirmed
- Image preview and proportional scaling;
- Plain text, Markdown, JSON, and XML;
- Asynchronous first-page PDF preview using QtPdf with opaque background compositing;
- Audio/video metadata using TagLib;
- Image EXIF metadata using Exiv2;
- Safe archive manifest preview using libarchive without extraction;
- Directory summaries;
- `Alt+P` preview toggle;
- Full automated regression: 20 suites PASS; Preview Pane: 106 assertions PASS; Split Layout: 99 assertions PASS.

### 0.27.0 — Quick Look on Space ✅ user-confirmed
- Large temporary preview opened via Space without launching external applications;
- Space or Esc closes Quick Look; file view retains focus for seamless arrow navigation;
- Tracks active Split View pane and single selection;
- Search, address bar, inline rename, popups, and dialogs preserve standard Space key handling;
- Shared Preview Pane backend with identical formats, limits, and stale-result protection;
- Full automated regression: **22 suites / 3598 assertions PASS**; focused assertions: Quick Look 19, Preview Pane 106, pane routing 269, Split Layout 99; all 9 manual KDE/CachyOS cases confirmed.

### 0.28.0 — Batch Rename ✅ released and installed (Variant A)
- Prefix/suffix, numbering, search/replace, regex, extension and case adjustments, plan preview and diagnostics.
- Local rename chains and swaps; cycles of 3+ items execute via multi-step sequence.
- Grouped Undo/Redo in supported scenarios; asynchronous swap execution and mutation barrier between swaps.
- Overwrite protection, file identity verification, and preflight revalidation.
- Production auto-recovery v2 covers qualifying local linear Execute/Undo/Redo operations.
- Full automated regression: 23 suites / 5409 assertions PASS; focused 2234 assertions PASS (including Batch Rename 1790); user confirmed on real files.

### 0.29.0 — Intelligent New Item Naming ✅ released and installed
- Pre-dialog collision detection proposing the first available name with gap-filling numbering.
- Extension, multi-dot, hidden prefix, and Unicode preservation.
- Non-forcing operations (`no-overwrite`, `no-resume`) with Undo/Redo preservation.
- Asynchronous fail-closed listings for remote KIO URLs.
- Full automated regression: 23 suites / 5430 assertions PASS; manual acceptance confirmed.

### 0.30.0 — Grouping and Per-Folder View Settings ✅ released and accepted
- **Stage 1 (Per-Folder View Mode):** Saved Icons/List/Details per normalized URL across panes, tabs, and restored sessions.
- **Stage 1b (View Menu Organization):** Cleaned View and Show submenus, maintaining working toggles and shortcuts.
- **Stage 1c (Icon Sizes):** Working levels (96, 64, 48, 32 px) saved per folder URL with rigid grid layout.
- **Stage 1d (Compact View):** Dense one-row items with 20 px icons filling top-to-bottom columns.
- **Stage 2 (Grouping):** Group by Type, Date, and Size across view modes; non-selectable headers excluded from file actions; local midnight calendar timer; KIO size metadata without recursive scans.
- Stage verification: Stage 1b focused 15 settings + 300 pane/menu PASS, full 24 suites / 5456 assertions PASS; Stage 1c focused 23 settings + 305 pane/menu PASS, full 24 suites / 5469 assertions PASS; Stage 2 Type 24/6197 PASS, Date 24/6213 PASS, Size 190 assertions PASS; final baseline focused 778 assertions PASS, full automated regression: 24 suites / 6278 assertions PASS; manual acceptance confirmed.

### 0.31.0 — Split View Synchronization ✅ user-confirmed
- **Stage 1 (Read-Only Pane Comparison):** Compares direct child items of left and right panes; classification: Same, Only Left, Only Right, Changed; case-preserving Unicode matching; KIO metadata (size, mtime, entry type); virtual paths blocked.
- **Stage 2 (Sync Plan Preview):** Deterministic sync plan generator; classes: No Action, Copy, Update, Conflict, Unsupported; zero deletion; no directory recursion; modal plan preview dialog.
- **Stage 3 (Safe Execution):** Asynchronous execution via `LocalFileCopyJob`; only `CopyFile` and `UpdateFile` executed; zero deletion; atomic preflight before mutating each file; race-condition-free Cancel accounting; automatic re-comparison.
- Focused `split_compare` 291 assertions PASS; focused pane actions 741 assertions PASS; full automated regression: 25 suites / 6569 assertions PASS; manual Cancel acceptance on 20,000 files (3330 copied / 16670 cancelled / 0 errors, 100% disk consistency) PASS.

### 0.32.0 — Architecture Cleanup ✅ complete
- Split Sync dependencies decoupled into acyclic modules;
- Shared application widgets and stylesheet extracted; test seams hardened;
- Explicit active-pane contract (`PaneContext` / `PaneAdapter`);
- `TabController`, `SearchUiController`, and `SelectionMenuController` extracted;
- Primary pane state and thumbnail cache extracted into `PrimaryBrowserPane`;
- Final Stage 8 baseline: **26 suites / 6662 assertions PASS**.

### 0.33.0 — Architecture Cleanup Continuation ✅ user-confirmed
- Extracted `LocationPresentation`, `NavigationHistory`, `PreviewCoordinator`, `ActionStateController`, `DirectoryListingCore`, `PaneMenuController`, and `DriveHomeCoordinator`;
- Primary and Split panes share `DirectoryListingCore`;
- Sidebar context menu lifetime crash fixed;
- Inline rename activation fixed;
- Final release baseline: **31 suites / 7852 assertions PASS**; release commit `a3710e4`, tag `v0.33.0`.

### 0.34.0 — Explorer UX / View & Navigation Polish ✅ user-confirmed
- Stage 2: `--version` CLI flag, Split address/breadcrumb parity, drive card spacing;
- Stage 3: Visual dimming for hidden files and folders;
- Stage 4: Unified inline rename across F2, context menu, and slow second click;
- Stage 5: Keyboard navigation routing and stable current/focus handling on `thispc:/` cards;
- Stage 6: Per-folder view profiles with snapshot Apply/Remove to subfolders;
- Stage 7: Nine icon size steps with menu controls and `Ctrl++`/`Ctrl+-` shortcuts;
- Final Split View visual parity polish;
- Final automated regression: 32 suites / 8317 assertions PASS; CLI `thispc-view 0.34.0`, exit code 0: PASS.

### 0.35.0 — Drives & Devices ✅ feature-complete, released (v0.35.0)
- **Stage 1:** Dynamic drive discovery via KDE Solid; event-driven `SolidDeviceMonitor` with 250 ms debouncing eliminates periodic polling and "Refreshing…" flicker;
- **Stage 2:** Detection and presentation of unmounted removable volumes on `thispc:/` as "Unmounted"; on-demand mounting via `DeviceMountController` on click/Enter with auto-navigation; mounted-only sidebar;
- **Stage 3:** Full asynchronous device removal suite via `DeviceRemovalController`:
  - "Unmount" performs clean filesystem-only unmount via native QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount` (power remains on);
  - "Safely Remove" coordinates unmounting member partitions followed by `org.freedesktop.UDisks2.Drive.PowerOff`;
  - Safely Remove supports already-unmounted volumes;
  - Media ejection ("Eject") on optical drives (`canEject`);
  - Safe redirection of Primary and Split panes from unmounted/removed locations back to `thispc:/`.
- Verified behavior maintained for NTFS filesystems.
- Final automated regression: 35 suites / 8562 assertions PASS; CLI `thispc-view 0.35.0`, exit code 0: PASS.

---

## Planned Releases

### 0.36.0 — Network & Remote Locations
- SMB, SFTP, FTP, and WebDAV via KIO where appropriate;
- Saved remote locations;
- Recent locations and reconnect actions where useful;
- KWallet integration for credentials where appropriate;
- Discovery only where reliable; never required for manual connections.

### 0.37.0 — Storage Tools
- Largest directories and files;
- Background disk scanning;
- Top-N space usage views;
- Optional treemap visualization in a future stage;
- SHA-256 / SHA-1 / MD5 hashing utilities;
- Duplicate discovery by size, then content hash;
- Safe review before file removal or moves.

### 0.38.0 — Advanced Properties / ACL
- POSIX ACL support;
- Detailed owner, group, inode, filesystem, and mount information;
- Access, modification, and change timestamps (atime/mtime/ctime);
- MIME type and checksum inspection;
- EXIF and rich media metadata;
- File hidden status and backend-dependent semantics (leading dot rename for local Unix vs. metadata flags);
- Extended attributes (xattrs);
- Symlink target resolution;
- Numeric permission editing (e.g., `0755`);
- Continued verified NTFS behavior rather than blanket assumptions.

### 0.39.0 — Advanced Search
- Type, filename, extension, date, and size filters;
- Files-only and folders-only toggles;
- Saved search queries;
- Baloo acceleration when available, falling back to current worker search;
- Quick in-place folder filter if semantics remain coherent with search.

### 0.40.0 — Administrator Fallback for Failed Operations
When standard operations receive permission denied errors, provide targeted `admin://` retry prompts instead of requiring entire windows to run elevated.

### 0.41.0 — Transfer Queue & Control
- Serial vs. parallel transfer modes;
- Concurrency limit configuration;
- Operation priority and reordering;
- Queue-wide pause, resume, and cancellation;
- Optional bandwidth throttling where supported by the underlying backend.

### 0.42.0 — Notifications + Operation History
- Plasma desktop notifications for completed background tasks;
- Recent operation log with source, destination, and result status;
- Safe retry for eligible failed operations;
- "Show in folder" action upon task completion.

### 0.43.0 — Extensions / Service Actions
- Leverage KDE Service Actions and clean extension contracts instead of building a redundant bespoke plugin ecosystem;
- Support adding custom context actions and integrations without editing core window sources.

### 0.44.0 — Advanced Split Sync
- Optional recursive comparison and synchronization;
- Dry-run and plan preview before execution;
- Ignore pattern support;
- Explicit conflict policies;
- Default remains zero-delete;
- Mirror/delete only as an explicit, high-risk mode with distinct confirmation;
- Preserve safety models, preflight checks, and revalidation established in 0.31.

### 0.45.0 — Image Printing / Print Pictures Workflow
- Replace and expand the single-item `Print` action with a predictable image-printing workflow;
- Handle multiple selected image files in a single job, accepting supported formats and handling mixed selections gracefully;
- Ergonomics inspired by Windows 11 "Print Pictures" without cloning pixel-for-pixel: large current-image preview, page count, and page navigation;
- Layout presets: full page, 13×18, 10×15, and multiple photos per page; optional contact sheets as natural extension;
- Printer selection, paper size, copy count, and fit-to-frame / crop toggles; DPI and media types shown only when supported by backend/driver;
- Qt Print Support and CUPS/IPP capability discovery where available;
- Independent preview/layout engine ensuring predictable layout UX prior to system print dialogs;
- Identical Primary and Split View operation with thorough layout and pagination tests.

---

## Unassigned Backlog

### Ark → ThisPC Drag & Drop Interoperability
- Accept external Drag & Drop from Ark / KDE archive views into standard ThisPC folders for single- and multi-item selections;
- Investigate actual Ark/KIO MIME, URI, and temporary extraction contracts first; avoid assuming local `file://` URLs or promising naive copy-based implementation;
- Default to Copy semantics; offer Move only when source and backend explicitly support it safely;
- Handle conflicts and Undo/Redo only where guarantees can be genuinely upheld;
- Ensure Primary/Split and view-mode parity;
- Test parsing via synthetic `QMimeData` and real interoperability via Ark smoke tests.

### Post-0.28.0 — Batch Rename Completion & Recovery
- Connect verified isolated swap engine v2 with production controllers, workers, and gates;
- Activate swap generator and startup allowlist after black-box validation;
- Design dedicated recovery protocol for cycles of 3+ items;
- Formalize and test KIO fallback guarantees;
- Power-loss validation restricted to disposable VMs/images, never user data.

### Smaller Features
- `Ctrl+Shift+T` — Reopen closed tab;
- Optional pinned tabs;
- Copy Path / Copy Filename;
- Recent Files extending Recent Locations;
- Enhanced Trash with Original Location and Deletion Date;
- Video thumbnails via KDE/KIO thumbnail infrastructure and `ffmpegthumbs`, asynchronous with caching and remote KIO fallback;
- Status bar details (item count, selected count, selected size, free disk space).

---

## Future Ideas for Consideration

Items in this section do not represent commitments or assignments to specific versions. Re-evaluate their utility during future roadmap reviews:

- Appearance candidate: adjustable dimming level for visible hidden items (current default: 0.40). Prefer simple presets over menu sliders if implemented.

---

## Before 1.0

Stability-focused testing and hardening cycle covering:
- Large files and directories;
- Tens of thousands of filesystem entries;
- Btrfs, ext4, and NTFS filesystems;
- Local and remote KIO backends;
- Split View parity;
- Undo/Redo conflict edge cases;
- Hardware device hotplugging;
- Process crash recovery;
- Long-running session and memory soak testing;
- Final accessibility and keyboard navigation pass;
- High-DPI and multi-monitor display polish;
- Comprehensive regression tests for file-operation safety.
