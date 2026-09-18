# 0.26.0 Preview Pane — Stage 5

Stage 5 adds a read-only archive manifest to the existing asynchronous PreviewPane.
It uses the project's existing libarchive formats and shared safe-path validation;
it does not run Ark, extract entries, launch files or write archive contents.

## Supported scope and limits

- Local ZIP, 7z, tar and tar.gz/tgz archives already accepted by the archive backend.
- 1 GiB maximum archive input, 10,000 entries and 8 GiB total declared entry size.
- The UI lists at most 500 entries and 64 KiB of text, while counts cover the
  accepted manifest up to the entry limit.
- Only regular files and directories are listed. Symlinks, hard links, special
  entries, absolute/traversal/Windows-like/control-character paths and invalid
  UTF-8 names are rejected.
- Entry data is never returned or materialized. libarchive advances with its
  skip operation so only validated header metadata crosses the worker boundary.
- Empty, damaged/incomplete, encrypted, unsupported and over-limit archives get
  explicit messages. No helper process or password dialog is started.
- The existing request token discards results after a newer selection or Alt+P;
  all libarchive objects are created, used and freed inside the worker task.

## Automated verification

The deterministic preview suite creates temporary ZIP fixtures containing a
directory, a UTF-8 name, an empty archive, a broken archive and an entry-limit
archive. It checks ordering/sizes, bounded failure, stale results and panel hiding,
alongside all earlier text/image/PDF/media/EXIF/folder/Split View assertions.

On 2026-09-18 the application configured and built successfully. The focused
runner compiled, but its normal private D-Bus startup was blocked by the sandbox
before the test executable started. A sandbox-only offscreen diagnostic without
that private bus completed **101 assertions PASS**. This is not a substitute for
the normal runner in KDE and is not manual acceptance.

## Manual KDE/CachyOS acceptance

Follow the Stage 5 section in `TEST_CHECKLIST.md` using disposable archives.
Manual Stage 3, Stage 4 and Stage 5 acceptance remains pending and separate from
automated results.
