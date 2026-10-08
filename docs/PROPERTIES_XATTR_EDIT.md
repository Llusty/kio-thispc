# Stage 3: user.* extended attribute editing

Stage 3: AUTOMATED PASS + FULL MANUAL PASS, confirmed by the user. Runtime version remains 0.39.0. The report below preserves historical stage verification.

Only local regular files and directories with a captured identity and supported user-xattr capability can be edited. Protected namespaces, remote URLs (including admin), symlinks, broken symlinks, and special entries are read-only. Raw attribute names without lossless UTF-8 representation remain read-only. Names must start exactly with user., have a nonempty suffix, contain no NUL, and fit in 255 encoded bytes. Names are never trimmed or normalized. Values are limited to 65536 bytes; filesystem-specific lower limits are reported.

New values default to UTF-8 text, with explicit hex available. Existing text uses a text editor; existing binary always uses strict hex. Hex accepts ASCII digit pairs with optional ASCII spaces, including contiguous pairs. Tabs, line breaks, prefixes, invalid digits and odd digit counts are rejected without partial parsing. Exact bytes are written; no automatic NUL terminator. Existing unchanged text preserves original bytes even when Qt normalizes paragraph separators for display. Existing xattr names cannot be renamed. TooLarge values cannot be edited, but user attributes may be removed.

Add uses XATTR_CREATE and refuses existing names. Edit uses XATTR_REPLACE and refuses missing names. Remove names exactly one user attribute and requires confirmation with the full name; already missing produces a stable conflict message. Operations are immediate, outside Properties Apply and global Undo/Redo.

## Identity and Linux API audit

The writer invokes Stage 1 revalidation before opening the path. It opens with O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | O_NOCTTY (and O_DIRECTORY for directories), then checks fstat device, inode and entry type against the original Properties snapshot. It revalidates the path once more immediately before fsetxattr/fremovexattr. All early failures block mutation. There is no path-based write fallback or privilege escalation. O_RDONLY means a write-only file can be denied although the kernel would otherwise allow an xattr write; this is a conservative Stage 3 limitation. O_NONBLOCK prevents FIFO blocking if the path races; a changed FD identity/type cannot reach a mutation syscall.

The final revalidation and mutation are not atomic with path replacement. If the path is renamed or unlinked immediately after the last check, the verified FD still writes the captured original inode, never the replacement or a symlink target. Parent-path components may be symlinks; the final captured inode/type check remains authoritative. Identity uses dev/ino/type, not persistent inode generation, so inode-number reuse after deletion is a general snapshot limitation. No attribute-value compare-and-swap is claimed: concurrent changes to the same attribute value can still be overwritten by explicit Replace, and concurrent Remove/Add can affect the current named attribute. CREATE/REPLACE enforce presence semantics only.

Linux user.* attributes on symlinks are restricted; Stage 1 already marks their editing unsupported. Stage 3 disables them without trying a following syscall, even for broken links. No remote local-path emulation exists.

Primary sources audited:
- [setxattr(2): FD API, CREATE/REPLACE, exact bytes and errors](https://man7.org/linux/man-pages/man2/setxattr.2.html)
- [removexattr(2): FD removal and ENODATA](https://man7.org/linux/man-pages/man2/removexattr.2.html)
- [xattr(7): namespace semantics and Linux limits](https://man7.org/linux/man-pages/man7/xattr.7.html)
- [open(2): NOFOLLOW, NONBLOCK, O_PATH restrictions](https://man7.org/linux/man-pages/man2/open.2.html)

## Asynchronous completion and consistency

The controller owns one in-flight mutation per widget and uses a shared two-worker pool. A target generation change discards delivery but cannot undo an in-flight syscall. Worker captures contain values and a backend copy, never widget pointers. Deleting Properties destroys connections safely. A successful or failed current-generation completion starts a fresh Stage 2 reader request. Reader generations discard old reads. Controls remain disabled during writes and refresh; protected controls are always disabled. Selection is preserved where the named attribute survives. One localized inline error displays the current failure; there is no popup storm.

Errno mapping covers EACCES/EPERM, ENOTSUP/EOPNOTSUPP, EROFS, ENOSPC/EDQUOT, ENOENT/ENOTDIR, EEXIST, ENODATA, ERANGE/E2BIG, and generic errors including EIO. No error retries imply create, overwrite, following symlinks or admin elevation.

Real smoke operates only on temporary user.thispc.* fixtures. Content, size, mode and mtime remain unchanged; ctime may change and is deliberately not invariant. Protected namespace mutations are never tested on the real host. Real remote KIO manual smoke remains NOT TESTED, non-blocking when automated zero-backend-call coverage passes.
