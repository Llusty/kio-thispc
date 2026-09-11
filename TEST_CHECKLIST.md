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

## 0.17 Drag & Drop
See `TASK_0.17_DRAG_DROP.md` for the full matrix.
The mandatory release blocker is:
- [ ] dragging an item from tab A, hovering tab B, then dropping on tab B's empty directory background succeeds with the intended action.
