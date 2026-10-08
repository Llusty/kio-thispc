# 0.40.0 Stage 4 — Numeric POSIX Permissions

Status: Stage 4 AUTOMATED PASS + FULL MANUAL PASS, confirmed by the user. Runtime/build version stays 0.39.0. The report below preserves historical stage verification.

## Entry worktree

HEAD, origin/main and v0.39.0^{} were all af39da5e1719e7d3fada3b437aab100260b9697f. Initial status contained only Stage 1–3 capability/xattr changes. Initial diff --check passed. Stage 1–3 manual acceptance is the user's supplied FULL MANUAL PASS; this stage did not repeat or relabel that manual work.

## Audit and scope

The existing modeless PropertiesDialog has nine rwx checkboxes, an existing recursive checkbox, an ACL editor, and shared Apply/OK/Cancel callbacks. It originally stored KIO UDS_ACCESS masked to 0777, and the nonrecursive branch called KIO::chmod(url, requestedMode) on every Apply. Stage 1 had already added device/inode/type EntryNoFollow identity revalidation. Initial local editing additionally depended on ownership and filesystem read-only state; generic remote editing was not enabled without administrator/local ownership. Numeric editing must not create remote capability discovery or an admin retry path.

The installed KF6 KIOCore headers were inspected: kio/simplejob.h exposes chmod(QUrl,int); kio/chmodjob.h exposes the existing list/mask/recursive overload with an explicit mutable-bit mask. There is no 9-bit restriction on the simple job argument. The original UI/model nevertheless discarded special bits, so the conservative Stage 4 policy is B: edit rwx only and repair preservation, without introducing special-bit editing or redesigning the permission model.

## Numeric policy and synchronization

PropertiesPosixMode is a syscall-free parser/formatter, capability predicate, checkbox converter and rwx/special-bit merge helper. Accepted input is exactly four ASCII octal digits with a leading zero: 0000–0777. No shorthand 755, signs, prefixes, whitespace, 8/9, incomplete strings or special-bit numeric input. The label explicitly says rwx; its PL/EN tooltip explains preservation of setuid/setgid/sticky.

Checkbox changes synchronously format the field. Valid numeric text synchronously updates all nine boxes. QSignalBlocker and a shared update guard suppress recursive/programmatic text signals. Invalid text leaves the last valid checkbox state pending, shows inline validation, and blocks Apply/OK before any write. A checkbox edit replaces invalid text with the canonical final checkbox mode.

## Write contract and safety

There is one existing KIO permission write branch; no direct production chmod, shell chmod or alternate backend. Nonrecursive requested mode merges the current known 07000 bits with checkbox rwx. KIO reads now retain 07777. Unchanged Apply does not dispatch chmod. A valid changed Apply sends one final mode to the existing simple KIO job. OK uses the same callback and closes only on success. Cancel destroys pending UI state. Merely opening or switching tabs never writes. Immediate Stage 3 xattr operations remain outside this contract.

The original Stage 1 revalidation remains in Apply. An additional local permission guard rejects absent identity and all non-SameTarget results before writes. Both use device/inode/type, not path equality. Missing and held-original/replace-at-same-path fixtures prove no replacement chmod. This is the existing path-based KIO contract with pre-write identity checks; it does not claim an atomic inode-pinned KIO transaction against adversarial concurrent replacement during a job.

Symlink and broken-symlink checkbox/numeric edits are disabled; there is no fallback chmod of a target. Numeric enablement requires a local regular file/directory, valid identity and Supported Stage 1 posixModeEditable, together with existing ownership/read-only gates. ReadOnly, PermissionDenied, Unsupported and Unknown are disabled. Remote schemes do no local capability syscalls and cannot enable numeric edits. The existing admin unlock and recursive checkbox are retained; numeric input is disabled while recursive mode is selected. No new recursive/admin behavior was added.

## ACL and errors

Numeric changes use the checkbox KIO chmod branch. They do not rewrite ACL blobs. After success, KIO stat refreshes actual mode and AclProvider refreshes the ACL widget. A production extended ACL fixture verifies that the kernel changes the mask/group class, retains the named user's stored rights, and exposes refreshed effective rights. Existing dirty-ACL precedence, explicit mask handling and ACL/recursive conflict policy remain intact.

Permission errors retain the existing KIO message/errorString and filesystem context. On chmod failure, a readable actual mode and ACL are refreshed; otherwise the pending state remains accompanied by the existing failure message. Full mode verification compares 07777, including preserved special bits; mismatch is not success. Missing/replaced errors block mutation with a controlled message. Invalid numeric input uses an inline PL/EN message without opening popups.

## Files changed for Stage 4

- src/propertiesposixmode.h: pure numeric policy/model.
- src/propertiesdialog.h: field, sync, validation, preservation, capability gate, refresh, no redundant chmod.
- src/acleditorwidget.h: refresh existing ACL presentation from fresh data.
- tests/properties-posix-mode.cpp: dedicated production/pure regression.
- tests/run-pane-actions.py: suite registration.
- CMakeLists.txt: header listed for both targets.
- ROADMAP.md and ROADMAP.pl.md: Stage 3 FULL MANUAL PASS; Stage 4 in progress.
- docs/PROPERTIES_POSIX_MODE.md: audit and acceptance report.

All preexisting Stage 1–3 files remain in the working tree. See git status for the combined uncommitted list.

## Manual acceptance

Fixture: /home/sebastianh/Pobrane/Testy/properties-040-stage4. README.md contains seven fish-compatible cases and the fresh build launcher; verify.py reads exact lstat mode with Python stdlib; replace.py holds the original inode and creates a 0600 replacement. It does not overwrite an existing held original. Numeric display is rwx only, e.g. disposable special.txt full 04644 displays 0644, rwx edit 0750 yields full 04750.

Remote KIO manual smoke remains NOT TESTED/non-blocking. Desktop manual acceptance remains pending; no full Stage 4 manual PASS is claimed. ReadOnly/PermissionDenied/Unsupported/Unknown numeric policy is tested via deterministic capabilities; real read-only mounts, privilege-denied worker failure and admin authorization were not manually exercised. No recursive numeric edit or direct special-bit edit is provided. Model selection of the running conversation cannot be changed or independently confirmed through the available tools.

STOP after Stage 4.

## Final automated results

Focused: 13 suites / 2828 checks / 0 failures / exit 0.

| Suite | Checks | Failures | Exit |
|---|---:|---:|---:|
| properties_posix_mode | 163 | 0 | 0 |
| properties_xattr_edit | 379 | 0 | 0 |
| properties_xattrs | 95 | 0 | 0 |
| properties_capabilities | 96 | 0 | 0 |
| properties | 147 | 0 | 0 |
| properties_data | 83 | 0 | 0 |
| properties_lifecycle | 97 | 0 | 0 |
| drive_properties | 131 | 0 | 0 |
| acl | 33 | 0 | 0 |
| acl_editor | 29 | 0 | 0 |
| panes | 1395 | 0 | 0 |
| selection_menu | 63 | 0 | 0 |
| remote_url | 117 | 0 | 0 |

Full regression: 62 suites / 33510 checks / 0 failures / exit 0.

| Suite | Checks | Failures | Exit |
|---|---:|---:|---:|
| trash | 998 | 0 | 0 |
| panes | 1395 | 0 | 0 |
| templates | 46 | 0 | 0 |
| tabs | 262 | 0 | 0 |
| sidebar_dnd | 45 | 0 | 0 |
| sidebar_layout | 64 | 0 | 0 |
| properties | 147 | 0 | 0 |
| search | 137 | 0 | 0 |
| actions | 175 | 0 | 0 |
| action_state | 20 | 0 | 0 |
| operations | 45 | 0 | 0 |
| local_transfer | 44 | 0 | 0 |
| transfer_plan | 33 | 0 | 0 |
| local_move | 53 | 0 | 0 |
| local_tree | 34 | 0 | 0 |
| tree_history | 23 | 0 | 0 |
| archive | 26 | 0 | 0 |
| archive_jobs | 468 | 0 | 0 |
| archive_menu | 1060 | 0 | 0 |
| archive_creation | 133 | 0 | 0 |
| split_layout | 143 | 0 | 0 |
| preview | 111 | 0 | 0 |
| quick_look | 27 | 0 | 0 |
| batch_rename | 1789 | 0 | 0 |
| view_settings | 497 | 0 | 0 |
| listing_core | 58 | 0 | 0 |
| drive_home | 20 | 0 | 0 |
| solid_monitor | 11 | 0 | 0 |
| device_mount | 49 | 0 | 0 |
| device_removal | 179 | 0 | 0 |
| split_compare | 291 | 0 | 0 |
| selection_menu | 63 | 0 | 0 |
| location_presentation | 54 | 0 | 0 |
| navigation_history | 14 | 0 | 0 |
| keyboard_navigation | 133 | 0 | 0 |
| remote_url | 117 | 0 | 0 |
| saved_remote | 101 | 0 | 0 |
| recent_reconnect | 118 | 0 | 0 |
| session_history | 164 | 0 | 0 |
| properties_data | 83 | 0 | 0 |
| properties_posix_mode | 163 | 0 | 0 |
| properties_xattr_edit | 379 | 0 | 0 |
| properties_xattrs | 95 | 0 | 0 |
| properties_capabilities | 96 | 0 | 0 |
| drive_properties | 131 | 0 | 0 |
| acl | 33 | 0 | 0 |
| acl_editor | 29 | 0 | 0 |
| checksums | 114 | 0 | 0 |
| metadata | 59 | 0 | 0 |
| properties_lifecycle | 97 | 0 | 0 |
| storage_scan | 21952 | 0 | 0 |
| storage_analysis | 97 | 0 | 0 |
| hash_utilities | 170 | 0 | 0 |
| duplicate_finder | 100 | 0 | 0 |
| duplicate_actions | 128 | 0 | 0 |
| storage_treemap | 179 | 0 | 0 |
| launch_url_resolver | 30 | 0 | 0 |
| preview_controller | 89 | 0 | 0 |
| image_video_previews | 103 | 0 | 0 |
| executable_previews | 73 | 0 | 0 |
| folder_previews | 96 | 0 | 0 |
| preview_settings | 67 | 0 | 0 |

Raw full log: /tmp/thispc-040-stage4-full.log; captured exit: /tmp/thispc-040-stage4-full.exit. RUN SUITE count = 62; duplicate_actions appears exactly once; version-cli is not a suite. Total 33510 is baseline 33347 plus 163 new suite assertions, verified from raw per-suite summaries.

Final build: PASS/exit 0. version-cli: 6/6 PASS/exit 0; executable reports thispc-view 0.39.0. Final git diff --check: PASS. HEAD/origin/main/v0.39.0^{} remain af39da5e1719e7d3fada3b437aab100260b9697f. Combined tracked diff before this documentation totals update: 9 files, 158 insertions, 56 deletions; untracked Stage 1–4 files are also present and intentionally uncommitted.

The earlier full attempt failed before suites because build.sh and the full harness preparation shared build output; the final full run was executed after build.sh finished. A guard-message correction was initially rejected by automatic approval review; the accepted correction retains both identity revalidation checks and adds the controlled message. No approval action remains blocked.

Recommendation: AUTOMATED PASS; Stage 4 manual acceptance pending (in progress, not FULL MANUAL PASS). STOP. No Stage 5, commit, tag, push or version bump.

## User-confirmed manual acceptance update — 2026-10-07

Stage 4: AUTOMATED PASS + FULL MANUAL PASS. The user confirmed baseline 0644, numeric/checkbox synchronization, Apply 0755, Cancel rollback, checkbox 0744, invalid-input rejection, setuid 04750 and sticky 01750 preservation, replacement identity guard with replacement remaining 0600, disabled symlink/broken-symlink editing with unchanged target, Primary/Split/Search parity, and no thispc-view process after closing. This supersedes the manual-pending status recorded above; automated results remain the historical Stage 4 results, not a new run.
