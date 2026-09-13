# HANDOFF_STATUS.md

- Current package: **0.20.0**.
- User-confirmed stable features through **0.20.0**: Undo/Redo, full Drag & Drop, Split View parity, KIO conflict handling, session restore, stable full-name layout, Quick Access/Favorites and recent locations.
- **0.19.0.4** stabilizes IconMode filename layout with fixed row geometry and the selected full-name callout.
- **0.20.0** adds persistent pinned folders, pin/unpin, Drag & Drop reorder, Quick Access in the sidebar and persistent recent locations.
- Optional recent files were intentionally deferred beyond 0.20.0.
- Current roadmap task after 0.20.0: **0.21.0 Architecture refactor + stabilization**.
- Local working tree: steps **1–10** applied, including the step 7 lambda and step 8 QObject::connect hotfixes. Step 9 extracts SearchController; step 10 extracts FileActions. These changes are not yet committed or released.
- Steps 8, 9 and 10 passed a clean local Debug build. Automated pane, tab DnD, PropertiesDialog, search and real FileActions/Undo checks are documented in `TASK_0.21_REFACTOR.md`.
- Next: **final stabilization and manual KDE acceptance**. Step 10 passed 577 automated assertions, including real KIO conflicts and Trash Undo/Redo. Keep version 0.20.0 until release preparation.
- `reference/failed-0.17.0.4` and handoff materials remain historical diagnostics only.
