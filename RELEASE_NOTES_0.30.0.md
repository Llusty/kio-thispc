# kio-thispc 0.30.0 — Grouping and per-folder view settings

## Scope

0.30.0 adds folder-scoped view preferences and Explorer-like grouping without
changing the installed 0.29.0 release until the final candidate is explicitly
approved and installed.

### Per-folder view settings

- Icons, List, Details and Compact are stored by normalized location URL.
- The same location preference follows the primary pane, Split View, tabs and
  restored sessions, including supported KIO/Search source URLs.
- Icon size is also stored per folder with four levels: 96, 64, 48 and 32 px.
- Compact is a dense multi-column mode with 20 px icons, stable cell width and
  top-to-bottom column filling.

### Grouping

- Choices: None, Type, Date modified and Size.
- Icons/List/Compact use `KCategorizedView`; Details uses presentation-only,
  disabled section rows. Headers are not files and have no file URL.
- File sorting is applied first and the category pass is stable, preserving the
  current key and direction inside every group.
- Type uses resolved file-type labels with folders in a deterministic group.
- Date uses `UDS_MODIFICATION_TIME`, the active local timezone, Monday week
  starts and disjoint Future/Today/Yesterday/This week/Last week/older/unknown
  buckets. Each pane schedules a lightweight recalculation after local midnight.
- Size uses the existing non-recursive `UDS_SIZE` from listing/Search metadata.
  Exact binary thresholds and the Folder/Unknown rules are documented in
  `docs/GROUPING_STAGE2_SIZE.md`.

Grouping performs no recursive size calculation, synchronous metadata scan or
extra remote KIO stat solely for categorization. Metadata is refreshed through
the existing asynchronous listing/Search paths.

## Selection and rendering fixes found during manual acceptance

Manual testing exposed two presentation issues in grouped Icon view:

- the selected-name outline could lose its bottom edge for short names;
- a stale `State_MouseOver` could remain painted on the previously hovered tile
  and look like a second selection.

Diagnostic logging confirmed that `QItemSelectionModel` contained only one
selected row in the latter case. The final implementation keeps the selection
model as the source of truth and derives grouped hover from the actual cursor
position. Ctrl/Shift multi-selection and drag behavior remain available.

## Acceptance

Manual acceptance is PASS for:

- per-folder view mode and icon-size persistence;
- Icons, List, Details and Compact;
- primary and Split View panes, tabs/session restore and Search;
- Group by Type, Date and Size;
- group-header context behavior, sorting inside groups and selection;
- short and expanded selected-name outlines;
- plain-click single selection and Ctrl/Shift multi-selection.

Final RC verification after the version/documentation bump: release build PASS;
focused `view_settings panes actions` 226 + 376 + 176 = 778 assertions PASS;
full `tests/run-pane-actions.py --all` 24 suites / 6278 assertions PASS;
`git diff --check` PASS; the active version audit reports 0.30.0 and no
selection-debug markers remain. The clean source archive and SHA-256 remain the
last technical packaging gate before separate approval for commit/tag/install.

## Deliberately outside 0.30.0

- continuous icon-size slider / Ctrl+wheel zoom;
- expanded keyboard navigation work for `thispc:/`, Enter, Backspace and
  Alt+Left/Right/Up;
- Tiles/Content, item checkboxes, extension visibility and a separate details pane;
- recursive folder-size calculation.

These remain separate roadmap work and are not release blockers for 0.30.0.

## Release gate

Do not commit/tag/install merely because the functional scope is complete. The
final candidate must pass the checks above, be packaged without local caches or
runtime data, and receive separate user approval before commit, tag `v0.30.0`
and installation.
