# 0.24.0 — Sidebar & Split View UX

## Stage 1 — equal panes and active-pane routing

Status: implemented, covered by the full automated regression suite and
accepted manually in KDE.

The first stage is a conservative Split View rebuild on top of the existing
`PaneId`/`PaneContext` routing. It does not change file-operation backends,
session data, search semantics or the `thispc:/` worker.

Acceptance requirements:

- both panes are equal participants;
- Split View always shows both address/breadcrumb sections in one row, with
  each section exactly matching the width and horizontal position of its pane;
- clicking either breadcrumb activates its pane;
- the shared toolbar and sidebar preserve the active pane;
- normal sidebar activation navigates the active pane, while **Open in other
  pane** targets the opposite pane;
- `Ctrl+L`, `F6`, Back, Forward, Up, Refresh, View and Sort work symmetrically;
- the secondary mini-toolbar is removed without losing Back/Forward/Up,
  View/Sort, pane swap or Split View close behavior;
- the active pane and its address section use a subtle palette-derived accent.

Implementation keeps one shared command/navigation toolbar. The primary
address row lives above the primary browser content and the split address row
lives above the split browser content, inside the same `QSplitter` children.
Their geometry therefore follows the pane divider directly rather than being
estimated or synchronized by a second splitter.

Deferred to later 0.24.0 stages:

- resizable and scrollable sidebar;
- full Search and `thispc:/` presentation parity between panes.

Automated coverage belongs in `tests/pane-actions.cpp`, including address/pane
geometry after divider resizing, active-pane preservation, sidebar routing,
keyboard navigation and removal of the secondary controls.

The complete automated suite passed **862/862 assertions** after Stage 1.

## Stage 2 — Drag & Drop into sidebar destinations

Status: implemented and covered by the full automated regression suite;
manual KDE visual/interaction acceptance remains.

Files and folders can be dropped onto real destination entries in Places,
Devices and Quick Access. Section headers, Recent entries and virtual
locations are not Drop targets. Valid targets receive palette-derived hover
feedback without changing widget geometry.

The sidebar forwards each accepted Drop once through
`ThisPcWindow::handleDroppedUrls()`. Copy/Move policy, cycle prevention,
conflict handling, the native/KIO decision and operation tracking therefore
remain owned by the existing `FileActions` path. Local directories are not
pre-filtered with `isWritable()`, preserving NTFS and administrator fallback
behavior. Dragging and dropping on the sidebar does not activate either pane.

Quick Access reorder MIME has priority over URI data, so an existing reorder
drag cannot become a file transfer even when both MIME formats are present.
Pinning a folder by dropping it on the Quick Access section header remains
deferred because the header continues to be a non-destination toggle.

Automated coverage belongs in `tests/sidebar-drag-drop.cpp`, including target
policy, feedback geometry, MIME priority, exactly-once dispatch, device and
Quick Access targets, and active-pane preservation. The complete automated
suite passes **905/905 assertions** after Stage 2.

## Stage 3 — resizable and scrollable sidebar

Status: implemented and verified by the full automated regression suite;
manual KDE visual/interaction acceptance remains.

The sidebar owns a vertical scroll area, so expanding sections or rebuilding
dynamic entries does not increase the main-window height. Horizontal scrolling
is disabled and long labels remain constrained to the selected sidebar width.
Quick Access, Recent and Places buttons elide overflowing text with an ellipsis
and expose the complete label in a tooltip. Device names also elide while
retaining their existing detailed tooltip. Elision affects painting only, so
the full label and destination remain available after resizing or rebuilding.
The sidebar/file-view boundary is a draggable splitter with a 205 px minimum,
235 px default and 480 px maximum.

The user's divider choice is stored as `sidebar/width`. Automatic compression
caused by narrowing the window does not replace that preference. Sidebar entry
rebuilds preserve the vertical scroll position when it remains valid, and
scrolling during a drag clears hover feedback from the previous Drop target.

The inner Split View splitter, pane routing and sidebar Drag & Drop remain
independent. Search and `thispc:/` presentation are outside this stage.

Automated coverage belongs in `tests/sidebar-layout.cpp`, with additional
regressions in `tests/sidebar-drag-drop.cpp` and `tests/pane-actions.cpp`.
Layout coverage uses the application stylesheet and compares rendered labels
with native Qt widgets containing the expected visible text. It also checks
tooltips, rebuilds, width persistence, compression and elision after resizing.

The focused sidebar/pane suites pass **283/283 assertions** (43 layout,
45 sidebar Drag & Drop and 195 pane assertions). The complete automated suite
passes **952/952 assertions** across 13 suites. Removing button elision in an
isolated test copy fails the rendered-label regression as expected.

## Stage 4 — Search and This PC active-pane symmetry

Status: implemented and covered by focused automated regression tests; manual
KDE visual and interaction acceptance remains.

The shared Search box, scope menu, filters and Stop action now resolve through
the active pane. Each pane retains its own Search text, scope and filters, and
owns an independent asynchronous `SearchController`. Starting, refreshing,
filtering or canceling Search in one pane therefore leaves the other pane's
location, draft, results and worker unchanged. Search URLs remain in the owning
pane's history and tab state, including their base, scope and filter parameters.

The secondary pane now presents `thispc:/` with the same folder and drive card
layout as the primary pane. Both pages are built by one shared function and use
the same drive inventory and refresh backend. Folder and drive cards navigate
their owning pane. Back, Forward, Up, Refresh, `Ctrl+L`, `F6`, pane swap,
sidebar activation and the Search breadcrumb preserve active-pane routing.

Search results in either pane support Icons, List and Details, including the
result Location column, active filters, progress, partial results after Stop
and friendly Search/This PC breadcrumb and address presentation. Closing Split
View cancels its hidden Search worker; reopening restores the stored Search URL
and reloads those results.

Focused coverage belongs in `tests/search-controller.cpp` and
`tests/pane-actions.cpp`, with the existing sidebar suites retained as routing
and layout regressions. Stage 4 does not change transfer backends, filename
layout behavior or release version metadata.
