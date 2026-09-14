# HANDOFF_STATUS.md

- Current release candidate: **0.23.0 — Native local transfer engine**.
- User-confirmed stable features through **0.23.0** include Undo/Redo, full Drag & Drop, Split View parity, session restore, stable full-name layout, Quick Access/Favorites, recent locations, the detailed transfer window and the native local transfer engine.
- Supported local files, directories, multiple sources and symbolic links use asynchronous native planning/execution with exact Pause/Resume, aggregate progress and safe `.thispc-part` publication.
- Native single-file conflicts support Overwrite/Rename/Skip. Permissions, timestamps and literal symlink targets are preserved.
- Native file/tree copy and move integrate with KDE-serial-ordered Undo/Redo; cross-filesystem moves are verified before source removal.
- KIO remains the backend for remote URLs and unresolved/unsupported local cases, without exposing misleading exact-pause capability.
- The detailed operation window keeps paused operations visible, shows current/average speed, ETA and a bounded graph with a vertical speed scale. File tooltips show file size.
- Automated regression is clean at **842/842 assertions** and `git diff --check` is clean.
- Full manual KDE/CachyOS acceptance is complete: single-file and tree Pause/Resume, cancellation, concurrent jobs, conflicts, symlinks, metadata, Btrfs→NTFS move, file/tree Undo/Redo, disk-full, device-loss and SHA-256 integrity all passed.
- Current roadmap task after release preparation: **0.24.0 — Resizable and scrollable sidebar**.
- `reference/failed-0.17.0.4` and handoff materials remain historical diagnostics only.
