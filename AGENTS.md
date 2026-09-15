# AGENTS.md — kio-thispc / thispc-view

## Mission
Develop `thispc-view`, a lightweight native Qt/KDE file manager inspired by Windows 11 Explorer while retaining KDE/KIO integration and the `thispc:/` KIO worker backend.

## Current baseline
- Work from the local `main` checkout. The current stable release is **0.24.0 — Sidebar & Split View UX**, committed and tagged as `v0.24.0`.
- Do **not** start new implementation from the failed 0.17.0.4 code.
- A reference branch named `reference/failed-0.17.0.4` and a patch under `handoff/` preserve the failed Drag & Drop attempt for inspection only.
- The next planned feature is **0.25.0 — Archives + richer New menu**. `TASK_0.24_SIDEBAR_SPLIT_UX.md`, `TASK_0.21_REFACTOR.md` and `TASK_0.17_DRAG_DROP.md` remain regression/historical references.

## User environment
Target the user's actual local environment first:
- Linux / KDE Plasma 6
- Qt 6
- KDE Frameworks 6; latest observed build log: **KF6 6.30.0**
- GCC **16.2.1**
- CMake + ECM
- Arch-like distribution using `pacman`
- shell: fish
- common workspace: `~/Pobrane/kio-thispc`

Harmless configure output seen on this machine:
- `fatal: to nie jest repozytorium gita...` when building an unpacked source without `.git`
- missing `WrapVulkanHeaders`
These are not build blockers unless a later error says otherwise.

## Mandatory development loop
For any code change:
1. Inspect the existing implementation before editing.
2. Prefer the smallest coherent change; do not rewrite unrelated working code.
3. Configure and compile locally.
4. If compilation fails, fix it before declaring the task complete.
5. Run the relevant smoke tests from `TEST_CHECKLIST.md`.
6. Preserve existing behavior in the primary pane and Split View.
7. Update the version consistently only when preparing a release.
8. Summarize exactly which files changed and why.

Use:
```bash
./scripts/build.sh
```
For an installable user test:
```bash
chmod +x install.sh
./install.sh
```

## Release discipline
When releasing a version, update all relevant places consistently:
- `CMakeLists.txt`
- version string/comment in `src/thispcview.cpp`
- visible status-bar version in `src/thispcview.cpp`
- `install.sh`
- `README.md`
- `CHANGELOG.md`
- `UPDATE.md`

Keep the user's versioning convention: feature release `0.X.0`, hotfixes `0.X.0.1`, `0.X.0.2`, etc.

## Compatibility / architecture rules
- Use Qt6/KF6 APIs, not Qt5/KF5 compatibility assumptions.
- KIO remains the preferred backend for file operations.
- `KIO::FileUndoManager` is the Undo/Redo authority for supported file operations.
- Keep `thispc:/` as a separate KIO worker backend; the GUI must not destroy that abstraction.
- Avoid blocking the UI thread for directory scans, thumbnails, transfer progress, hashing, or future folder-size work.
- Both primary and split panes should eventually share behavior instead of maintaining divergent duplicate logic.
- `src/thispcview.cpp` is already very large; avoid making the architecture worse. Refactor only when it clearly reduces duplication/risk.

## File-operation safety
Never perform destructive mass operations merely to make a test pass.
In particular:
- never suggest or run mass `chmod -R` / `chown -R` on the user's Windows, game, or NTFS partitions;
- never modify `/etc/fstab`, NTFS UserMapping, ACLs, or mount options unless the user explicitly asks for that task;
- do not permanently delete user files as an automated test;
- use temporary test directories/files created specifically for testing;
- prefer Trash over permanent deletion in manual test instructions.

Known NTFS background is documented in `docs/DECISIONS.md`; do not "simplify" it away.

## UI principles
- Dark/light appearance must follow the KDE/Qt palette; avoid hard-coded text colors that become unreadable.
- Preserve the current Windows-Explorer-inspired layout without trying to clone Windows pixel-for-pixel.
- Keep interactions unsurprising for KDE users and interoperable with Dolphin/Plasma where practical.
- Long-running operations must remain cancellable and must not freeze the UI.
- Do not expose `KIO::CopyJob::suspend()` as exact local pause; manual 0.22 testing proved that worker I/O/progress can continue and be buffered. Real pause belongs to the chunked 0.23 local transfer engine.

## Drag & Drop rules for 0.17
Do not guess from screenshots alone. Verify actual Qt event flow on the user's runtime.
Before implementing a custom event filter, inspect:
- `QAbstractItemView` drag/drop configuration,
- `viewport()` acceptance,
- model/view drop behavior,
- item flags / supported actions,
- whether events arrive at the view or viewport,
- whether an existing handler consumes/ignores the event.

Acceptance criteria are in `TASK_0.17_DRAG_DROP.md`.
