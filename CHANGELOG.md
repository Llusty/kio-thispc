[English](CHANGELOG.md) | [Polski](CHANGELOG.pl.md)

# Changelog

## 0.35.0 — Drives & Devices

- **Stage 1:** Dynamic drive and storage device discovery backed by KDE Solid; event-driven `SolidDeviceMonitor` with 250 ms debouncing reacts to hotplug and mount transitions without periodic polling or flickering "Refreshing…" status.
- **Stage 2:** Detection and presentation of unmounted removable volumes on the `thispc:/` home view with a dedicated `drive-removable-media` icon and "Unmounted" status label; `DeviceMountController` enables on-demand mounting (click or Enter) with automatic navigation into the mounted folder; the sidebar displays mounted volumes only.
- **Stage 3:** Full asynchronous lifecycle management for removable devices (`DeviceRemovalController`):
  - "Unmount" action performs a clean filesystem-only unmount exclusively via native asynchronous QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount`, keeping device power on and retaining volume visibility;
  - "Safely Remove" action coordinates unmounting of all mounted partitions on the given physical drive, followed by asynchronous `org.freedesktop.UDisks2.Drive.PowerOff`;
  - Safely Remove functionality works seamlessly for volumes that are already unmounted;
  - Media ejection support ("Eject") on optical drives with `canEject` capability via `Solid::OpticalDrive::eject()`;
  - Safe redirection of Primary and Split panes away from unmounted or detached mountpoints back to `thispc:/`, without altering the state of unaffected panes.
- **Final automated regression:** 35 test suites / 8562 assertions PASS; build, CLI `thispc-view 0.35.0`, and manual acceptance PASS.

## 0.34.0 — Explorer UX / View & Navigation Polish

- **Stage 2:** `--version` flag executes before GUI initialization and exits with code 0; Split View features address/breadcrumb toggling parity; controlled spacing for drive cards.
- **Stage 3:** Hidden files and directories are visually dimmed without compromising selection, hover, or focus legibility.
- **Stage 4:** F2, context-menu Rename, and slow second click share unified inline rename handling; Batch Rename remains a separate flow.
- **Stage 5:** Keyboard navigation routing added with stable current/focus handling on `thispc:/` cards.
- **Stage 6:** View mode, icon size, sorting, and grouping profiles persist per URL; Apply/Remove to subfolders utilizes snapshot ancestor rules without tree scanning.
- **Stage 7:** Icons mode provides nine size increments, menu controls, and `Ctrl++`, `Ctrl+=`, and `Ctrl+-` shortcuts routed to the active pane.
- Final Split View visual parity polish shares header semantics and view metrics, maintains deterministic Details columns, symmetrical per-URL profiles, and practically identical runtime address-bar geometry.
- Non-blocking follow-up: ~1 px / sub-pixel vertical alignment polish between header/bar and active-pane accent line relative to gray frame.
- Non-blocking future polish: change `PointingHandCursor` on `thispc:/` folder and drive cards to standard `ArrowCursor`, consistent with normal directory listings.
- Removable media discovery deferred to 0.35 Drives & Devices; Split separator double-click to restore 50/50 remains a future polish idea.
- Final automated regression: 32 test suites / 8317 assertions PASS; build, final CLI `thispc-view 0.34.0`, and isolated install smoke test PASS.

## 0.33.0 — Architecture Cleanup Continuation

- Continued behavior-preserving modularization without introducing new user-facing features.
- Extracted `LocationPresentation`, `NavigationHistory`, `PreviewCoordinator`, `ActionStateController`, `DirectoryListingCore`, `PaneMenuController`, and `DriveHomeCoordinator` from window and pane code paths.
- Primary pane and Split View share `DirectoryListingCore` while retaining independent state and existing KIO semantics.
- Resolved sidebar context-menu lifetime crash during drive refreshes, eliminating dangling action references.
- Fixed inline rename activation: fast double-click opens items reliably, while slow second click enters inline rename.
- Final regression after inline rename fix: 31 test suites / 7852 assertions PASS.
- Common `BrowserPane` and deferred UX items remain outside 0.33.0 scope.

## 0.32.0 — Architecture Cleanup

- Completed behavior-preserving architectural cleanup without adding user-facing features.
- Decoupled Split Sync into acyclic models, executor, and dialogs, preserving 0.31.0 semantics.
- Extracted shared application widgets and stylesheet; hardened test harness seams.
- Introduced explicit active-pane contract via `PaneContext` and `PaneAdapter`.
- Extracted `TabController`, `SearchUiController`, and `SelectionMenuController` from `ThisPcWindow`.
- Extracted `PrimaryBrowserPane` widget containing URL state, KIO listing, rendering, and thumbnail caching for the primary pane.
- Regression fixes for hover/focus and a crash identified during component extraction; declared functional scope preserved.
- Common `BrowserPane` (Stage 9) deliberately deferred to 0.33; not part of 0.32.0.
- Final regression after Stage 8: 26 test suites / 6662 assertions PASS.
- Post-0.31.0 arithmetic correction: full regression covers 25 suites and 6569 assertions (6278 previous + 291 `split_compare`), not 6618. Tag and commit remain unchanged.

## 0.31.0 — Split View Synchronization

- Safe folder comparison and synchronization across Split View panes (`Ctrl+Alt+C`, Tools / View menu).
- **Stage 1 (Compare Panes):** Asynchronous comparison of immediate children in left and right Split View panes (direct children only, no recursion); classification into Same, Only Left, Only Right, and Changed based on KIO metadata (size, mtime, entry type); case-preserving Unicode name matching; virtual paths (`thispc:/`, `thispcsearch:/`) safely blocked; 100% read-only.
- **Stage 2 (Sync Plan Preview):** Deterministic sync plan generator for Left → Right and Right → Left directions; plan classes: No Action (NoAction), Copy (CopyFile), Update (UpdateFile), Conflict, Unsupported; zero deletion (no Delete / no mirror); no recursive folder synchronization; modal plan preview dialog (`SyncPlanPreviewDialog`) tied to generation tokens.
- **Stage 3 (Safe Execution):** Safe asynchronous plan execution via `LocalFileCopyJob`; only `CopyFile` and `UpdateFile` operations executed; zero deletion; no recursion; conflicts and unsupported items untouched; atomic preflight before mutating each file; race-condition-free Cancel lifecycle with worker thread completion waiting; automatic re-comparison upon completion.
- Verification: Focused `split_compare` 291 assertions PASS; focused pane actions 741 assertions PASS; full regression 25 test suites / 6569 assertions PASS; manual Cancel acceptance test on 20,000 files PASS.

## 0.30.0 — Grouping and per-folder view settings

- Saved Icons/List/Details/Compact mode per normalized folder URL, shared across primary pane, Split View, tabs, and session restore.
- Organized View menu, four icon sizes (96, 64, 48, 32 px), and dense Compact view mode.
- Grouping by Type, Modification Date, and Size in Icons/List/Details/Compact with non-selectable presentation headers excluded from file actions.
- Date grouping uses local calendar boundaries and recalculates after midnight; Size grouping uses KIO size metadata without recursive scanning.
- Sort key and direction preserved within groups; missing metadata and directories handled in distinct groups.
- Fixed selection outline in Icons and removed stale hover state in grouped views.
- Manual acceptance of Type/Date/Size across all four view modes confirmed. Full regression: 24 test suites / 6278 assertions PASS.

## 0.29.0 — Intelligent New Item Naming

- Files, folders, and template items propose the first available name before displaying editable dialogs; numbering fills gaps while preserving extensions, hidden prefixes, and Unicode.
- Post-dialog preflight blocks detected collisions. Creation operations never use `Overwrite` or `Resume` and preserve Undo/Redo registration.
- Remote KIO uses two asynchronous listings, preserves source pane target, and fails closed on listing errors.
- Details and limitations documented in `RELEASE_NOTES_0.29.0.md`. Manual acceptance confirmed.

## 0.28.0 — Batch Rename

- Batch rename with preview: prefix/suffix, numbering, search/replace, case/extension, and regex.
- Preflight collision and inode-swapping checks; active Split View pane support.
- Qualifying local linear batches feature unified session Undo/Redo and production journal v2; process crash recovery resumes recorded Execute, Undo, or Redo direction upon restart.
- Swaps and cycles preserve payloads and offer session Undo/Redo; automatic crash recovery for cycles remains disabled.
- Full details in `RELEASE_NOTES_0.28.0.md`.

## 0.27.0 — Quick Look on Space

- Large, temporary Quick Look preview opened with Space without launching associated applications.
- Pressing Space or Esc closes the preview.
- Operates exclusively from active file view, does not steal focus, and follows selection and active Split View pane changes.
- Search, `Ctrl+L`, inline rename, popups, and dialogs preserve standard Space key handling.
- Shared Preview Pane backend provides identical formats, limits, messages, and stale-result protection without duplicating parsers.
- Full regression: 22 test suites / 3598 assertions PASS; all 9 manual KDE/CachyOS test cases confirmed.
- Focused: Quick Look 19, Preview Pane 106, pane routing 269, and Split Layout 99 assertions PASS.

## 0.26.0 — Preview Pane

- Preview panel toggled with `Alt+P`.
- Scaled image preview fitting available pane space.
- Plain text, Markdown, JSON, and XML preview.
- Asynchronous first-page PDF preview via QtPdf without UI blocking; opaque white background compositing fix.
- Audio/video metadata via TagLib.
- Image EXIF metadata via Exiv2 without replacing the image.
- Safe archive manifest preview for ZIP, 7z, tar, and tar.gz via libarchive without extracting.
- Directory summaries and Split View right-pane breadcrumb fixes.
- Full regression: 20 test suites PASS, Preview Pane 106 assertions PASS, Split Layout 99 assertions PASS.

## 0.25.0 — Archives + richer New menu

- Archive creation for ZIP, 7z, and tar.gz via asynchronous libarchive job.
- Retained "Send to → ZIP", added 7z/tar.gz formats.
- Safe publication without overwriting, progress/cancel/error handling, and tests.
- Details in `docs/ARCHIVE_STAGE3.md`.

## 0.24.0

- Symmetrical Split View panes: independent breadcrumbs, equal navigation, and single shared toolbar/sidebar routed to the active pane.
- Removed asymmetric right-pane mini-toolbar while preserving Swap panels and close actions.
- Drag & Drop for files and folders to supported sidebar destinations via `FileActions`.
- Vertical sidebar scrolling, resizable width (205–480 px), and persistent user width setting.
- Long sidebar labels elided on the right with full names in tooltips.
- Independent Search state, results, filters, progress, and jobs for both panes.
- Full `thispc:/` / This PC card presentation in both panes.
- Full regression: **1022/1022 assertions PASS**; manual KDE/CachyOS acceptance PASS.

## 0.23.0

- Native local copy/move engine with exact Pause/Resume at chunk boundaries.
- Safe publication via `.thispc-part`, cleanup on cancellation, and controlled write-error, disk-full, and device-loss handling.
- Native Overwrite/Rename/Skip conflict handling for single files.
- Asynchronous planning and execution for directories, multiple sources, and symbolic links.
- Permissions, nanosecond timestamps, and literal symlink targets preserved.
- Cross-filesystem Move publishes data before removing sources and integrates with Undo/Redo.
- Unified command ordering between native tree history and KIO history.
- Operation window supports real Pause/Resume, vertical speed graph scaling, and file size in tooltips.
- Full regression: **842/842 assertions PASS**; manual acceptance confirmed (including SHA-256 integrity).

## 0.22.0

- Non-blocking detailed active-operations window opens automatically on task start.
- Multiple simultaneous operations merge into a single dynamic-height window with scroll limits.
- Displays current file, source/destination summary, processed data, prominent percentage, speed, and ETA.
- Speed-over-time graph with bounded 120-sample history.
- Collapsible details with persistent state across refreshes.
- Unified task cancellation across detail window and compact panel.
- Detail window closes after the last active operation; compact panel preserves history.
- Full regression with dedicated `OperationManager` test suite.

## 0.21.0

- Conservative architectural refactor of `thispc-view` without intentional behavior changes.
- Extracted: `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController`, and `FileActions`.
- Shared helpers and types unified in `browsercommon.h`.
- Reduced `src/thispcview.cpp` from ~14,178 to ~7,250 lines.
- Reduced view and operation duplication between primary pane and Split View.
- Added test coverage for `PropertiesDialog`, `SearchController`, and `FileActions`.
- Final automated validation: 577 assertions PASS; full manual KDE/CachyOS testing completed without regressions.

## 0.20.0

- Added **Quick Access** / Favorites section to left sidebar.
- Pin and unpin folders via context menu.
- Drag & Drop reordering of pinned folders with order saved in `QSettings`.
- Added **Recent** section tracking visited locations across sessions.
- Stable icon view name layout from 0.19.0.4 preserved.

## 0.19.0.4

- Rigid, uniform tile grid geometry (`setGridSize` + `setUniformItemSizes(true)`) in IconMode — selecting an item never changes `sizeHint()` or shifts neighboring rows.
- Filename text rendered via `QTextLayout` with `QTextOption::WrapAtWordBoundaryOrAnywhere`; proper wrapping for whitespace-free names (underscores, dashes, numbers, Unicode).
- Normal icon mode: maximum 2 lines of text with ellipsis `…` on the second line.
- Full Names mode: uniform tile height with up to 4 lines of text.
- Full name of selected item displayed as a viewport-level callout in `DirectoryListWidget::paintEvent()` after base `QListWidget::paintEvent()`.
- Callout anchored to `visualItemRect()`, bounded by viewport edges, rendered above items without mouse interception.
- Removed `QLabel#selectedNameOverlay`, legacy overlay logic, and magic constants (124 px text width).
- ListMode and DetailsMode unchanged.

## 0.19.0.3

- Removed dynamic height expansion on selected tiles.
- Full name of selected items displayed as an overlay without moving adjacent rows.
- Full Names mode features fixed tile height for three lines of text.
- Overlay tracks scrolling, resizing, and selection changes.

## 0.19.0.3

- Improved selected item full name sizing: `sizeHint` consults `selectionModel` so long names receive adequate height for wrapping.
- Geometry correction on session restore: tab bar and active pane repositioned after initial event-loop pass.
- Tab restoration, Split View, and Full Names toggle preserved.

## 0.19.0.1

- Hotfix for full names on selected items in icon/list view.
- Custom delegate paints items directly via Qt style, ensuring `ElideNone` and `WrapText` are not overridden by base delegate.
- Very long names expand across multiple lines as needed upon selection.

## 0.19.0

- Added session restore: tabs, active tab, and navigation history.
- Split View state saved per tab: location, view mode, and sort order.
- Saved splitter state restores pane widths.
- Explicit path launch argument bypasses previous session restoration.
- Selected item in icon view expands full name.
- Added persistent `Full Names` toggle on action bar and `Restore Previous Session` in View menu.

## 0.18.0

- Interactive KIO conflict handling enabled for copy and move operations.
- Conflicts handled during paste, Drag & Drop, and "Send to".
- Native KIO dialog offers overwrite, skip, rename / suggest name, and "apply to all" options.
- Source and destination sizes and timestamps provided to conflict dialogs.
- Renaming to an existing filename utilizes the same conflict handling mechanism.
- Conflict dialog cancellation recognized cleanly without spurious error dialogs.

## 0.17.0.5

- Integrated Stages A/B from local development branch.
- Split View parity: shortcuts, toolbar, and context menu operate on the active pane.
- Directory ↔ directory and Primary ↔ Split Drag & Drop.
- `ExplorerTabBar` with 650 ms hover tab switching and Drop on tabs.
- `Qt::KeyboardModifiers` routed through Drop flow: `Ctrl+drag` = Copy, `Shift+drag` = Move.
- Default drag without modifiers suggests Move on same storage and Copy across different storages.
- Normal drop displays Copy/Move context menu.
- Cycle protection against copying/moving folders into themselves or descendants.
- Registered with `KIO::FileUndoManager`.

## 0.16.0.1

- Hotfix for Undo/Redo on rename operations.
- Rename registered via `KIO::moveAs()` + `FileUndoManager::recordCopyJob()`.
- Fix covers renaming from main view and Properties dialog.

## 0.16.0

- Native Undo/Redo backed by `KIO::FileUndoManager`.
- Dedicated Undo and Redo toolbar icons; shortcuts `Ctrl+Z`, `Ctrl+Y`, and `Ctrl+Shift+Z`.
- Operation history covers copy, move, rename, Trash, folder creation, and file creation.
- Moving to Trash registered as `FileUndoManager::Trash` for restoration.
- Copy/move registered with `KIO::CopyJob` metadata for safe reversal.
- Undo/Redo actions update availability and descriptions dynamically.
- Directory panes refresh automatically after Undo/Redo.
- Minimum KDE Frameworks version raised to 6.17 for native `redo()`.

## 0.15.4

- Operation popup offset to the right; overlaps main window only when available margin is ~96 px.
- Automatic bounds clamping to available desktop geometry.
- Legible version label in bottom-right corner using window text palette.

## 0.15.4

- Rebuilt operation popup from `QMenu/QWidgetAction` to custom `Qt::Popup`.
- Dynamic height calculated from active operation cards.
- List expands up to ~560 px before vertical scrolling engages; width ~410 px.
- Header and footer remain pinned and visible.
- Added `v0.15.4` label in bottom-right status area.

## 0.15.2

- Dynamic height for operation popup expanding up to ~560 px with scrollbar cap.

## 0.15.1

- Replaced fixed bottom operation pane with compact popup panel.
- Added operation status icon on toolbar with active task counter badge.
- Popup contains active and completed operation history with clearing option.

## 0.15.0

- Added collapsible bottom file operation manager panel.
- Active KIO jobs displayed as task list with individual and aggregate progress bars.
- Displays processed bytes, total size, speed, and source/destination descriptions.
- Single-operation and cancel-all support via `KJob::kill(KJob::EmitResult)`.
- Status distinction: Completed, Canceled, Error.

## 0.14.2.3

- Removed premature NTFS/fuseblk warnings before executing changes.
- Filesystem types `fuseblk`, `ntfs`, and `ntfs3` not treated as automatically restricted.
- Permission changes evaluated via real `chmod` and read-back verification.
- Helpful mount options hints shown only upon rejection or mode discrepancy.

## 0.14.2.2

- Compilation fix for `QT_USE_QSTRINGBUILDER`.

## 0.14.2.1

- Compilation fix for literal newline sequences.

## 0.14.2

- NTFS/ntfs3/fuseblk permissions tab unlocking via `admin://`/PolicyKit.
- Recursive folder permission changes via `KIO::ChmodJob`.

## 0.14.1

- Unified styling and behavior for Split View.
