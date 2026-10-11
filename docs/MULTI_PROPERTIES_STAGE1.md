# 0.40.1 Multi-selection Properties — Stage 1

Status: Stage 1 implemented; earlier manual acceptance passed for small aggregates,
Ctrl/context menu/Alt+Enter, Primary/Split/Search, appearance, progress, cancellation
and single-item parity. The three follow-up fixes require one final manual retest.
Historical test totals below are not fresh validation of these follow-up fixes.
Version remains 0.40.0. No Stage 2 writes, installation, release or Git publication.

## Architecture and scope

`AggregateSnapshot` captures selected URL and local lstat identity. The aggregate
read-only model has no editing API. `AggregatePropertiesScan` runs in QtConcurrent;
`AggregatePropertiesDialog` owns a cancellation token and a watcher with guarded
QObject delivery. Closing the dialog cancels work without joining on the UI thread.
Both action state and item context menu enable Properties for multiple selection.
The existing active pane context supplies Primary, Split and Search selections.
Single-item handlers and PropertiesDialog are unchanged.

Local paths only (including mounted filesystems through the local OS interface).
Remote/virtual items and special files are reported unsupported, with partial
results for supported items in mixed selections. No KIO recursive remote scan.
Scanner uses no-follow descriptor-relative directory traversal, remains on the
selected root device, and never opens a symlink target. Limits: 4,096 selected roots, 1,000,000 visited
entries and 128 directory levels; partial results disclose a limit.

Counts include selected roots and recursive hidden entries. Regular files,
directories and symlinks have separate counts. Logical bytes include regular-file
contents only, including sparse logical length. Hardlink names count separately.
Directory identity and parent-directory/name identity suppress overlap between
selected ancestors and children without collapsing distinct hardlink names.
## Reported allocation and formatting (Stage 1 follow-up)

Allocation follows Storage Tools 0.38: sum nonnegative `st_blocks * 512` for
regular files, deduplicated by `(st_dev, st_ino)` across the entire selection.
Logical bytes and file counts still include each distinct hardlink directory entry.
Selected parent/child overlap is suppressed independently of selection order.
Directories, symlink inode storage and special files do not contribute allocation;
symlink targets are never followed. Empty supported folders therefore show 0 B.
Traversal never descends across a device boundary; an explicitly selected root
on another device is scanned independently. Same-device bind mounts are not
identified as foreign devices by this policy.

This is allocation **reported by the filesystem**, not unique physical usage.
Different inodes sharing Btrfs reflink/CoW extents are not deduplicated; compression
and NTFS filesystem reporting may alter values. Sparse files retain their full
logical length while allocation uses reported blocks. Negative block counts or
allocation arithmetic overflow increment an unavailable counter (in the tooltip)
and make the result partial. Unsupported URLs/special files, skipped devices,
read failures, changes, limits and cancellation all make allocation partial too.
If no regular-file allocation was obtained and the result is incomplete, display
Unavailable rather than a misleading complete zero. Otherwise retain the subtotal.
No snapshot is atomic across file content changes; root block counts are included
in identity revalidation. Cancellation/close retains the shared token and existing
guarded QObject delivery without waiting for the worker on the UI thread.

Counts use locale grouping. Sizes below 1024 remain exact (8 B, 9 B); larger
sizes use two decimal places and binary units plus grouped exact bytes, e.g.
`255,11 MiB (267 498 675 bajtów)` (Qt may use nonbreaking group spaces).
Wrapping labels and shared dialog geometry are preserved.

The old test launcher isolated XDG_CONFIG_HOME without user-dirs.dirs, making Qt
fall back to ~/Downloads. Production uses QStandardPaths::DownloadLocation and
has no regression. /tmp/thispc-stage1-run.sh copies the real user-dirs.dirs into
/tmp/thispc-stage1-runtime/config and starts /tmp/thispc-stage1-build/bin/thispc-view
at ~/Pobrane/Testy. It changes no user configuration and creates no Downloads.


Snapshot roots are revalidated before and after traversal; directory descriptors
are revalidated before and after listing. Changed/missing/unreadable entries
produce partial results. Selected root/parent filesystem notifications and Solid storage events invalidate
the open window when identity changes. The summary is a snapshot, not a recursive
live filesystem monitor. File-content changes during traversal are not atomic.

## Manual gate and cleanup

Run `/tmp/thispc-stage1-run.sh`; do not install into /usr.
Test one case at a time: two files via context menu; two folders with independently
known recursive counts; mixed files/folders; Alt+Enter without navigation;
Primary/Split/Search; cancellation and closing a busy dialog; single-item
Properties visual and behavior parity. Also test selected parent plus child.

Stop after Stage 1 pending explicit MANUAL PASS. Only after acceptance remove
known one-off manual fixtures, then inventory ~/Pobrane/Testy again and verify it
is empty. Unknown/old data must be identified separately and never silently
removed. Initial inventory on 2026-10-08: ~/Pobrane/Testy exists and is empty.
Old release drafts and outputs in the source repository were not modified.
Automated fixture ownership belongs to the test runner's disposable directories.

Final focused verification: 341 aggregate Properties checks and 28 ActionStateController assertions, exit 0.

Inventory at follow-up audit (2026-10-08): ~/Pobrane/Testy contains
thispc-0401-stage1-folders/FolderA/plik1.txt (4 B) and
FolderA/Podfolder/plik2.txt (5 B), plus its existing folders. These were preserved.
Older project-local stage1-manual contains one.txt (3 B), two.txt (5 B),
folder-a/inside.txt (7 B), folder-a/nested/.hidden (3 B), folder-b/other.txt (11 B)
and linked-file -> one.txt. No deletion performed, no new fixture there.
Existing source outputs/ and release drafts remain protected. Cleanup follows
explicit final acceptance and identification of fixture ownership/report backups.

## Fresh follow-up verification and final gate (2026-10-08)

Final configure/build exit 0; focused 5/5 suites exit 0:
396 aggregate checks, 28 action-state assertions, 159 PropertiesDialog assertions,
97 lifecycle assertions, 21952 storage-scan assertions. Full final suite 65/65,
exit 0. CLI --version/-v 6/6 assertions, exit 0, still 0.40.0.
Qt directory-map probe reproduces old ~/Downloads fallback and verifies fixed
~/Pobrane mapping. git diff --check exit 0; /usr/bin/thispc-view checksum unchanged.
These are fresh automated results; historical manual PASS is not a new retest.

One pending MANUAL RETEST: run /tmp/thispc-stage1-run.sh, open
~/Pobrane/Testy/thispc-stage1-retest-allocation, select duzy-sparse.bin and pusty.txt,
then Alt+Enter. Expect 2 files/0 folders, 255,11 MiB (267 498 675 bajtów)
and 4,00 KiB (4 096 bajtów) reported allocation on the current FS; verify wrapping.
Close Properties and click Pobrane; expect /home/sebastianh/Pobrane.
This new fixture was created exclusively in Testy (sparse 267498675 B with one
written byte, empty 0 B file), without replacing older fixtures. Its manifest
is /tmp/stage1-manual-fixture.json and is backed up with the final report evidence.
STOP pending explicit acceptance. Cancel needs no repeated manual test after its
historical PASS and fresh automated coverage. No cleanup before acceptance.
