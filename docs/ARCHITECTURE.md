# ARCHITECTURE.md

## Components
### `src/thispc.cpp`
KIO worker backend for `thispc:/`. It enumerates the virtual This PC content and drive/storage information using KDE/Solid/KIO infrastructure.

### `src/thispcview.cpp`
Native Qt/KDE application. It currently contains most UI and controller logic in one translation unit. Major conceptual regions include custom address/breadcrumb widgets, sidebar/drive widgets, directory views, Split View, operation manager, properties/permissions, search, file actions and Undo/Redo.

### `src/thispc.json`
KIO worker plugin metadata.

### `data/org.kde.thispcview.desktop`
Desktop entry for the native application.

### `icons/`
Drive/removable-device usage icons used by the This PC presentation.

## KIO-first file operations
Prefer KIO jobs for copy/move/trash/rename/create where possible, so operations integrate with KDE behavior, progress reporting and `KIO::FileUndoManager`.

## Undo/Redo
`KIO::FileUndoManager` is initialized by `ThisPcWindow`. The 0.16.0 rename bug is an important lesson: record the actual KIO operation in the way FileUndoManager expects rather than inventing metadata that does not match the executed job.

## Operation UI
The operation manager listens to KJob/KIO progress and maintains operation cards. The accepted UI is a custom top-level-ish `Qt::Popup`/`QFrame`, not a `QMenu`, because QMenu sizing caused clipping. The popup is intentionally offset mostly to the right of the application window and has a capped scroll area.

## Future architecture
0.21 is intentionally reserved for extracting reusable controllers/views before feature growth makes `thispcview.cpp` too risky to maintain.
