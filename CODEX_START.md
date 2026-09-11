# CODEX_START.md

## First session
Open a terminal in the extracted repository and start Codex from there.

Before asking it to edit anything, verify:
```bash
git status
git branch --all
git log --oneline --decorate -5
```

The intended starting branch is `main` at stable 0.16.0.1.

## Recommended first prompt to Codex
Copy/paste this:

> Read `AGENTS.md`, `PROJECT_CONTEXT.md`, `ROADMAP.md`, `DEVELOPMENT.md`, `TEST_CHECKLIST.md`, and `TASK_0.17_DRAG_DROP.md` before changing code. We are continuing kio-thispc from the user-confirmed stable 0.16.0.1 baseline. Create a feature branch for 0.17 Drag & Drop. Reproduce and diagnose the Qt6/KF6 event flow locally instead of blindly applying the failed 0.17.0.4 patch. The key bug to solve is that a real drag can start and tabs react, but the main directory viewport/background rejects the drop with the prohibited cursor. Inspect the reference branch `reference/failed-0.17.0.4` only as evidence of failed approaches. Build after each coherent change using `./scripts/build.sh`; do not claim completion while the build fails. Preserve all 0.16.0.1 behavior, KIO FileUndoManager integration, Split View, operation popup and NTFS permission behavior. When you believe it is fixed, give me a concise manual test matrix focused on current-directory background drop, folder-target drop, tab hover, Split View, same/cross-filesystem action choice, Dolphin interoperability and Undo.

## Useful comparison commands
```bash
git diff main..reference/failed-0.17.0.4 -- src/thispcview.cpp
git show reference/failed-0.17.0.4:src/thispcview.cpp | less
less handoff/failed-0.17.0.4.patch
```

## What success looks like
Do not stop at "compiles". The user must be able to drag a disposable file to another tab/directory and drop it on the open directory's **empty background**, with no prohibited cursor and with the correct copy/move action.
