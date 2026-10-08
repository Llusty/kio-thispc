# 0.40 Stage 5 — Hidden semantics

Status: Stage 5 AUTOMATED PASS + FULL MANUAL PASS, confirmed by the user (including clean shutdown). Runtime version: 0.39.0. Stage 5 real NTFS/remote smoke was NOT TESTED at that stage; the Stage 6 handoff confirms real NTFS tests. Real remote KIO remains NOT TESTED. See PROPERTIES_FINAL_AUDIT_040.md. The report below preserves historical stage verification; current integration is documented in PROPERTIES_STAGE6_INTEGRATION.md.

## Official contract and worktree audit

Official user-supplied contract: audit Unix/NTFS/other backends, present true hidden state, write only where semantics are unambiguous and safe. Unsupported or ambiguous writes stay disabled/read-only. Stage 1–4: automated + FULL MANUAL PASS, confirmed by the user. Both roadmaps now retain the official six-stage contract and historical results.

Entry HEAD is af39da5e1719e7d3fada3b437aab100260b9697f. All entry changes are attributed:

- Stage 1: propertiescapabilities.h, PropertiesData/Provider capability plumbing, PropertiesDialog EntryNoFollow identity snapshot/revalidation; properties-capabilities.cpp.
- Stage 2: propertiesxattrs.h, MetadataWidget/PropertiesDialog integration, propertiesxattrwidget.h read-only rendering; properties-xattrs.cpp.
- Stage 3: propertiesxattrwriter.h and propertiesxattrwidget.h mutation UI; properties-xattr-edit.cpp; PROPERTIES_XATTR_EDIT.md.
- Stage 4: propertiesposixmode.h, PropertiesDialog numeric synchronization/validation/preservation and readback, AclEditorWidget refresh; properties-posix-mode.cpp; PROPERTIES_POSIX_MODE.md.
- Shared registrations: CMakeLists.txt and run-pane-actions.py. ROADMAP.md/pl.md record all stages.

No unexplained change was found. Stage 1–4 edits remain intact and uncommitted. Inspection covered the tracked diff, untracked modules/tests, audit documents and Properties architecture. Older 0.37/0.38 stage numbering is historical, not the contract for this work.

## API audit and backend matrix

Primary sources:
- [KIO UDSEntry](https://api.kde.org/kio-udsentry.html): UDS_HIDDEN overrides the dot-name default, including an explicit zero. It is a visibility directive, not proof of a native filesystem attribute.
- [KIO 6.30 file worker](https://raw.githubusercontent.com/KDE/kio/v6.30.0/src/kioworkers/file/file_unix.cpp): listDir has an NTFS hidden probe using system.ntfs_attrib_be; stat does not run that probe. Rename checks destinations with lstat and honors the no-overwrite job flag. Its checks and rename are not atomic against hostile concurrent substitution.
- [KFileItem](https://raw.githubusercontent.com/KDE/kio/master/src/core/kfileitem.cpp): default hidden semantics are a leading dot in a name longer than one character, overridden by worker metadata.
- [Linux NTFS3](https://cdn.kernel.org/doc/html/latest/filesystems/ntfs3.html): NTFS has its own HIDDEN attribute and mount visibility policy. Dot-name is not a replacement for that attribute.

Installed KIO headers expose UDS_HIDDEN and existing list/rename jobs but no portable native-hidden mutation job. The existing Stage 3 writer expressly excludes system.*. No protected-namespace writer, shell, QProcess, native flag ioctl, mount change or administration path is introduced.

| Backend | Read semantics | Stage 5 write |
|---|---|---|
| Local ext2/ext3/ext4/Btrfs/XFS/tmpfs | Leading dot; KIO override takes precedence | Dot-name rename through shared KIO path, only with valid identity, ordinary file/directory, writable/readable parent, writable filesystem, known listing and no UDS override |
| NTFS/ntfs3/FUSE/fuseblk | KIO-reported visibility; native flag remains explicitly unknown without verified native flag API | Disabled; no dot-name emulation of native HIDDEN |
| FAT/exFAT/CIFS/NFS/unknown local mount | KIO visibility; native attribute unknown | Disabled |
| SMB/SFTP/FTP/WebDAV/admin/virtual URLs | Explicit UDS_HIDDEN or KIO name fallback from a successful listing | Disabled; no local filesystem calls in hidden provider remote branch |
| Symlink/broken symlink | Own entry name and worker visibility report; no claim about native target attribute | Disabled |
| Missing/replaced/unreadable/redirection/no listing match | Unknown/error; no invented false state | Disabled |

The new UI explicitly labels visibility as “Hidden in ThisPC (KIO)”, the pending hide action as “Hide” (PL: “Ukryj”), and native attribute separately. A backend directive is never labeled a verified native attribute. The absence of UDS_HIDDEN never means that an NTFS native flag is false. ThisPC uses direct ListJob metadata in its listings; Dolphin/KDirLister .hidden-file conventions are not added or claimed. Stage 5 does not alter filtering behavior in browser views.

## Architecture and acceptance criteria

propertieshidden.h holds the policy, cancellable KIO parent-list provider and compact General-tab widget. It retains no full parent listing, requests no recursion and stops after finding one matching name. Local reads check the original device/inode/type before and after asynchronous metadata delivery. Cancellation, target generations, receiver lifetime and redirection guards discard unrelated results. Remote reads stay in KIO.

The checkbox stages a dot prefix/removal in the existing Name field; no operation runs until Apply/OK. Cancel discards UI state. Multi-dot, root, invalid/trim-sensitive names, native/unknown filesystems, worker overrides, unavailable identity, read-only parents/filesystems and links cannot enable it. Leading-dot editing is explicitly a rename, not a native flag mutation. The checkbox and Name field synchronize without recursive signals; saved visibility remains distinct from pending edits.

One shared KIO::moveAs branch remains the write authority, with the existing recovery gate, Undo record callback and refresh callbacks. Stage 5 adds identity revalidation immediately before rename, after any prior permission operation. Destination guards now include broken symlinks. No Overwrite flag is passed. Successful rename refreshes hidden metadata and General data at the new URL while retaining the captured identity. This remains the existing path-based KIO contract, not an inode-pinned atomic rename. A concurrent replacement between worker checks and rename is outside that guarantee.

Acceptance:
1. Display actual KIO state and its source, respect explicit UDS_HIDDEN=0, never infer a native attribute from the directive.
2. Supported Unix checkbox stages exactly one dot-name rename; Apply/OK uses shared KIO/Undo; Cancel and opening do not mutate.
3. Same inode/content/mode/mtime after successful rename; readback uses new URL.
4. Existing file/directory/broken-symlink destination is protected; missing/replaced source is blocked.
5. Native/unknown/remote/virtual/link writes are disabled. Missing/read errors show unknown, not “No”.
6. Closing/canceling/changing targets suppresses stale asynchronous results.
7. Real Primary/Split/Search route opens the same production widget and provider.
8. Focused/build/version/diff/full regression pass; desktop manual acceptance remains pending.

## Scope limits / NOT TESTED

No real NTFS/FAT/CIFS/remote service was modified or mounted for tests. Real backend-native flag reads and remote live smoke remain NOT TESTED. No promise is made for worker-specific visibility absent from listing metadata, mount-level suppression, Dolphin .hidden conventions, native hidden mutation, recursive hidden changes, hostile atomic rename races, or inode generation reuse. A long/slow parent listing can delay visibility; it remains asynchronous and cancellable. External changes while a dialog is idle require reopening; writes revalidate identity. Stage 6 integration/mount-removal matrix is not started.

## Manual Test 1 and UI polish (2026-10-07)

User-reported functional Test 1 PASS: regular.txt is visible/editable, .hidden.txt is hidden/editable, ..ambiguous.txt is hidden/read-only. The same test exposed overlapping/clipped General text, so manual acceptance remains pending.

The checkbox now reads “Ukryj” / “Hide”. The hidden form grows non-fixed fields, wraps long rows and propagates height-for-width through minimum vertical size policies for its text and container. Layout constraints preserve the required height. General is inside a resizable vertical scroll area, so rows stay readable when the content exceeds the available dialog height. No fixed row heights were added. KIO/native source text and behavior explanations retain their meaning; policy, provider, identity/capability rules and write path are unchanged.

Production-dialog geometry coverage exercises EN/PL, visible/hidden/ambiguous files and requested widths of 630/500 pixels. It checks wrapped text height, containment and overlap with other text, the checkbox and neighboring General rows. Offscreen results do not replace desktop acceptance. Recheck layout and the new label for the same three files, including resizing/scrolling, then continue remaining manual tests. Stage 6 remains unstarted; runtime version 0.39.0; no release operations.

UI polish verification: focused 15 suites / 3811 checks / 0 failures / exit 0; full 63 suites / 34457 checks / 0 failures / exit 0. Counts parsed from raw per-suite summaries, one summary per suite, no duplicate names. Hidden suite: 947 checks. Final scripts/build.sh, version-cli (6/6) and git diff --check: PASS. Logs/exits and parsed totals are saved under /home/sebastianh/Pobrane/2026-10-07/referenced-chatgpt-conversation-this-is-an-3/outputs.

The initial sandbox attempt stopped before test execution because D-Bus socket creation was blocked. The first unsandboxed geometry run exposed compression by the enclosing General form. The final focused/full results above include the scroll-area fix; prior attempt logs are retained.

## Original Stage 5 verification (before UI polish)

Focused: 15 RUN SUITE / 3137 checks / 0 failures / exit 0. New properties_hidden suite: 172 checks. All affected Properties, ACL, lifecycle, pane, selection, remote URL and Search suites pass.

Full: 63 RUN SUITE / 33682 checks / 0 failures / exit 0. Each suite has one parsed success summary; no duplicate suite names. duplicate_actions appears exactly once. Totals were parsed from raw suite summaries, including alternate Batch Rename/saved_remote/duplicate_actions output formats; not inferred from the old baseline.

Build (scripts/build.sh): PASS / exit 0. version-cli: 6/6 PASS / exit 0. Runtime and CMake version 0.39.0. git diff --check: PASS / exit 0. main, HEAD, origin/main and v0.39.0^{} remain af39da5e1719e7d3fada3b437aab100260b9697f.

Raw final logs and captured exits: /tmp/thispc-040-stage5-focused-3.log/.exit, /tmp/thispc-040-stage5-full.log/.exit, /tmp/thispc-040-stage5-build.log/.exit, /tmp/thispc-040-stage5-version.log/.exit, /tmp/thispc-040-stage5-diff-check.log/.exit. Durable copies plus parsed per-suite results are in the calling chat outputs directory.

The first focused attempt compiled but sandbox D-Bus socket creation failed (127 from dbus-run-session; harness exit 1). The second found a test setup error: a default This PC window instead of a fixture-folder window during asynchronous route verification. No product failure was inferred; the fixture-window setup was corrected and the final focused and full runs both pass. Earlier raw attempt logs and exits are preserved.

Manual fixture: /home/sebastianh/Pobrane/Testy/properties-040-stage5. README.md has seven fish-compatible cases, setup.py creates without overwriting existing fixtures, replace.py holds the old inode and creates a 0600 replacement, verify.py checks identity/content/mode/mtime. Initial fixture verifier exit 0 is setup validation, not desktop acceptance. The Stage 5 fixture remains until the user finishes manual acceptance.

Stage 5 changed files: src/propertieshidden.h; src/propertiesdialog.h; tests/properties-hidden.cpp; tests/run-pane-actions.py; CMakeLists.txt; ROADMAP.md; ROADMAP.pl.md; docs/PROPERTIES_HIDDEN_SEMANTICS.md. The previous Stage 4 acceptance documentation update and all Stage 1–4 changes remain preserved.

STOP. Stage 6 not started. No commit, tag, push, installation or version bump.

Accepted Stage 4 fixture was archived to the chat outputs (properties-040-stage4-accepted.tar.gz); originals retained in chat work/accepted-stage4-fixture. Stage 5 fixture was not moved or deleted.
