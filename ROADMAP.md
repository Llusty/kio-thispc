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

### 0.36.0 — Network & Remote Locations ✅ completed, released (v0.36.0)
- SMB, SFTP, FTP, and WebDAV via KIO where appropriate;
- Saved remote locations;
- Recent locations and reconnect actions where useful;
- KWallet integration for credentials where appropriate;
- Discovery only where reliable; never required for manual connections.
- Native KIO/KWallet owns credentials; ThisPC persists no passwords.
- Manual acceptance Stages 1–5: PASS; live discovered SMB server path: NOT TESTED because no discoverable LAN services were available.
- Final automated regression: 39 suites / 9075 assertions PASS; CLI `thispc-view 0.36.0`, exit code 0: PASS.

### 0.37.0 — Properties ✅ completed, released (v0.37.0)
- Stage 1: Properties Data Model + General Info ✅ completed
- Stage 2: Drive / Filesystem Properties ✅ completed
- Stage 3: POSIX Permissions + ACL ✅ completed
- Stage 4: SHA-256 Checksums ✅ completed
- Stage 5: Metadata via KDE / KFileMetaData ✅ completed
- Stage 6: Final Properties UX / Capability Polish (modeless File/Folder/Drive Properties) ✅ completed
- Manual acceptance Stages 1–6: FULL MANUAL PASS.
- Final automated regression: 46 suites / 9690 assertions PASS; CLI `thispc-view 0.37.0`, exit code 0: PASS.

### 0.38.0 — Storage Tools ✅ completed, released (v0.38.0)
- Largest directories and files;
- Background disk scanning;
- Top-N space usage views;
- SHA-256 / SHA-1 / MD5 hashing utilities;
- Duplicate discovery;
- Safe review before file removal or moves;
- Interactive Treemap ("Mapa zajętości") with squarified layout, canonical physical ownership, and drill-down;
- Modeless dialog shutdown and natural event loop lifetime fix.
- Stage 1: Storage Scan Core ✅ completed
- Stage 2: Largest Files & Directories / Top-N ✅ completed
- Stage 3: Hash Utilities ✅ completed
- Stage 4: Duplicate Finder ✅ completed
- Stage 5: Safe Review & Actions ✅ completed
- Stage 6: Treemap + Final Storage Tools Polish & Lifetime Fix ✅ completed
- Manual acceptance Stages 1–6: FULL MANUAL PASS.
- Final automated regression: 52 suites PASS; build, CLI `thispc-view 0.38.0`, exit code 0: PASS.

**Global Principles & Safety Rules:**
- **Local Only:** Recursive storage scan and duplicate finder are strictly local-only in 0.38. Remote KIO URLs (SFTP, SMB, FTP, WebDAV) are explicitly out of scope for recursive scanning.
- **No Symlink Follow:** By default, do not follow symlinks recursively. A symlink is counted as its own directory entry, but its target is never traversed recursively (prevents loops and escaping the scan root).
- **Hardlink Aware:** Hardlinks are tracked by stable local identity (`st_dev` + `st_ino`). Physical files are not multiply counted towards physical storage usage, and hardlinks sharing the same inode are never treated as duplicate-content candidates.
- **Mount / Filesystem Boundaries:** By default, the scan does not traverse into other mounted filesystems under the scan root. Boundary enforcement relies on actual filesystem identity (`st_dev`), not path heuristics. Future opt-in ("include other filesystems") remains possible without being required in Stage 1 UI.
- **Logical vs. Allocated Size:** Distinguish between logical file size and allocated size on disk (block allocation) where supported by the filesystem. Sparse files are represented accurately without assuming logical equals allocated space.
- **Hidden Files:** Hidden files and directories are included by default in space usage totals. Filtering them from results views must not alter calculated storage totals.
- **Errors & Diagnostics:** Permission denied, vanished files, unreadable entries, and transient stat failures are normal scan outcomes; aggregate them into status counters (skipped, inaccessible, disappeared, errors) rather than showing per-item error popups.
- **Read-Only First & Zero Automatic Deletion:** Stages 1–4 are strictly read-only analysis. File mutations occur only in Stage 5 after explicit user review. Automated "delete all duplicates except one" without conscious manual selection is strictly prohibited.

**Stage 1 — Storage Scan Core:**
- Asynchronous recursive local scanning engine keeping the UI fully responsive;
- Local filesystem paths only; remote KIO locations (SFTP, SMB, FTP, WebDAV) are excluded;
- Progress reporting, Cancel support, scanned item counters, and processed byte counters where meaningful;
- Symlink safety: count entry without traversing targets; no symlink loops or escaping scan root;
- Hardlink accounting: stable identity via `st_dev` + `st_ino` avoids double-counting physical usage;
- Filesystem boundary control: stay on scan root filesystem (`st_dev`) by default;
- Size metrics: logical size alongside allocated/disk usage (sparse file awareness);
- Hidden files/directories counted in storage totals by default;
- Aggregated error reporting (skipped, inaccessible, disappeared, errors) without disruptive popups;
- Strictly read-only; no file mutations.

**Stage 2 — Largest Files & Directories / Top-N:**
- Largest files and largest directories views powered by Stage 1 scan data (no duplicate scanning engine);
- Top-N ranking, interactive column sorting, and drill-down into directories;
- "Show in folder" and navigation to selected item location;
- Logical vs. allocated size presentation where appropriate;
- Strict data consistency with Storage Scan Core results.

**Stage 3 — Hash Utilities:**
- Supported algorithms: SHA-256 (default), SHA-1 (compatibility / integrity check), MD5 (compatibility / integrity check);
- SHA-1 and MD5 clearly presented for integrity/compatibility only, never as secure cryptographic algorithms;
- Asynchronous hashing of local regular files with progress, cancellation, and copy result to clipboard;
- Reuse established safe patterns from the 0.37 Properties checksum implementation where architectural design permits, avoiding unnecessary code duplication.

**Stage 4 — Duplicate Finder:**
- Multi-step detection pipeline:
  1. Local regular files only;
  2. Size grouping (partition candidates by exact byte size);
  3. Hash computation only for groups with identical sizes (no disk I/O on unique files);
  4. Content hash comparison confirms duplicate candidates.
- File name, extension, and timestamps are never treated as proof of duplicate content;
- Hardlink deduplication: hardlinks sharing an inode are recognized and not presented as duplicate-content files;
- Minimal I/O footprint via collision-only hashing.

**Stage 5 — Safe Review & Actions:**
- Mandatory interactive review before any mutation: user explicitly selects items; no automatic "delete all except one";
- Safe actions: Trash, Move, and Show in folder;
- Reuse existing safe ThisPC infrastructure: `FileActions`, `OperationManager`, Undo/Redo, and verified transfer/trash paths (no raw direct `unlink()`);
- Preflight revalidation of target items to prevent acting on vanished or replaced files.

**Stage 6 — Final Storage Tools UX & Regression:**
- Comprehensive UI polish, visual consistency, and Primary/Split pane parity where feature is available;
- Polished progress and Cancel responsiveness;
- Aggregated error-summary display;
- Large-directory performance and memory usage validation;
- Full automated regression suite and manual KDE/CachyOS acceptance testing;
- Release documentation and version bump only after full test PASS;
- **Treemap ("Mapa zajętości"):** Pure in-memory Squarified Treemap visualization with canonical physical hardlink ownership, logical extent metric switching, sub-tree drill-down, hover tooltips, and keyboard navigation.
- **Modeless Lifecycle & Shutdown Fix:** Deterministic cascading dialog close and `done(r)` -> `close()` wiring ensuring proper `WA_DeleteOnClose` and natural Qt event loop termination.


### 0.39.0 — Native File Integration & Rich Previews ✅ completed, released (v0.39.0)

**Stage 1 — Local File Launch / KIO Integration** ✅ completed (automated & manual pass)
- Standard KDE Open/Execute workflow via `KIO::OpenUrlJob` (`setShowOpenOrExecuteDialog(true)`, `setRunExecutables(false)`);
- Local `.exe` and binary files respect desktop MIME associations (e.g. Wine/Bottles) without `ERR_ACCESS_DENIED` blocks;
- Correct local `file://` URL semantics and directory listing redirection (`thispc:/` -> local mount path);
- Pure deterministic `LaunchUrlResolver` enforcing exact `drive.id` matching, lexical traversal rejection, and remote scheme preservation;
- Primary pane, Split View pane, and Search results parity;
- Safe remote executable policy preserved; no hardcoded Wine launcher, no `QProcess` or shell bypass, no security compromise.

**Stage 2 — Preview Core / Thumbnail Architecture** ✅ completed (automated & manual pass):
- Dedicated preview and thumbnail coordinator module (`src/previewcontroller.h` / `src/previewcontroller.cpp` or `src/thumbnailcontroller.h` / `.cpp`);
- Asynchronous scheduling using KDE's native preview stack (`KIO::PreviewJob` / thumbnail creators);
- Visible-item prioritization and per-pane tracking;
- Immediate cancellation and generation tokens on directory changes;
- In-memory thumbnail caching integrating with standard KDE thumbnail storage;
- Safe window close and multi-window lifecycle handling.

**Stage 3 — Image & Video Thumbnails** ✅ completed (automated & manual pass):
- Image previews: PNG, JPEG, WebP, and common formats supported by system thumbnailers;
- Video thumbnail frames via KDE preview infrastructure (`ffmpegthumbs` / system thumbnailers);
- Device pixel ratio and icon size awareness without freezing the UI thread;
- Fallback to standard MIME icon when thumbnail unavailable.

**Stage 4 — Executable / File Icon Previews** ✅ completed (automated & manual pass):
- Native icon extraction for Windows `.exe` and PE binaries matching Dolphin's presentation;
- Reuse KDE thumbnailer/plugin infrastructure where available;
- Clean fallback chain: successful preview -> native/system icon -> standard MIME icon.

**Stage 5 — Folder Content Previews** ✅ completed (automated & manual pass):
- Miniature previews of folder contents (e.g. image folders) matching Dolphin's presentation;
- Standard KDE directory thumbnailing integration without ad-hoc disk scans;
- Fallback to standard folder icon when preview is unavailable or empty.

**Stage 6 — View Integration & UX** ✅ completed (automated & manual pass):
- Unified preview presentation across Primary pane, Split pane, and Search results;
- Supported across view modes: Icons, List, Details, and Compact;
- View toggle: `View -> Show Previews` (Polish: `Widok -> Pokaż podglądy`);
- Consistency with existing per-folder View Settings and persistence model.

**Stage 7 — Performance / Lifetime / Final Polish** ✅ completed (automated & manual pass):
- Zero GUI thread freezing, zero blocking file I/O;
- Bound memory footprint; avoid loading all directory items at once;
- Local filesystem previews enabled; remote locations (`sftp:`, `smb:`, `fish:`) default to disabled;
- Thorough automated regression suite and manual KDE/Plasma 6 acceptance testing.


### 0.40.0 — Advanced Properties Follow-up
- Extended attributes (`xattr` / inspect & edit where supported);
- Numeric POSIX permission editing (`0755`) synchronized with existing UI;
- Hidden-state semantics with backend awareness (leading-dot rename for Unix vs. real metadata/attribute flags).

### 0.41.0 — Advanced Search
- Type, filename, extension, date, and size filters;
- Files-only and folders-only toggles;
- Saved search queries;
- Baloo acceleration when available, falling back to current worker search;
- Quick in-place folder filter if semantics remain coherent with search.

### 0.42.0 — Administrator Fallback for Failed Operations
When standard operations receive permission denied errors, provide targeted `admin://` retry prompts instead of requiring entire windows to run elevated.

### 0.43.0 — Transfer Queue & Control
- Serial vs. parallel transfer modes;
- Concurrency limit configuration;
- Operation priority and reordering;
- Queue-wide pause, resume, and cancellation;
- Optional bandwidth throttling where supported by the underlying backend.

### 0.44.0 — Notifications + Operation History
- Plasma desktop notifications for completed background tasks;
- Recent operation log with source, destination, and result status;
- Safe retry for eligible failed operations;
- "Show in folder" action upon task completion.

### 0.45.0 — Extensions / Service Actions
- Leverage KDE Service Actions and clean extension contracts instead of building a redundant bespoke plugin ecosystem;
- Support adding custom context actions and integrations without editing core window sources.

### 0.46.0 — Advanced Split Sync
- Optional recursive comparison and synchronization;
- Dry-run and plan preview before execution;
- Ignore pattern support;
- Explicit conflict policies;
- Default remains zero-delete;
- Mirror/delete only as an explicit, high-risk mode with distinct confirmation;
- Preserve safety models, preflight checks, and revalidation established in 0.31.

### 0.47.0 — Image Printing / Print Pictures Workflow
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
- Optional action and event sound effects candidate: unobtrusive audio cues for long-running jobs, errors, or operation finishes (strictly non-committal future candidate).
- System integration candidate: allow ThisPC to become the default handler/file manager for inode/directory, using standard XDG/KDE mechanisms; detect the current handler and provide a safe way to restore the previous one.
- Storage visualization candidate: interactive Treemap space usage view (deferred from 0.38 core scope as a non-blocking future candidate).

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
