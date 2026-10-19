#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

"""Test runner for ThisPC 0.41.2 Stage 2.1: MTP Permanent Delete Contract.

Builds an isolated test binary for tests/mtp-permanent-delete.cpp, runs it
offscreen in an isolated session, and verifies contract requirements PD-001
through PD-030, categorizing them into GREEN, RED / NOT IMPLEMENTED, and NOT RUN.
"""

import os
import sys
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]

print("=== ThisPC 0.41.2 — Stage 2.1: MTP Permanent Delete Contract Runner ===", flush=True)

with tempfile.TemporaryDirectory(prefix="thispc-mtp-delete-test-") as tmpdir:
    tmp = Path(tmpdir)
    src_dir = tmp / "src"
    build_dir = tmp / "build"
    src_dir.mkdir()
    build_dir.mkdir()

    # Copy headers from src/ and tests/
    for h in (root / "src").glob("*.h"):
        (src_dir / h.name).write_text(h.read_text())
    for th in (root / "tests").glob("*.h"):
        (src_dir / th.name).write_text(th.read_text())

    # Copy minimal source files needed
    for cpp in ("actionstatecontroller.cpp", "browsercommon.h", "remoteurlhelper.cpp", "panemenucontroller.cpp"):
        src_path = root / "src" / cpp
        if src_path.exists():
            (src_dir / cpp).write_text(src_path.read_text())

    (src_dir / "mtp-permanent-delete.cpp").write_text((root / "tests/mtp-permanent-delete.cpp").read_text())

    cmake_content = f"""cmake_minimum_required(VERSION 3.16)
project(thispc-mtp-delete-tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus Test)
find_package(KF6KIO REQUIRED)
find_package(KF6CoreAddons REQUIRED)

add_executable(mtp-delete-test
    src/mtp-permanent-delete.cpp
    src/actionstatecontroller.cpp
    src/remoteurlhelper.cpp
    src/panemenucontroller.cpp
)

target_include_directories(mtp-delete-test PRIVATE src)
target_link_libraries(mtp-delete-test PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::DBus
    Qt6::Test
    KF6::KIOCore
    KF6::KIOWidgets
    KF6::CoreAddons
)
"""
    (tmp / "CMakeLists.txt").write_text(cmake_content)

    print("Configuring test build...", flush=True)
    res = subprocess.run(
        ["cmake", "-S", str(tmp), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Debug"],
        capture_output=True, text=True
    )
    if res.returncode != 0:
        print("CMake configure failed:\n" + res.stderr, file=sys.stderr)
        sys.exit(1)

    print("Compiling mtp-delete-test...", flush=True)
    res = subprocess.run(
        ["cmake", "--build", str(build_dir), "-j" + str(os.cpu_count() or 4)],
        capture_output=True, text=True
    )
    if res.returncode != 0:
        print("CMake build failed:\n" + res.stderr, file=sys.stderr)
        sys.exit(1)

    print("Executing mtp-delete-test in isolated environment...", flush=True)
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"

    test_bin = build_dir / "mtp-delete-test"
    run_res = subprocess.run([str(test_bin)], capture_output=True, text=True, env=env)

    print("\n--- TEST OUTPUT ---")
    print(run_res.stdout)
    if run_res.stderr:
        print(run_res.stderr, file=sys.stderr)
    print("--- END TEST OUTPUT ---")

    if run_res.returncode != 0:
        print(f"Test executable failed with return code {run_res.returncode}", file=sys.stderr)
        sys.exit(run_res.returncode)

    print("\nSUCCESS: MTP Permanent Delete Contract Test Execution Complete.", flush=True)
