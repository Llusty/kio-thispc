#!/usr/bin/env bash
set -euo pipefail
cmake_ver=$(sed -nE 's/^project\(kio-thispc VERSION ([^ ]+) .*/\1/p' CMakeLists.txt)
printf 'CMake version: %s\n' "$cmake_ver"
grep -nF "Version $cmake_ver" src/thispcview.cpp || true
grep -nF "v$cmake_ver" src/thispcview.cpp || true
grep -nF "kio-thispc $cmake_ver" install.sh || true
printf '\nReview README/CHANGELOG/UPDATE manually before release.\n'
