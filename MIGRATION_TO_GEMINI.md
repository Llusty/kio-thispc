# Migration to Gemini CLI

## Requirements

Gemini CLI requires Node.js 20+ and an internet connection.

Check:

```bash
node --version
npm --version
```

## Install the stable Gemini CLI

```bash
npm install -g @google/gemini-cli
```

If you do not want a global installation, you can run it with:

```bash
npx @google/gemini-cli
```

## Start in this repository

```bash
cd ~/Pobrane/kio-thispc
gemini
```

On first use, complete the sign-in flow offered by Gemini CLI.

Then verify project memory:

```text
/memory show
```

The project uses:

- `GEMINI.md` for persistent instructions;
- `.gemini/settings.json` with manual/default approval mode;
- `.geminiignore` to keep build artifacts out of file discovery;
- `TASK_0.19_NAME_LAYOUT.md` as the immediate task.

## Recommended approval policy

Keep the default approval mode initially. Allow normal read/edit/build/test commands after checking them, but manually review commands involving `sudo`, deletion, Git history rewriting or mounted NTFS volumes.
