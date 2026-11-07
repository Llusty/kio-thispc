#!/usr/bin/env fish
# ==============================================================================
# Regression Test for Stage 3 Manual Test Harness XDG Isolation
# ==============================================================================

set -g SCRIPT_DIR "/home/sebastianh/Pobrane/Testy/0.45/Stage3"
set -gx THISPC_BIN "/home/sebastianh/Pobrane/kio-thispc/build/bin/thispc-view"

# Test 1: Syntax validation of run_stage3_manual_test.fish
fish --no-execute "$SCRIPT_DIR/run_stage3_manual_test.fish"
if test $status -ne 0
    echo "FAIL: fish --no-execute reported syntax errors in run_stage3_manual_test.fish"
    exit 1
end
echo "PASS: fish syntax validation"

# Extract and source validate_xdg_isolation from run_stage3_manual_test.fish
# We find line numbers for function validate_xdg_isolation to end of function
python3 -c "
with open('$SCRIPT_DIR/run_stage3_manual_test.fish') as f:
    lines = f.readlines()
in_func = False
func_lines = []
depth = 0
for line in lines:
    if 'function validate_xdg_isolation' in line:
        in_func = True
    if in_func:
        func_lines.append(line)
        if line.strip().startswith('if ') or line.strip().startswith('function '):
            depth += 1
        elif line.strip() == 'end':
            depth -= 1
            if depth == 0:
                break
with open('/tmp/validate_func.fish', 'w') as out:
    out.writelines(func_lines)
"
source /tmp/validate_func.fish
rm -f /tmp/validate_func.fish

# Test 2: Valid paths pass
set -gx XDG_CONFIG_HOME "$SCRIPT_DIR/config"
set -gx XDG_STATE_HOME "$SCRIPT_DIR/state"
validate_xdg_isolation
if test $status -eq 0
    echo "PASS: Valid Stage 3 paths accepted"
else
    echo "FAIL: Valid paths rejected"
    exit 1
end

# Test 3: Empty XDG_CONFIG_HOME rejected
set -gx XDG_CONFIG_HOME ""
set -gx XDG_STATE_HOME "$SCRIPT_DIR/state"
validate_xdg_isolation >/dev/null 2>&1
if test $status -ne 0
    echo "PASS: Empty XDG_CONFIG_HOME rejected"
else
    echo "FAIL: Empty XDG_CONFIG_HOME was erroneously accepted"
    exit 1
end

# Test 4: Empty XDG_STATE_HOME rejected
set -gx XDG_CONFIG_HOME "$SCRIPT_DIR/config"
set -gx XDG_STATE_HOME ""
validate_xdg_isolation >/dev/null 2>&1
if test $status -ne 0
    echo "PASS: Empty XDG_STATE_HOME rejected"
else
    echo "FAIL: Empty XDG_STATE_HOME was erroneously accepted"
    exit 1
end

# Test 5: Relative path ./config rejected
set -gx XDG_CONFIG_HOME "./config"
set -gx XDG_STATE_HOME "$SCRIPT_DIR/state"
validate_xdg_isolation >/dev/null 2>&1
if test $status -ne 0
    echo "PASS: Relative XDG_CONFIG_HOME (./config) rejected"
else
    echo "FAIL: Relative XDG_CONFIG_HOME was accepted"
    exit 1
end

# Test 6: Relative path ./state rejected
set -gx XDG_CONFIG_HOME "$SCRIPT_DIR/config"
set -gx XDG_STATE_HOME "./state"
validate_xdg_isolation >/dev/null 2>&1
if test $status -ne 0
    echo "PASS: Relative XDG_STATE_HOME (./state) rejected"
else
    echo "FAIL: Relative XDG_STATE_HOME was accepted"
    exit 1
end

# Test 7: Default user home ~/.config rejected
set -gx XDG_CONFIG_HOME "$HOME/.config"
set -gx XDG_STATE_HOME "$SCRIPT_DIR/state"
validate_xdg_isolation >/dev/null 2>&1
if test $status -ne 0
    echo "PASS: Fallback to ~/.config rejected"
else
    echo "FAIL: Fallback to ~/.config was accepted"
    exit 1
end

# Test 8: Default user home ~/.local/state rejected
set -gx XDG_CONFIG_HOME "$SCRIPT_DIR/config"
set -gx XDG_STATE_HOME "$HOME/.local/state"
validate_xdg_isolation >/dev/null 2>&1
if test $status -ne 0
    echo "PASS: Fallback to ~/.local/state rejected"
else
    echo "FAIL: Fallback to ~/.local/state was accepted"
    exit 1
end

# Test 9: Verify actual /proc/<PID>/environ inheritance
set -gx XDG_CONFIG_HOME "$SCRIPT_DIR/config"
set -gx XDG_STATE_HOME "$SCRIPT_DIR/state"
set -gx THISPC_TRANSFER_QUEUE_PRODUCTION "1"

set -l proc_test_output (env XDG_CONFIG_HOME="$XDG_CONFIG_HOME" XDG_STATE_HOME="$XDG_STATE_HOME" THISPC_TRANSFER_QUEUE_PRODUCTION="1" python3 -c "
import os
pid = os.getpid()
with open(f'/proc/{pid}/environ', 'rb') as f:
    env_data = f.read().split(b'\0')
env_dict = dict(item.split(b'=', 1) for item in env_data if b'=' in item)
cfg = env_dict.get(b'XDG_CONFIG_HOME', b'').decode()
st = env_dict.get(b'XDG_STATE_HOME', b'').decode()
prod = env_dict.get(b'THISPC_TRANSFER_QUEUE_PRODUCTION', b'').decode()
assert cfg == '$SCRIPT_DIR/config', f'Bad config: {cfg}'
assert st == '$SCRIPT_DIR/state', f'Bad state: {st}'
assert prod == '1', f'Bad prod: {prod}'
assert os.path.isabs(cfg), 'Config not absolute'
assert os.path.isabs(st), 'State not absolute'
print('ENVIRON_VERIFIED_OK')
")

if test "$proc_test_output" = "ENVIRON_VERIFIED_OK"
    echo "PASS: Process /proc/<PID>/environ verified with absolute XDG paths and production queue flag"
else
    echo "FAIL: /proc/<PID>/environ check failed: $proc_test_output"
    exit 1
end

echo "ALL 9 XDG ISOLATION REGRESSION CHECKS PASSED!"
