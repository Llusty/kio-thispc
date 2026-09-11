# TASK 0.19 — Session restore and full names

## Session restore
- Save all open tabs on clean application close.
- Restore tab order and active tab when launched without an explicit URL argument.
- Save/restore each tab's primary navigation history.
- Save/restore whether Split View is enabled for each tab.
- Save/restore the split-pane URL, view mode and sort state.
- Preserve splitter widths through the existing `split/state` setting.
- Save/restore which pane was active when practical.
- `thispc-view <path-or-url>` intentionally bypasses previous-session restore.
- View menu contains a persistent `Restore previous session` toggle, enabled by default.

## Full names
- In icon mode, a selected item expands and wraps its complete name.
- Unselected items stay compact and ellipsized by default.
- `Pełne nazwy` / `Full names` on the file command toolbar makes all names full.
- The full-name preference persists through QSettings.
- Existing tooltips remain available.
