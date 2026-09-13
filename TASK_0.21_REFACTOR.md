# 0.21.0 — staged architecture refactor

## Confirmed starting state, 2026-09-13

- Repository: ~/Pobrane/kio-thispc, branch main, HEAD 551e707 (0.20.0).
- Before editing, the contents of CMakeLists.txt and all affected source files
  were compared against an in-memory reconstruction of the ten supplied
  patches: steps 1–8 plus the step 7 and step 8 hotfixes. All files matched.
- All refactor changes are still uncommitted; the eight extracted headers
  were untracked. The index was empty. Do not reset these changes.
- The only other initial changes were lost executable bits on build.sh,
  check-version.sh and gemini-preflight.sh. Those bits have been restored.
- thispcview.cpp initially had 8093 lines. No sources were changed before
  this starting state was confirmed.

## Completed stages

1. DirectoryView widgets and filename layout.
2. SplitBrowserPane and shared browser helpers.
3. Shared directory rendering, sort and drag/drop setup.
4. Sidebar, Quick Access, recent locations and devices.
5. SessionManager; existing settings keys preserved.
6. OperationManager, including the existing operation popup.
7. UndoController, including explicit this->showStatus in constructor lambdas.
8. PropertiesDialog, including four explicit QObject::connect calls.
9. SearchController: asynchronous KIO jobs, partial results, deduplication,
   progress, cancellation, generation guards, 120 ms render batching,
   search URL helpers and existing type/date/size filter predicates.
10. FileActions: create file/folder, rename, Trash, clipboard paste, Send To
    copy dispatch and drag/drop transfer dispatch. KIO conflict delegates and
    UndoController recording remain intact. The window passes pane snapshots
    before modal dialogs; it keeps selection, menus, drag/drop policy and
    operation completion/error UI. childUrlWithName moved to browsercommon.h.

Step 9 keeps search controls, drive/root selection, navigation and result
presentation in ThisPcWindow. The controller owns the search results; the
window takes a snapshot when rendering, so view sorting cannot mutate the
controller's result collection. Changing hidden-file visibility is forwarded
to the controller. Destroying it cancels its outstanding workers.

Step 10 reduced thispcview.cpp from 7595 to 7250 lines. The version remains 0.20.0 during development.

## Validation

Run from the repository:

```bash
./scripts/build.sh
python3 tests/run-pane-actions.py --all
```

The runner builds disposable copies of the source and extracted headers with
fresh CMake AUTOMOC output. Only test copies expose private members. It uses
Qt offscreen, isolated settings and a private D-Bus without desktop service
activation. Local Unix socket access is required; no internet is needed.

Available suites: default pane actions, --tabs, --properties, --search, --actions, --all.

- Step 8: clean Debug build of both targets; 171 pane assertions, 194 tab
  drag/drop assertions, 83 PropertiesDialog assertions passed.
- Step 9: clean Debug build of both targets; the same suites plus 70 search
  assertions passed.
- Step 10: clean Debug build of both targets; 171 pane, 194 tab DnD,
  83 properties, 70 search and 59 real FileActions assertions passed (577 total).
  The new suite performs create/copy/move/rename/Trash, drives actual KIO
  overwrite/cancel dialogs and verifies rename, move and Trash Undo/Redo.
  The trashed fixture is restored before test cleanup. Fixture and isolated
  XDG_DATA_HOME live on the project filesystem so Trash is writable.
- Pane and tab tests exercise real Qt events/selection/dialogs but intercept
  destructive file-operation dispatch.
- Properties tests execute real KIO rename/chmod/stat on disposable files,
  including Apply, OK, Cancel, permission read-back and recursive folder chmod.
- Search tests execute real filenamesearch workers on disposable roots,
  including overlapping roots, empty results, replacement searches, filters,
  cancellation, partial results, late worker signals, views and Split View.
- Filter predicates were also mechanically compared to step 8: only their
  ownership and parameter names changed.
- The filenamesearch worker may treat an unavailable base location as empty
  results rather than propagate its underlying listing error. This existing
  backend behavior is preserved.

## Remaining acceptance and next stage

Manually check the built build/bin/thispc-view in the real KDE session:
search scopes (folder/drive/This PC), search in multiple tabs, Stop, sort and
filter controls, restored search tabs, and visual layout in both panes.
Properties/admin:// authorization and NTFS permission read-back still require
user-controlled checks on suitable disposable items. Offscreen tests do not
constitute visual or PolicyKit/NTFS acceptance.

Next: final integration/stabilization review and manual KDE acceptance,
including operation popup, conflicts, pane refresh, clipboard cut cleanup and
session restore. Review remaining includes/ownership and release metadata
(including the old install.sh banner). No further feature growth in this
refactor. Prepare release metadata only after acceptance; do not commit/tag
this as a user-confirmed stable release yet.

Runtime logs contain Qt offscreen/KIO dialog layout diagnostics and KIO Trash
warnings while enumerating unrelated mounted trash locations. The disposable
Trash operation and both history directions passed. No mount settings or
existing trash directories were changed.
