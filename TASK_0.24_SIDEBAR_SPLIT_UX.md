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
