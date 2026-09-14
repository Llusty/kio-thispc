# 0.24.0 — Sidebar & Split View UX

## Stage 1 — equal panes and active-pane routing

Status: implemented and covered by the full automated regression suite;
manual KDE visual/interaction acceptance remains.

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

- Drag & Drop into sidebar destinations;
- resizable and scrollable sidebar;
- full Search and `thispc:/` presentation parity between panes.

Automated coverage belongs in `tests/pane-actions.cpp`, including address/pane
geometry after divider resizing, active-pane preservation, sidebar routing,
keyboard navigation and removal of the secondary controls.

The complete automated suite passes **862/862 assertions** after Stage 1.
