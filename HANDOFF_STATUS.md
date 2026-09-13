# HANDOFF_STATUS.md

- Current release candidate: **0.22.0 — Advanced transfer window**.
- User-confirmed stable features through **0.22.0** include Undo/Redo, full Drag & Drop, Split View parity, KIO conflict handling, session restore, stable full-name layout, Quick Access/Favorites, recent locations and the detailed active-operation window.
- The detailed window opens automatically, combines concurrent active operations, sizes itself to their count, shows speed/average/ETA/graph and closes after the last task. The compact top-right popup retains completed history.
- Manual KDE testing confirmed the responsive details toggle, readable progress, automatic open/close and multi-operation layout.
- A KIO-based pause prototype was rejected and removed: `KIO::CopyJob::suspend()` does not reliably stop local worker I/O at the displayed byte position.
- Automated validation includes a dedicated `OperationManager` suite in addition to the existing pane, tab DnD, Properties, search and FileActions suites.
- Next roadmap task: **0.23.0 Native local transfer engine**, providing chunked local I/O and genuine pause/resume while retaining KIO for remote/appropriate operations.
- `reference/failed-0.17.0.4` and handoff materials remain historical diagnostics only.
