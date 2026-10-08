# 0.40 Stage 6 — Final integration & regression

Current acceptance: **AUTOMATED PASS + FULL MANUAL PASS**, prior manual acceptance confirmed by the user in the final audit handoff on 2026-10-08. No manual tests were rerun by the auditor. See PROPERTIES_FINAL_AUDIT_040.md for current automated results and the ACL symlink follow-up.

The report below is historical (2026-10-07), including its pending/NOT TESTED labels; current handoff supersedes those labels only for explicitly confirmed cases. ext4 and real remote KIO remain NOT TESTED. RESULTS.md is preserved verbatim.

Historical status: **AUTOMATED PASS / manual pending** (2026-10-07). Stage 1–5: automated + FULL MANUAL PASS, confirmed by the user. Runtime/build version remains **0.39.0**. 0.40 is not completed. No commit, tag, push, installation or version bump.

## Worktree audit before changes

Local main checkout: `/home/sebastianh/Pobrane/kio-thispc`; HEAD `af39da5e1719e7d3fada3b437aab100260b9697f`. Inspected both roadmaps, tracked diff/status, Stage 3/4/5 audit documents, capability/xattr/POSIX/hidden modules and related tests, Properties lifecycle, action routes, device/mount controllers and Solid notifications. Stage 1/2 history and verification are recorded in the roadmaps and Stage 3/5 audits.

Every entry change was attributable to Stage 1–5:

- Stage 1: propertiescapabilities.h, PropertiesData/Provider plumbing, original identity guards, properties-capabilities.cpp.
- Stage 2: propertiesxattrs.h, MetadataWidget and PropertiesXattrWidget viewer, properties-xattrs.cpp.
- Stage 3: propertiesxattrwriter.h, mutation widget, properties-xattr-edit.cpp, PROPERTIES_XATTR_EDIT.md.
- Stage 4: propertiesposixmode.h, numeric/shared chmod integration, AclEditorWidget refresh, properties-posix-mode.cpp, PROPERTIES_POSIX_MODE.md.
- Stage 5: propertieshidden.h, dialog General scroll/wrapping and shared rename guards, properties-hidden.cpp, PROPERTIES_HIDDEN_SEMANTICS.md.
- Shared registrations/status: CMakeLists.txt, tests/run-pane-actions.py, ROADMAP.md/pl.md.

No foreign/unexplained change was found. All inherited changes remain uncommitted. No synced ChatGPT project source was edited.

## Integration matrix

| Surface | Open/capability | xattr viewer/editor | POSIX/ACL | Hidden/rename | identity/link/read-only | Apply/OK/Cancel and refresh |
|---|---|---|---|---|---|---|
| Primary | shared production dialog/provider | real add/readback, inherited edit/remove and namespace guards | real numeric chmod, ACL after rename | real KIO dot-name rename | Stage 1–5 guards, loss invalidation | real production actions and shared callbacks |
| Split | same shared implementation | same | same | same | same | same |
| Search | real file target from Search action context | same | same | same | same | same; view refresh routes through existing Search refresh |

New 209-check suite exercises production routing on all three surfaces, real KIO chmod/moveAs, real FD xattr mutation/readback, ACL write after rename, old-path replacement protection, content/identity/mode/xattr interaction, Apply/OK/Cancel and refresh callbacks. Search results are injected into the real action view for deterministic routing; actual KIO Search/filter/cancel behavior is independently covered by the Search suite. Offscreen tests do not establish desktop manual acceptance.

The mutation-callback test intercepts pane refresh after it reaches the real shared callback; it does not pretend to verify KDE desktop painting. Existing adapter/Primary/Split/Search refresh implementation was audited and its tests pass. Desktop listing/filter/search presentation remains in the manual checklist.

## Fixes

1. **ACL retarget after rename:** successful rename refreshes ACL data at the new URL. Before this fix, the ACL widget retained its original path. The test places a replacement at that old path and verifies the next ACL write modifies the renamed item only.
2. **xattr refresh integration:** successful immediate xattr mutation emits attributesChanged and invokes the shared pane refresh and General provider reload. xattrs stay outside Apply/Cancel/Undo; existing async generation guards remain.
3. **General readback:** successful Apply reloads PropertiesDataProvider, including chmod-only changes.
4. **Event-driven target loss:** local dialogs watch the target and its path ancestors with QFileSystemWatcher and observe SolidDeviceMonitor notifications. Watching only the inode/parent missed a moved parent, so ancestor observations cover path-component disappearance. Reads use the original no-follow identity; it is never replaced by a newly resolved replacement. Detection latches unavailable and disables Name, numeric/checkbox permissions, Hidden, ACL and xattr mutation, hides admin unlock and shows a controlled error. Apply/OK cannot resurrect write access; reopening is required. Watches/notifications are local-only and owned by the dialog; no directory scanning or polling was added.
5. **Close sequencing:** PropertiesLifecycle defers main-window close while a PropertiesWindow is busy. QPointer owner tracking schedules retry when Apply becomes idle, so parent destruction cannot delete the dialog inside KJob::exec. Tests cover explicit busy deferral and a close event inside a real chmod Apply.
6. **Deterministic xattr test:** a prior test waited for a worker read count, then dereferenced an unrendered table cell. A reproduced core/backtrace identified QTableWidgetItem::text(this=0) at that test line. The test now waits for the new-generation row to render before inspecting it. Generation/stale-result assertions are retained; no product failure was hidden.

Original identity and pre-rename revalidation guards remain. xattr writes are FD-pinned/no-follow. KIO chmod/rename and ACL remain their existing path-based contracts: preflight identity checks do not promise an atomic inode-pinned transaction against adversarial substitutions inside an already-running backend job. In-flight xattr invalidation suppresses stale UI delivery, not an already-started syscall; it never targets an unconfirmed replacement.

## Disappearance / mount / lifetime coverage

Production dialog tests cover external deletion, external rename, held-original replace-at-same-path, parent-directory relocation, listing/model navigation while Properties is open, disabled mutations, controlled errors and close sequencing. Existing lifecycle/provider suites additionally cover missing directories, checksum/metadata worker close, coexistence and provider cancellation.

The deterministic storage test disconnects filesystem watches, makes the old path unavailable and emits the actual SolidDeviceMonitor::devicesChanged signal. It verifies the dialog's production unavailable transition. DeviceRemovalController, DeviceMountController, SolidDeviceMonitor and DriveProperties suites cover their simulated removal/accessibility state machines. This is **not a real unmount or physical unplug test**. Hardware/eject/real mount loss remain manual/NOT TESTED.

## Filesystem/backend matrix

| Backend | Automated evidence | Real desktop / hardware |
|---|---|---|
| Btrfs | **TESTED**: new suite's disposable disk fixture resolves Btrfs; real capability, xattr, KIO mode/hidden and ACL interaction. Final manual fixture is on Btrfs and seeded xattrs without errors. | **PENDING** |
| ext4 | **TESTED deterministic policy**: supported Unix Hidden/POSIX; generic local syscall/identity/xattr tests. No available ext4 mount was detected. | **NOT TESTED** |
| NTFS / ntfs3 / fuseblk | **TESTED deterministic policy**: no dot-name native-hidden emulation; POSIX remains capability/ownership/mount driven, with readback. Actual mounts detected through a read-only audit; no data was changed. | **NOT TESTED / manual pending** for live capability/native-hidden desktop smoke |
| Remote | **TESTED deterministic**: no injected local syscalls in resolver/revalidation; inherited reader/writer tests enforce remote Unsupported/read-only and zero local mutation. | **NOT TESTED**: no safe endpoint |

Host mount audit found Btrfs and NTFS: `/mnt/c`, `/mnt/d`, `/mnt/g` (fuseblk), `/mnt/e-raid`, `/mnt/f-raid` (ntfs3); no ext4. Actual host mount options are rw. The sandbox's synthetic ro view was not mistaken for real host read-only configuration. No NTFS test folder was created or modified by the agent.

## Tests and exact final results

New: tests/properties-integration.cpp (209 checks); registration in tests/run-pane-actions.py. Modified: tests/properties-xattr-edit.cpp (379 checks), fixing the worker/UI synchronization race. Focused includes all Properties Stage 1–5, PropertiesData/lifecycle/DriveProperties, ACL/editor, panes/selection, remote URL, Search and device/removal/mount/Solid suites.

| Verification | Suites | Checks | Failures | Exit |
|---|---:|---:|---:|---:|
| Focused final | 19 | 4360 | 0 | 0 |
| Full final | 64 | 34666 | 0 | 0 |
| version-cli | — | 6 | 0 | 0 |
| scripts/build.sh | — | — | — | 0 |
| git diff --check | — | — | — | 0 |

Full totals were parsed from the **final raw log**, with exactly 64 RUN SUITE entries, 64 success summaries, and no duplicate suite names; duplicate_actions runs once. Harness exit 0 confirms every subprocess completed successfully. No totals were inferred from historical baselines.

Durable logs/exits and per-suite JSON: `/home/sebastianh/Pobrane/kio-thispc/outputs/properties-040-stage6/`:
- thispc-stage6-focused-pass.log/.exit and .totals.json
- thispc-stage6-full.log/.exit and .totals.json
- thispc-stage6-build.log/.exit
- thispc-stage6-version.log/.exit
- thispc-stage6-diff-check.log/.exit
- parse-totals.py reproduces the count from the raw log.

Earlier failed compile/test attempts, the parent-relocation failure and xattr core backtrace/debug logs are preserved separately; none is the final acceptance baseline. Core binary contents are not packaged. Fixture script self-check is setup verification only, not manual acceptance.

## Stage 6 changed files

- src/propertiesdialog.h: target-loss state, observation, lifecycle owner deferral, ACL retarget, General/xattr refresh.
- src/propertiesxattrwidget.h: invalidation and successful mutation notification.
- src/thispcview.cpp: defer owner close during busy Properties Apply.
- tests/properties-integration.cpp: new production integration suite.
- tests/properties-xattr-edit.cpp: wait for rendered new-generation result.
- tests/run-pane-actions.py: suite registration.
- ROADMAP.md and ROADMAP.pl.md: Stage 1–5 confirmed; Stage 6 AUTOMATED PASS/manual pending, latest Stage 5 polish totals retained.
- docs/PROPERTIES_XATTR_EDIT.md, PROPERTIES_POSIX_MODE.md, PROPERTIES_HIDDEN_SEMANTICS.md: current acceptance status over historical reports.
- docs/PROPERTIES_STAGE6_INTEGRATION.md: this audit/report.
- outputs/properties-040-stage6/: untracked raw evidence and captured exits, attributed to Stage 6.

The previously modified CMakeLists, AclEditorWidget, MetadataWidget, PropertiesData/Provider and Stage 1–5 modules/tests are inherited, not unrelated Stage 6 changes.

## Manual acceptance and STOP

Final fixture: `/home/sebastianh/Pobrane/Testy/properties-040-stage6`.
README.md and RESULTS.md define six groups in order: Primary/Split/Search final parity; disappearance/replacement/parent; Btrfs interaction; safe ext4/NTFS folder and native-hidden presentation; disposable real mount/eject/removal; repeated dialog/app close and `pgrep -a -x thispc-view`.

Scripts: setup.py (new folders only), action.py (recorded disposable targets only), verify.py (read-only identity/content/mode/xattr reporting). All commands are fish compatible. NTFS/ext4 setup requires the user to select and create a dedicated new ThisPC-040-stage6-fixture folder; no automatic mounting, privilege change, mount options, UserMapping or user data writes. A temporary copy verified the scripts; final fixture data remain untouched for user acceptance.

Real NTFS/remote smoke from Stage 5 remains NOT TESTED until actually performed. Real mount/physical removal and ext4 are not guessed. Stage 6 FULL MANUAL PASS requires the user's results. Out of scope: SELinux labels, chattr/immutable, Linux file capabilities, larger ACL redesign, admin:// retry, new remote write semantics and release preparation.

**STOP — AUTOMATED PASS / manual pending. No commit, tag, push or version bump.**
