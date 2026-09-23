# 0.30.0 Stage 1 — per-folder view mode

## Implemented scope

- The Icons/List/Details/Compact mode is stored for each normalized location.
- One preference is shared by the primary pane, Split View, tabs and restored
  sessions; it follows the folder rather than the widget that displayed it.
- Local and KIO URLs use the same encoded-URL identity rules. Path segments are
  normalized and the encoded identity is hashed before it is used as a
  `QSettings` key.
- A location without a saved value uses the existing global view-mode setting.
  A corrupt or out-of-range saved value also falls back safely.
- Changing a mode updates both the location preference and the existing global
  fallback. Applying a saved value during navigation does not overwrite another
  location or a tab snapshot.
- Refresh and repeated navigation reapply the same location preference without
  scanning directory contents or relying on file metadata.

## Automated acceptance

- independent local folders and independent remote KIO URLs;
- normalized equivalent URL identity and invalid-setting fallback;
- persistence through a fresh `QSettings` read;
- primary navigation restores the folder mode;
- Split View restores it and sees the same preference as the primary pane;
- tab switches and a saved/restored two-pane session reapply both locations;
- the existing Icons/List/Details/Compact and two-pane action matrix remains green.

## Stage 2 — grouping (complete for 0.30.0)

Stage 2 provides explicit None/Type/Date/Size grouping. Icons, List and Compact
use `KCategorizedView`, where category headers are presentation owned by the
view and are not model rows. Details uses disabled, non-selectable section rows
with an explicit non-file role. Both representations keep headers out of
selection, activation, context-menu item mapping, clipboard, drag sources,
Preview and Quick Look.

The preference is stored per normalized location URL and is shared by the main
pane, Split View, tabs, Search/source URLs and session restore. The existing
file sort runs before a stable category pass, so the selected key and direction
remain intact inside each group.

Date grouping uses modification timestamps supplied by KIO and local-calendar
boundaries (including Monday week starts and midnight refresh). Size grouping
uses the non-recursive `UDS_SIZE` already present in listing/Search metadata;
folders and unknown sizes are deterministic separate groups. Grouping never
adds recursive scans, synchronous stats or extra remote metadata requests.

Automated tests cover date boundaries/DST, size boundaries ±1 byte, zero,
unknown metadata, directories, sorting, Search/source URLs, both panes, tabs and
settings persistence. Manual acceptance covered Type/Date/Size in all four view
modes, Split View, Search, header context menus, sorting, selection and restart
persistence. The final categorized-view hover fix also prevents a stale hover
from looking like a second selection while keeping the selection model unchanged.

See `GROUPING_STAGE2_TYPE.md`, `GROUPING_STAGE2_DATE.md` and
`GROUPING_STAGE2_SIZE.md` for exact semantics and slice verification.

## View menu organization

The first menu-only follow-up keeps the existing behavior and changes only its
presentation. Icons, List and Details form one exclusive group routed to the
active pane. Show contains the already implemented shared toggles for hidden
items, image thumbnails, Preview Pane and full names. Existing Ctrl+H and Alt+P
shortcuts, check states and toolbar actions are reused rather than duplicated.

Tiles/Content/Compact, a distinct details pane, item checkboxes, extension
visibility and a persistent Navigation Pane toggle are not exposed until their
underlying behavior exists. They are a separate view-feature backlog and are
not part of Stage 2 grouping by type/date/size.

Automated verification after this menu stage: build PASS, focused view settings
and pane/menu routing 15 + 300 assertions PASS, full regression 24 suites / 5456
assertions PASS.

## Stage 1c: icon sizes

The Icon size submenu offers Very large (96 px), Large (64 px), Medium (48 px)
and Small (32 px). Large preserves the pre-0.30 icon geometry and is the
fallback for folders without a stored preference. The setting is keyed by the
same normalized directory URL as the view mode and therefore follows a folder
across the primary pane, Split View, tabs and session restore.

Only Icons mode consumes this preference. List and Details retain their stable
24 px and 22 px geometry. Every icon size uses a fixed grid derived only from
the preference and the existing Full names option; the selected-name callout
is painted over the viewport and never resizes or moves neighboring items.

## Stage 1d: Compact view

Compact is a fourth per-folder view mode. It uses dense, single-line 20 px-icon
rows, fills each column from top to bottom, and then continues in the next
column. Names are elided within a stable 240 px cell so large directories retain
uniform layout cost. Qt's visual cursor navigation supplies Up/Down within a
column and Left/Right between columns. It is available in the primary pane and
Split View, and reuses all existing selection, activation, context-menu,
drag/drop and file-action paths. The Icons-only full-name callout remains
unchanged and is disabled by construction outside Icons mode.

Automated verification after Stage 1c: build PASS, focused view-settings and
pane/menu routing 23 + 305 assertions PASS, full regression 24 suites / 5469
assertions PASS, and `git diff --check` PASS.
