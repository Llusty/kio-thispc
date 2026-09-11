# TASK 0.18 — Copy/move conflicts

## Goal
A destination collision must not simply abort copy/move operations. Use KIO's interactive conflict handling so conflicts discovered anywhere in a CopyJob can be resolved without replacing the existing operation engine.

## Covered operation paths
- clipboard paste (copy and cut/move);
- Drag & Drop copy/move;
- Send To directory copies;
- normal rename (`KIO::moveAs`) when the requested target already exists.

## User decisions supplied by KIOWidgets
Depending on conflict type and whether multiple items are involved, KIO may offer overwrite, overwrite-all, skip, skip-all/auto-skip, rename, suggested/automatic new name, merge/write into an existing directory, or retry-related decisions. The native dialog receives source/destination sizes and timestamps from CopyJob.

## Integration rules
- attach the default KIOWidgets UI delegate explicitly even though jobs use `HideProgressInfo`;
- keep `KJobUiDelegate::AutoHandlingDisabled` because `watchFileOperation()` owns final error UI;
- preserve `KIO::FileUndoManager` recording;
- preserve the custom operation popup;
- treat `KIO::ERR_USER_CANCELED` as cancellation, not as a failure requiring a second warning dialog.

## Manual acceptance test
1. Create source and destination folders.
2. Put two files with the same name but different size/content in them.
3. Copy source file into destination and confirm a conflict dialog appears.
4. Test overwrite, skip and rename/suggest-new-name.
5. Repeat with multiple colliding files and verify all-items choices if offered.
6. Repeat through Ctrl+C/Ctrl+V, Ctrl+X/Ctrl+V and Drag & Drop.
7. Test a nested conflict inside copied directories.
8. Cancel a conflict and verify the job is shown as cancelled without a second error dialog.
9. Verify Undo after a successfully resolved copy/move.
