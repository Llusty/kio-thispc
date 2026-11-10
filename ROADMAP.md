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
- **0.18.0 — Copy/Move Conflict Handling** ✅ completed, released (`v0.18.0-stable`)
  - Interactive KIO/KIOWidgets conflict UI for clipboard, Drag & Drop, and Send To;
  - Overwrite / skip / rename or suggested new name;
  - Remembered decisions for multiple conflicts;
  - Source/destination size and timestamp comparison;
  - Nested conflicts handled by `KIO::CopyJob`;
  - Clean cancellation without secondary error dialogs.

---

## Completed Releases

### 0.19.0.4 — Session Restore + Full Names ✅ completed, released (v0.19.0.4)
- Restore tabs, active tab, locations, and primary navigation history;
- Restore Split View per tab, including right-pane location/view/sort state;
- Restore splitter widths;
- View menu option to reopen previous session;
- Selected icon expands to show its full wrapped name;
- Toolbar toggle `Full names` persists across launches;
- Fixed rigid IconMode grid and bounded multi-line filename layout;
- Selected full-name callout no longer shifts neighboring row geometry.

### 0.20.0 — Quick Access / Favorites / Recent ✅ completed, released (v0.20.0)
- Pin/unpin folders;
- Drag reordering with persistent order;
- Favorites / Quick Access in sidebar;
- Persistent recent locations;
- Optional recent files deferred.

### 0.21.0 — Architecture Refactor + Stabilization ✅ completed, released (v0.21.0)
- Extracted `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController`, and `FileActions`;
- Relocated shared browser helpers and types to `browsercommon.h`;
- Reduced duplication between primary and split panes;
- Reduced `src/thispcview.cpp` from ~14,178 to ~7,250 lines;
- Added and expanded regression tests for pane actions, tab Drag & Drop, Properties, search, and file actions;
- 577 automated assertions PASS;
- Full manual KDE/CachyOS acceptance passed without regressions.

### 0.22.0 — Advanced Transfer Window ✅ completed, released (v0.22.0)
- Standard movable/minimizable window opens automatically for active operations;
- Displays current file, source/destination summary, processed/total size, prominent percentage, current/average speed, and ETA;
- Bounded speed-over-time graph;
- Multiple simultaneous operations in a single dynamically sized window;
- Responsive expandable details and per-operation cancellation;
- Automatic close after the last active operation completes;
- Compact popup remains the operation-history view;
- Misleading KIO-based pause was rejected during manual acceptance and removed.

### 0.23.0 — Native Local Transfer Engine ✅ completed, released (v0.23.0)
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

### 0.24.0 — Sidebar & Split View UX ✅ completed, released (v0.24.0)
- Symmetrical Split View panes with aligned per-pane breadcrumbs and a shared toolbar/sidebar routed to the active pane;
- Sidebar Drag & Drop into supported Places, devices, and Quick Access destinations;
- Vertically scrollable, resizable sidebar with persistent user-selected width and long-label ellipsis/tooltips;
- Independent Search state, results, filters, progress, and jobs for both panes;
- Full `thispc:/` / This PC card presentation in both panes;
- Full automated regression: **1022/1022 assertions PASS**;
- Full Stage 1–4 manual KDE/CachyOS acceptance confirmed.

### 0.25.0 — Archives + Richer New Menu ✅ completed, released (v0.25.0)
- **Archives:** Native KDE/Ark-oriented integration; ZIP / 7z / tar.gz formats; Extract Here / Extract To / Add to archive; Landlock ABI 3+ process confinement.
- **New Menu:** Folder, empty file, text, Markdown; user templates; Empty Trash with confirmation dialog.
- Split View width protection for long paths.
- Stage 3 automated regression: 20 suites / 3470 assertions PASS; manual acceptance confirmed.

### 0.26.0 — Preview Pane ✅ completed, released (v0.26.0)
- Image preview and proportional scaling;
- Plain text, Markdown, JSON, and XML;
- Asynchronous first-page PDF preview using QtPdf with opaque background compositing;
- Audio/video metadata using TagLib;
- Image EXIF metadata using Exiv2;
- Safe archive manifest preview using libarchive without extraction;
- Directory summaries;
- `Alt+P` preview toggle;
- Full automated regression: 20 suites PASS; Preview Pane: 106 assertions PASS; Split Layout: 99 assertions PASS.

### 0.27.0 — Quick Look on Space ✅ completed, released (v0.27.0)
- Large temporary preview opened via Space without launching external applications;
- Space or Esc closes Quick Look; file view retains focus for seamless arrow navigation;
- Tracks active Split View pane and single selection;
- Search, address bar, inline rename, popups, and dialogs preserve standard Space key handling;
- Shared Preview Pane backend with identical formats, limits, and stale-result protection;
- Full automated regression: **22 suites / 3598 assertions PASS**; focused assertions: Quick Look 19, Preview Pane 106, pane routing 269, Split Layout 99; all 9 manual KDE/CachyOS cases confirmed.

### 0.28.0 — Batch Rename ✅ completed, released (v0.28.0, Variant A)
- Prefix/suffix, numbering, search/replace, regex, extension and case adjustments, plan preview and diagnostics.
- Local rename chains and swaps; cycles of 3+ items execute via multi-step sequence.
- Grouped Undo/Redo in supported scenarios; asynchronous swap execution and mutation barrier between swaps.
- Overwrite protection, file identity verification, and preflight revalidation.
- Production auto-recovery v2 covers qualifying local linear Execute/Undo/Redo operations.
- Full automated regression: 23 suites / 5409 assertions PASS; focused 2234 assertions PASS (including Batch Rename 1790); user confirmed on real files.

### 0.29.0 — Intelligent New Item Naming ✅ completed, released (v0.29.0)
- Pre-dialog collision detection proposing the first available name with gap-filling numbering.
- Extension, multi-dot, hidden prefix, and Unicode preservation.
- Non-forcing operations (`no-overwrite`, `no-resume`) with Undo/Redo preservation.
- Asynchronous fail-closed listings for remote KIO URLs.
- Full automated regression: 23 suites / 5430 assertions PASS; manual acceptance confirmed.

### 0.30.0 — Grouping and Per-Folder View Settings ✅ completed, released (v0.30.0)
- **Stage 1 (Per-Folder View Mode):** Saved Icons/List/Details per normalized URL across panes, tabs, and restored sessions.
- **Stage 1b (View Menu Organization):** Cleaned View and Show submenus, maintaining working toggles and shortcuts.
- **Stage 1c (Icon Sizes):** Working levels (96, 64, 48, 32 px) saved per folder URL with rigid grid layout.
- **Stage 1d (Compact View):** Dense one-row items with 20 px icons filling top-to-bottom columns.
- **Stage 2 (Grouping):** Group by Type, Date, and Size across view modes; non-selectable headers excluded from file actions; local midnight calendar timer; KIO size metadata without recursive scans.
- Stage verification: Stage 1b focused 15 settings + 300 pane/menu PASS, full 24 suites / 5456 assertions PASS; Stage 1c focused 23 settings + 305 pane/menu PASS, full 24 suites / 5469 assertions PASS; Stage 2 Type 24/6197 PASS, Date 24/6213 PASS, Size 190 assertions PASS; final baseline focused 778 assertions PASS, full automated regression: 24 suites / 6278 assertions PASS; manual acceptance confirmed.

### 0.31.0 — Split View Synchronization ✅ completed, released (v0.31.0)
- **Stage 1 (Read-Only Pane Comparison):** Compares direct child items of left and right panes; classification: Same, Only Left, Only Right, Changed; case-preserving Unicode matching; KIO metadata (size, mtime, entry type); virtual paths blocked.
- **Stage 2 (Sync Plan Preview):** Deterministic sync plan generator; classes: No Action, Copy, Update, Conflict, Unsupported; zero deletion; no directory recursion; modal plan preview dialog.
- **Stage 3 (Safe Execution):** Asynchronous execution via `LocalFileCopyJob`; only `CopyFile` and `UpdateFile` executed; zero deletion; atomic preflight before mutating each file; race-condition-free Cancel accounting; automatic re-comparison.
- Focused `split_compare` 291 assertions PASS; focused pane actions 741 assertions PASS; full automated regression: 25 suites / 6569 assertions PASS; manual Cancel acceptance on 20,000 files (3330 copied / 16670 cancelled / 0 errors, 100% disk consistency) PASS.

### 0.32.0 — Architecture Cleanup ✅ completed, released (v0.32.0)
- Split Sync dependencies decoupled into acyclic modules;
- Shared application widgets and stylesheet extracted; test seams hardened;
- Explicit active-pane contract (`PaneContext` / `PaneAdapter`);
- `TabController`, `SearchUiController`, and `SelectionMenuController` extracted;
- Primary pane state and thumbnail cache extracted into `PrimaryBrowserPane`;
- Final Stage 8 baseline: **26 suites / 6662 assertions PASS**.

### 0.33.0 — Architecture Cleanup Continuation ✅ completed, released (v0.33.0)
- Extracted `LocationPresentation`, `NavigationHistory`, `PreviewCoordinator`, `ActionStateController`, `DirectoryListingCore`, `PaneMenuController`, and `DriveHomeCoordinator`;
- Primary and Split panes share `DirectoryListingCore`;
- Sidebar context menu lifetime crash fixed;
- Inline rename activation fixed;
- Final release baseline: **31 suites / 7852 assertions PASS**; release commit `a3710e4`, tag `v0.33.0`.

### 0.34.0 — Explorer UX / View & Navigation Polish ✅ completed, released (v0.34.0)
- Stage 2: `--version` CLI flag, Split address/breadcrumb parity, drive card spacing;
- Stage 3: Visual dimming for hidden files and folders;
- Stage 4: Unified inline rename across F2, context menu, and slow second click;
- Stage 5: Keyboard navigation routing and stable current/focus handling on `thispc:/` cards;
- Stage 6: Per-folder view profiles with snapshot Apply/Remove to subfolders;
- Stage 7: Nine icon size steps with menu controls and `Ctrl++`/`Ctrl+-` shortcuts;
- Final Split View visual parity polish (with Split separator double-click 50/50 reset deferred outside 0.34.0 and tracked under *Future Ideas for Consideration*);
- Final automated regression: 32 suites / 8317 assertions PASS; CLI `thispc-view 0.34.0`, exit code 0: PASS; release commit `2555547`, tag `v0.34.0`.

### 0.35.0 — Drives & Devices ✅ completed, released (v0.35.0)
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

### 0.40.1 — Multi-Selection Properties & Bulk Permissions ✅ completed, released (v0.40.1)
- Multi-Selection Properties dialog (`AggregatePropertiesDialog`) for multiple selected files and directories across Primary View, Split View, and Search (`Alt+Enter` / context menu), released as commit `c0e2f29` (tag `v0.40.1`);
- Aggregate General tab computing total file, folder, and symbolic link counts, cumulative logical size, and filesystem block allocation (`st_blocks`) with hardlink deduplication (`st_dev` + `st_ino`) via an asynchronous, cancellable background scanner;
- Bulk POSIX Permissions tab allowing selective tri-state modification of individual `rwx` bits across directly selected local regular files and directories without recursion, preserving untouched `rwx` bits and special bits (`setuid`, `setgid`, `sticky`) and reflecting kernel ACL mask semantics;
- Strict safety gates: mixed selections containing symbolic links (live or broken), remote KIO URLs, or unsupported filesystems remain read-only; `O_PATH | O_NOFOLLOW` descriptor pinning validates inode and mount identity before and after mutation with explicit confirmation defaulting to "No".

### 0.41.0 — Advanced Search & Responsive Search UI ✅ completed, released (v0.41.0)

**Delivered scope in 0.41.0 (tag `v0.41.0`, commit `292b8bb`):**
- Filename, category, MIME type, extension, modification date range, and size filters, plus files-only / folders-only target selection;
- Explicit search scope: current folder only, current folder + subfolders, or chosen location/volume — never automatically scanning all drives;
- Active filters combine with AND; alternative values within one filter combine with OR (e.g. `jpg`/`png`); inactive filters impose no restriction;
- Sortable results and consistent Primary/Split/Search integration, preserving independent pane queries, jobs, and `PaneSearchState` across "Swap Panes";
- Asynchronous execution on the independent search worker (including unindexed locations, removable volumes, and NTFS), Cancel, stale-result protection after query/navigation changes, and safe handling of mount loss.

**Staged delivery & deferred items:**
1. **Stage 1 — Search Query Model & Architecture (✅ FULL PASS, shipped in 0.41.0):** Defined the query/scope/filter contract and semantics (`SearchQuery`, `SearchPlan`, `SearchMatcher`); separated query model, execution backend, and UI coordination with full automated and manual acceptance.
2. **Stage 2 — Advanced Filters (✅ FULL PASS, shipped in 0.41.0):** Implemented multi-criteria filters and AND / within-filter OR logic in the independent worker path with automated and manual acceptance.
3. **Stage 3 — Search UI/Results Integration (✅ FULL PASS — Stage 3A–3D.4 accepted, shipped in 0.41.0):** Compact non-expanding 320 px **Search** bar + responsive **SearchPopover (`Scope | Mode | Filters`)** with Name Matching Modes (`Auto / Text / Glob`), active filter funnel badge, separate clear button `X`, Wayland/HiDPI responsiveness, full keyboard/accessibility navigation, and Primary/Split parity.
4. **Stage 4 — Saved Searches (⏸ deferred outside 0.41.0):** Persistence module for saved search queries/scopes/filters was not implemented in 0.41.0 and remains tracked under *Search Follow-up — No Assigned Version*.
5. **Stage 5 — Optional Baloo Integration (⏸ deferred outside 0.41.0):** Conditional Baloo adapter was deferred outside 0.41.0; release 0.41.0 shipped on the independent worker backend and tracks optional Baloo evaluation under *Search Follow-up — No Assigned Version*.
6. **Stage 6 — Performance/Safety/Regression (⏸ partially covered in Stages 1–3 regression; standalone Stage 6 cycle not executed before `v0.41.0`):** Automated regression for the shipped Stages 1–3 scope (`search_query`, `search_controller`: module boundaries, cancellation, stale-result protection, mount loss, and Primary/Split routing) and manual Stage 3A–3D.4 acceptance were completed in commit `292b8bb` (`v0.41.0`); a separate standalone Stage 6 stress and cross-filesystem (`ext4`/Btrfs/NTFS/removable) manual acceptance cycle was not executed after deferring Stages 4–5.

Quick in-place folder filtering, content search, advanced AND/OR/NOT query logic, Saved Searches, and optional Baloo integration remain in the Unassigned Backlog.

### 0.41.1 — MTP Device Detection & Folder Size in Properties ✅ completed, released (v0.41.1)

Follow-up release (tag `v0.41.1`, commit `3cd2f31`) delivering MTP device integration and asynchronous directory size metrics:

- **Stage 1 — MTP Device Detection & Hotplug UX (✅ completed):**
  - Automatic detection of MTP devices (Android smartphones, cameras) on "This PC" and the navigation sidebar, transparent redirection from raw KIO UDI paths (`mtp:udi=...`) to friendly `mtp:/<Device Name>` URLs, hotplug monitoring (`Solid` + `org.kde.kmtpd5`), disconnect banner with "Reconnect" action, and full Primary/Split pane compatibility.
- **Stage 2 — Folder Size & Disk Usage in Properties (✅ completed):**
  - Asynchronous, non-blocking recursive calculation of cumulative logical file size, reported filesystem block allocation (`st_blocks * 512`) with Btrfs/NTFS accuracy tooltips, and exact file/subfolder/symlink counts in local directory Properties dialogs, with symlink loop protection (`NoFollow`), hardlink deduplication, mount boundary isolation, and instant cancellation on dialog close.

### 0.41.2 — Automatic Directory Refresh & MTP Permanent Delete ✅ completed, released (v0.41.2)
- Automatic directory monitoring via `KDirWatch` (local filesystems) and `org.kde.KDirNotify` (KDE Plasma D-Bus) with quiet in-place refresh preserving selection, focus, scroll position, active inline rename (`EditingState`), and search filters (debounced 150 ms window / 500 ms max deadline);
- Guaranteed automatic source and destination directory refresh after internal operations (Cut/Copy/Paste, Drag & Drop, Delete, Rename, Trash, Restore) without pressing `F5`;
- Dedicated "Delete Permanently…" action (`Shift+Delete`) and Smart Delete Fallback on regular `Delete` for MTP device storage (`mtp:/`), with root and top-level storage boundary protection (normalized MTP path depth `>= 3`), released as commit `061e0a0` (tag `v0.41.2`).

### 0.42.0 — Performance & Memory Optimization ✅ completed, released (v0.42.0)
- MIME sorting optimization (Decorate-Sort-Undecorate Schwartzian transform, up to 21.2x faster on 10,000 items);
- Primary/Split lazy inactive models in `DirectoryView` (saving ~15–18 MB RSS per 10k items in single pane and ~39.4 MB RSS in dual-pane sessions);
- Memory reduction and view virtualization footprint improvements while preserving selection, focus, and scroll position across view switches;
- Automated and manual acceptance FULL PASS (69/69 test suites, 114,308 assertions);
- Official release published and installed (commit `23b1db6`, tag `v0.42.0`).

### 0.43.0 — Administrator Fallback for Failed Operations ✅ completed, released (v0.43.0)
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
  - Stage 5 — Final Integration, Safety Regression & Release Readiness ✅ FULL PASS (17/17 automated scenarios / 161 assertions + 3/3 manual GUI spot-check; full regression 74/74 suites PASS; release commit `33f7b3f`, tag `v0.43.0`)


### 0.44.0 — Transfer Queue & Control ✅ completed, released (v0.44.0)
- **Production Transfer Queue Active by Default:** Standard Copy and Move operations from Primary View, Split View, Drag & Drop, Cut/Copy/Paste, and Split View Synchronization route through `TransferQueueController` by default upon application startup (`activateTransferQueueProductionDispatchByDefault()`, with `THISPC_TRANSFER_QUEUE_PRODUCTION=0` retained as a diagnostic override);
- **Serial vs. Parallel Execution Modes:** Dynamic switching between Serial mode (1 active transfer) and Parallel mode with configurable concurrency limit (`1..8`, default `2`) and starvation protection;
- **Task Priority & Manual Reordering:** `High`, `Normal`, and `Low` priority levels and manual reordering (`Move Up`, `Move Down`, `Move to Top`, `Move to Bottom`) with deterministic FIFO tie-breaking;
- **Per-Task & Global Queue Controls:** Per-task and queue-wide Pause, Resume, and Cancel controls, honest per-backend pause capability reporting, and close-window confirmation while transfers are active or queued;
- **Optional Bandwidth Throttling:** Thread-safe shared token-bucket rate limiter (`TransferBandwidthLimiter`) in `KiB/s` or `MiB/s` for chunked local transfers (`LocalFileCopyWorker`), with explicit `No limit` indicators for non-throttlable backends;
- **Unified Operations Manager:** Consolidated single-window operations manager with summary telemetry, expandable real-time speed graph, rich task cards (bytes, speed, ETA, mini speed chart), session history of completed/cancelled/failed operations (including legacy KIO Trash/Delete/Archive jobs), **Clear History**, auto-open/auto-close (`CloseWindowOnComplete`), persistent settings (`QSettings`), responsive settings scrolling (`QScrollArea` at `480 × 290 px`), and Quick Look multimedia stop-on-close fix;
- **Staging, Identity Verification & MTP Move Release Block (`BLOCK MOVE`):** Deferred `jobFactory` instantiation, pre-dispatch preflight revalidation, `TransferReservationRegistry` path-conflict protection, `.thispc-part` staging, `LocalFileIdentity` verification, `UndoController` active-queue barriers, MTP Copy + hotplug support, and a strict release safety block (`MtpMoveVerdict::RefusedUnsupported`) refusing all Move directions involving `mtp:/` (`MTP → Local`, `Local → MTP`, `MTP → MTP`, and intra-device MTP Move) so source files are never deleted without race-free post-transfer verification.

**Core Principles & Architectural Safety Rules:**
1. **Decouple Scheduler from Presentation:** Separate transfer scheduling and queue state coordination from UI presentation in `OperationManager`.
2. **Preserve Existing Safe Engines:** Keep existing verified transfer engines (`LocalFileCopyJob`, `LocalFileMoveJob`, `KIO::CopyJob`) intact without regression.
3. **Protect Target Paths Against Concurrent Access:** Prevent concurrent operations from writing to, moving, or mutating identical or overlapping destination paths.
4. **Preflight Revalidation Prior to Execution:** Revalidate sources, destinations, free space, and permissions immediately before an operation actually transitions from queued to running.
5. **No Feigned Pause:** Never represent unsupported pause as effective pause when a backend cannot genuinely pause (e.g. KIO workers).
6. **Safe Application Shutdown:** Ensure safe application lifecycle handling when operations are running or queued without data corruption.
7. **Protect Undo/Redo and Conflict Ordering:** Ensure Undo/Redo history and conflict-resolution ordering remain deterministic and protected against concurrent execution races.
8. **Isolate Administrator Retry:** Do not enqueue delayed or deferred Administrator Retry operations into the general unprivileged transfer queue.
9. **No Persistence / Crash Recovery in 0.44:** Do not implement persistent queue storage or crash recovery across application restarts in 0.44.
10. **Scope Boundary with 0.45:** Do not pull desktop notifications, persistent history logs, or "Show in folder" from 0.45 into 0.44.
11. **Strictly Optional Bandwidth Throttling:** Keep bandwidth throttling strictly optional and restricted to backends that genuinely support chunk-level delay (`LocalFileCopyWorker`).
12. **Verified Concurrency Limits:** Default concurrency limit of 2 and configurable range of 1–8 verified across automated and manual concurrency benchmarks.

**Staged Delivery Summary:**
- **Stage 0 — Architecture & Safety Audit:** ✅ FULL PASS — Comprehensive audit of existing operation dispatchers, backend capabilities, state machines, concurrency safety, Undo/Redo, and lifecycle contracts.
- **Stage 1 — Queue Core & Scheduler:** ✅ FULL PASS — Independent queue coordination engine, job state model, safe dispatch contracts, and slot reservation without premature job creation.
- **Stage 2 — Sequential / Parallel & Limits:** ✅ FULL PASS — Serial versus parallel execution modes, configurable concurrency limits (`1..8`, default `2`), dynamic limit adjustments, and starvation protection.
- **Stage 3 — Queue UI & Settings:** ✅ FULL PASS — `OperationManager` queue visualization, queued vs. active task distinction, configuration settings, and dual-language Polish/English UX.
- **Stage 4 — Priority & Reordering:** ✅ FULL PASS — High / Normal / Low priority levels, manual reordering of waiting jobs, and FIFO determinism.
- **Stage 5 — Global Pause / Resume / Cancel:** ✅ FULL PASS — Queue-wide controls, accurate per-backend pause state, safe cascading cancellation, and application shutdown confirmation.
- **Stage 6 — Optional Bandwidth Throttling:** ✅ FULL PASS — Shared token-bucket rate limiter (`TransferBandwidthLimiter`), chunk delay pacing in `LocalFileCopyWorker`, live limit adjustments, and honest per-task capability badges.
- **Stage 7 — Integration, Unified Operations Manager, MTP Safety Gate & Final Regression:** ✅ FULL PASS — End-to-end integration across Primary/Split views, Drag & Drop, Paste, Split Sync, Unified Operations Manager (`U1–U8.1`, `UX-A..D`), `.thispc-part` staging, MTP Copy/hotplug, MTP Move safety block (`BLOCK MOVE`), ASan/UBSan, full regression (76/76 suites PASS, 2,335 `transfer_queue` assertions PASS), R3 manual acceptance (8/8 PASS), system installation verified (R6 6/6 PASS), release commits `109106e` / `d515815`, tag `v0.44.0`, and official GitHub release published.

---

## Planned Releases

**Planned:** accepted future work assigned to target versions (`0.45.0–0.48.0`), not yet implemented.

### 0.45.0 — Notifications + Operation History Extensions
- **Stage 1 — KDE Plasma Notifications:** ✅ CLOSED / IMPLEMENTED / AUTOMATED PASS / MANUAL ACCEPTED (7/7)
  - Native KDE Plasma desktop notifications via `KF6::Notifications` (`TransferNotificationManager`) for completed and failed Copy/Move transfers running in the background;
  - Accurate observer detection (`isUserObserving()`): suppressed when ThisPC window is active, dispatched when ThisPC is inactive or minimized;
  - Error notifications dispatched unconditionally regardless of focus; cancelled operations emit no notifications;
  - Batch completion coalescing window (250 ms) preventing desktop flooding during rapid small transfers;
  - Full user configuration in Transfer Queue settings (`DesktopNotificationsEnabled`) with persistent `QSettings` storage;
  - Standard `DefaultEvent` fallback and `data/org.kde.thispcview.notifyrc` specification matching desktop-entry `org.kde.thispcview`;
  - 13/13 automated test scenarios pass (74 assertions), full regression suite 78/78 PASS;
  - Manual acceptance: 7/7 PASS (A: background copy, B: active suppression, C: minimized window, D: error notification, E: cancel suppression, F: settings toggle & restart persistence, G: 6-task batch coalescing).
- **Stage 2 — Persistent / Extended Operation History:** ✅ CLOSED / IMPLEMENTED / AUTOMATED PASS / MANUAL ACCEPTED
  - **Stage 2.1 — Persistent History Model & Safe Storage:** ✅ IMPLEMENTED / AUTOMATED PASS (20/20 test suites, 1637 assertions)
    - Value-type descriptor `TransferHistoryRecord` and thread-safe, non-blocking `TransferHistoryManager`;
    - JSON serialization with schema versioning (`schemaVersion: 1`), XDG State location (`~/.local/state/kio-thispc/thispc-view/operations_history.json`);
    - FIFO ring buffer (200 records), atomic write via `QSaveFile`, multi-process lock via `QLockFile` (500 ms timeout), 0600 file permissions;
    - Security Gate A & B: generation/epoch mechanism (`historyEpoch`), symlink rejection, corrupted JSON backup preservation (`.corrupted.<ts>.<N>`), CRDT local delta synchronization.
  - **Stage 2.2 — Production Transfer History Integration:** ✅ IMPLEMENTED / AUTOMATED PASS (36/36 test suites, 1804 assertions) / MANUAL ACCEPTED (6/6)
    - Full end-to-end integration of `TransferHistoryManager` into `TransferQueueController`;
    - Temporal metadata: `enqueuedTimeUtc`, `startTimeUtc`, `endTimeUtc`, 64-bit volume accuracy (`totalBytes`, `processedBytes`), `durationMs`, average speed, KIO error codes and diagnostics;
    - Strict idempotency (`historyRecorded` guard) and fault isolation (history storage failures never abort or alter Copy/Move jobs);
    - Security Gate A verified: `clear()` in Instance A increments disk epoch, preventing delayed writes from Instance B from resurrecting wiped history;
    - Safe shutdown: `flushSync()` synchronous debounced save flush upon window close and controller destruction;
    - Manual acceptance 6/6 PASS: A (single Copy Completed), B (multi-file Copy Completed, itemCount=2), C (Copy Failed, errorCode=100 with KDE notification), D (3 records preserved across restarts), E (Move Completed, persistent record), F (Copy Cancelled, persistent record, source preserved, no partial destination or staging).
  - **Stage 2.3 — OperationWindow Persistent History & UX Polish:** ✅ IMPLEMENTED / AUTOMATED PASS / MANUAL ACCEPTED (6/6)
    - Integration of persistent history records with existing `OperationWindow` (no separate bespoke window without explicit decision);
    - Archival cards rendered from `TransferHistoryManager` with distinct `operationHistoryBadge` ("History" / "Historia");
    - Task deduplication between in-session active/completed transfers and archival database records (using `task.historyUuid`);
    - Comprehensive terminal state visualization (Completed, Cancelled, Failed with error text), timestamps, durations, and byte totals;
    - Strict UI safety: no active control buttons (Pause/Cancel/Resume) on archival cards;
    - Safe dismiss button (`detailedOperationDismissButton`) removes cards from the active session view without deleting or modifying disk records;
    - Zero blocking I/O or stat queries when rendering archival cards referencing disconnected USB/MTP storage;
    - Resolution of **UX Item A**: compact initial window geometry (~580×380 px) matching accepted screenshot layout without excessive vertical blank space, preserving manual resizing, multi-task scrolling, Wayland and HiDPI scaling, and persistent geometry restoration via `QSettings`;
    - Resolution of **UX Item B**: speed graph polish on instantaneous transfers (suppresses "Waiting for speed samples…" after rapid transfers under 500 ms; displays clear terminal status "Transfer completed instantly" / "Transfer cancelled" / "Transfer failed");
    - Automated test suite: 10 new test suites (46 total history suites, 1863 assertions PASS), full regression 79/79 PASS;
    - Manual acceptance 6/6 PASS: A (5 archival cards loaded on start), B (details, dismiss, and restart restoration), C (deduplication on copy), D (compact 580x380 px geometry and resize persistence), E (speed graph instant vs 20 MB transfer), F (safe expansion, disconnected locations, scrolling, and store integrity).
  - **Stage 2.4 — OperationPopup History Integration & UX C:** ✅ IMPLEMENTED / AUTOMATED PASS / MANUAL ACCEPTED (6/6)
    - Full integration of persistent history (`TransferHistoryManager`) with toolbar `OperationPopup`;
    - Resolution of **UX Item C**: eliminated misleading "No history" in `OperationPopup` while archival and completed operations exist;
    - Unified deduplication model (`historyUuid`) ensuring active, queued, and archival records are counted accurately without duplicates;
    - Neutral terminology: displays "Historia: N" / "History: N" across mixed terminal statuses (Completed, Failed, Cancelled) instead of mislabeling non-successful operations;
    - Accurate empty state handling: displays "Brak operacji plikowych i historii." / "No file operations or history." strictly when the history database is genuinely empty;
    - Reactive, non-blocking summary updates on `recordsChanged` and `historyManagerChanged` without disk I/O on popup open;
    - Seamless consistency between `OperationPopup` and `OperationWindow`, with quick navigation via `m_showDetailsButton`;
    - Preserved compact popup geometry and bounded height (~180–200 px in resting state);
    - Automated test suite: 8 new test suites (54 total history suites, 1897 assertions PASS), full regression 79/79 PASS;
    - Manual acceptance 6/6 PASS: A (7 archival records correctly displayed on start), B (open OperationWindow from popup with 7 matching cards), C (live update during active transfers, badge, counters, copying 2 files), D (full restart restoration of 9 records), E (clean empty history profile correctly shows no operations), F (dual-view consistency and 9-record store integrity).
  - **Stage 2.5 — History Settings, Retention Policy & Multi-Instance Final Acceptance:** ✅ CLOSED / ACCEPTED / BACKED UP / COMMITTED / PUSHED (8/8 PASS)
    - History settings integration in `TransferQueueSettingsWidget` ("Record operation history" toggle, retention limits 50/100/200/500, "Clear history…" action);
    - Safe confirmation dialog (`promptClearHistoryConfirmation`) ensuring user awareness that only history logs are removed and actual files on disk remain untouched;
    - Strict resurrection prevention: completed tasks purged from queue memory and storage cannot be re-saved or resurrected (`historyEpoch` tracking);
    - Hard size ceiling (2 MiB) with oldest-record trimming priority over record count;
    - Multi-instance concurrency synchronization with `QLockFile` and epoch tracking (Security Gate A);
    - Real-time cross-process synchronization using `QFileSystemWatcher` with 150 ms debouncing and async background reload (`QtConcurrent::run`), resilient to atomic inode renames by `QSaveFile`;
    - Counter consistency and cleanup integrity: `TransferQueueController` clears terminal session tasks from past epochs upon `historyCleared()`, eliminating zombie cards, and `OperationPopup` strictly prevents false "Saved history: 1" displays when the persistent store is empty;
    - Toolbar overflow palette fix (`applicationstyle.cpp`): eliminated washed-out background when expanding hidden buttons in dark/isolated themes (`background: palette(window)`);
    - Batch Rename multi-instance recovery audit and fix (`batchrenamerecovery.h`): per-PID instance directories (`instance-<PID>`) preventing lock contention while preserving orphaned recovery journals;
    - Full automated verification: 13 new history test suites (67 history suites / 2008 assertions PASS), `transfer_queue` (2335 checks PASS), `notifications` (13/13 PASS), `operations` (57 assertions PASS), full regression 77 suites PASS;
    - Full user manual acceptance (8/8 PASS — Nov 6, 2026):
      - Scenario A (history settings defaults and persistence): PASS;
      - Scenario B (retention limits 50/100/200/500): PASS;
      - Scenario C (disable and re-enable history recording): PASS;
      - Scenario D (clear history confirmation, cancel, data integrity): PASS;
      - Scenario E (OperationPopup, OperationWindow and persistent store consistency): PASS;
      - Scenario F (two independent processes, live sync, historyEpoch, zero resurrection): PASS;
      - Scenario G (Copy, Move and KDE Plasma notifications): PASS;
      - Scenario H (full restart, 3 records restored, data integrity, zero staging leftovers, smooth GUI): PASS.
    - All Stage 2 Persistent History capabilities are fully implemented, manually accepted, backed up and committed.
- **Stage 3 — Safe Retry for Eligible Operations:** ✅ CLOSED / ACCEPTED / BACKED UP / COMMITTED / PUSHED (A–H 8/8 + UX 6/6, commit `752b56d`)
  - Safe retry for eligible failed operations (Copy and Move) in current session;
  - Safety preflight validation: source file existence, source identity (device/inode) immutability, target directory write permissions, destination conflict detection (including `.thispc-part` staging detection);
  - Archival cards after application restart strictly keep Retry disabled with descriptive tooltip;
  - Automated test coverage: 43 Stage 3 retry contract tests (71 assertions), full regression 77/77 suites PASS, Stage 3 XDG isolation 9/9 PASS;
  - Manual Acceptance: Scenarios A–H 8/8 PASS; UX Follow-up UX.1–UX.6 6/6 PASS;
  - **Dynamic Live Retry Eligibility Monitoring:**
    - Event-driven monitoring via `QFileSystemWatcher` with 150 ms debounce for failed tasks in open `OperationWindow`;
    - Automatically updates button availability (`isEnabled()`) and tooltip on disk events (chmod, restore missing file, remove conflict/part) without reopening window;
    - Strictly bound resource caps (max 50 dirs, 100 files), zero polling, active only when window is visible;
    - Automated unit tests UX.1–UX.6 PASS (91 `operations` assertions).
- **Stage 4 — Show in Folder Action:** ✅ CLOSED / ACCEPTED / BACKED UP / COMMITTED / PUSHED (8/8 PASS)
  - Internal "Show in folder" ("Pokaż w folderze") action for `Completed` Copy and Move operations across both in-session queue cards and archival persistent history cards after restart (never launches external Dolphin or shell processes);
  - Single-item completed transfers open the destination directory in the active ThisPC pane (`Primary` or `Split`) and select + scroll to the transferred file, directory, or symlink (respecting conflict-renamed `targetUrls`);
  - Multi-item completed transfers open the destination directory with selection cleared so no arbitrary single item is selected;
  - Strict fail-closed validation (`resolveShowInFolder`): rejects `Failed`, `Cancelled`, and active tasks; rejects missing/renamed target items or missing destination directories; rejects symlinked destination directories (`lstat`), relative paths, and `..` traversal; rejects disconnected removable media, `mtp:/`, remote KIO (`smb://`, `sftp://`, `fish://`, etc.), and virtual/unsupported schemes (`trash:/`, `admin:/`, `http://`);
  - Strict pane independence between `Primary` and `Split` views: navigates and updates selection exclusively in the active pane without altering the inactive pane's location or selection;
  - Live `QFileSystemWatcher` integration in open `OperationWindow` dynamically updates button availability and tooltip when destination files/directories are removed or restored;
  - Automated verification: Scenarios A–L PASS (203 `operations` assertions), full regression 77/77 suites PASS, Stage 4 XDG isolation 9/9 PASS;
  - Manual Acceptance (8/8 PASS — Nov 8, 2026): A (Completed Copy single file), B (Completed Move single file), C (archival history after restart), D (multi-file copy with clean selection), E (live target file removal and restoration), F (live destination directory removal and restoration), G (Primary/Split View strict independence), H (H.1 Failed without Show in Folder + H.2 controlled 32 MiB Cancelled without Show in Folder or Retry, verified disk integrity, zero `.thispc-part` leftovers, and persisted `Cancelled` history record).
- **Stage 5 — Security Audit, Remediation, Polish, Packaging & Release 0.45.0:** (In Progress)
  - **Stage 5.1 — Comprehensive Security Audit:** ✅ COMPLETED (`docs/SECURITY_AUDIT_045.md`) — Independent manual and automated security audit (`GCC 16.2.1 -Wall -Wextra -Wformat=2 -Wundef`, `clang-tidy 23.1.4`, `ASan` + `UBSan`) across the entire ThisPC codebase (Areas A–G) and formal 9-vector threat model:
    - Confirmed **0 `CRITICAL`**, **0 `HIGH`**, and **0 malicious code / backdoor** findings;
    - Identified **2 `MEDIUM`** findings (`SEC-045-01`: `heap-use-after-free` in `OperationWindow::rebuild()` when an uncancelled 120 ms `scheduleRebuild()` timer overlaps synchronous `rebuild()`; `SEC-045-02`: non-overwrite publication in `LocalFileCopyWorker` and `LocalFileMoveJob` using `QFile::rename` instead of `RENAME_NOREPLACE`) and **6 `LOW`** hardening items (`SEC-045-03`..`SEC-045-08`);
    - **Security Release Gate Verdict (Stage 5.1):** **`BLOCKED`** pending Stage 5.2 remediation and regression verification.
  - **Stage 5.2 — Security Remediation & Hardening:** ✅ CLOSED / AUTOMATED PASS / MANUAL ACCEPTED (4/4) / BACKED UP / COMMITTED / PUSHED — All 8 findings (`SEC-045-01`..`SEC-045-08`) fully addressed:
    - Fixed `heap-use-after-free` in `OperationWindow` / `TransferQueueViewWidget` via dedicated timer cancellation, generation counters, detached widgets, and `deleteLater()` (`SEC-045-01`);
    - Implemented atomic `publishLocalFileNoOverwrite()` using `SYS_renameat2(..., RENAME_NOREPLACE)` in `LocalFileCopyWorker`, `LocalFileMoveJob`, and `kiostagedtransferjob.h` (`SEC-045-02`; initial transitional `link/unlink` fallback was fully superseded in Follow-up 5.2.1 by strict fail-closed refusal);
    - Hardened atomic rollback: directory moves without `RENAME_NOREPLACE` fail closed (`EINVAL`/`EOPNOTSUPP`) rather than falling back to non-atomic overwrites;
    - Fixed use-after-move in `TransferQueueController::enqueue` (`SEC-045-03`);
    - Wrapped runtime `THISPC_TEST_HARNESS` checks in `#ifdef THISPC_TEST_HARNESS` (`SEC-045-04`);
    - Hardened external process argument handling with `--` and absolute paths in `thispcview.cpp`, `appwidgets.cpp`, and `selectionmenucontroller.cpp` (`SEC-045-05`);
    - Hardened `AclController::write()` with symlink rejection, descriptor identity checks, and `BatchRenameRecoveryGate` mutation gating (`SEC-045-06`);
    - Hardened `uninstall.sh` with strict path whitelisting, directory traversal rejection, and staging root support (`SEC-045-07`);
    - Added `m_details` null check, `m_activeJob` safe cancellation copy, and thread-safe `qt_error_string()` across worker threads (`SEC-045-08`);
    - Automated verification: production build PASS, strict GCC 16.2.1 build PASS, `clang-tidy` PASS (with accepted informational `cert-err33-c` notes documented), 19/19 `ASan + UBSan` suites PASS (0 errors), 77/77 full regression suites PASS, isolated uninstall tests PASS;
    - Manual Acceptance (4/4 PASS — Nov 8, 2026): 1. `OperationWindow` stability under rapid details toggle PASS; 2. Destination name conflict resolution, intact SHA-256, zero `.thispc-part` leftovers PASS; 3. Show in Folder on hardened transfers PASS; 4. `THISPC_TEST_HARNESS=1` production isolation PASS;
    - Verified Backup: `/mnt/g/ThisPC backup/ThisPC-0.45.0-Stage5.2-ManualAccepted-2026-11-08/`;
    - **Security Follow-up 5.2.1 — Atomic Publish Rollback Race & Fail-Closed Remediation:** ✅ COMPLETED / AUTOMATED PASS — Addressed race conditions in fallback publish:
      - Fully removed non-atomic `link(2)`/`unlink(2)` fallback and TOCTOU rollback race (`SEC-045-02.1`);
      - Enforced strict fail-closed refusal when `SYS_renameat2(..., RENAME_NOREPLACE)` is unsupported (`EOPNOTSUPP`/`ENOSYS`/`EINVAL`), returning clear localized error messages;
      - Guaranteed zero foreign file deletion, intact source/destination SHA-256, and verified `.thispc-part` staging cleanup;
      - Automated verification: `local_transfer` PASS (63 assertions), `local_move` PASS (73 assertions), ASan/UBSan PASS (0 errors), LSan PASS (0 leaks on transfer suites; LSan `NOT VERIFIED` on Qt GUI widget runs with `detect_leaks=0`), production build PASS, full regression 77/77 PASS;
      - Verified Backup: `/mnt/g/ThisPC backup/ThisPC-0.45.0-Stage5.2.1-FollowUp-2026-11-08/`;
    - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** until Stage 5.3 final release verification.
  - **Stage 5.3 — Final Polish, Packaging & Release 0.45.0:**
    - **Stage 5.3.1 — Release Readiness & UX Closeout:** ✅ CLOSED / AUTOMATED PASS / MANUAL ACCEPTED (3/3) / BACKED UP / COMMITTED / PUSHED:
      - Audited and verified UX Items A–C (OperationWindow default compact geometry 580×380 px, instantaneous transfer graph terminal state handling without misleading waiting labels, toolbar popup history count consistency matching detailed window);
      - Added 14 automated assertions to `tests/operation-manager.cpp` (217 total suite assertions passing);
      - Manual Acceptance (3/3 FULL PASS — Nov 8, 2026): 1. UX A compact geometry & resize persistence PASS; 2. UX B instantaneous transfer "Transfer ukończony natychmiastowo" PASS; 3. UX C popup/window history count consistency & clear history PASS;
      - Audited release inventory and packaging script `scripts/build-rc-archives.py`: implemented dynamic version discovery from `CMakeLists.txt`, safe isolated staging via `tempfile.TemporaryDirectory`, and verified core package integrity without prematurely bumping production versions;
      - Prepared comprehensive multi-filesystem real-world test plan (Btrfs, ext4, NTFS-3G / ntfs3, FAT32/vfat, exFAT, USB storage);
      - Removed documentation contradiction regarding transitional `link/unlink` fallback in Stage 5.2 summary;
      - Verified Backup: `/mnt/g/ThisPC backup/ThisPC-0.45.0-Stage5.3.1-ManualAccepted-2026-11-08/`;
      - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (system version remains 0.44.0 untouched; no git tags or public release created).
    - **Stage 5.3.2A — Filesystem Compatibility Preflight:** ✅ COMPLETED:
      - Conducted read-only inventory of mounted and available filesystems (Btrfs on NVMe root/home, tmpfs on /tmp, vfat EFI, NTFS via ntfs-3g/fuseblk on /mnt/c, /mnt/d, /mnt/g, NTFS via kernel ntfs3 on /mnt/e-raid, /mnt/f-raid, unmounted FAT32 USB on /dev/sde);
      - Analyzed atomic publication mechanisms in `LocalFileCopyJob`, `LocalFileMoveJob`, and `KioStagedTransferJob` regarding `RENAME_NOREPLACE` fail-closed semantics;
      - Prepared isolated test environment `~/Pobrane/Testy/0.45/Stage5.3.2-Filesystems/` with fixtures, SHA-256 sums, `tools/check_rename_noreplace` feature probe, and safe non-destructive test script `test_filesystem_transfer.fish`;
      - Verified native `RENAME_NOREPLACE`, collision protection, and `RENAME_EXCHANGE` on local Btrfs filesystem (PASS);
      - Formulated non-destructive manual acceptance plan (Scenarios A–H) for cross-filesystem evaluation.
    - **Stage 5.3.2B — NTFS-3G Compatibility Probe:** ✅ COMPLETED:
      - Executed probe in user-approved isolated directory `/mnt/d/ThisPC_FS_Test/`;
      - Confirmed that `ntfs-3g` / FUSE does NOT support `SYS_renameat2` with `RENAME_NOREPLACE` or `RENAME_EXCHANGE`, returning `errno 22 (EINVAL)`;
      - Identified critical compatibility regression risk: copying or moving files to NTFS-3G partitions without overwrite would fail closed in 0.45.0, whereas it succeeded in 0.44.0;
      - Conducted read-only inspection of mounted FAT32 pendrive at `/run/media/sebastianh/7871-F299` (vfat, 1.9 GiB free, zero writes performed).
    - **Stage 5.3.2C — NTFS-3G Safe Publication Research:** ✅ COMPLETED:
      - Developed and executed diagnostic probe `tools/check_linkat_probe` in isolated `/mnt/d/ThisPC_FS_Test/probe_linkat_test/`;
      - Confirmed `linkat()` is fully SUPPORTED on `ntfs-3g`: creates valid hard link (same inode, nlink=2), strictly rejects collisions with `errno 17 (EEXIST)` preserving destination content intact, and unlinking staging file leaves standalone published file with nlink=1;
      - Analyzed architectural safety: publishing staging via `linkat()` followed by staging `unlink()` eliminates the destination rollback race because destination is never unlinked on failure;
      - Evaluated FAT32 / vfat limitations where hard links are unsupported;
      - Security Release Gate remains **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (system version 0.44.0 untouched).
    - **Stage 5.3.2D — Safe Publication & Move Security Design Review:** ✅ COMPLETED:
      - Conducted thorough review of atomic publication, `LocalFileMoveJob` TOCTOU risks, and Overwrite variants on filesystems lacking `RENAME_EXCHANGE`;
      - Identified critical flaw in current `LocalFileMoveJob::rollbackDestination()` (`QFile::remove(destination)` upon source deletion failure);
      - Design review found that architectural corrections are strictly required before any implementation.
    - **Stage 5.3.2D.1 — Security Design Corrections:** ✅ COMPLETED:
      - Segregated publication APIs: descriptor linkat fallback strictly reserved for verified owned staging files; user source Move forbidden from using linkat;
      - Designed descriptor-based staging protection against TOCTOU path substitutions;
      - Established safe Move source handling: refusing to delete source if exact identity cannot be guaranteed, eliminating dangerous destination rollback, fail-safe duplicate retention;
      - Solidified Overwrite on filesystems without `RENAME_EXCHANGE` as strictly fail-closed;
      - Defined deterministic regression test matrix (Scenarios 1–10).
    - **Stage 5.3.2D.2 — Final Security Design Gate:** ✅ COMPLETED:
      - Empirically verified non-root descriptor publication on NTFS-3G via `linkat(stageFd, "", dstDirFd, name, AT_EMPTY_PATH)` and `/proc/self/fd/NN` (both PASS with strict `EEXIST` collision rejection and SHA-256 integrity);
      - Proved that renaming staging file on disk while keeping descriptor open still publishes original inode;
      - Established architectural boundary: standard Linux VFS does not offer atomic conditional unlink by inode, requiring Fail-Closed source retention rather than TOCTOU rollback;
      - Fixed `removeVerifiedPartial()` flaw where invalid identity previously permitted unconditional deletion.
    - **Stage 5.3.2E.1/E.2 — Safe NTFS-3G Copy Implementation & Final Closure:** ✅ CLOSED / AUTOMATED PASS / MANUAL ACCEPTED (3/3) / BACKED UP / COMMITTED / PUSHED:
      - **Copy on Btrfs and NTFS-3G:**
        - Descriptor-based `linkat()` (`AT_EMPTY_PATH` with `/proc/self/fd` fallback) is the primary publication mechanism for owned staging files;
        - Atomic collision protection via kernel `EEXIST`; destination files are never overwritten;
        - Verified SHA-256 payload integrity matching source files;
        - Manual Acceptance: 3/3 FULL PASS by user (E1-A sample_doc.txt, E1-B skip conflict preserving original SHA-256, E1-C sample_image.bin);
      - **Hardening and Verification:**
        - Hardened `removeVerifiedPartial()` requiring `expectedIdentity.valid == true` and matching inode identity;
        - Separated Move API (`moveLocalFileNoOverwrite()`) strictly using atomic `RENAME_NOREPLACE` without hardlink fallback;
        - Automated regression: 77/77 test suites PASS; `local_transfer`: 85/85 assertions PASS; `local_move`: 73/73 assertions PASS;
        - ASan/UBSan clean on transfer engines;
        - Backup verified at `/mnt/g/ThisPC backup/ThisPC-0.45.0-Stage5.3.2E.2-FinalClosure-2026-11-09/`;
      - **Remaining Limitations & Security Considerations:**
        1. VFAT compatibility: VFAT does not support hardlinks. Current Copy mechanism will require dedicated compatibility testing and likely a separate safe publication path (Linux supports `RENAME_NOREPLACE` on VFAT, but actual behavior must be confirmed on real user devices);
        2. Staging cleanup protection: pre-unlink verification (`fstatat` before `unlinkat`) mitigates substitution risk, but POSIX VFS lacks atomic conditional unlinking by inode. Real trust model assumes non-hostile concurrent modifications by same UID in directory;
        3. History test harness LSan report: 6464 B in 48 allocations in `history.cpp` test harness retained as a standalone tracking item for verification, distinct from production code;
        4. Move and Overwrite on NTFS-3G remain restricted;
      - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (system version 0.44.0 untouched).
    - **Stage 5.3.2F — Safe VFAT Copy Implementation & Closure:** ✅ CLOSED / AUTOMATED PASS / MANUAL ACCEPTED (5/5) / BACKED UP / COMMITTED / PUSHED:
      - **VFAT RENAME_NOREPLACE Publication:**
        - Dedicated, strictly guarded publication path for VFAT (`MSDOS_SUPER_MAGIC`) using native Linux kernel `SYS_renameat2(..., RENAME_NOREPLACE)`;
        - Non-VFAT filesystems lacking hardlink support fail closed with `EOPNOTSUPP`;
        - Atomic collision protection via kernel `EEXIST` (errno 17), preserving existing destination files bit-for-bit without overwrite;
        - Full case-insensitive collision protection confirmed on FAT32;
        - Pre-rename verification of staging identity (`st_dev`, `st_ino`, `st_size`, symlink check) against held staging descriptor;
        - Directory durability enforced via `fsync(destDirFd)`;
      - **Verification and Manual Acceptance:**
        - Automated regression: 77/77 test suites PASS; `local_transfer`: 113/113 assertions PASS; `local_move`: 73/73 assertions PASS;
        - Clean ASan/UBSan runs on all transfer test suites;
        - Verified SHA-256 payload integrity across all tests;
        - Zero staging leftovers (`.thispc-part` cleanly unlinked on error/collision or consumed on atomic publication);
        - Manual Acceptance on USB FAT32 pendrive (UUID `7871-F299`): **5/5 FULL PASS** by user;
        - Backup verified at `/mnt/g/ThisPC backup/ThisPC-0.45.0-Stage5.3.2F-VFAT-ManualAccepted-2026-11-09/`;
      - **Remaining Limitations & Security Considerations:**
        - TOCTOU limitations for staging source: because Linux VFS does not support descriptor-based `renameat2` with `AT_EMPTY_PATH`, VFAT publication references the staging entry by directory name. While pre-rename `fstatat` and post-rename checks detect tampering, the trust model assumes no hostile concurrent process running under the same UID in the destination directory;
        - Move and Overwrite on VFAT and NTFS-3G remain restricted;
      - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (system version 0.44.0 untouched).
    - **Stage 5.3.2G.1 — Move Compatibility & Safety Audit:** ✅ AUDIT COMPLETED:
      - Comprehensive architectural audit of `LocalFileMoveJob`, `LocalFileCopyJob`, `moveLocalFileNoOverwrite()`, KIO Move, and transfer history;
      - Identified destructive rollback vulnerability in copy-and-remove fallback: destination unlinking on source removal failure resulted in data loss;
      - Identified TOCTOU vulnerability in path-based source unlinking without identity re-verification;
      - Analyzed filesystem characteristics (Btrfs atomic rename, NTFS-3G EINVAL, VFAT atomic rename, ntfs3 kernel driver);
    - **Stage 5.3.2G.2A — Source Preservation Safety Fix:** ✅ CLOSED / AUTOMATED PASS / BACKED UP / COMMITTED / PUSHED:
      - **Elimination of Destructive Rollback:** Once destination is published, it is NEVER removed or rolled back on source removal failure or post-copy verification error in non-overwrite mode (`rollbackDestination()` and all destination rollback mechanisms completely removed from `LocalFileMoveJob`);
      - **Fail-Closed Overwrite Gate:** In `LocalFileMoveJob`, any Move with overwrite is temporarily rejected fail-closed immediately in `begin()` before copying, publishing, exchanging, or touching destination or source, returning `LocalFileMoveError::InvalidInput` with a clear message that safe file move with overwrite is not currently supported;
      - **Safe Source Verification & Removal (Source Preservation):** Preserved in defined trust model: prior to unlinking source, verifies identity against `m_sourceIdentity` recorded at job start (`st_dev`, `st_ino`, `st_size`, `st_mtim.tv_sec`, `st_mtim.tv_nsec`, non-symlink). Refuses unlinking (`LocalFileMoveError::Verification`, fail-closed) if source was modified or replaced during transfer;
      - **Cryptographic Data Integrity Check:** Re-reads source on disk and compares its SHA-256 hash against the exact transferred stream checksum (`transferredChecksum()`), detecting in-place byte modifications even if `mtime` was forged/restored via `utimensat`;
      - **Race-Free Directory Descriptors & Documented TOCTOU Boundary:** Safe unlinking uses parent directory descriptor with `fstatat(..., AT_SYMLINK_NOFOLLOW)` and `unlinkat()`; test harness Scenario M documents that POSIX Linux lacks conditional `unlinkat_if_ino`;
      - **Genuine Status Reporting & History Safety:** Reports `LocalFileMoveError::SourceRemove`, `Verification`, or `InvalidInput`; transfers with unremoved sources or rejected overwrite moves are rejected by `LocalMoveHistory` and flagged as errors in queue;
      - **Scope Boundary & Release Gate:** The separate Copy Overwrite path (`LocalFileCopyJob`) remains subject to Final Security Release Gate review;
      - **Automated Verification:** `local_move` suite: 126 assertions PASS; `local_transfer`: 113/113 PASS; full regression: 77/77 test suites PASS; ASan: clean;
      - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (system version 0.44.0 untouched).
    - **Stage 5.3.2G.2B — Safe Same-Volume NTFS-3G Move:** ✅ CLOSED / AUTOMATED PASS / MANUAL ACCEPTED (3/3) / BACKED UP / COMMITTED / PUSHED:
      - **Same-Volume NTFS-3G Non-Overwrite Move:** Safely restores move operations between directories of the same NTFS-3G volume using verified staging copy + publish via descriptor `linkat()` + safe source removal;
      - **Verified Mount Identification:** Strictly checks `isNtfs3gMount(sourcePath, destinationPath)` to confirm both paths reside on the same mounted NTFS-3G volume (`FUSE_SUPER_MAGIC`, `fuseblk`, fstab/mountinfo ntfs-3g verification); never falls back on arbitrary `EINVAL` or non-NTFS FUSE mounts;
      - **Atomic Collision Protection:** Kernel `linkat()` prevents overwriting existing destination files (`EEXIST`); destination and source remain bit-for-bit preserved;
      - **Cryptographic Data Integrity (Source & Destination):** Full SHA-256 verification of destination on disk and source on disk against `transferredChecksum()` before source removal;
      - **Safe Source Removal & TOCTOU Boundaries:** Source unlinked through parent directory descriptor `unlinkat()` after verifying identity against `m_sourceIdentity`; POSIX TOCTOU boundary documented;
      - **Zero Leftover Staging & Error Preservation:** Staging `.thispc-part` cleanly removed in all paths; published destination is NEVER rolled back on source removal failure;
      - **Move with Overwrite Status:** Remains strictly **FAIL-CLOSED** across all Move operations;
      - **Verification & Acceptance:**
        - Unit test suite: `local_move` expanded to 167 assertions PASS (Scenarios A–I);
        - Real NTFS-3G Integration on `/mnt/d/ThisPC_FS_Test/stage532g2b_move_test`: PASS (safe move, SHA-256 integrity, EEXIST collision, zero staging leftovers);
        - Manual GUI Acceptance on `/mnt/d/ThisPC_FS_Test/`: **3/3 FULL PASS** by user;
        - Full regression: 77/77 test suites PASS; ASan/UBSan clean;
      - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (system version 0.44.0 untouched).
    - **Stage 5.3.2G.3A — Cross-Filesystem Move Routing & Safety Audit:** ✅ AUDIT COMPLETED:
      - Resolved routing discrepancy: confirmed single-file cross-filesystem moves route to `LocalFileMoveJob::startCopyAndRemove()`;
      - Identified critical security containment risks: destructive destination rollback in `LocalTransferJob::transferLink()`, unverified KIO::move fallback on conflicts/unsupported entries, and missing check on destination directory fsync return value in `linkat` publication;
      - Formulated safety containment requirements for unverified recursive/multi-item moves;
    - **Stage 5.3.2G.3A.1 — Local Move Safety Containment & Durability Gate:** ✅ CLOSED / AUTOMATED PASS / BACKED UP / COMMITTED / PUSHED:
      - **Durability Gate Implementation:** Established a strict durability gate in `LocalFileMoveJob` prior to source removal. If parent directory synchronization `fsync(destDirFd)` returns `EINVAL`, `ENOTSUP`, or `EIO`, the source file is preserved and unlinking is refused;
      - **Partial Outcome & Anti-Rollback Protection:** If destination publication succeeded before a sync failure, destination is preserved without destructive rollback, and a partial result (`LocalFileMoveError::Durability`, `thispcPartialMoveOutcome = true`) is reported to GUI, transfer queue, and history;
      - **Copy Compatibility Intact:** Normal Copy operations remain functional on filesystems where directory `fsync` is unsupported (`EINVAL`/`ENOTSUP` like tmpfs), completing with verified data while exposing durability status;
      - **Distinction of Safety Boundaries:**
        - *Collision Protection:* Atomic `EEXIST` / `RENAME_NOREPLACE` prevents silent overwriting;
        - *Data Integrity:* Cryptographic SHA-256 validation verifies exact on-disk byte streams before source removal;
        - *Target Durability:* Directory `fsync` ensures directory metadata durability before unlinking the source;
        - *TOCTOU Mitigation:* Inode/device identity verification (`fstatat`) restricts substitution risks within POSIX VFS boundaries;
        - *Power Loss Limits:* Post-crash guarantees remain bounded by hardware write caches, driver implementations, and non-transactional filesystem boundaries. No claim of universal duplicate outcomes or absolute power-cut immunity is made;
      - **Fail-Closed Move Containment:** Temporarily blocks unverified local moves (multi-item, directory, symlink, overwrite) fail-closed before any filesystem mutation;
      - **KIO Bypass Prevention:** Intercepted all paths to `KIO::move` / `KIO::moveAs` for local moves; disabled fallback to KIO on conflict or unsupported items in `LocalTransferJob`;
      - **Elimination of Symlink Rollback:** Removed destructive `QFile::remove(entry.destinationPath)` in `LocalTransferJob::transferLink()`;
      - **Preservation of Core Functionality:** Regular single-file Move without overwrite and all Copy operations (single, batch, directory, symlink) remain 100% active and functional;
      - **Automated Verification:** `local_move` suite expanded to 224 assertions PASS (Scenarios A–L); `local_transfer`: 113/113 PASS; `local_tree`: 61/61 PASS; `tree_history`: 17/17 PASS; `actions`: 216/216 PASS; `panes`: 1413/1413 PASS; full regression: 77/77 test suites PASS; ASan/UBSan clean;
      - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (system version 0.44.0 untouched).
    - **Stage 5.3.2G.3B — Safe Single-File Cross-Filesystem Move:** ✅ AUTOMATED PASS:
      - **Distinction Between Atomic Rename and Copy-and-Remove:** Same-volume native moves on Btrfs/ext4 execute atomic `SYS_renameat2(..., RENAME_NOREPLACE)`, whereas cross-filesystem moves (and same-volume NTFS-3G moves) route through the explicit `LocalFileMoveJob::startCopyAndRemove()` pipeline; atomic filename rename is never conflated with confirmed hardware write durability;
      - **Supported Scenarios:** Single regular local file cross-filesystem moves without overwrite (0-byte empty files and large binary streams, Polish characters, Unicode, and spaces in filenames);
      - **Safety Verification Sequence:** Source/target validation → staging copy with SHA-256 stream digest → payload `fsync` & supported metadata (`chmod`, `utimensat`) → non-overwrite publication (`linkat` / VFAT `SYS_renameat2`) → directory durability `fsync(destDirFd)` → on-disk SHA-256 check of destination → re-check of source identity (`fstatat`) and SHA-256 → descriptor-based source removal (`unlinkat`). On any post-publication failure (`EINVAL`, `ENOTSUP`, `EIO`, `SourceRemove`, `Verification`), destination is NEVER rolled back or deleted, source is preserved, `thispcPartialMoveOutcome = true` is set, and partial outcomes are never recorded as completed/undoable;
      - **Residual TOCTOU & Power Loss Bounds:** Bounded by POSIX VFS semantics (`fstatat`, `unlinkat` via parent dir descriptors) and storage hardware write caches/drivers; no claim of absolute power-loss immunity or complete TOCTOU elimination is made;
      - **Move Containment Strictly Retained:** Local moves of directories, symlinks, multi-item batches, and overwrite moves remain strictly blocked fail-closed without KIO::move fallback; Copy paths remain intact;
      - **Physical Test Isolation (`THISPC_REAL_CROSSFS_TEST=1`):**
        - *Deterministic Isolated Suite:* `local_move` passes 298 assertions without touching physical disk D: (physical volume integration reports explicit `SKIP` unless opt-in `THISPC_REAL_CROSSFS_TEST=1` is set); test hooks (`simulateCrossFilesystem()`) are compiled strictly under `#ifdef THISPC_TEST_HARNESS`;
        - *Real Volume Integration (earlier verified under `THISPC_REAL_CROSSFS_TEST=1` prior to final opt-in guard):* 329 assertions PASS across `/home` (Btrfs) ↔ `/mnt/d/ThisPC_FS_Test` (NTFS-3G `fuseblk`) using mount/superblock validation and unique `QTemporaryDir` isolation;
    - **Stage 5.3.2G.3C — Cross-Filesystem Single-File Manual Acceptance:** ✅ MANUAL ACCEPTED (4/4 FULL PASS):
      - **MA-G3C-1 (Btrfs → NTFS-3G, Ctrl+X/Ctrl+V):** 44 B file moved cleanly, matching SHA-256, source unlinked, zero staging leftovers — PASS;
      - **MA-G3C-2 (NTFS-3G → Btrfs, Drag & Drop):** Verified moving back from D: to local Btrfs, matching SHA-256, source on D: unlinked, zero staging leftovers — PASS;
      - **MA-G3C-3 (Collision + Skip):** Pre-existing destination (60 B) vs source (44 B) resolved via Skip with both SHA-256 hashes intact and zero overwrites — PASS;
      - **MA-G3C-4 (Directory Move Refusal):** Directory move attempt Btrfs → NTFS-3G safely rejected fail-closed with containment notice and intact source — PASS;
    - **Stage 5.3.2G.3C.1 — Directory Watch Reentrancy Crash Fix:** ✅ AUTOMATED PASS / MANUAL ACCEPTED (3/3 FULL PASS):
      - **Root Cause (`SIGSEGV / SEGV_MAPERR`, PID 84245):** During external deletion (`rmdir`) of a directory displayed in Split View, `KDirWatch::deleted()` invoked `DirectoryWatchController::onKDirWatchDeleted()`, which emitted synchronous `directoryDeleted()` while iterating `m_watchedEntries`. Slot `ThisPcWindow::handleWatchedDirectoryDeleted()` navigated away (`m_splitPane->setCurrentUrl(kThisPcUrl)` → `updateDirectoryWatchSubscriptions()` → `removeConsumer()`), erasing the active `QMap` RB-tree node and causing `++it` (`std::_Rb_tree_increment`) heap-use-after-free / crash;
      - **Remediation:** Applied Two-Phase Snapshot collection prior to signal emission across all `m_watchedEntries` iterations (`onKDirWatchDeleted()`, `handleFileMoved()`/`handleFileRenamed()`, `onKDirWatchDirty()`, `onKDirWatchCreated()`, `checkAndScheduleMatching()`), added per-entry `generation` counters with pre-dispatch validity re-checks, guarded controller lifetime with `QPointer`, prevented timer UAF via `deleteLater()`, and preserved `refCount`, debouncing, `maxDeadlineMs`, and existing navigation fallback policies;
      - **Automated Verification:** Pre-fix test run confirmed ASan `heap-use-after-free` reproduction (`EXIT_CODE=1`); post-fix new deterministic suite `directory_watch` passes 80/80 assertions (Scenarios A–L + real `inotify` deletion) cleanly under ASan/UBSan (`EXIT_CODE=0`); Directory Refresh contract suite passes 154 GREEN;
      - **Manual Crash Recheck (3/3 FULL PASS):** Verified Split-pane external deletion fallback to `thispc:/` (Recheck 1), shared directory deletion fallback Primary→parent & Split→`thispc:/` under PID 95337 without new coredumps (Recheck 2), and continued automatic refresh in recovered parent (Recheck 3);
    - **Stage 5.3.2G.3D — Final Security Review & Git Closure:** ✅ COMPLETED / READY FOR FORMAL GIT CLOSURE:
      - Comprehensive review of diff against `b7894ad7d2ef4bdd561e962bb0f3c8067e80789d` (9 scoped files; `release-artifacts/` retained untracked intact; version `0.44.0` preserved pending final 0.45.0 release; `TransferQueueController` warning traced to intentional test Scenario 26);
      - Full regression: 78/78 test suites PASS (`EXIT_CODE=0`); ASan/UBSan clean;
      - **Remaining Move Constraints & Next Stage Candidates:** Safe directory tree/multi-item/symlink/overwrite moves and physical VFAT USB tests remain outside G.3B and fail-closed; UX-01 remains deferred under Future UX Candidates;
      - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (system version 0.44.0 untouched).
    - **Stage 5.3.2G.4A — VFAT USB Move Qualification & Test Preparation:** ✅ AUTOMATED PASS:
      - **Zero Production Code Changes:** Security and routing audit across [`LocalFileMoveJob`](src/localfilemovejob.h), [`LocalFileCopyJob`](src/localfilecopyjob.h), [`publishOwnedStagingNoOverwrite()`](src/localfileoverwrite.h), [`FileActions`](src/fileactions.h), and [`LocalMoveHistory`](src/localfilemovejob.h) confirmed existing single-file non-overwrite VFAT paths (`SYS_renameat2(..., RENAME_NOREPLACE)` for both same-volume moves and `MSDOS_SUPER_MAGIC` staging publication + `fsync(destDirFd)` + SHA-256 verification + `unlinkat`) satisfy fail-closed source preservation without modifying production files;
      - **Test Suite Extension (`local_move` 298 → 354 assertions):** Extended `tests/local-file-move-job.cpp` with deterministic simulated VFAT scenarios (G4A Test 1a–7) exercising `#ifdef THISPC_TEST_HARNESS` hooks (`simulateVfatFilesystem()`, `simulateDescriptorLinkatUnsupported()`, `simulateCrossFilesystem()`, `simulateRenameNoReplaceUnsupported()`, `simulateFsyncError()`, `classifyWriteError(ENOSPC/EFBIG)`). Simulated runs on Btrfs/tmpfs explicitly complement — rather than substitute for — real kernel FAT32 acceptance;
    - **Stage 5.3.2G.4B — VFAT Physical USB Manual Acceptance:** ✅ MANUAL ACCEPTED (5/5 FULL PASS) & CLEANUP PASS:
      - Executed against physical FAT32/VFAT USB flash drive (`UUID 7871-F299`, `/dev/sde1`, `/run/media/sebastianh/7871-F299`, `rw, nosuid, nodev, utf8, flush`) paired with local Btrfs (`~/Pobrane/Testy/0.45/Stage5.3.2G.4/`) under commit `ec12bcf` (`PID 111192`):
        - **MA-G4B-1 (Btrfs → VFAT, Ctrl+X/Ctrl+V):** 55 B file moved with verified SHA-256 (`76522d...`), source unlinked, zero staging leftovers — FULL PASS;
        - **MA-G4B-2 (VFAT → VFAT same-volume Move):** 55 B file moved into `sub/` with intact SHA-256 and old entry removed — FULL PASS;
        - **MA-G4B-3 (VFAT → Btrfs, Unicode/spaces):** `g4b zażółć gęślą.txt` (61 B) moved with UTF-8 diacritics intact, verified SHA-256 (`9d08d9...`), USB source unlinked — FULL PASS;
        - **MA-G4B-4 (Case-insensitive VFAT Collision + Skip):** `g4b-conflict.txt` (44 B) vs `G4B-CONFLICT.TXT` (60 B) correctly detected collision across case; Skip kept both SHA-256 hashes intact with zero overwrite (Replace button unused; Move overwrite remains untested and fail-closed) — FULL PASS;
        - **MA-G4B-5 (Directory Move Containment):** `g4b-folder-block-test` rejected fail-closed with containment notice; `keep_me.txt` (49 B, `61c6d8...`) preserved intact — FULL PASS;
      - **Controlled Environment Cleanup:** 7/7 SHA-256 checksums verified; controlled files and `SHA256SUMS` removed; temporary Btrfs and USB directories removed via `rmdir` (zero `rm -rf`) — PASS;
    - **Stage 5.3.2G.4C — VFAT Move Final Security Review & Git Closure:** ✅ COMPLETED / READY FOR FORMAL GIT CLOSURE:
      - **Scoped Deliverables (3 files):** `tests/local-file-move-job.cpp`, `ROADMAP.md`, `ROADMAP.pl.md` (`0` production files modified; `release-artifacts/` retained untracked intact; system version remains `0.44.0`);
      - **Confirmed VFAT Compatibility Scope vs Untested Pairs:** Physically accepted for `Btrfs → VFAT`, `VFAT → VFAT` (same-volume), `VFAT → Btrfs`, case-insensitive VFAT collision, and directory move containment. Direct physical `NTFS-3G ↔ VFAT` moves were **not** physically tested (audited under shared Copy-and-Remove contract); directory, multi-item, symlink, and overwrite moves remain strictly fail-closed; durability remains bounded by VFS/kernel/hardware guarantees;
      - **Open Directory Refresh Contract Items (Unchanged / Non-Gate):** `154 GREEN` does not represent full contract completion; `DR-058` (NTFS/FUSE inotify latency), `DR-059` (VFAT/FAT32 USB storage change detection/cleanup), and `DR-062` (search result view live update) remain `RED / NOT IMPLEMENTED`, and `DR-009` / `DR-010` (Plasma desktop external cut/paste) remain `NOT RUN` — tracked separately for 0.45.0 evaluation;
      - **Deferred UX Observation:** `Candidate UX-01` (relative timestamp wording in Conflict Dialog) remains deferred without release assignment;
      - **Security Release Gate Status:** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (next planned stage: **Stage 5.3.2H — Safe Directory & Multi-File Move**).

### 0.45.0 — Security Release Gate
- Prior to official release, a mandatory comprehensive security audit is required, covering unauthorized code execution, potential backdoors, administrative privilege escalation, third-party dependencies, installer integrity, and file operation safety;
- Any unresolved critical or high-severity vulnerabilities are strict release blockers;
- This policy applies unconditionally to 0.45.0 and all future releases.

#### Tracked UX Backlog Items (0.45.0 Scope — Closed in Stage 5.3.1):
- **UX Item A — Default Size of "File Operations" Window:** ✅ CLOSED / VERIFIED
  Compact initial geometry (~580×380 px) showing one expanded card fully without excessive vertical blank space, while preserving manual resizing, multi-task scrolling, Wayland, and HiDPI scaling.
- **UX Item B — Speed Graph on Instantaneous Transfers:** ✅ CLOSED / VERIFIED
  Completed operations with fewer than 2 speed samples (transfers under 500 ms) do not display "Waiting for speed samples…"; suppress graph or display appropriate completed terminal status ("Transfer ukończony natychmiastowo").
- **UX Item C — History Consistency Between Popup and Detailed Window:** ✅ CLOSED / VERIFIED
  Toolbar `OperationPopup` does not display "No history" while completed operations exist in the session or persistent storage; summary displays exact completed task count.

#### Future UX Candidates (Unscheduled / Under Observation):
- **Candidate UX-01 — Conflict dialog incorrect relative timestamp comparison:**
  Observed "Source item is older" / "Rzecz źródłowa jest starsza" message despite displaying a newer source timestamp. Issue observed on NTFS-3G and VFAT mounts. Retained for future analysis without assignment to a specific release.

### 0.46.0 — Standalone Architecture & Security Audit
- Comprehensive audit of KDE Frameworks and Qt dependencies;
- Design of an independent ThisPC core decoupled from desktop-specific runtime requirements;
- Optional, loosely coupled KDE integrations;
- Alternative Linux backends for cross-desktop environments;
- Desktop compatibility analysis (Linux Mint / Cinnamon, Pop!_OS / COSMIC, XFCE, etc.);
- Formal application threat model;
- Secure packaging, deployment, and update verification models;
- Staged migration plan preserving all safety and transactional guarantees without regression.
*(Note: Design and planning phase; architectural implementation is not yet started.)*

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
- Saved Searches (deferred Stage 4 from the original 0.41 search plan): persistent storage for validated query, filter, scope, and sort definitions with explicit reruns and unavailable-location handling.
- Optional Baloo Integration (deferred Stage 5 from the original 0.41 search plan): conditional backend adapter evaluated against worker semantics, index coverage, and completeness, retaining automatic fallback to the independent search worker.
- Quick in-place filtering of the current folder: a valuable separate mechanism, not recursive search; define its own view/filter semantics.
- Search by file content: a separate capability requiring format/backend, performance, and safety review.
- Advanced query logic AND/OR/NOT: separate from the simple AND between active filters and alternatives within one filter shipped in 0.41.0.

### Extensions / Service Actions — No Assigned Version
- Leverage KDE Service Actions and clean extension contracts instead of building a redundant bespoke plugin ecosystem;
- Support adding custom context actions and integrations without editing core window sources.

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
- **UX Candidate A — OperationWindow Size Reset (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Ability to restore the default size of the "File Operations" window (580×380 px) without manually dragging window edges.
  - Preferred interface:
    - Dedicated "Restore default size" option in the window settings menu;
    - Possible future integration with centralized ThisPC settings dialog;
    - Optional supplemental gesture (e.g. Shift + double-click) strictly if ergonomically justified.
  - The restored default geometry must be persistently saved as the active window geometry.
- **UX Candidate B — Split View Equalize 50/50 (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Double-clicking the splitter handle between panes with the left mouse button immediately equalizes pane widths to 50/50.
  - Requirements:
    - Standard single double-click with LMB without requiring keyboard modifiers;
    - No changes to open directories, selections, or internal pane state;
    - Full respect for minimum pane width constraints;
    - Preserves existing manual dragging of the splitter separator.
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
