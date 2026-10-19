#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

"""Test runner for ThisPC 0.41.2 Stage 1.2: Directory Refresh Contract.

Builds an isolated test binary for tests/directory-refresh.cpp, runs it
offscreen in an isolated session, and verifies contract requirements DR-001
through DR-069, categorizing them into GREEN, RED / NOT IMPLEMENTED, and NOT RUN.
"""

import os
import sys
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]

print("=== ThisPC 0.41.2 — Stage 1.2: Directory Refresh Contract Runner ===", flush=True)

with tempfile.TemporaryDirectory(prefix="thispc-dir-refresh-test-") as tmpdir:
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

    # Copy minimal source files needed for DirectoryListingCore & helpers
    for cpp in ("directorylistingcore.cpp", "directorywatchcontroller.cpp", "browsercommon.h", "remoteurlhelper.cpp", "locationpresentation.cpp"):
        src_path = root / "src" / cpp
        if src_path.exists():
            (src_dir / cpp).write_text(src_path.read_text())

    (src_dir / "directory-refresh.cpp").write_text((root / "tests/directory-refresh.cpp").read_text())

    cmake_content = f"""cmake_minimum_required(VERSION 3.16)
project(thispc-directory-refresh-tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus Test)
find_package(KF6KIO REQUIRED)
find_package(KF6JobWidgets REQUIRED)
find_package(KF6ItemViews REQUIRED)
find_package(KF6Solid REQUIRED)
find_package(KF6FileMetaData REQUIRED)
find_package(KF6Config REQUIRED)
find_package(KF6CoreAddons REQUIRED)
find_package(LibArchive REQUIRED)
find_package(ZLIB REQUIRED)
find_package(TagLib REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBACL REQUIRED IMPORTED_TARGET libacl)

add_executable(directory-refresh-test
    src/directory-refresh.cpp
    src/directorylistingcore.cpp
    src/directorywatchcontroller.cpp
    src/remoteurlhelper.cpp
    src/locationpresentation.cpp
    src/directoryview.h
)

target_include_directories(directory-refresh-test PRIVATE
    ${{CMAKE_CURRENT_SOURCE_DIR}}/src
    ${{LibArchive_INCLUDE_DIRS}}
)

target_link_libraries(directory-refresh-test PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus Qt6::Test
    KF6::KIOCore KF6::KIOWidgets KF6::JobWidgets KF6::ItemViews KF6::Solid KF6::FileMetaData KF6::ConfigCore KF6::CoreAddons
    ${{LibArchive_LIBRARIES}} ${{ZLIB_LIBRARIES}} ${{TagLib_LIBRARIES}} PkgConfig::LIBACL
)
"""
    (tmp / "CMakeLists.txt").write_text(cmake_content)

    print("Configuring test build...", flush=True)
    subprocess.run(["cmake", "-S", str(tmp), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Debug"], check=True)

    print("Compiling directory-refresh-test...", flush=True)
    subprocess.run(["cmake", "--build", str(build_dir), "-j2"], check=True)

    # Isolated D-Bus session config
    bus_config = tmp / "test-bus.conf"
    bus_config.write_text("""<busconfig>
<type>session</type>
<listen>unix:tmpdir=/tmp</listen>
<auth>EXTERNAL</auth>
<policy context="default">
<allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/>
</policy>
</busconfig>
""")

    runtime_dir = tmp / "runtime"
    runtime_dir.mkdir(parents=True, mode=0o700)
    data_dir = tmp / "data"
    data_dir.mkdir(parents=True)

    env = dict(
        os.environ,
        QT_QPA_PLATFORM="offscreen",
        QT_FORCE_STDERR_LOGGING="1",
        XDG_RUNTIME_DIR=str(runtime_dir),
        XDG_DATA_HOME=str(data_dir),
        DISPLAY="",
        WAYLAND_DISPLAY="",
        LANG="C.UTF-8",
        LC_ALL="C.UTF-8"
    )

    cmd = ["dbus-run-session", f"--config-file={bus_config}", "--", str(build_dir / "directory-refresh-test")]
    print("Executing directory-refresh-test in isolated environment...", flush=True)
    result = subprocess.run(cmd, env=env, capture_output=True, text=True)

    print("\n--- TEST OUTPUT ---")
    print(result.stdout)
    if result.stderr:
        print(result.stderr)
    print("--- END TEST OUTPUT ---\n")

    if result.returncode != 0:
        print(f"FAILED: directory-refresh-test exited with code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)

    print("SUCCESS: Directory Refresh Contract Test Execution Complete.")
