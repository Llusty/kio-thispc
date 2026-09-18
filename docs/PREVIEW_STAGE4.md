# 0.26.0 Preview Pane — Stage 4

Stage 4 adds read-only audio/video metadata through TagLib and a bounded EXIF summary
through Exiv2. Metadata loading runs in the existing Preview worker task: it never starts
playback, launches a helper process or blocks the GUI thread. The existing request token
discards results after a newer selection or after hiding the pane.

## Scope and limits

- Local audio/video files only; remote URLs are never fetched.
- Media files above 4 GiB are rejected before TagLib is opened.
- Text values are stripped of embedded line breaks, each value is capped at 1024
  characters and the complete summary at 64 KiB.
- Audio/video fields: title, artist, album, year, track, duration, bitrate, sample rate
  and channels when the container exposes them.
- EXIF fields: camera make/model, capture time, exposure, aperture, ISO and focal length.
- EXIF errors are intentionally non-fatal: the image preview remains available.

The deterministic preview regression creates its own one-second WAV, JPEG with EXIF,
invalid MP4 and the existing image/PDF/text/folder fixtures. It uses no personal media.

## Manual KDE/CachyOS acceptance

Use disposable or non-sensitive local files and follow the Stage 4 section in
`TEST_CHECKLIST.md`. Confirm real MP3/FLAC/WAV, TagLib-supported MP4/M4V and camera JPEG behavior, rapid
selection changes, Alt+P cancellation and the 4 GiB guard. This manual acceptance remains
separate from automated regression results.
