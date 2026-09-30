#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

set -euo pipefail

cd "$(dirname "$0")"

pkill -f '/kf6/kio/thispc' 2>/dev/null || true
pkill -f 'kio_thispc' 2>/dev/null || true
pkill -x thispc-view 2>/dev/null || true

if [[ ! -f build/install_manifest.txt ]]; then
    echo "Brak build/install_manifest.txt."
    echo "Nie usuwam niczego automatycznie bez manifestu instalacji."
    exit 1
fi

echo "==> Usuwanie plików z manifestu CMake"
while IFS= read -r file; do
    [[ -n "$file" ]] && sudo rm -f -- "$file"
done < build/install_manifest.txt

if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

echo "kio-thispc 0.4 usunięty."
