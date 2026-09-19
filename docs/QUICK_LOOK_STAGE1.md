# 0.27.0 — Quick Look on Space

Status: implementation complete; all 9 manual KDE/CachyOS acceptance cases were confirmed by the user on 2026-09-19.

## Staged plan

1. **Core interaction (complete):** Space/Esc overlay, active-pane
   routing, focus-safe keyboard handling, shared asynchronous preview backend,
   stale-result protection and deterministic regression coverage.
2. **KDE acceptance and UI polish (complete):** verified real Plasma/Wayland stacking,
   palette, scaling, minimum-window geometry and keyboard behavior across all
   view modes; make only evidence-based visual/accessibility adjustments.
3. **Release preparation (in progress):** update version and release documentation,
   then repeat focused and full automated regression. Commit, tag, package creation,
   installation and publication remain separate authorized steps.

## Design

Quick Look is a large, temporary presentation layer around the existing
`PreviewPane`. It deliberately reuses the same asynchronous loaders, format
support, size limits, messages, image scaling and request tokens. No parser or
media playback path is duplicated.

The overlay is a child of the main window, does not take keyboard focus and is
routed from the currently active browser pane. Space is accepted only while the
active directory list/details view owns focus. Escape closes an open overlay.
Text editors, Search, address/rename editors, popups and modal dialogs keep
their normal Space/Escape behavior.

Selection changes and active-pane changes submit a new preview request. Hiding
the overlay invokes `PreviewPane::hideEvent`, invalidating both the content
request and image-scaling request so late results cannot reappear.

## Manual KDE checklist

- In the left pane select a supported image, text file and PDF; press Space and
  confirm a large preview opens without launching another application.
- With Quick Look open, use arrow keys through supported, unsupported and
  oversized files; confirm the title/content follows the current selection and
  the UI remains responsive.
- Enable Split View, open Quick Look from each pane, then switch the active pane;
  confirm the preview follows the active pane only.
- Press Space and Esc to close; confirm focus remains in the file view and
  normal keyboard navigation continues.
- Type spaces in Search, Ctrl+L, inline rename and a file dialog; confirm Quick
  Look does not open.
- Toggle Alt+P before and during Quick Look; confirm Preview Pane still works
  independently and has no stale result after being hidden and shown again.
- Close Quick Look while a large preview is loading, change selection rapidly
  and resize the window; confirm no stale content, freeze or crash.
