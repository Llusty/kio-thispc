# Task: fix icon-view filename layout without moving the grid

## Current version

0.19.0.3

## What already works

- Session restore works correctly in manual Plasma testing.
- Tabs, active tab and Split View restore correctly after restarting the application.
- The toolbar action **Pełne nazwy / Full names** exists and persists.
- The filename overlay introduced in 0.19.0.3 prevents the old behavior where one selected item increased its own `sizeHint()` and pushed lower rows downward.

## Current problems observed manually under KDE Plasma

### 1. Selected filenames are still inconsistent

Some selected items display more of the filename, while other long filenames are still clipped/truncated or show an awkward fragment rather than the complete name.

The expected behavior in normal icon mode is:

- ordinary unselected items may remain compact and elided;
- selecting a long item must make its **complete filename readable**;
- showing the complete selected filename must **not change the geometry of the icon grid or move rows below it**;
- the solution must work for ZIP/TAR archives, EXE files, video files and long folder names, not only a subset of names;
- scrolling/resizing must keep the expanded selected label visually anchored to its item.

### 2. Global “Full names” mode makes the icon grid look badly laid out

When **Pełne nazwy / Full names** is enabled, labels wrap inconsistently and the visual rhythm of rows/cells becomes poor. Some names still appear incomplete while other cells consume more vertical text space.

The expected global mode is:

- all cells in a row/grid use a **uniform geometry**;
- enabling the mode must not create random per-item heights;
- filenames should be substantially more readable than normal compact mode;
- the visual grid must stay aligned and calm;
- if truly unlimited full names cannot coexist with a sane fixed grid, propose a clear bounded policy (for example a uniform maximum number of lines plus selected-item overlay) before implementing it.

## Important diagnosis requirement

Do **not** immediately add another `sizeHint()` workaround.

First inspect and explain how the following interact in the current implementation:

- `QListWidget` icon mode;
- `setGridSize()` / `setSpacing()` / wrapping;
- `QStyledItemDelegate::sizeHint()`;
- `QStyleOptionViewItem::textElideMode` and `WrapText`;
- the 0.19.0.3 selected-name overlay;
- `visualItemRect()` / `visualRect()` and viewport coordinates;
- selection changes, scrolling, resizing and model refresh;
- why certain filenames still become clipped while others work.

Use the screenshots in `handoff/screenshots/` as the visual reference.

## Desired workflow

1. Diagnose only; do not edit yet.
2. Propose 1–2 robust designs and explain the tradeoffs.
3. Prefer a solution that keeps the normal icon grid fixed.
4. After the user approves the design, implement it.
5. Build locally with `./scripts/build.sh` and fix all compile errors.
6. Run relevant automated tests and `git diff --check`.
7. Leave the version at 0.19.0.3 until the fix is manually confirmed under Plasma.

## Regression checklist

Do not break:

- session restoration;
- selected item outline and normal selection behavior;
- drag & drop;
- context menus;
- rename (F2);
- tooltips;
- Split View;
- icon/list/details switching;
- Full names setting persistence.
