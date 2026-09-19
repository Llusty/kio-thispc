#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

echo "==> kio-thispc 0.27.0"
echo "==> Czysty build"
rm -rf build

echo "==> Konfiguracja"
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr

echo "==> Kompilacja"
cmake --build build -j"$(nproc)"

echo "==> Instalacja backendu KIO, aplikacji i ikon"
sudo cmake --install build

echo "==> Odświeżanie cache KDE"
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

pkill -f '/kf6/kio/thispc' 2>/dev/null || true
pkill -f 'kio_thispc' 2>/dev/null || true

echo
echo "Gotowe."
echo
echo "Widok klasyczny w Dolphinie:"
echo "  dolphin 'thispc:/'"
echo
echo "Aplikacja Ten komputer:"
echo "  thispc-view"
echo
echo "Aplikacja „Ten komputer” powinna też pojawić się w menu Plasma."
