# First Gemini CLI session

Start Gemini CLI from the repository root and paste this as the first task:

> Read `GEMINI.md` and `TASK_0.19_NAME_LAYOUT.md`. Inspect the current 0.19.0.3 implementation and the screenshots under `handoff/screenshots/`. Do not modify files yet. Diagnose why selected long filenames are still inconsistently clipped and why enabling “Pełne nazwy / Full names” makes the icon grid visually misaligned. Trace Qt's actual item geometry and delegate/overlay behavior. Then propose at most two robust solutions, with a recommendation. Preserve fixed grid geometry, session restore, Drag & Drop and Split View. Do not implement until you have shown the diagnosis and plan.

Useful Gemini CLI commands:

- `/memory show` — verify that project context from `GEMINI.md` is loaded.
- `/memory reload` — reload context after editing GEMINI.md.
- `/stats model` — inspect model usage/statistics if available in the installed release.
- `/quit` — leave the session.

To resume the latest CLI session later:

```bash
gemini -r latest
```
