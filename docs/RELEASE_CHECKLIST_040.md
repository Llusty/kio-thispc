# Release checklist — ThisPC 0.40.0

Explicit user authorization received 2026-10-08. The original DRAFT remains local historical reference.

- Repo and upstream baseline af39da5 verified; all expected Stage 1–6 changes preserved.
- Version 0.40.0, final PL/EN notes, README/CHANGELOG/UPDATE/ROADMAP integration complete.
- Additional symlink rename/ACL production regression: PASS, 228 checks.
- Build/focused/full/version/diff gates: PASS; see [release verification](PROPERTIES_RELEASE_040.md).
- Candidate TAR.GZ/ZIP completeness, independent Release build, CLI 6/6 and isolated staging install/uninstall: PASS.
- Selective source/docs commit and annotated v0.40.0: checked at finalization.
- Final tag archives must match tested candidate sources; publish both archives and SHA256SUMS.
- Push main and tag without force; verify remote targets; create and verify actual GitHub Release and downloadable assets.
- Only after verified publication, record released status in both roadmaps with a separate documentation commit; do not move the tested tag.
- Preserve outputs, historical drafts, Testy, RESULTS.md and user fixtures.

Publication status is verified externally after all local gates; this checklist does not claim publication ahead of GitHub confirmation.
