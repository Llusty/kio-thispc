# DEVELOPMENT.md

## Recommended Codex workflow
This repository is deliberately shipped as a Git repository. Stay on `main` for new development unless creating a dedicated feature branch.

```bash
git status
git switch main
git switch -c feature/0.17-drag-drop
./scripts/build.sh
```

Then make one focused change at a time and rebuild.

## Build without install
```bash
./scripts/build.sh
```

Equivalent manual commands:
```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

## Install for the user's normal runtime test
```bash
chmod +x install.sh
./install.sh
```
Then:
```bash
thispc-view
```

`install.sh` performs a clean build; preserve that behavior.

## Useful source landmarks
To orient quickly:
```bash
grep -n '^class ' src/thispcview.cpp
grep -n 'FileUndoManager' src/thispcview.cpp
grep -n 'SplitBrowserPane' src/thispcview.cpp
grep -n 'operationPopup' src/thispcview.cpp
```

## Release checklist
Before calling a build a release:
```bash
./scripts/check-version.sh
./scripts/build.sh
```
Then execute the relevant manual tests from `TEST_CHECKLIST.md`.

## Packaging convention
Historically the user receives both ZIP and TAR.GZ, each containing a top-level `kio-thispc/` directory.
For local Codex work, Git commits/tags are preferred; archives are only needed when handing a release back to the user.

## Git convention suggested for future work
- stable releases on `main`;
- feature branches such as `feature/0.17-drag-drop`;
- hotfix branch only when needed;
- tag user-confirmed stable versions, e.g. `v0.17.0` after confirmation.

Avoid huge commits mixing refactor + feature + formatting.
