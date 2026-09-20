# 0.29.0 Stage 2 — remote KIO naming

## Implemented scope

- Remote file, folder and template creation obtains an asynchronous `listDir`
  snapshot before opening the editable name dialog.
- The snapshot is used to suggest the first exact free name, including numbering
  gaps, complete multi-part suffixes and Unicode names.
- After the user accepts or edits the name, a second asynchronous directory
  snapshot checks the chosen name. A collision or any listing error aborts the
  operation without dispatching a create job.
- Switching the active pane does not retarget the request: the destination URL
  is captured when the action starts. A newer New-item request invalidates the
  older callback. Closing the window cancels the live KIO job, and guarded late
  callbacks are ignored.
- Final `storedPut`, `copyAs` and `mkdir` operations have no `Overwrite` or
  `Resume` flag and no conflict UI that could opt into overwriting.

## Safety boundary

This is a fail-closed preflight, not an atomic remote reservation. There is an
unavoidable race between the second listing and the final KIO request. KIO's
documented no-overwrite contract is the final protection: a conforming worker
must fail if the destination appeared in that interval. The application does
not claim stronger atomicity than the protocol and worker provide.

`KIO::stat(..., DestinationSide)` is intentionally not used as the final
existence oracle. KIO documents that a worker unable to determine existence may
optimistically report a destination as absent. Re-listing the parent produces a
bounded single request, avoids sequential `stat` calls over occupied numbers,
and lets every read/list/authentication/permission failure abort safely.

Remote protocols are supported only when both directory listing and the final
no-overwrite operation obey the KIO contract. Read-only locations, unsupported
listing, authentication failures, timeouts and ambiguous errors are rejected;
they are never treated as proof that a name is free. Case folding,
normalization, server-side aliases and stale server listings remain
protocol-defined, so the final no-overwrite response remains authoritative.

## Automated coverage

The isolated FileActions backend covers asynchronous pre-dialog behavior,
files/folders/templates, numbering gaps, Unicode and multi-part suffixes,
manual editing, post-dialog collision, list failure, cancel, superseded
callbacks and window destruction. It does not use external networks or user
data. Local Stage 1 tests continue to exercise real disposable KIO operations
and Undo/Redo.

## Manual acceptance status

The user confirmed the basic flow on a disposable remote KIO location on
2026-09-20. This is a limited manual PASS: it does not claim that connection
failure, every protocol, every item below or an atomic reservation was tested.

Additional non-blocking coverage:

- Exercise at least one disposable remote KIO worker/location that supports
  listing and no-overwrite creation.
- Confirm file, folder and template suggestions and manual edits.
- Confirm authentication/listing failure creates nothing.
- Confirm switching Split View panes while a listing is pending does not change
  the captured destination.
- Confirm closing the window while a listing is pending produces no dialog.
- Where practical, create the accepted name from a second client before the
  second check/final request and confirm the existing object is preserved.
