# TEST_CHECKLIST.md

## Build sanity
- [ ] clean configure succeeds
- [ ] `thispc` KIO module builds
- [ ] `thispc-view` builds
- [ ] no new compiler errors
- [ ] review new warnings rather than ignoring them blindly

## Core regression smoke test
- [ ] `thispc-view` starts
- [ ] Ten komputer opens
- [ ] home folders open
- [ ] drive entries open
- [ ] back/forward/up work
- [ ] tabs open/close/reorder
- [ ] Split View opens/closes and both panes navigate
- [ ] icons/list/details still render
- [ ] sorting works
- [ ] search still works
- [ ] operation popup opens and remains positioned mostly to the right
- [ ] starting an operation automatically opens the detailed operation window
- [ ] one and multiple simultaneous operations size the detailed window correctly
- [ ] current file, bytes, prominent percentage, speed, average, ETA and graph update
- [ ] Mniej/Szczegóły and Cancel respond during a fast transfer
- [ ] completed operations disappear from the detailed window but remain in compact history
- [ ] the detailed window closes after the final active operation
- [ ] version label is visible on dark theme

## File operations
Use disposable test files only.
- [ ] create folder
- [ ] create file
- [ ] rename
- [ ] copy
- [ ] move
- [ ] Trash
- [ ] Undo rename
- [ ] Redo rename
- [ ] Undo Trash restore
- [ ] Undo/Redo buttons enable/disable correctly

## Permissions
Only test on disposable files.
- [ ] normal POSIX permission edit on a Linux filesystem
- [ ] read-back verification works
- [ ] no blanket NTFS-disable behavior returns
- [ ] admin fallback remains targeted


## Sidebar & Split View regression

- [ ] both Split View panes navigate independently
- [ ] shared toolbar/sidebar actions target the active pane
- [ ] F6 switches the active pane without losing pane state
- [ ] This PC / `thispc:/` cards work in both panes
- [ ] Search query, filters, results and Stop remain independent per pane
- [ ] sidebar Drag & Drop works after scrolling
- [ ] Quick Access reorder works after scrolling
- [ ] sidebar width persists after restart
- [ ] long sidebar labels use right-side ellipsis and show the full tooltip
- [ ] no horizontal sidebar scrollbar appears

## Automated regression

- [ ] `python3 tests/run-pane-actions.py --all` exits with code 0
- [ ] all focused suites pass
- [ ] `git diff --check` is clean

## 0.25.0 Stage 2 — archiwa

- [ ] Wykonać listę manual KDE z docs/ARCHIVE_STAGE2.md, wyłącznie na danych jednorazowych.
- [ ] Focused: TMPDIR=/tmp python3 tests/run-pane-actions.py --suites archive archive_jobs archive_menu.
- [ ] Pełna regresja: TMPDIR=/tmp python3 tests/run-pane-actions.py --all.
- [ ] git diff --check; bez commita/taga przed manualną akceptacją.
