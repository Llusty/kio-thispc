# ThisPC 0.40.0 — release verification (2026-10-08)

All local pre-publication gates PASS. Source/runtime: 0.40.0. GitHub publication is verified separately; this document does not claim publication in advance.

| Check | RUN SUITE / unique | Checks | Failures | Exit |
|---|---:|---:|---:|---:|
| Additional symlink ACL security sanity | 1 / 1 | 228 | 0 | 0 |
| Focused | 15 / 15 | 4359 | 0 | 0 |
| Full | 64 / 64 | 35064 | 0 | 0 |
| Version CLI (separate, not a suite) | — | 6 | 0 | 0 |

No duplicate suites. Counts were parsed from RAW logs and cross-checked against individual success summaries and exit files. Configure/build, advisory version check, working and staged diff checks PASS.

The additional security sanity executes the production Properties dialog, real KIO rename and ACL write refusal on disposable Btrfs fixtures. Both live and broken symlinks remain SymlinkUnavailable after rename; target mode stays 0644. All three ACL refresh paths retain the captured symlink policy. No new manual retest was required by this passing production-path regression and review. Prior user-confirmed Stage 1–6 FULL MANUAL PASS remains historical and was not rerun.

Source TAR.GZ and ZIP use the kio-thispc-0.40.0/ prefix, include all six new modules and six new tests, documentation, CMake, scripts, desktop/icons and MIT LICENSE. Generated outputs/build/fixtures and historical handoff data are excluded. All extracted candidate files were compared with their staged Git blobs; ZIP content matched TAR.GZ. The extracted source configured and built independently in Release mode, and version-cli passed 6/6.

DESTDIR install PASS. The uninstaller was exercised with a manifest mapped exclusively into the temporary staging root; its sudo wrapper permitted only removal of those files, while process/cache commands were replaced by no-ops. Every installed file was removed; missing-manifest refusal also passed. This verifies file-removal logic without touching the user's running applications or real installation. The live install.sh/sudo installation and desktop cache restart were not executed.

RAW evidence and exit files: outputs/properties-040-release-20261008/ in the original checkout (deliberately excluded from source assets). SHA256SUMS is shipped separately to avoid an archive self-checksum cycle. The final tag archive is checked against the independently built candidate; only release documentation may differ.

README, CHANGELOG, UPDATE and ROADMAP are maintained in PL/EN. Required libacl/pkg-config dependencies, current archive instructions, manifest requirement and the version-neutral uninstall banner are corrected. Historical drafts and audit remain preserved.

NOT TESTED: real ext4 and real remote KIO. Deterministic policies passed. Earlier hardware acceptance covers Btrfs, NTFS fuseblk/ntfs3 and real USB vfat removal/reconnect, Primary/Split/Search, lifetime and UI. Path-based KIO/libacl cannot provide atomic protection against hostile replacement during an operation. Scope excludes SELinux labels, chattr/immutable, Linux file capabilities, larger ACL redesign and admin:// retry.

No reset, clean, force push, modification of Testy/RESULTS.md, live system installation or filesystem mount/ACL changes were performed. Publication authorization came directly from the user. Publication confirmation and any subsequent roadmap status commit must follow actual GitHub verification.
