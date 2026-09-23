# 0.30.0 Stage 2 — Group by Size slice

## Byte ranges

The grouping uses the `KIO::UDSEntry::UDS_SIZE` value already supplied by the
current directory listing or Search result. KiB, MiB, GiB and TiB are exact
binary multiples (1,024; 1,048,576; 1,073,741,824; 1,099,511,627,776 bytes).
The ranges are ordered and disjoint:

| Group | Exact condition |
| --- | --- |
| Folders | `isDir`, regardless of reported size |
| Empty (0 B) | regular entry, size = 0 |
| 1 B – 1023 B | 1 ≤ size < 1,024 |
| 1 KiB – under 1 MiB | 1,024 ≤ size < 1,048,576 |
| 1 MiB – under 1 GiB | 1,048,576 ≤ size < 1,073,741,824 |
| 1 GiB – under 1 TiB | 1,073,741,824 ≤ size < 1,099,511,627,776 |
| 1 TiB and larger | size ≥ 1,099,511,627,776 |
| Unknown size | size < 0, including absent `UDS_SIZE` (`-1`) |

Folder size is deliberately not calculated. No recursive scan, synchronous
`stat`, or additional remote KIO request is made by grouping. A refreshed KIO
listing or Search result reclassifies entries using its newest metadata.

The existing sort runs first. A stable category pass then preserves its key and
direction inside each group. The same categorized list and presentation-only
Details headers used by Type and Date are used for Size; headers have no file
URL, selection, clipboard, drag/drop, item menu, Preview or Quick Look action.
The mode persists per normalized folder or Search URL in both panes and tabs.

## Manual acceptance

Manual acceptance is PASS for Icons, List, Details and Compact, Split View and
Search, header context menus, sorting, selection, per-folder persistence and
restart. During acceptance an apparent double selection was traced to a stale
categorized-view hover state: the selection model contained exactly one row,
but Breeze painted the stale hover like another selected tile. The final fix
recomputes hover from the actual cursor position. The selected-name outline was
also corrected for both short and expanded names.

This slice completes the functional Stage 2 scope. Version 0.30.0 is still
subject to the final release-candidate build/regression/diff/archive checks
before commit, tag and installation.
