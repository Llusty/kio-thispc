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
- Final Split View visual parity polish (with Split separator double-click 50/50 reset deferred outside 0.34.0 and tracked under *Future Ideas for Consideration*);
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

**Planned:** accepted work assigned to a version. Completed entries below retain their historical status and evidence.

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


### 0.40.0 — Advanced Properties Follow-up ✅ completed, released (v0.40.0)

Release verified on GitHub on 2026-10-08: [v0.40.0](https://github.com/Llusty/kio-thispc/releases/tag/v0.40.0), commit `4c87d21651819e18b88c688404ef3625ab7f3352`. Final focused 15 suites / 4359 checks; full 64 unique suites / 35064 checks / 0 failures / exit 0; version-cli 6/6, source-package build and isolated staging install/uninstall PASS. Prior FULL MANUAL PASS was not rerun. See [release verification](docs/PROPERTIES_RELEASE_040.md).

- Stage 1: Properties capability layer (backend/locality, explicit symlink policy, identity snapshot and revalidation) — automated + FULL MANUAL PASS;
  Real remote KIO manual smoke: NOT TESTED (no configured environment), non-blocking with automated zero-local-syscall coverage.
  Stage 1 automated baseline: 59 suites / 32873 checks / 0 failures; version-cli 6/6 PASS.
- Stage 2: read-only extended attributes (`xattr`) viewer — automated + FULL MANUAL PASS (60 suites / 32968 checks / 0 failures);
  Read-only Metadata section; EntryNoFollow; remote Unsupported with zero local syscalls; bounded async pool and previews.
  Limits: 64 KiB/value, 256 KiB total raw bytes, 256 entries, 1024 UTF-16 units/text preview, 64 bytes/binary preview, 3 size-race retries.
- Stage 3: safe `user.*` xattr add/edit/remove — automated + FULL MANUAL PASS;
  Stage 3 automated PASS: properties_xattr_edit 379 assertions; focused 12 suites / 2665 checks; full 61 suites / 33347 checks / 0 failures / exit 0; build and version-cli 6/6 PASS, version 0.39.0.
  Verified FD writes, strict UTF-8/text and hex, immediate operations outside Apply/Undo; no symlink or remote writes.
  Real remote KIO manual smoke: NOT TESTED, non-blocking with automated zero-write-call coverage.
- Stage 4: numeric POSIX permissions (`0755`), rwx-only with special bits preservation — automated + FULL MANUAL PASS;
  Stage 4 automated PASS: properties_posix_mode 163 assertions; focused 13 suites / 2828 checks; full 62 suites / 33510 checks / 0 failures / exit 0; build and version-cli 6/6 PASS, version 0.39.0. Manual acceptance confirmed by the user.
- Stage 5 — Hidden semantics: automated + FULL MANUAL PASS (confirmed by the user).
  Latest Stage 5 UI-polish baseline: focused 15 suites / 3811 checks; full 63 suites / 34457 checks / 0 failures / exit 0; no duplicate suites; build, version-cli 6/6 and diff check PASS, version 0.39.0. Audit: docs/PROPERTIES_HIDDEN_SEMANTICS.md.
- Stage 6: final integration and regression across Primary, Split, and Search — AUTOMATED PASS + FULL MANUAL PASS (prior acceptance confirmed by the user on 2026-10-08; not rerun manually in this audit). Historical focused: 19 suites / 4360 checks; full: 64 suites / 34666 checks / 0 failures / exit 0, no duplicate suites. Build, version-cli 6/6 and diff check PASS. Historical audit runtime: 0.39.0. Release 0.40.0 is now verified on GitHub; this Stage 6 paragraph preserves the historical audit evidence. Current audit: docs/PROPERTIES_FINAL_AUDIT_040.md. Audit: docs/PROPERTIES_STAGE6_INTEGRATION.md.

Out of scope for 0.40: SELinux label editing, `chattr`/immutable flags, Linux file capabilities, a larger ACL redesign, and `admin://` retry.

#### Official 0.40 contract — Advanced Properties Follow-up

Release intent: Advanced Properties Follow-up without expanding the project into system administration.

- **Stage 1 — Properties capability layer:** backend/filesystem recognition; capability detection; explicit symlink policy; identity/revalidation before writes. Completed: automated + FULL MANUAL PASS.
- **Stage 2 — Extended Attributes viewer:** xattr reading; namespaces; text versus binary data; value copying; no editing. Completed: automated + FULL MANUAL PASS.
- **Stage 3 — Safe xattr editing:** local user.* add/edit/remove; meaningful errors; no dangerous security.*, trusted.* or system.* editing. Completed: automated + FULL MANUAL PASS.
- **Stage 4 — Numeric POSIX permissions:** octal field; bidirectional synchronization with existing GUI; correct validation; rwx editing with existing special bits preserved. Completed: automated + FULL MANUAL PASS.
- **Stage 5 — Hidden semantics:** audit Unix, NTFS and other backends; present the true hidden state; write only where semantics are unambiguous and safe. Unsupported or ambiguous writes remain disabled/read-only; no cross-backend emulation. automated + FULL MANUAL PASS (confirmed by the user).
- **Stage 6 — Final integration & regression:** Primary/Split/Search; disappearing files; mount removal; ext4/Btrfs/NTFS; lifetime; full automated and manual acceptance. AUTOMATED PASS + FULL MANUAL PASS (user-confirmed prior acceptance); release audit: docs/PROPERTIES_FINAL_AUDIT_040.md.

Out of scope: SELinux labels; chattr/immutable; Linux file capabilities; larger ACL redesign; admin:// retry.

### 0.41.0 — Advanced Search

**Planned scope:**
- Required filename, MIME type, extension, date, and size filters; files-only / folders-only selection;
- Explicit search scope: current folder only, folder + subfolders, or a chosen location/volume; never automatically scan all drives;
- Active filters combine with AND; alternative values within one filter combine with OR (e.g. jpg/png). Inactive filters impose no restriction. Stage 1 must define name matching/case rules, MIME matching, extension normalization, date field/time zone/range boundaries, size units/range boundaries, and folder applicability before implementation;
- Sortable results and consistent Primary/Split/Search integration, preserving independent pane queries and jobs;
- Saved searches preserving query, filters, scope, and sorting; rerun explicitly and handle unavailable locations;
- Asynchronous execution, Cancel, stale-result protection after query/navigation changes, and safe handling of mount loss;
- A working independent fallback using the current search worker is required, including unindexed locations, removable volumes, and NTFS;
- Baloo is only a conditional optimization when index coverage, scope, and completeness are trustworthy; otherwise use the worker fallback. No incomplete indexed results presented as a complete search.

**Staged plan (in progress — Stages 1–3 accepted; Stage 3 fully accepted with Stage 3D.4 final polish; Stages 4–6 not started):** Every stage requires modular architecture, relevant automated tests, and manual user acceptance before being considered complete.

1. **Stage 1 — Search Query Model & Architecture (FULL PASS):** Define the query/scope/filter contract and semantics above; separate query model, execution backend, and UI coordination, reusing existing search infrastructure. Automated model/validation/backend-contract tests; manual user acceptance of semantics and architecture plan.
2. **Stage 2 — Advanced Filters (FULL PASS):** Implement required filters and AND / within-filter alternatives in the independent worker path. Automated matching, boundary, files/folders, and scope tests; manual acceptance on representative files and folders.
3. **Stage 3 — Search UI/Results Integration (FULL PASS — Stage 3A–3D.4 accepted):** Replace the less visible magnifier drop-down menu with a clear layout: compact non-expanding **Search** bar + responsive **SearchPopover (`Scope | Mode | Filters`)** (separate, visible Scope, Name Mode (`Auto / Text / Glob`), and Filters controls; distinct indication of active filters; precise "Current folder and subfolders" label; responsiveness in narrower windows and HiDPI; keyboard navigation and accessibility; identical behavior in Primary/Split; filter indicator and distinct clear button). Add scope/filter controls, sorting, progress/Cancel, and independent Primary/Split/Search state through shared UI/result modules. Automated pane routing, sorting, cancellation, stale-result, and keyboard/accessibility tests; manual acceptance in both panes and Search.
4. **Stage 4 — Saved Searches:** Add a separate persistence module for validated query/scope/sort data and explicit reruns. Automated round-trip, invalid-data, and unavailable-location tests; manual acceptance of save/load/rerun behavior.
5. **Stage 5 — Optional Baloo Integration (conditional):** Evaluate a separate backend adapter against worker semantics, index coverage/completeness, and measurable benefit. Automated backend parity and fallback tests; manual acceptance on indexed/unindexed locations, removable volumes, and NTFS. Defer this stage if safety or benefit cannot be demonstrated; it does not block worker-based 0.41.
6. **Stage 6 — Performance/Safety/Regression:** Review module boundaries, responsiveness, resource limits, cancellation, mount loss, and lifetime. Automated full regression and stress/safety tests; manual user acceptance on large folders, removable volumes, and ext4/Btrfs/NTFS before any separate release decision.

Quick in-place folder filtering, content search, and advanced AND/OR/NOT query logic remain outside 0.41; see Unassigned Backlog.

### 0.41.1 — MTP Device Detection & Folder Size in Properties

Planned follow-up release focusing on device detection and directory size metrics:

- **Stage 1 — MTP Device Detection Fix:**
  - Automatic detection of MTP devices, integration with This PC, hotplug monitoring, KIO MTP worker integration, and full Primary/Split pane compatibility.
- **Stage 2 — Folder Size & Disk Usage in Properties:**
  - Recursive folder size and disk usage calculation in Properties dialogs (focusing on Btrfs and NTFS).
  - Cumulative logical file size calculation.
  - Reported filesystem disk allocation (`st_blocks`) with clear indication of accuracy limits (particularly for Btrfs compression, deduplication, and reflinks).
  - Accurate counts of files and subdirectories.
  - Asynchronous background calculation without UI freezing.
  - Progressive progress updates and cancellation support.
  - Robust handling of symbolic links, unreadable directories, and permission errors.
  - Performance optimization for large directory trees and complete Primary/Split view independence.

### 0.42.0 — Performance & Memory Optimization ✅ user-confirmed
- MIME sorting optimization;
- Primary/Split lazy inactive models;
- Memory reduction and view virtualization footprint improvements;
- Automated and manual acceptance FULL PASS (69/69 test suites, 114,308 assertions);
- Official release published and installed.

### 0.43.0 — Administrator Fallback for Failed Operations ✅ user-confirmed
When standard operations receive permission denied errors, provide targeted `admin://` retry prompts instead of requiring entire windows to run elevated.
- **No Global Administrator Mode:** ThisPC never runs the graphical window as `root`; privilege elevation is strictly scoped to a single, explicitly confirmed operation via KDE's `admin://` KIO worker and PolicyKit (Polkit), with a single-use 60-second `AdminUserConsent` token and automatic retry-loop prevention (`AlreadyRetriedAsAdmin`).
- **Supported GUI Operations (Central Allowlist `isGuiAdminRetryAllowed`):**
  - *Copy File To Protected (`file://` → `admin://`):* Single regular local file copied into a protected directory without automatic overwrite.
  - *Copy File From Protected (`admin://` → `file://`):* Single regular local file read via `admin://` and written to an unprivileged `file://` destination so the resulting file is owned by the standard user (`uid == getuid()`) with strict `0600` (`rw-------`) permissions.
  - *Create Protected File (`admin://`):* Single file creation via `KIO::storedPut`, preserving template/initial payload (`thispcFileContents`) and structurally qualifying `KIO::ERR_CANNOT_TRUNCATE` (error 177) only when destination directory write denial (`EACCES`/`EPERM`) and absence of pre-existing targets/`.part` files are verified.
  - *Create Protected Folder (`admin://`):* Single directory creation via `KIO::mkdir`.
- **Blocked Operations & KIO Backend Limitations:**
  - *Administrative Rename & Move (`BLOCKED` in GUI):* Security audit of KDE `kio-admin` (`RenameCommand::execute()` calling `QFile::rename()` → `rename(2)` without `RENAME_NOREPLACE`) confirmed the backend cannot guarantee atomic kernel-level no-overwrite against a race after preflight validation. Consequently, administrative `Rename` and `Move` remain strictly blocked in GUI (`OperationDisallowedInGui`).
  - *Destructive, Batch & Non-Local Operations (`BLOCKED`):* Permanent deletion (`Delete`), `Trash`/`EmptyTrash`, multi-item batch operations, directory moves, symlinks/broken symlinks, hardlinks, archives, and virtual/remote schemes (`thispc:/`, `trash:/`, `mtp:/`, `smb:/`, `sftp:/`) are rejected fail-closed.
- **Granular Error Classification, Localization & Polkit Semantics:**
  - Distinguishes true permission denial (`EACCES`/`EPERM`, `ERR_ACCESS_DENIED`, `ERR_WRITE_ACCESS_DENIED`, `ERR_CANNOT_MKDIR`, `ERR_CANNOT_OPEN_FOR_READING`) from read-only filesystems (`ST_RDONLY`/`EROFS`), disk full (`ENOSPC`/`EDQUOT`), collisions (`EEXIST`), and missing paths (`ENOENT`).
  - Localized Polish error diagnostics for atomic local move failure (`Nie można przenieść pliku atomowo.`), partial destination creation failure (`Nie można utworzyć tymczasowego pliku docelowego.`), and KIO error 177 (`ERR_CANNOT_TRUNCATE`), while keeping error qualification strictly decoupled from translated strings.
  - Handles Polkit cancellation and authorization denial cleanly as `AuthorizationCancelledOrDenied` / `UserCancelled` without UI hangs or retry loops.
- **Fail-Closed Artifact Preservation (`AdminArtifactOwnershipAudit`):**
  - Automatic deletion (`::unlink`) of destination artifacts after failed or interrupted transfers is disabled because out-of-process KIO workers do not provide race-free inode provenance to the GUI client.
  - Any partial or unverified artifact (including `.part` files) is preserved on disk, audited across 9 ownership/safety checks, reported clearly to the user, and protected against concurrent access via a process-wide active target path registry.
- **Undo/Redo & Primary/Split View Integrity:**
  - Privileged operations never record misleading unprivileged entries in `UndoController` or `LocalMoveHistory`, while preserving existing normal Undo/Redo history.
  - `FileOperationContext` snapshots remain immutable across Primary/Split pane switches, pane swaps, clipboard changes, or Split View closure, and `OperationManager` tracks progress and cancellation cleanly.
- **Staged Delivery Summary:**
  - Stage 0 — Architecture & Safety Audit ✅ FULL PASS
  - Stage 1 — Admin Retry Contract ✅ FULL PASS (67 assertions)
  - Stage 2 — Safe Copy & Create ✅ FULL PASS (43 tests / 177 assertions + manual sandbox pass)
  - Stage 3 — Rename & Move Security Gate ✅ CONDITIONAL PASS (43 tests / 71 assertions; blocked in GUI due to KIO non-atomic rename)
  - Stage 4 — GUI Administrator Retry & Artifact Security Gate ✅ FULL PASS (72 tests / 228 assertions + 12/12 manual GUI acceptance pass)
  - Stage 5 — Final Integration, Safety Regression & Release Readiness ✅ FULL PASS (17/17 automated scenarios / 161 assertions + 3/3 manual GUI spot-check; full regression 74/74 suites PASS)

### 0.44.0 — Transfer Queue & Control
- Serial vs. parallel transfer modes;
- Concurrency limit configuration;
- Operation priority and reordering;
- Queue-wide pause, resume, and cancellation;
- Optional bandwidth throttling where supported by the underlying backend.

### 0.45.0 — Notifications + Operation History
- Plasma desktop notifications for completed background tasks;
- Recent operation log with source, destination, and result status;
- Safe retry for eligible failed operations;
- "Show in folder" action upon task completion.

### 0.46.0 — Extensions / Service Actions
- Leverage KDE Service Actions and clean extension contracts instead of building a redundant bespoke plugin ecosystem;
- Support adding custom context actions and integrations without editing core window sources.

### 0.47.0 — Advanced Split Sync
- Optional recursive comparison and synchronization;
- Dry-run and plan preview before execution;
- Ignore pattern support;
- Explicit conflict policies;
- Default remains zero-delete;
- Mirror/delete only as an explicit, high-risk mode with distinct confirmation;
- Preserve safety models, preflight checks, and revalidation established in 0.31.

### 0.48.0 — Image Printing / Print Pictures Workflow
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

Valuable features to consider, with no assigned version or delivery date.

### Search Follow-up — No Assigned Version
- Quick in-place filtering of the current folder: a valuable separate mechanism, not recursive search; define its own view/filter semantics.
- Search by file content: a separate capability requiring format/backend, performance, and safety review.
- Advanced query logic AND/OR/NOT: separate from the simple AND between active filters and alternatives within one filter required in 0.41.

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
- Video thumbnail extensions only: the KDE/KIO + `ffmpegthumbs` baseline, asynchronous scheduling, caching, and fallback already shipped in 0.39; any further format/backend or remote KIO refinements remain unassigned candidates;
- Status bar details (item count, selected count, selected size, free disk space).

---

## Future Ideas for Consideration

Loose, non-binding candidates (**OPTIONAL / UNASSIGNED / NOT COMMITTED**) that may never be implemented. Items in this section do not represent commitments or assignments to specific versions (and are not assigned to 0.44–0.48 or any new release). Re-evaluate their utility during future roadmap reviews:

- Appearance candidate: adjustable dimming level for visible hidden items (current default: 0.40). Prefer simple presets over menu sliders if implemented.
- Optional action and event sound effects candidate: unobtrusive audio cues for long-running jobs, errors, or operation finishes (strictly non-committal future candidate).
- System integration candidate: allow ThisPC to become the default handler/file manager for inode/directory, using standard XDG/KDE mechanisms; detect the current handler and provide a safe way to restore the previous one.
- Storage visualization extensions only: the interactive Treemap baseline shipped in 0.38; further visualization refinements remain non-binding candidates.
- A custom search index only if Baloo and the independent worker fallback prove insufficient; no implementation commitment or assigned version.
- **Classic Menu Bar & Command Organization (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Optional classic menu bar: `File | Edit | View | Tools | Help` (`Plik | Edycja | Widok | Narzędzia | Pomoc`).
  - Example organization:
    - *File:* create, open, properties, close.
    - *Edit:* Undo/Redo, Cut/Copy/Paste, rename, delete.
    - *View:* view modes, sorting, grouping, Split View.
    - *Tools:* search, comparison, future tools.
    - *Help:* about application, documentation, bug reporting.
  - Preserve access to frequently used actions on the toolbar; never replace the existing UI without a prior UX design and explicit user approval.
- **Customizable Toolbar (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Optional selection of visible toolbar buttons and reordering of buttons without removing any underlying functionality from the application.
  - Persistent settings storage and an option to restore the default toolbar layout.
- **Collapsible Sidebar (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Dedicated collapse/expand toggle button for the left sidebar, ideally aligned with the Back/Forward navigation buttons.
  - Preserve sidebar width and visibility state across application restarts; optional keyboard shortcut; no automatic/unsolicited auto-collapsing.
- **Shortcuts in Context Menus (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Display the actual assigned keyboard shortcuts next to context menu actions.
  - Shortcut labels must reflect the real active bindings, including after user customization in settings.
  - Option to show or hide shortcut labels in context menus.
- **Central ThisPC Settings Dialog (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Single, cohesive settings window consolidating features that require configuration.
  - Settings category organization to be defined in a dedicated future UX review; never add placeholder, artificial, or non-functional options prematurely.
- **Context Menu Customization (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Ability to hide selected context menu entries (e.g. "Open in Dolphin" / „Otwórz w Dolphinie”).
  - Separate per-context configuration for: files, folders, drives, and empty folder background.
  - Hiding a menu entry must never disable the underlying feature itself or its keyboard shortcut.
  - Persistent settings storage and ability to restore default menu items.
  - Distinct from the 0.46 KDE Service Actions support — those address external/custom action extensions, whereas this candidate governs visibility of context menu entries.
- **Configurable Keyboard Shortcuts (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Ability to reassign or clear the keyboard shortcut for a given action.
  - Requires conflict detection and resolution, command search/filtering, persistent storage, restoring individual or all shortcuts to defaults, and live synchronization of displayed shortcut labels across the UI.
- **Configurable Recent Locations Count in Sidebar (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - User-configurable limit for the number of visible Recent Locations entries in the sidebar, preferably via a numeric `QSpinBox`.
  - Minimum: `0`; maximum: to be determined during design.
  - The limit applies strictly to visible sidebar items: it must not delete stored location history and must not introduce duplicate entries.
  - Separate proposal from the existing `Recent Files` backlog candidate.
- **Split View — Separator Double-Click 50/50 Reset (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Double-clicking the splitter handle between panes with the left mouse button immediately restores an equal 50/50 pane ratio (moved from the historical 0.34 polish note into active non-binding candidates).
  - Requires no `Ctrl`, `Alt`, or `Shift` modifier keys; do not add modifier combinations without a concrete, distinct purpose.
  - Preserve standard splitter dragging and independent Primary/Split pane operation.
- **ThisPC Phone Integration — KDE Connect (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Optional future phone integration:
    - Wireless browsing of phone storage;
    - File copying between phone and computer (phone ↔ PC);
    - Dedicated interface for SMS/MMS and notifications;
    - Potential Bluetooth integration.
  - Requires a prior technical audit of KDE Connect capabilities and available APIs; never promise features unsupported by a specific phone platform or backend.

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
