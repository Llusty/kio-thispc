# kio-thispc 0.29.0 — Intelligent New names

This tree is a release candidate. Commit, tag and installation remain pending
separate final approval.

## Candidate changes

- New files, folders and items created from templates suggest the first free
  name before the editable dialog opens.
- Numbering fills gaps and is inserted before the complete extension; hidden
  names and Unicode are preserved.
- Local and remote destinations are rechecked after the dialog, and creation
  jobs never opt into overwrite.
- Remote KIO suggestions are asynchronous and retain the directory captured
  from the initiating Split View pane.
- Remote listing errors, authentication failures, cancellation, stale
  callbacks and window closure fail without starting creation.
- Existing Undo/Redo registration is retained where the KIO operation supports
  it.

## Known limits

- Remote preflight is not an atomic reservation. The final guarantee is the
  KIO worker/protocol's no-overwrite behavior; protocols that cannot satisfy
  directory listing or this contract are not safely supported.
- Remote case sensitivity, Unicode normalization, aliases and cache freshness
  are protocol/server-defined.
- The user confirmed the basic disposable remote Stage 2 flow. This does not
  claim testing of connection failure, every KIO protocol, every checklist
  scenario or atomic name reservation.
- The 0.28 recovery contract is unchanged: automatic recovery v2 covers only
  qualifying local linear Batch Rename Execute/Undo/Redo. Swap/cycle recovery,
  KIO fallback, power-loss guarantees and non-cooperating processes remain out
  of scope; see `RELEASE_NOTES_0.28.0.md`.

## Release checklist

- [x] Focused FileActions (176 assertions) and pane-routing (289 assertions)
      tests pass.
- [x] Full regression passes: 23 suites / 5430 assertions.
- [x] Production build passes and `git diff --check` is clean.
- [x] Manual local Stage 1 acceptance was confirmed by the user.
- [x] Basic manual disposable remote Stage 2 flow confirmed by the user.
- [x] Stage 2 safety boundary and known limits retained in the release notes.
- [x] Project version and release documentation updated consistently.
- [x] Fresh RC verification after the version bump: build PASS; FileActions 176,
      pane routing 289, Batch Rename 1790; full regression 23 suites / 5430
      assertions PASS.
- [x] Clean source archive and SHA-256 prepared without `.git`, build output,
      Python cache or runtime recovery journals.
- [ ] Commit the approved tree, create `v0.29.0`, build the release artifact and
      install it only after a separate final confirmation.
