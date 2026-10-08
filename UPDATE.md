[English](UPDATE.md) | [Polski](UPDATE.pl.md)

# Update to 0.40.0

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.40.0.tar.gz
cd kio-thispc-0.40.0
chmod +x install.sh
./install.sh
```

- Properties now detects backend capabilities and retains a no-follow identity snapshot, rejecting writes after an item disappears, moves, is replaced, or loses its mount.
- Extended Attributes shows bounded text/binary previews and supports immediate local user.* add/edit/remove through verified file descriptors. These operations are outside Apply/Cancel/Undo. security.*, trusted.* and system.* remain protected.
- Numeric POSIX permissions (0000–0777) synchronize with permission checkboxes, preserve existing special bits, and refresh ACL state using normal kernel mask semantics.
- Hidden presentation distinguishes KIO visibility, Unix dot-name semantics, and unverified native flags. Safe Unix dot-name changes use the shared rename path; NTFS native Hidden is not emulated or written.
- Primary, Split and Search share Properties behavior, refresh, target-loss guards and safe close sequencing. ACL remains unavailable for links after rename, including broken links.
- Manual Stage 1–6 acceptance is prior user-confirmed evidence. Live ext4 and remote KIO remain NOT TESTED; deterministic policies are covered automatically. Final release verification is recorded in [the release verification](docs/PROPERTIES_RELEASE_040.md); the pre-release audit remains historical.

Excluded: SELinux labels, chattr/immutable, Linux file capabilities, larger ACL redesign, admin:// retry.

---

# Updating to 0.39.0

This release adds native local-file launching and rich, asynchronous previews through KDE/KIO.

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.39.0.tar.gz
cd kio-thispc-0.39.0
chmod +x install.sh
./install.sh
```

## Scope of 0.39.0

- correct local `file://` resolution and the native KDE Open/Execute workflow;
- KIO image/video thumbnails, Windows executable icon previews, and folder content previews;
- per-location **Show Previews** persistence through `DirectoryViewProfile`;
- asynchronous bounded batching, cache and generation safety in `PreviewController`;
- visible-only preview scheduling for large directories;
- remote previews disabled;
- F5 invalidation and deterministic preview-job lifetime handling.

## Verification of 0.39.0

- manual acceptance Stages 1–7: FULL MANUAL PASS;
- clean build, version CLI, focused regression, and full regression: see the release commit validation.

---

# Updating to 0.36.0

This release adds Network & Remote Locations through native KIO: manual SMB, SFTP, FTP, WebDAV, and WebDAVS connections, Saved Remote Locations, Recent/Reconnect, transactional session/history restore, and optional native discovery.

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.36.0.tar.gz
cd kio-thispc-0.36.0
chmod +x install.sh
./install.sh
```

## Scope of 0.36.0

- manual remote connections through KIO and native KIO/KWallet authentication;
- Saved Remote Locations and success-confirmed Recent/Reconnect;
- transactional restore of remote sessions and Primary/Split history;
- optional `remote:/` and `smb:/` discovery without a custom scanner;
- no password persistence in ThisPC/QSettings.

## Verification of 0.36.0

- manual acceptance Stages 1–5: PASS;
- build: PASS;
- full regression: 39 suites / 9075 assertions PASS;
- CLI `thispc-view 0.36.0`, exit code 0: PASS;
- live discovered SMB server path: NOT TESTED — no discoverable LAN services available.

---

# Updating to 0.35.0

This release introduces complete removable storage and drive management (Drives & Devices): dynamic device detection via KDE Solid, unmounted volume presentation on `thispc:/`, on-demand mounting, clean filesystem-only unmounting, safe physical removal (Safely Remove) with drive power-off, and optical disc media ejection (Eject).

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.35.0.tar.gz
cd kio-thispc-0.35.0
chmod +x install.sh
./install.sh
```

## Scope of 0.35.0

- Dynamic drive and removable device discovery through KDE Solid;
- Elimination of periodic polling timers and "Refreshing…" status flicker (event-driven `SolidDeviceMonitor` with 250 ms debouncing);
- Discovery and presentation of unmounted removable volumes on `thispc:/` labeled as "Unmounted";
- On-demand mounting (`DeviceMountController`) upon click or Enter with immediate directory navigation;
- Sidebar displays mounted volumes only;
- "Unmount" action executes clean filesystem-only unmounting via native asynchronous QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount` without cutting device power;
- "Safely Remove" action coordinates unmounting across all mounted partitions of the physical drive, followed by asynchronous `org.freedesktop.UDisks2.Drive.PowerOff`;
- Safely Remove operates seamlessly on already-unmounted volumes;
- "Eject" action for optical drives (`canEject`);
- Automatic redirection of Primary and Split panes away from unmounted or detached mountpoints back to `thispc:/`.

## Verification of 0.35.0

- Manual hardware acceptance for Stages 1, 2, and 3 on real USB storage: PASS;
- Clean build: PASS;
- Full automated regression: 35 test suites / 8562 assertions PASS;
- CLI `thispc-view 0.35.0`, exit code 0: PASS;
- Zero periodic polling and zero UI thread blocking.

---

# Updating to 0.34.0

This release completes Explorer UX / View & Navigation Polish: unifies view and navigation behaviors between Primary and Split View panes while preserving existing KIO semantics.

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.34.0.tar.gz
cd kio-thispc-0.34.0
chmod +x install.sh
./install.sh
```

## Scope of 0.34.0

- Stable CLI `--version` flag, Split address/breadcrumb parity, and drive card spacing;
- Visual dimming for hidden files and directories;
- Unified inline rename;
- Keyboard navigation routing and stable current/focus handling in `thispc:/`;
- Per-folder view profiles with snapshot Apply/Remove to subfolders;
- Nine icon size steps with keyboard shortcuts;
- Final visual parity polish across Primary and Split panes.
- Minor 1 px / accent line follow-up does not block the release. Removable drive detection remains in 0.35, and double-click 50/50 splitter reset is a future idea.

## Verification of 0.34.0

- Manual acceptance of functional stages and visual parity polish: PASS;
- Clean build: PASS;
- Full automated regression: 32 test suites / 8317 assertions PASS;
- CLI `thispc-view 0.34.0`, exit code 0: PASS.

---

# Updating to 0.33.0

This release continues architectural cleanup without adding new user-facing features. Preserves 0.32.0 behavior alongside regression fixes identified during extraction.

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.33.0.tar.gz
cd kio-thispc-0.33.0
chmod +x install.sh
./install.sh
```

## Scope of 0.33.0

- `LocationPresentation` and `NavigationHistory`;
- `PreviewCoordinator` and `ActionStateController`;
- Shared `DirectoryListingCore` across Primary and Split panes;
- `PaneMenuController` and `DriveHomeCoordinator`;
- Sidebar context-menu lifetime crash fix during drive refresh;
- Inline rename activation fix.

## Verification of 0.33.0

- Manual acceptance of Stages 1–8 and inline rename fix: PASS;
- Full automated regression: 31 test suites / 7852 assertions PASS;
- Clean build: PASS.

---

# Updating to 0.32.0

This release concludes architectural reorganization without introducing new user-facing features. Preserves 0.31.0 behavior alongside regression fixes identified during refactoring.

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.32.0.tar.gz
cd kio-thispc-0.32.0
chmod +x install.sh
./install.sh
```

## Scope of 0.32.0

- Acyclic Split Sync modules;
- Decoupled application widgets, stylesheet styling, and hardened test harness;
- `PaneContext` / `PaneAdapter`;
- `TabController`, `SearchUiController`, and `SelectionMenuController`;
- `PrimaryBrowserPane` with standalone state, KIO directory listing, item rendering, and thumbnail caching;
- Stage 9 / shared `BrowserPane` deferred to 0.33.

## Verification of 0.32.0

- Manual acceptance for all 0.32 stages: PASS;
- Baseline after Stage 8: 26 test suites / 6662 assertions PASS;
- Final build: PASS;
- Focused regression: 10 test suites / 1738 assertions PASS;
- Full automated regression: 26 test suites / 6662 assertions PASS;
- `git diff --check`: PASS.

---

# Updating to 0.31.0

This release introduces safe, non-destructive folder comparison and synchronization in Split View mode.

The following commands install the release from a new directory without removing existing repository checkouts:

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.31.0.tar.gz
cd kio-thispc-0.31.0
chmod +x install.sh
./install.sh
```

## What's New

- Split View pane comparison and synchronization (`Ctrl+Alt+C` shortcut).
- **Stage 1 (Pane Comparison):** Asynchronous direct item comparison between Left and Right panes with classification (Identical, Left Only, Right Only, Modified) based on KIO metadata (size, mtime, entry type), matching Unicode names and case sensitivity.
- **Stage 2 (Plan Preview):** Deterministic sync plan generator for Left → Right and Right → Left directions with action classification (Unchanged, Copy, Update, Conflict, Unsupported).
- **Stage 3 (Safe Execution):** Asynchronous file copying and updating (`LocalFileCopyJob`) with zero deletion operations (no Delete/Mirror), directory recursion skipping, and protection for conflicts and unsupported entries.
- **Filesystem Safety:** Atomic preflight immediately prior to mutating each file (path validation, permissions, rejection of `.` and `..`).
- **Deterministic Cancel Lifecycle:** Race condition elimination (no off-by-one errors), waiting for active worker thread terminal signal, and 100% UI report consistency with physical filesystem state (verified via 20,000-file cancellation audit).
- **Automatic Refresh:** Immediate pane re-comparison upon synchronization completion.

## Pre-Release Verification

- Manual acceptance for Stage 1, Stage 2, and Stage 3: PASS.
- 20,000-file cancellation stress test: 100% UI consistency with filesystem state (3330 copied / 16670 cancelled / 0 errors, no partial files) — PASS.
- Final build, focused tests, full automated regression: 25 test suites / 6569 assertions PASS, `git diff --check` (post-release arithmetic correction: 6278 + 291; tag unchanged).

---

# Updating to 0.30.0

This release introduces persistent per-folder view settings and Explorer-like grouping by file type, modification date, and size.

The following commands install the release from a new directory without removing existing repository checkouts:

```bash
cd ~/Downloads
tar -xzf kio-thispc-0.30.0.tar.gz
cd kio-thispc-0.30.0
chmod +x install.sh
./install.sh
```

## What's New

- Icons/List/Details/Compact modes are persisted per normalized folder URL and shared across Primary pane, Split View, tabs, and session restore.
- View menu includes structured view modes, Show submenu, and four icon sizes: 96/64/48/32 px, also persisted per folder.
- Compact view uses a dense multi-column layout with vertical flow filling.
- Grouping: None / Type / Date Modified / Size. Current sort order is preserved within each group.
- Date grouping uses the local calendar with distinct groups for Future/Today/Yesterday/This week/Last week and earlier ranges; missing date maps to Unknown date.
- Size grouping relies strictly on KIO metadata without recursive scanning or blocking synchronous stat calls; folders and unknown sizes have dedicated groups.
- Group headers are non-selectable items excluded from selections, file menus, Drag & Drop, clipboard, Preview Pane, and Quick Look.
- Fixed filename selection outline and lingering hover state in grouped views that previously could appear as duplicate selection despite single model selection.

## Pre-Release Verification

- Manual acceptance for Stage 1 and Type/Date/Size grouping: PASS.
- Icons/List/Details/Compact, Split View, Search, per-folder persistence, sorting, Ctrl/Shift multi-select, group headers, and selection outlines: PASS (manual).
- Final build, focused tests, full automated regression, `git diff --check`, and SHA-256 archive generation re-run on final 0.30.0 candidate before commit/tag/install.

---

# Updating to 0.26.0

This release introduces the Preview Pane with the `Alt+P` toggle shortcut.

The following commands install the release from a new directory without removing existing repository checkouts:

```bash
cd ~/Downloads
unzip kio-thispc-0.26.0.zip
cd kio-thispc-0.26.0
chmod +x install.sh
./install.sh
```

## What's New

- Previews for images, plain text, Markdown, JSON, and XML.
- Asynchronous first-page PDF preview rendering.
- Audio/video metadata and EXIF inspection.
- Archive content preview for ZIP, 7z, tar, and tar.gz.
- Directory summaries.
- Fixed breadcrumbs in Split View.
- Fixed white background rendering for PDF previews.

## Verification

- Compilation: PASS.
- Full automated regression: 20 test suites PASS.
- Preview Pane: 106 assertions PASS.
- Split Layout: 99 assertions PASS.
- Manual KDE/CachyOS acceptance: Confirmed.

---

# Updating to 0.25.0

The following commands install from a new directory without removing existing repository checkouts:

```bash
cd ~/Downloads
mkdir -p kio-thispc-0.25.0
unzip kio-thispc-0.25.0.zip -d kio-thispc-0.25.0
cd kio-thispc-0.25.0
chmod +x install.sh
./install.sh
```

## 0.25.0 — Archives & New Menu

- Markdown and user templates in the New menu, and Empty Trash with confirmation.
- Secure extraction of ZIP, 7z, tar, and tar.gz; archive creation for ZIP, 7z, and tar.gz.
- Stable Split View pane proportions with long path breadcrumbs.
- Automated regression: 20 test suites, 3470 assertions PASS; Stage 3 manually accepted.
- Archive extraction requires Ark, Linux Landlock ABI 3+, and non-overwriting publication support. See `docs/ARCHIVE_STAGE2.md` and `docs/ARCHIVE_STAGE3.md`.

---

# Updating to 0.24.0

```bash
cd ~/Downloads
rm -rf kio-thispc
unzip kio-thispc-0.24.0.zip
cd kio-thispc
chmod +x install.sh
./install.sh
```

## 0.24.0 — Sidebar & Split View UX

- Symmetrical Split View panes with breadcrumbs dynamically fitted to pane widths;
- Unified toolbar and sidebar interact with the currently active pane;
- Drag & Drop files and directories into supported sidebar locations;
- Vertically scrollable and resizable sidebar with persistent preferred width;
- Long sidebar item labels elided with right-side `…` and full name shown in tooltip;
- Independent Search in both panes, including filters, scope, live results, progress, and Stop action;
- Full `This PC` view available in the right Split pane;
- Full automated regression: **1022/1022 assertions**;
- Full manual acceptance across Stages 1–4 on KDE/CachyOS;
- No user configuration migration needed.

## 0.23.0 — Native Local Transfer Engine

- Real Pause/Resume support for local file transfers;
- Safe `.thispc-part` partial files and controlled cleanup;
- Native Overwrite / Rename / Skip conflict handling;
- Directories, multi-source operations, and symlinks with aggregated progress tracking;
- Preserves permissions, timestamps, and symlink targets;
- Cross-filesystem Move and native Undo/Redo for files and directory trees;
- KIO remains the backend for remote and unsupported protocols;
- Graceful handling of disk-full, device-loss, and I/O errors without crashing;
- Vertical histogram speed scale and file size tooltip display;
- Full automated regression: **842/842 assertions**;
- Full manual KDE/CachyOS validation completed successfully;
- No user configuration migration needed.

## 0.22.0 — Advanced Transfer Window

- Automatic dedicated details window for active file operations;
- Consolidated overview of multiple concurrent tasks with dynamically adjusted window height;
- Current file, source/destination summary, total size, progress percentage, current and average speed, and ETA;
- Speed graph with bounded sample history;
- Collapsible transfer details and individual task cancellation;
- Automatic window close upon completion of all active tasks;
- Compact in-window transfer panel continues to log operation history;
- No user configuration migration needed.

## 0.21.0 — Architecture Refactoring & Stabilization

- Core components decoupled from `src/thispcview.cpp` into modular units;
- Reduced code duplication between Primary pane and Split View;
- Preserved settings compatibility and session restore integrity;
- Added and expanded tests for panes, tab Drag & Drop, `PropertiesDialog`, search, and `FileActions`;
- Final automated regression suite: 577 assertions passing;
- Comprehensive manual KDE/CachyOS test completed without regressions;
- No user configuration migration required.

## 0.20.0 — Quick Access / Favorites / Recent

- New `Quick Access` section in the left sidebar;
- Pin and unpin directories via context menu;
- Reorder pinned folders using Drag & Drop;
- Persistent storage of pinned folders and custom order via `QSettings`;
- New `Recent` section displaying recently visited locations;
- Persistent recent locations history across application sessions;
- Preserved stable filename layout introduced in 0.19.0.4.

## 0.19.0.4 — Stable Filename Layout in Icon View

- Fixed, rigid grid geometry in IconMode (`gridSize` + `uniformItemSizes`);
- Selecting an item never alters `sizeHint()` or displaces rows beneath;
- Text layout rendered via `QTextLayout` with `WrapAtWordBoundaryOrAnywhere`;
- Maximum 2 lines in standard mode (last line elided cleanly);
- Maximum 4 lines in "Full Names" mode while preserving uniform tile heights;
- Full unclipped filename of selected item rendered as a viewport callout after base `paintEvent`;
- Completely removed legacy QLabel overlay;
- No alterations to ListMode or DetailsMode.
