# 0.23.0 — Native local transfer engine

The goal is real local pause/resume at a known byte boundary without weakening KIO support for remote URLs or unrelated operations. Each stage must build and pass its focused tests before the next stage begins.

## Stage 1 — single-file copy primitive

Status: complete; integrated into user file actions by later stages.

- background worker copies one regular local file in bounded chunks;
- output is written to a `.thispc-part` file and published only after success;
- pause waits for a chunk boundary and exposes the exact stopped byte offset;
- resume continues the same worker and partial file;
- cancellation removes the partial file;
- an existing destination or partial file fails safely;
- tests compare SHA-256 content and verify that both reported bytes and partial-file size remain unchanged during pause.

## Stage 2 — metadata and failure semantics

Status: complete and covered by focused tests.

- preserves permissions plus access and modification timestamps before publication;
- distinguishes source-open/read, destination-exists/create/write, disk-full, metadata and publication errors;
- existing destinations remain untouched;
- a pre-existing `.thispc-part` is rejected and retained for explicit future recovery rather than overwritten or deleted.

## Stage 3 — directories and operation planning

Status: complete and covered by focused tests; execution is integrated by later stages.

- build an immutable plan for directory trees, regular files and symbolic links;
- calculate totals without blocking the UI;
- capture destination conflicts and their type/size inputs before execution;
- keep source data untouched throughout copy.

## Stage 4 — move and Undo/Redo integration

Status: complete and covered by focused tests; application integration is completed by later stages.

- same-filesystem file moves use an atomic rename;
- cross-device-style moves copy and verify publication before removing the source;
- cancellation retains the source and cleans the partial destination;
- a source-removal failure rolls back the newly published destination;
- native history records only completed, safely reversible moves and provides tested Undo/Redo jobs;
- merging native history with the application actions and existing KIO history remains part of Stage 5.

## Stage 5 — application integration

Status: complete and covered by focused integration tests; extended by Stages 7–10.

- supported single regular local-file copy/move operations use the native engine;
- directories, multiple sources, symbolic links, existing-target conflicts and remote URLs retain KIO;
- the detailed operation window exposes pause/resume only for genuinely suspendable jobs;
- native and KIO commands share Undo/Redo ordering through KDE command serials;
- native copy Undo checks the published file snapshot before removal, while native move Undo/Redo uses safe move jobs;
- compact history, cancellation, conflict UI, clipboard clearing and automatic detailed-window behavior remain intact.

## Stage 6 — single-file manual validation

Status: user-confirmed before handoff; do not repeat this acceptance work.

- large files across the user's real local filesystems;
- multiple simultaneous transfers and independent pause/resume;
- cancellation, disk-full simulation and device disconnect;
- byte-integrity checks before release preparation.

Confirmed: two 30–60 second pauses without byte progress; Cancel and Pause→Cancel cleanup;
independent concurrent jobs; Btrfs→NTFS move with Pause/Resume and Undo/Redo; controlled
disk-full and device-loss errors; matching SHA-256 for a 32 GiB copy. The paused-window
fix, graph speed scale and file-size tooltips are also user-confirmed.

## Stage 7 — single-file native conflicts

Status: implemented; build and 213 focused assertions pass (FileActions 72,
OperationManager 44, local copy 44, local move/history 53).

- retain the KDE conflict dialog for Overwrite, Rename and Skip;
- overwrite copies into the existing partial-file mechanism and atomically exchanges
  the completed file with the old destination; unsupported exchange fails safely;
- retain the old destination until a move has also removed its source; restore it
  on source-removal failure, and name recovery data if rollback cannot finish;
- reject changed destinations and self-overwrite; paused/cancelled overwrites keep
  the original target, with byte-exact completion tested;
- Rename retains ordinary native Undo/Redo; Skip/Cancel do not clear cut data;
- completed overwrites are irreversible after backup cleanup and block older Undo
  commands that could act on replaced paths; newer reversible operations still work;
- file/directory type conflicts, target symlinks and self-copy retain KIO handling.

## Stage 8 — directory and multiple-source execution

Status: complete; build and focused tests pass, and UI routing is integrated by Stage 10.

- execute directories and multiple sources with aggregate byte/current-file progress;
- delegate exact Pause/Resume to the current native job, including independent jobs;
- cancel the current partial while retaining completed entries and remaining sources;
- remove moved source directories only after their contents have completed;
- reject unresolved conflicts, self-copy and overlapping selections before execution;
- propagate directory-read errors instead of treating unreadable folders as empty.

## Stage 9 — symbolic links and directory metadata

Status: complete; build and focused tests pass. Relative, broken, directory and external symbolic links
retain their literal link contents without traversal. New-directory permissions
and nanosecond access/modification timestamps are restored after child operations;
pre-existing merged directories retain their permissions.

## Stage 10 — integration and fallback validation

Status: complete and covered by the full regression suite: 842/842 assertions pass; final manual acceptance is user-confirmed.

- route supported local directories, multiple sources and symbolic links through
  asynchronous native planning and execution for clipboard, Drag & Drop and Send To;
- expose aggregate bytes and the current file in the existing detailed/compact operation UI;
- retain KIO for remote URLs, file/directory type conflicts, conflicting targets,
  self-copy and unsupported local entries; the wrapper drops Suspendable capability
  before starting KIO so the detailed window never offers misleading Pause;
- a pause requested during planning is retained and prevents native execution or KIO
  fallback until Resume;
- completed native tree copies and moves join the KDE-serial-ordered Undo/Redo history;
  history validates inode/metadata and directory contents before acting, preserves
  unrelated entries in merged directories, and refuses changed trees safely;
- full regression totals: panes 173, tab DnD 194, Properties 83, search 70,
  FileActions 90, OperationManager 45, local copy 44, plan 33, local move 53,
  tree execution 34 and tree history 23 assertions.

Implementation and manual acceptance are complete for the full roadmap scope.
User-confirmed manual validation covers native conflicts, directory trees, symbolic
links, metadata, exact Pause/Resume, concurrent jobs, cancellation, cross-filesystem
moves, file/tree Undo/Redo, disk-full, device-loss and SHA-256 integrity.
0.23.0 is approved for release preparation.
