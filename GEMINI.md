# kio-thispc — instructions for Gemini CLI

You are working on **kio-thispc / thispc-view**, a native Qt6/KF6 file manager for KDE Plasma that aims to provide a Windows Explorer-like workflow while using KDE/KIO infrastructure.

## Current state

- Current working version: **0.19.0.3**.
- Last broadly stable baseline before 0.19 UI/session work: tag **v0.18.0-stable**.
- Session restore in 0.19 works in manual Plasma testing: tabs, active tab, split view and last active view are restored correctly.
- Drag & Drop, Split View parity, Undo/Redo and conflict handling are already implemented and manually tested in earlier versions. Do not regress them.
- The current known bug is the **icon-view filename layout/painting** described in `TASK_0.19_NAME_LAYOUT.md` and illustrated in `handoff/screenshots/`.

## Mandatory workflow

1. Read the current task and relevant code before editing.
2. Diagnose the real Qt item-view geometry/painting behavior before adding another workaround.
3. Keep changes minimal and localized unless a larger refactor is clearly justified.
4. After every meaningful implementation step, run `./scripts/build.sh`.
5. Fix all compile errors caused by your changes before proceeding.
6. Run existing tests where relevant and add focused tests when practical.
7. Run `git diff --check` before declaring work complete.
8. Do **not** bump the version or commit unless the user explicitly asks.
9. Clearly report what could not be tested automatically and needs manual Plasma verification.

## Safety rules

- Never run destructive commands against `/mnt/c`, `/mnt/d`, `/mnt/e`, `/mnt/f`, `/mnt/g` or other mounted Windows/NTFS volumes.
- Never run mass `chmod -R` / `chown -R` on Windows/NTFS partitions.
- Do not use `sudo` unless the user explicitly approves the exact command and it is truly necessary.
- Prefer temporary directories under `/tmp` or the project tree for tests.
- Do not overwrite or reset user work with `git reset --hard`, `git clean -fd`, checkout-discard operations, or force pushes without explicit approval.

## Architecture constraints

- Preserve KIO as the backend for file operations.
- Preserve `KIO::FileUndoManager` behavior.
- Preserve the operation popup/manager, tabs, Split View, context menus, Properties, admin integration, search, Send To and printing.
- The main source is large (`src/thispcview.cpp`); avoid unrelated churn.
- KDE/Qt behavior under real Plasma matters more than synthetic tests alone.

## Project context

@./PROJECT_CONTEXT.md
@./DEVELOPMENT.md
@./TEST_CHECKLIST.md
@./TASK_0.19_NAME_LAYOUT.md

For future work, consult `ROADMAP.md`.
