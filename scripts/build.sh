#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

set -euo pipefail
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j"$(nproc)"
