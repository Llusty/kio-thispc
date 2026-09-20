# 0.29.0 Stage 1 — intelligent New names

## Acceptance criteria

- Local file, folder and template creation proposes the first free name before
  the name dialog opens; numbering starts at 1 and fills gaps.
- Numbering is inserted before the complete extension, while leading-dot names
  remain extensionless.
- The destination is the directory snapshot captured from the initiating pane,
  including either side of Split View.
- A second local existence check runs after the dialog.  A race aborts safely,
  reports the next suggestion and never overwrites the object that won the name.
- KIO jobs retain the existing Undo/Redo registration.  Creation jobs do not
  offer an overwrite decision.

Remote KIO destinations keep their existing non-overwrite job semantics in this
stage.  They do not yet promise a synchronous pre-dialog suggestion because the
answer requires an asynchronous remote listing/stat operation.
