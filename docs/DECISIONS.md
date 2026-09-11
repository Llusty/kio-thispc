# DECISIONS.md

## Keep assisted/native KDE infrastructure independent
The project should add Explorer-like UX without bypassing KIO/Solid unnecessarily.

## NTFS permissions: attempt and verify
Do not disable permission editing solely because the filesystem reports ntfs/fuseblk/ntfs3.
Attempt the KIO permission operation and read the result back. Only then explain mount/UserMapping limitations if the requested mode did not stick.

User history relevant to diagnostics:
- `ntfs-3g` with `uid=1000,gid=1000,umask=022` produced fixed-looking modes and ignored chmod;
- true NTFS permissions were enabled with `.NTFS-3G/UserMapping` plus `permissions` mount behavior;
- user verified actual chmod changes on Windows C and G;
- existing Windows ACL/security descriptors can still produce ownership/mode differences.

Never mass chmod/chown whole NTFS/Windows/game partitions.

## Operation popup
Bottom panel was rejected visually. QMenu-based popup clipped operation cards. Custom `Qt::Popup` fixed dynamic sizing. Keep the current accepted UX unless the user requests redesign.

## Status version label
Use palette-aware readable foreground (`window-text` or equivalent), not `palette(mid)` on dark themes.

## Undo rename
Manual `KIO::rename` + custom FileUndoManager Rename recording did not undo correctly in this app. User-confirmed fix: `KIO::moveAs()` + `recordCopyJob()`.

## Release verification
A successful static scan is not enough for Qt/KDE interaction changes. Drag & Drop, popup/window behavior, keyboard shortcuts and KIO jobs require a real local build/runtime test.
