#!/usr/bin/env python3
"""Verify version flags complete before any GUI platform initialization."""

import os
from pathlib import Path
import re
import subprocess


root = Path(__file__).resolve().parents[1]
binary = root / "build/bin/thispc-view"
cmake = (root / "CMakeLists.txt").read_text()
match = re.search(r"^project\(kio-thispc VERSION ([^ ]+)", cmake, re.MULTILINE)
if not match:
    raise RuntimeError("PROJECT_VERSION not found")
expected = f"thispc-view {match.group(1)}\n"

environment = dict(os.environ)
for name in ("DISPLAY", "WAYLAND_DISPLAY", "QT_QPA_PLATFORM"):
    environment.pop(name, None)

checks = 0
for flag in ("--version", "-v"):
    result = subprocess.run(
        [binary, flag],
        env=environment,
        text=True,
        capture_output=True,
        timeout=5,
        check=False,
    )
    assert result.returncode == 0, (flag, result.returncode, result.stderr)
    checks += 1
    assert result.stdout == expected, (flag, result.stdout, expected)
    checks += 1
    assert result.stderr == "", (flag, result.stderr)
    checks += 1

print(f"PASS: {checks} CLI version assertions")
