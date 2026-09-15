# HANDOFF_STATUS.md

- Current release candidate: **0.24.0 — Sidebar & Split View UX**, user-confirmed on `main`; final release commit and `v0.24.0` tag are pending.
- Stable behavior from 0.23.0 remains intact: native local transfer engine, exact Pause/Resume, `.thispc-part`, conflicts, metadata, cross-filesystem Move and ordered native/KIO Undo/Redo.
- Split View now uses equal panes with aligned per-pane breadcrumbs and one shared toolbar/sidebar routed to the active pane.
- Sidebar Drag & Drop uses the existing `FileActions` path and preserves active-pane state.
- Sidebar is vertically scrollable and resizable from 205–480 px with persisted preferred width; long labels use right-side ellipsis and full-name tooltips.
- Search has independent query, filters, results, progress and jobs for both panes, and both panes provide the full `thispc:/` / This PC card view.
- Shared Search controls, navigation, `Ctrl+L`, breadcrumbs, View and Sort consistently follow the active pane.
- The complete 0.24.0 suite passes **1022/1022 assertions** and `git diff --check` is clean.
- Full Stage 1–4 KDE/CachyOS manual acceptance passed.
- Next roadmap milestone: **0.25.0 — Archives + richer New menu**.
- `reference/failed-0.17.0.4` and handoff materials remain historical diagnostics only.
