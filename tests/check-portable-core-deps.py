#!/usr/bin/env python3
"""
Automated dependency-boundary gate for ThisPC 0.46.0 Stage 1B / R1 (`thispc-core`).

Verifies Gate G2 (Portable core-only runtime requires zero KF6 and zero GIO):
  1. Reads CMake-generated target manifest (`--manifest`) populated from real
     CMake target properties (`SOURCES`, `LINK_LIBRARIES`,
     `INTERFACE_LINK_LIBRARIES`, `POSITION_INDEPENDENT_CODE`).
  2. Verifies `POSITION_INDEPENDENT_CODE` is enabled and `LINK_LIBRARIES` /
     `INTERFACE_LINK_LIBRARIES` contain ONLY allowlisted Qt6 Core/Concurrent
     targets.
  3. Scans every source in the target manifest (direct + local transitive) for
     both angle-bracket `<...>` and quoted `"..."` forbidden includes (KF6/KIO/
     Solid/KFileMetaData/KConfig/KCoreAddons/KNotifications/KDirWatch, GLib/GIO,
     QtGui/QtWidgets/QtPdf/QtWebEngine/QtPrintSupport/QtDBus, optional native libs).
  4. Inspects `CMakeCache.txt` (unless `--allow-kde-cache` is passed for KDE
     compatibility builds) to confirm `THISPC_BUILD_PORTABLE_CORE_ONLY=ON` and
     zero forbidden packages were configured.
  5. Verifies `libthispc-core.a` exists and is non-empty.
  6. Inspects ELF `DT_NEEDED`, `RPATH`/`RUNPATH` (`readelf -d`), and transitive
     `ldd` on `--smoke-binary`, forbidding direct `libglib-` and any `libgio-` /
     `libgobject-` / `libKF6` / `libKIO` / `libSolid` / `libQt6Gui` /
     `libQt6Widgets` / `libQt6Pdf` / `libQt6WebEngine` / etc., while permitting
     the legitimate indirect `libglib-2.0` dependency of system `libQt6Core`.

Supports `--self-test` to execute isolated negative tests proving the gate fails
on forbidden headers, link targets, ELF dependencies, RPATHs, and malformed manifests.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ALLOWED_LINK_TARGETS = frozenset({
    "Qt6::Core",
    "Qt6::Concurrent",
})

FORBIDDEN_INCLUDE_PREFIXES = (
    # KDE Frameworks / KIO / Solid
    "KIO/", "kio/", "Solid/", "solid/", "KFileMetaData/", "KJobWidgets/",
    "KWidgetsAddons/", "KCompletion/", "KItemViews/", "KConfig", "KCoreAddons",
    "KNotification", "KDesktopFile", "KService", "KJob", "KUrl", "KLocalizedString",
    "KDir", "KFile", "KProtocol", "KMountPoint", "KPropertiesDialog", "KOpenWithDialog",
    "KRecentDocument", "KRecentDirs", "KFormat", "KShell", "KUser", "KSharedConfig",
    "KPluginFactory", "KF6/",
    # GLib / GIO / GObject
    "glib", "gio/", "gobject", "gmodule",
    # Forbidden Qt modules
    "QPdf", "QtPdf", "QWebEngine", "QtWebEngine", "QDBus", "QtDBus", "QPrint", "QtPrint",
    "QtGui", "QtWidgets",
    # QtWidgets headers
    "QWidget", "QMainWindow", "QDialog", "QApplication", "QMessageBox", "QMenu", "QAction",
    "QToolBar", "QToolButton", "QPushButton", "QLabel", "QLineEdit", "QComboBox",
    "QCheckBox", "QRadioButton", "QSpinBox", "QDoubleSpinBox", "QSlider", "QProgressBar",
    "QProgressDialog", "QSplitter", "QStackedWidget", "QTabWidget", "QTabBar",
    "QTreeView", "QListView", "QTableView", "QTreeWidget", "QListWidget", "QTableWidget",
    "QHeaderView", "QAbstractItemView", "QStyledItemDelegate", "QStyle", "QProxyStyle",
    "QCommonStyle", "QFrame", "QGroupBox", "QScrollArea", "QScrollBar",
    "QVBoxLayout", "QHBoxLayout", "QGridLayout", "QFormLayout", "QLayout", "QBoxLayout",
    "QDialogButtonBox", "QFileDialog", "QInputDialog", "QColorDialog", "QFontDialog",
    "QPlainTextEdit", "QTextEdit", "QTextBrowser", "QStatusBar", "QMenuBar", "QToolTip",
    "QWhatsThis", "QSizePolicy", "QSpacerItem", "QButtonGroup", "QGraphics", "QCompleter",
    "QShortcut", "QSystemTrayIcon",
    # QtGui headers
    "QGuiApplication", "QIcon", "QPixmap", "QImage", "QPainter", "QPen", "QBrush",
    "QColor", "QPalette", "QFont", "QCursor", "QClipboard", "QDrag", "QDropEvent",
    "QDragEnterEvent", "QDragMoveEvent", "QDragLeaveEvent", "QKeyEvent", "QMouseEvent",
    "QWheelEvent", "QContextMenuEvent", "QCloseEvent", "QShowEvent", "QHideEvent",
    "QResizeEvent", "QPaintEvent", "QMoveEvent", "QFocusEvent", "QHoverEvent",
    "QDesktopServices", "QScreen", "QWindow", "QStyleHints", "QKeySequence",
    "QStandardItem", "QTextDocument", "QTextCursor", "QTextCharFormat", "QTextBlockFormat",
    "QMovie", "QFileIconProvider", "QFileSystemModel", "QRegion", "QPolygon",
    "QTransform", "QLinearGradient", "QRadialGradient", "QBackingStore",
    # Optional native media/archive/ACL libraries
    "taglib/", "exiv2/", "archive.h", "archive_entry.h", "zlib.h", "sys/acl.h", "acl/",
)

FORBIDDEN_TRANSITIVE_SHARED_LIBS = (
    re.compile(r"libKF6", re.I),
    re.compile(r"libKIO", re.I),
    re.compile(r"libSolid", re.I),
    re.compile(r"libgio-", re.I),
    re.compile(r"libgobject-", re.I),
    re.compile(r"libgmodule-", re.I),
    re.compile(r"libQt6Widgets", re.I),
    re.compile(r"libQt6Gui", re.I),
    re.compile(r"libQt6Pdf", re.I),
    re.compile(r"libQt6WebEngine", re.I),
    re.compile(r"libQt6PrintSupport", re.I),
    re.compile(r"libQt6DBus", re.I),
    re.compile(r"libarchive", re.I),
    re.compile(r"libtag\.", re.I),
    re.compile(r"libexiv2", re.I),
    re.compile(r"libacl", re.I),
)

# Direct DT_NEEDED also forbids direct libglib- linkage (indirect libglib-2.0 via
# system libQt6Core is permitted in ldd).
FORBIDDEN_DIRECT_SHARED_LIBS = FORBIDDEN_TRANSITIVE_SHARED_LIBS + (
    re.compile(r"libglib-", re.I),
)

FORBIDDEN_CACHE_PREFIXES = (
    "ECM_DIR",
    "KF6_DIR",
    "KF6KIO_DIR",
    "KF6Solid_DIR",
    "KF6ItemViews_DIR",
    "KF6FileMetaData_DIR",
    "KF6Config_DIR",
    "KF6CoreAddons_DIR",
    "KF6Notifications_DIR",
    "Qt6Gui_DIR",
    "Qt6Widgets_DIR",
    "Qt6Pdf_DIR",
    "Qt6WebEngine_DIR",
    "Qt6PrintSupport_DIR",
    "Qt6DBus_DIR",
    "LibArchive_INCLUDE_DIR",
    "LibArchive_LIBRARY",
    "TagLib_DIR",
    "exiv2_DIR",
    "LIBACL_FOUND",
    "GIO_FOUND",
    "GLIB_FOUND",
)


def parse_manifest(manifest_path: Path) -> tuple[dict[str, str], list[str]]:
    if not manifest_path.is_file():
        return {}, [f"Target manifest not found: {manifest_path}"]
    data: dict[str, str] = {}
    for line in manifest_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        if "=" not in line:
            return {}, [f"Malformed manifest line (missing '='): {line}"]
        key, value = line.split("=", 1)
        data[key.strip()] = value.strip()

    required = ("TARGET", "CORE_ONLY", "POSITION_INDEPENDENT_CODE", "SOURCES", "LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES")
    missing = [k for k in required if k not in data]
    if missing:
        return {}, [f"Malformed manifest: missing key(s): {', '.join(missing)}"]
    if data["TARGET"] != "thispc-core":
        return {}, [f"Unexpected manifest TARGET: {data['TARGET']!r}"]
    return data, []


def split_cmake_list(raw: str) -> list[str]:
    return [item.strip() for item in raw.split(";") if item.strip()]


def check_manifest_properties(manifest: dict[str, str]) -> list[str]:
    errors: list[str] = []
    pic = manifest["POSITION_INDEPENDENT_CODE"].upper()
    if pic not in ("1", "ON", "TRUE"):
        errors.append(f"thispc-core POSITION_INDEPENDENT_CODE must be enabled (got: {manifest['POSITION_INDEPENDENT_CODE']!r})")

    for field in ("LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES"):
        libs = split_cmake_list(manifest[field])
        if not libs:
            errors.append(f"thispc-core {field} is unexpectedly empty")
        for lib in libs:
            if lib not in ALLOWED_LINK_TARGETS:
                errors.append(f"Forbidden link target in thispc-core {field}: {lib}")

    sources = split_cmake_list(manifest["SOURCES"])
    if not sources:
        errors.append("thispc-core SOURCES list is empty in manifest")
    return errors


def is_forbidden_include(inc: str) -> bool:
    normalized = inc.lstrip("./")
    return normalized.startswith(FORBIDDEN_INCLUDE_PREFIXES)


def check_includes(repo_root: Path, source_entries: list[str]) -> list[str]:
    src_dir = repo_root / "src"
    inc_re = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)

    errors: list[str] = []
    visited: set[Path] = set()
    stack: list[Path] = []
    for entry in source_entries:
        if entry.endswith(".rule"):
            # CMake AUTOMOC synthetic timestamp.rule entry, not a C++ source file
            continue
        p = Path(entry)
        if not p.is_absolute():
            p = repo_root / p
        p = p.resolve()
        if not p.is_file():
            errors.append(f"Missing core source file from target manifest: {entry}")
            continue
        stack.append(p)

    while stack:
        cur = stack.pop()
        if cur in visited:
            continue
        visited.add(cur)
        content = cur.read_text(encoding="utf-8", errors="replace")
        try:
            display_path = cur.relative_to(repo_root)
        except ValueError:
            display_path = cur
        for delim, inc in inc_re.findall(content):
            if is_forbidden_include(inc):
                errors.append(f"Forbidden include {delim}{inc}{'>' if delim == '<' else '\"'} in {display_path}")
                continue
            if delim == '"':
                candidate = (cur.parent / inc).resolve()
                if not candidate.is_file():
                    candidate = (src_dir / inc).resolve()
                if candidate.is_file() and candidate not in visited:
                    stack.append(candidate)

    print(f"[GATE] Include scan checked {len(visited)} files (direct + transitive): {len(errors)} violation(s)")
    return errors


def check_cmake_cache(build_dir: Path) -> list[str]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return [f"CMakeCache.txt not found in {build_dir}"]
    errors: list[str] = []
    core_only_found = False
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("THISPC_BUILD_PORTABLE_CORE_ONLY:BOOL=ON"):
            core_only_found = True
        for prefix in FORBIDDEN_CACHE_PREFIXES:
            if line.startswith(prefix + ":") or line.startswith(prefix + "="):
                val = line.split("=", 1)[1].strip() if "=" in line else ""
                if val and "NOTFOUND" not in val and val.upper() not in ("0", "OFF", "FALSE"):
                    errors.append(f"Forbidden CMake dependency configured in core-only build: {line}")
    if not core_only_found:
        errors.append("THISPC_BUILD_PORTABLE_CORE_ONLY:BOOL=ON not present in CMakeCache.txt")
    print(f"[GATE] CMakeCache.txt check: {len(errors)} violation(s)")
    return errors


def inspect_readelf_output(readelf_stdout: str, binary_label: str) -> list[str]:
    errors: list[str] = []
    needed = re.findall(r"\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]", readelf_stdout)
    print(f"[GATE] readelf -d DT_NEEDED ({len(needed)}): {', '.join(needed)}")
    for lib in needed:
        for pat in FORBIDDEN_DIRECT_SHARED_LIBS:
            if pat.search(lib):
                errors.append(f"Forbidden direct DT_NEEDED shared library in {binary_label}: {lib}")

    rpaths = re.findall(r"\((?:RPATH|RUNPATH)\)\s+Library (?:rpath|runpath):\s+\[([^\]]+)\]", readelf_stdout)
    for rp in rpaths:
        errors.append(f"Unexpected RPATH/RUNPATH in {binary_label}: [{rp}]")
    return errors


def inspect_ldd_output(ldd_stdout: str, binary_label: str) -> list[str]:
    errors: list[str] = []
    for line in ldd_stdout.splitlines():
        lib_token = line.strip().split(" ", 1)[0]
        for pat in FORBIDDEN_TRANSITIVE_SHARED_LIBS:
            if pat.search(lib_token):
                errors.append(f"Forbidden transitive ldd dependency in {binary_label}: {line.strip()}")
    print(f"[GATE] ldd transitive scan: {len(errors)} violation(s)")
    return errors


def check_binary_deps(smoke_binary: Path) -> list[str]:
    if not smoke_binary.is_file():
        return [f"Smoke binary not found: {smoke_binary}"]
    errors: list[str] = []

    if shutil.which("readelf"):
        proc = subprocess.run(
            ["readelf", "-d", str(smoke_binary)],
            capture_output=True, text=True, check=True,
        )
        errors.extend(inspect_readelf_output(proc.stdout, smoke_binary.name))
    else:
        errors.append("readelf not available in PATH")

    if shutil.which("ldd"):
        proc = subprocess.run(
            ["ldd", str(smoke_binary)],
            capture_output=True, text=True, check=True,
        )
        errors.extend(inspect_ldd_output(proc.stdout, smoke_binary.name))
    else:
        errors.append("ldd not available in PATH")

    return errors


def run_negative_self_tests() -> int:
    failures: list[str] = []

    def expect_fail(label: str, errs: list[str]) -> None:
        if not errs:
            failures.append(f"Expected violation for [{label}] but got 0 errors")
        else:
            print(f"[SELF-TEST PASS] {label} -> rejected ({errs[0]})")

    def expect_pass(label: str, errs: list[str]) -> None:
        if errs:
            failures.append(f"Expected 0 errors for [{label}] but got: {errs}")
        else:
            print(f"[SELF-TEST PASS] {label} -> accepted")

    with tempfile.TemporaryDirectory(prefix="thispc-gate-selftest-") as td:
        root = Path(td)
        (root / "src").mkdir()

        # 1. Forbidden angle-bracket and quoted includes
        f1 = root / "src/bad_qtgui.h"
        f1.write_text("#include <QStandardItemModel>\n", encoding="utf-8")
        expect_fail("forbidden <QStandardItemModel>", check_includes(root, ["src/bad_qtgui.h"]))

        f2 = root / "src/bad_kf6_quoted.h"
        f2.write_text('#include "KIO/Job"\n', encoding="utf-8")
        expect_fail('forbidden quoted "KIO/Job"', check_includes(root, ["src/bad_kf6_quoted.h"]))

        f3 = root / "src/bad_gio.h"
        f3.write_text("#include <gio/gio.h>\n", encoding="utf-8")
        expect_fail("forbidden <gio/gio.h>", check_includes(root, ["src/bad_gio.h"]))

        f4 = root / "src/bad_kdirwatch.h"
        f4.write_text("#include <KDirWatch>\n", encoding="utf-8")
        expect_fail("forbidden <KDirWatch>", check_includes(root, ["src/bad_kdirwatch.h"]))

        # 2. Malformed & forbidden manifest link targets / missing PIC
        bad_manifest = root / "bad-manifest.txt"
        bad_manifest.write_text("TARGET=thispc-core\nSOURCES=src/a.cpp\n", encoding="utf-8")
        _, m_errs = parse_manifest(bad_manifest)
        expect_fail("malformed manifest missing keys", m_errs)

        expect_fail(
            "forbidden KF6::KIOCore link target",
            check_manifest_properties({
                "TARGET": "thispc-core",
                "CORE_ONLY": "1",
                "POSITION_INDEPENDENT_CODE": "1",
                "SOURCES": "src/a.cpp",
                "LINK_LIBRARIES": "Qt6::Core;KF6::KIOCore",
                "INTERFACE_LINK_LIBRARIES": "Qt6::Core;Qt6::Concurrent",
            }),
        )
        expect_fail(
            "missing POSITION_INDEPENDENT_CODE",
            check_manifest_properties({
                "TARGET": "thispc-core",
                "CORE_ONLY": "1",
                "POSITION_INDEPENDENT_CODE": "",
                "SOURCES": "src/a.cpp",
                "LINK_LIBRARIES": "Qt6::Core;Qt6::Concurrent",
                "INTERFACE_LINK_LIBRARIES": "Qt6::Core;Qt6::Concurrent",
            }),
        )

        # 3. Direct DT_NEEDED & RPATH checks
        expect_fail(
            "forbidden direct DT_NEEDED libKF6KIOCore",
            inspect_readelf_output(" 0x01 (NEEDED) Shared library: [libKF6KIOCore.so.6]\n", "fake-bin"),
        )
        expect_fail(
            "forbidden direct DT_NEEDED libglib-2.0",
            inspect_readelf_output(" 0x01 (NEEDED) Shared library: [libglib-2.0.so.0]\n", "fake-bin"),
        )
        expect_fail(
            "unexpected RUNPATH",
            inspect_readelf_output(" 0x1d (RUNPATH) Library runpath: [/tmp/untrusted]\n", "fake-bin"),
        )

        # 4. Transitive ldd: reject libgio/libgobject, allow indirect libglib-2.0 from Qt6Core
        expect_fail(
            "forbidden transitive ldd libgio-2.0",
            inspect_ldd_output("\tlibgio-2.0.so.0 => /usr/lib/libgio-2.0.so.0 (0x0)\n", "fake-bin"),
        )
        expect_pass(
            "legitimate indirect ldd libglib-2.0 via Qt6Core",
            inspect_ldd_output(
                "\tlibQt6Core.so.6 => /usr/lib/libQt6Core.so.6 (0x0)\n"
                "\tlibglib-2.0.so.0 => /usr/lib/libglib-2.0.so.0 (0x0)\n",
                "fake-bin",
            ),
        )

    if failures:
        for f in failures:
            print(f"[SELF-TEST FAIL] {f}", file=sys.stderr)
        return 1
    print("[SELF-TEST] ALL 12 NEGATIVE/POSITIVE GATE CHECKS PASSED.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify ThisPC portable-core dependency boundaries")
    parser.add_argument("--self-test", action="store_true", help="Run isolated negative/positive gate self-tests")
    parser.add_argument("--repo-root", type=Path, default=None)
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--build-dir", type=Path, default=None)
    parser.add_argument("--allow-kde-cache", action="store_true",
                        help="Skip CMakeCache.txt core-only check (for KDE-compat builds verifying thispc-core)")
    parser.add_argument("--smoke-binary", type=Path, default=None)
    parser.add_argument("--core-archive", type=Path, default=None)
    args = parser.parse_args()

    if args.self_test:
        return run_negative_self_tests()

    if args.repo_root is None or args.manifest is None:
        print("[GATE] ERROR: --repo-root and --manifest are required unless --self-test is used.", file=sys.stderr)
        return 2

    repo_root = args.repo_root.resolve()
    manifest, errors = parse_manifest(args.manifest.resolve())
    if errors:
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    core_sources = split_cmake_list(manifest["SOURCES"])
    print(f"[GATE] Manifest target={manifest['TARGET']} core_only={manifest['CORE_ONLY']} sources={len(core_sources)} pic={manifest['POSITION_INDEPENDENT_CODE']}")

    errors.extend(check_manifest_properties(manifest))
    errors.extend(check_includes(repo_root, core_sources))

    if args.build_dir is not None and not args.allow_kde_cache:
        errors.extend(check_cmake_cache(args.build_dir.resolve()))
    elif args.allow_kde_cache:
        print("[GATE] CMakeCache.txt check: skipped (--allow-kde-cache for KDE-compat build; target/include/ELF checks enforced)")

    if args.core_archive is not None:
        archive = args.core_archive.resolve()
        if not archive.is_file() or archive.stat().st_size == 0:
            errors.append(f"Static archive missing or empty: {archive}")
        else:
            print(f"[GATE] Static archive verified: {archive.name} ({archive.stat().st_size} bytes)")

    if args.smoke_binary is not None:
        errors.extend(check_binary_deps(args.smoke_binary.resolve()))

    if errors:
        print("\n[GATE] FAILED with violations:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print("[GATE] PASS: portable core boundary verified (0 KF6/ECM/GIO/QtGui/QtWidgets/QtPdf/QtWebEngine dependencies).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
