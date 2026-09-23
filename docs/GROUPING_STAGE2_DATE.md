# 0.30.0 Stage 2 — Group by Date slice

## Semantics

- **Date means modification time** (`KIO::UDSEntry::UDS_MODIFICATION_TIME`) for
  both files and directories. No access/change/creation time is substituted.
- KIO epoch seconds are converted to the active system timezone before calendar
  grouping. The week starts on Monday (ISO calendar).
- Buckets are disjoint and ordered: Future, Today, Yesterday, This week
  (excluding Today/Yesterday), Last week, Earlier this month, Earlier this year,
  Older, and Unknown date.
- A missing, non-positive or invalid modification timestamp is deterministic and
  appears in Unknown date. Future timestamps are kept visible in their own group.
- The existing file sort runs first. A stable category pass then groups those
  rows, preserving the selected sort key and direction within every date group.

## Runtime contract

- Icons, List and Compact use real `KCategorizedView` categories. Details uses
  presentation-only, non-file, disabled and non-selectable section rows.
- Category headers never acquire source URLs and therefore cannot enter file
  actions, selection, clipboard, drag/drop, Preview, Quick Look or item menus.
- Primary and Split View panes each schedule a single lightweight refresh for
  the next local midnight. The next deadline is recalculated after firing, so
  23-hour and 25-hour DST days are handled as calendar days.
- Data changes continue to flow through the existing asynchronous KIO listing
  and Search result paths. Grouping performs no synchronous stat, scan or
  recursive KIO request.
- The selected mode is stored per normalized folder/source URL and is shared by
  tabs, Search, KIO locations and session restore under the existing settings
  contract.

## Completion status

- Date grouping received manual acceptance (cases 1–5).
- Group by Size is documented separately in `GROUPING_STAGE2_SIZE.md` and has
  also received manual acceptance. No Stage 2 grouping slice remains open for
  the 0.30.0 release candidate.

## Automated verification

- Build: PASS.
- Focused: view settings/date boundaries 161, pane/actions 376, Search 124,
  tabs/DnD 228, Quick Look 19, Preview 106, archive menu 1060 — all PASS.
- Full regression: 24 suites / 6213 assertions PASS.
- `git diff --check`: PASS.
