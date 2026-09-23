# 0.30.0 Stage 2 — Group by Type slice

## Implemented

- `None` and `Type` choices are available from the shared Sort menu, its
  Explorer-like context submenu, and the Split View pane menu.
- The choice is stored per normalized URL through `DirectoryViewSettings`.
- File metadata remains in ordinary model rows. `KCategorizedView` paints true
  category headers for Icons, List and Compact without inserting fake items.
- Details inserts presentation-only section rows marked non-file, disabled and
  non-selectable. Activation and context-menu mapping reject those rows.
- The existing file sort runs first. A stable category pass then groups the
  already-sorted files, preserving the selected key and direction within each
  type group.
- Folders form a deterministic first group. Other groups use the resolved,
  localized file-type label and deterministic locale-aware ordering.
- Search results use each result's own metadata and retain source URLs.
- KIO uses only metadata already supplied by the listing; no recursive scan or
  synchronous metadata fetch was added.

## Completion status

Group by modification date is documented in `GROUPING_STAGE2_DATE.md` and Group
by Size in `GROUPING_STAGE2_SIZE.md`. Both later slices are implemented and
manually accepted as part of the 0.30.0 release candidate. Type itself was also
manually accepted in Icons/List/Details/Compact, Split View, Search, sorting,
header context menus and per-folder persistence.

## Verification

- Build: PASS.
- Focused: view settings/grouping 145, pane/actions 376, Search 124,
  tabs/DnD 228, Quick Look 19, Preview 106, archive menu 1060 — all PASS.
- Full regression: 24 suites / 6197 assertions PASS.
- `git diff --check`: PASS.
