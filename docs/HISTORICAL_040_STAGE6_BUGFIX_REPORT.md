# Stage 6: two bug fixes — MANUAL PENDING

Version remains 0.39.0. No commit, tag, push or release. Existing Stage 6 work was preserved.

## Root causes

1. ActionStateController tied Properties to recoverySafe, a mutation fence. The shared menu/action availability therefore disabled the inspector together with Rename/Trash/Paste when writes were fenced. No filesystem-specific Properties condition exists at this point; the particular reason the live recovery gate was fenced is not established by the screenshot. Properties now requires one valid nonempty selected URL. Apply retains its recovery checks; all other action gates and backend capabilities remain unchanged. Deterministic fuseblk/ntfs3 fixtures check readable identity, read-only Hidden policy and inspector availability with writes disabled, including Search.

2. General embedded PropertiesHiddenWidget as a spanning QFormLayout row. The inner form's minimum-size constraint grew the child for wrapped labels, while the outer form retained insufficient row allocation. A production geometry test reproduced Hidden at y=40, height=142, with Type already at y=137. General now uses QVBoxLayout for the name form, Hidden widget and details form, so height-for-width allocation covers the entire Hidden block. Rendering invalidates inner and parent geometry caches. No fixed heights and no Hidden/capability semantics changed.

## Files changed in this task

- src/actionstatecontroller.cpp
- src/propertiesdialog.h
- src/propertieshidden.h
- tests/action-state-controller.cpp
- tests/properties-hidden.cpp
- This output directory contains verification artifacts, not release files.

## Validation

- Focused: 15 unique suites, 4,177 assertions, PASS.
- Full on final sources/tests: 64 unique suites, 35,045 assertions/checks, PASS; no duplicate suites. See totals.json and full.raw.log.
- The focused production layout checks cover PL/EN, ten open/close cycles, narrow/wide/narrow resize and asynchronous reload. Final full regression additionally checks vertical scroll and geometry after actual Apply + Hidden rename + refresh (Hidden suite: 1,318 assertions).
- Fresh scripts/build.sh configure/build and final incremental build: PASS.
- version-cli: 6/6 PASS, 0.39.0.
- git diff --check: PASS.
- Offscreen geometry tests do not establish desktop manual acceptance. ext4 remains NOT TESTED.

## User manual re-test

Close the old application instance and start the freshly built ~/Pobrane/kio-thispc/build/bin/thispc-view.

A. Open /mnt/g/ThisPC-040-stage6-fixture/primary and right-click regular.txt. Confirm Właściwości is enabled and opens. Inspect Ogólne, Metadane and Uprawnienia: state should reflect actual filesystem capabilities, and unsupported/read-only controls must remain disabled. Repeat opening from Split and Search. Opening Properties must not enable fenced Rename/Trash/Write actions.

B. On the local fixture /home/sebastianh/Pobrane/Testy/properties-040-stage6/data/interaction, use the existing regular.txt or .regular.txt. Enable display of hidden files if needed. Open/close Properties 5–10 times. In an open dialog toggle Ukryj, press Zastosuj, and inspect the refreshed dialog; then toggle back and Apply. Confirm Hidden/native-hidden/description never overlap Typ/MIME/Rozmiar or later rows. Narrow and widen the dialog, and reduce its height to confirm vertical scroll permits access to all rows. Check PL and EN. Do not reset fixture contents or re-run setup.

Stage 6 remains MANUAL PENDING until the user confirms both re-tests. No FULL MANUAL PASS is assigned here.
