#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

set -euo pipefail

cd "$(dirname "$0")"

MANIFEST_PATH="${THISPC_MANIFEST_PATH:-build/install_manifest.txt}"
UNINSTALL_ROOT="${THISPC_UNINSTALL_ROOT:-}"
NO_SUDO="${THISPC_NO_SUDO:-0}"

if [[ -n "$UNINSTALL_ROOT" ]]; then
    UNINSTALL_ROOT="$(realpath -m -- "$UNINSTALL_ROOT")"
fi

if [[ ! -f "$MANIFEST_PATH" || -L "$MANIFEST_PATH" ]]; then
    echo "Brak prawidłowego pliku manifestu: $MANIFEST_PATH" >&2
    echo "Nie usuwam niczego automatycznie bez zweryfikowanego manifestu instalacji." >&2
    exit 1
fi

validate_manifest_entry() {
    local entry="$1"
    if [[ -z "$entry" ]]; then
        echo "Błąd bezpieczeństwa manifestu: pusty wiersz." >&2
        return 1
    fi
    if [[ "$entry" != /* ]]; then
        echo "Błąd bezpieczeństwa manifestu: ścieżka względna jest niedozwolona: $entry" >&2
        return 1
    fi
    if [[ "$entry" =~ (^|/)\.\.(/|$) ]] || [[ "$entry" == *'//'* ]] || [[ "$entry" =~ [[:cntrl:]] ]]; then
        echo "Błąd bezpieczeństwa manifestu: niedozwolone sekwencje ścieżki: $entry" >&2
        return 1
    fi

    local rel="$entry"
    if [[ -n "$UNINSTALL_ROOT" ]]; then
        if [[ "$entry" != "$UNINSTALL_ROOT"/* ]]; then
            echo "Błąd bezpieczeństwa manifestu: ścieżka spoza katalogu głównego ($UNINSTALL_ROOT): $entry" >&2
            return 1
        fi
        rel="${entry#"$UNINSTALL_ROOT"}"
    fi

    case "$rel" in
        /usr/bin/thispc-view|/usr/local/bin/thispc-view|\
        /usr/lib/qt6/plugins/kf6/kio/thispc.so|/usr/lib64/qt6/plugins/kf6/kio/thispc.so|\
        /usr/local/lib/qt6/plugins/kf6/kio/thispc.so|/usr/local/lib64/qt6/plugins/kf6/kio/thispc.so|\
        /usr/lib/plugins/kf6/kio/thispc.so|/usr/lib64/plugins/kf6/kio/thispc.so|\
        /usr/share/applications/org.kde.thispcview.desktop|/usr/local/share/applications/org.kde.thispcview.desktop|\
        /usr/share/knotifications6/org.kde.thispcview.notifyrc|/usr/local/share/knotifications6/org.kde.thispcview.notifyrc)
            ;;
        /usr/share/icons/hicolor/scalable/places/kio-thispc-*.svg|\
        /usr/local/share/icons/hicolor/scalable/places/kio-thispc-*.svg)
            local base
            base="$(basename -- "$rel")"
            if [[ ! "$base" =~ ^kio-thispc-(drive|removable)-[0-9]+\.svg$ ]]; then
                echo "Błąd bezpieczeństwa manifestu: nieznana nazwa ikony: $entry" >&2
                return 1
            fi
            ;;
        *)
            echo "Błąd bezpieczeństwa manifestu: ścieżka spoza dozwolonej białej listy kio-thispc: $entry" >&2
            return 1
            ;;
    esac

    local parent
    parent="$(dirname -- "$entry")"
    if [[ -L "$parent" ]]; then
        echo "Błąd bezpieczeństwa manifestu: katalog nadrzędny jest dowiązaniem symbolicznym: $parent" >&2
        return 1
    fi
    if [[ -e "$parent" ]]; then
        local real_parent
        real_parent="$(realpath -e -- "$parent")"
        local expected_prefix="${UNINSTALL_ROOT}/usr/"
        if [[ "$real_parent"/ != "$expected_prefix"* ]]; then
            echo "Błąd bezpieczeństwa manifestu: katalog nadrzędny wskazuje poza $expected_prefix: $real_parent" >&2
            return 1
        fi
    fi
    if [[ -L "$entry" ]]; then
        echo "Błąd bezpieczeństwa manifestu: plik docelowy jest dowiązaniem symbolicznym: $entry" >&2
        return 1
    fi
    if [[ -d "$entry" ]]; then
        echo "Błąd bezpieczeństwa manifestu: wpis wskazuje na katalog zamiast pliku: $entry" >&2
        return 1
    fi
    return 0
}

mapfile -t MANIFEST_FILES < "$MANIFEST_PATH"
if [[ "${#MANIFEST_FILES[@]}" -eq 0 ]]; then
    echo "Błąd bezpieczeństwa manifestu: manifest jest pusty." >&2
    exit 1
fi

for file in "${MANIFEST_FILES[@]}"; do
    if ! validate_manifest_entry "$file"; then
        echo "Przerwano deinstalację — żaden plik nie został usunięty." >&2
        exit 1
    fi
done

if [[ "$NO_SUDO" != "1" ]]; then
    pkill -f '(kioworker|kioslave5?).*thispc\.so' 2>/dev/null || true
    pkill -x thispc-view 2>/dev/null || true
fi

echo "==> Usuwanie zweryfikowanych plików z manifestu CMake"
for file in "${MANIFEST_FILES[@]}"; do
    if [[ "$NO_SUDO" == "1" ]]; then
        rm -f -- "$file"
    else
        sudo rm -f -- "$file"
    fi
done

if [[ "$NO_SUDO" != "1" ]] && command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

echo "kio-thispc usunięty."
