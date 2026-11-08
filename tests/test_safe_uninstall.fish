#!/usr/bin/env fish
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT
#
# Deterministic isolated security test for SEC-045-04 and SEC-045-07:
# - Production binary build/bin/thispc-view has zero runtime THISPC_TEST_HARNESS hooks
# - uninstall.sh strictly validates install_manifest.txt in an isolated staging tree
#   without touching the host system.

set -g REPO_ROOT (realpath (dirname (status --current-filename))/..)
set -g PASS_COUNT 0

function assert_true --argument-names cond desc
    if test "$cond" = "true"
        set -g PASS_COUNT (math $PASS_COUNT + 1)
        echo "  [PASS] $desc"
    else
        echo "  [FAIL] $desc" >&2
        exit 1
    end
end

set -l STAGE_DIR (mktemp -d /tmp/thispc-sec04507-stage.XXXXXX)

function cleanup --on-event fish_exit
    if test -n "$STAGE_DIR" -a -d "$STAGE_DIR"
        rm -rf -- "$STAGE_DIR"
    end
end

echo "=== SEC-045-04: Production binary THISPC_TEST_HARNESS guard check ==="
set -l PROD_BIN "$REPO_ROOT/build/bin/thispc-view"
assert_true (test -x "$PROD_BIN"; and echo true; or echo false) "SEC-045-04.1: build/bin/thispc-view exists and is executable"
set -l HARNESS_STRINGS (strings "$PROD_BIN" | grep -c "THISPC_TEST_HARNESS"; or true)
assert_true (test "$HARNESS_STRINGS" = "0"; and echo true; or echo false) "SEC-045-04.2: build/bin/thispc-view contains 0 runtime THISPC_TEST_HARNESS strings"

echo "=== SEC-045-07: Safe uninstall.sh manifest validation in isolated staging root ==="
set -g FAKE_ROOT "$STAGE_DIR/root"
mkdir -p "$FAKE_ROOT/usr/bin"
mkdir -p "$FAKE_ROOT/usr/lib/qt6/plugins/kf6/kio"
mkdir -p "$FAKE_ROOT/usr/share/applications"
mkdir -p "$FAKE_ROOT/usr/share/knotifications6"
mkdir -p "$FAKE_ROOT/usr/share/icons/hicolor/scalable/places"
mkdir -p "$STAGE_DIR/outside"

set -l F_BIN "$FAKE_ROOT/usr/bin/thispc-view"
set -l F_KIO "$FAKE_ROOT/usr/lib/qt6/plugins/kf6/kio/thispc.so"
set -l F_DESK "$FAKE_ROOT/usr/share/applications/org.kde.thispcview.desktop"
set -l F_NOTIF "$FAKE_ROOT/usr/share/knotifications6/org.kde.thispcview.notifyrc"
set -l F_ICON1 "$FAKE_ROOT/usr/share/icons/hicolor/scalable/places/kio-thispc-drive-0.svg"
set -l F_ICON2 "$FAKE_ROOT/usr/share/icons/hicolor/scalable/places/kio-thispc-removable-50.svg"
set -l F_OUTSIDE "$STAGE_DIR/outside/secret.txt"
set -l F_OTHER_BIN "$FAKE_ROOT/usr/bin/other-tool"

for f in $F_BIN $F_KIO $F_DESK $F_NOTIF $F_ICON1 $F_ICON2 $F_OUTSIDE $F_OTHER_BIN
    echo "payload" > "$f"
end

function run_staged_uninstall --argument-names manifest_file
    env THISPC_UNINSTALL_ROOT="$FAKE_ROOT" \
        THISPC_MANIFEST_PATH="$manifest_file" \
        THISPC_NO_SUDO=1 \
        bash "$REPO_ROOT/uninstall.sh" >/dev/null 2>&1
end

# 1. Missing manifest rejected
run_staged_uninstall "$STAGE_DIR/nonexistent_manifest.txt"
assert_true (test $status -ne 0; and echo true; or echo false) "SEC-045-07.1: Missing manifest is rejected"

# 2. Empty manifest rejected
set -l M_EMPTY "$STAGE_DIR/manifest_empty.txt"
touch "$M_EMPTY"
run_staged_uninstall "$M_EMPTY"
assert_true (test $status -ne 0; and echo true; or echo false) "SEC-045-07.2: Empty manifest is rejected"

# 3. Relative path rejected and 0 files deleted
set -l M_REL "$STAGE_DIR/manifest_rel.txt"
printf "%s\nusr/bin/thispc-view\n" "$F_BIN" > "$M_REL"
run_staged_uninstall "$M_REL"
assert_true (test $status -ne 0 -a -f "$F_BIN"; and echo true; or echo false) "SEC-045-07.3: Relative path rejected before any deletion"

# 4. Path traversal '..' rejected and 0 files deleted
set -l M_TRAV "$STAGE_DIR/manifest_trav.txt"
printf "%s\n%s/usr/bin/../../../outside/secret.txt\n" "$F_BIN" "$FAKE_ROOT" > "$M_TRAV"
run_staged_uninstall "$M_TRAV"
assert_true (test $status -ne 0 -a -f "$F_BIN" -a -f "$F_OUTSIDE"; and echo true; or echo false) "SEC-045-07.4: Path traversal (..) rejected before any deletion"

# 5. Path outside allowed prefix (/etc/passwd or outside dir) rejected
set -l M_OUT "$STAGE_DIR/manifest_out.txt"
printf "%s\n%s\n" "$F_BIN" "$F_OUTSIDE" > "$M_OUT"
run_staged_uninstall "$M_OUT"
assert_true (test $status -ne 0 -a -f "$F_BIN" -a -f "$F_OUTSIDE"; and echo true; or echo false) "SEC-045-07.5: Path outside /usr prefix rejected before any deletion"

# 6. Non-whitelisted binary under /usr/bin rejected
set -l M_NONWL "$STAGE_DIR/manifest_nonwl.txt"
printf "%s\n%s\n" "$F_BIN" "$F_OTHER_BIN" > "$M_NONWL"
run_staged_uninstall "$M_NONWL"
assert_true (test $status -ne 0 -a -f "$F_BIN" -a -f "$F_OTHER_BIN"; and echo true; or echo false) "SEC-045-07.6: Non-whitelisted file in /usr/bin rejected before any deletion"

# 7. Symlink target rejected
set -l F_SYMLINK "$FAKE_ROOT/usr/share/icons/hicolor/scalable/places/kio-thispc-drive-95.svg"
ln -s "$F_OUTSIDE" "$F_SYMLINK"
set -l M_SYM "$STAGE_DIR/manifest_sym.txt"
printf "%s\n%s\n" "$F_BIN" "$F_SYMLINK" > "$M_SYM"
run_staged_uninstall "$M_SYM"
assert_true (test $status -ne 0 -a -f "$F_BIN" -a -f "$F_OUTSIDE"; and echo true; or echo false) "SEC-045-07.7: Symlink entry in manifest rejected before any deletion"
rm -f "$F_SYMLINK"

# 8. Valid whitelisted manifest succeeds and removes only whitelisted files
set -l M_VALID "$STAGE_DIR/manifest_valid.txt"
printf "%s\n%s\n%s\n%s\n%s\n%s\n" \
    "$F_BIN" "$F_KIO" "$F_DESK" "$F_NOTIF" "$F_ICON1" "$F_ICON2" > "$M_VALID"
run_staged_uninstall "$M_VALID"
assert_true (test $status -eq 0; and echo true; or echo false) "SEC-045-07.8: Valid whitelisted manifest accepted"
assert_true (test ! -e "$F_BIN" -a ! -e "$F_KIO" -a ! -e "$F_DESK" -a ! -e "$F_NOTIF" -a ! -e "$F_ICON1" -a ! -e "$F_ICON2"; and echo true; or echo false) "SEC-045-07.9: All whitelisted staged files removed"
assert_true (test -f "$F_OUTSIDE" -a -f "$F_OTHER_BIN"; and echo true; or echo false) "SEC-045-07.10: Non-manifest files remain untouched"

echo "=== ALL $PASS_COUNT SAFE UNINSTALL & PRODUCTION GUARD CHECKS PASSED ==="
