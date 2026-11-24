# ThisPC 0.46.0 — Stage 1B (R1/R2): Portable Qt6/Linux Core Foundation & Build-System Separation

**Status:** `STAGE 1B — TECHNICALLY ACCEPTED / FINAL CHECKPOINT PREPARED` (NOT yet `STAGE 1B — FULLY CLOSED / COMMITTED / PUSHED`; NOT a public release, NOT installed to `/usr`).
**Accepted starting baseline:** Stage 1A checkpoint commit `45a91286a730b1e0a5cf6acc5942fddd7bab9cbf`.
**Installed production release (unchanged):** ThisPC `0.45.2` (`/usr/bin/thispc-view`).

---

## 1. Objective & Architectural Boundary

Stage 1B introduces the first real, production-relevant build-system and target separation between:
1. **`thispc-core` (`STATIC` library, `POSITION_INDEPENDENT_CODE ON`):** portable Qt6/Linux core using only narrow Qt6 modules (`Qt6::Core`, `Qt6::Concurrent`) and C++20/POSIX APIs — strictly free of ECM, KDE Frameworks (KF6), KIO, Solid, GIO/GLib (direct), `Qt6::Gui`, `Qt6::Widgets`, `Qt6::Pdf`, `Qt6::WebEngine`, `Qt6::PrintSupport`, `Qt6::DBus`, and optional native media/ACL libraries (`LibArchive`, `TagLib`, `Exiv2`, `libacl`).
2. **KDE compatibility targets (`THISPC_BUILD_PORTABLE_CORE_ONLY=OFF`, default):** `thispc` (KIO worker module), `thispc-view`, `thispc-view-stage3c1-test`, `thispc-merge-preview`, and `thispc-view-merge-dev`, all linking `PRIVATE thispc-core` so `thispc-core` is genuinely shared between the portable core-only build and the full KDE application build.

**Intentional temporary placement (deferred cleanup `AUD-1B-09`):** `src/applicationstyle.cpp` (QSS stylesheet string builder, `#include <QString>` only) currently resides in `thispc-core` because it has zero QtGui/QtWidgets header or link dependencies; it is a tracked candidate to move into the future portable Qt Widgets UI layer in Stages 3–5.

---

## 2. Source Inventory & Real Compilation Coverage (31/31 Files in `thispc-core`)

Every single one of the **31** files in `THISPC_PORTABLE_CORE_SOURCES` now receives **real C++20 compilation** under `THISPC_BUILD_PORTABLE_CORE_ONLY=ON` (both CachyOS Qt 6.12.0 and Ubuntu 24.04.5 LTS Qt 6.4.2) as well as under the KDE compatibility build:
- **4 compiled `.cpp` translation units + their 4 `.h` headers (8 files):** `applicationstyle.{h,cpp}`, `portablepaths.{h,cpp}`, `remoteurlhelper.{h,cpp}`, `savedremotelocation.{h,cpp}`.
- **15 header-only modules compiled via AUTOMOC (`mocs_compilation.cpp`) and `tests/portable-core-smoke.cpp`:** `applicationlifecycle.h`, `checksumdata.h`, `checksumjob.h`, `duplicateactiondata.h`, `duplicatefinderdata.h`, `duplicatefinderjob.h`, `duplicategroupmodel.h`, `storagescandata.h`, `storagescanworker.h`, `storagescanjob.h`, `storageanalysisdata.h`, `storageanalysisaccumulator.h`, `storagetreemapdata.h`, `storagetreemaplayout.h`, `transferbandwidthlimiter.h`.
- **8 non-`Q_OBJECT` header-only modules compiled & contract-checked via `tests/portable-core-headers-compile.cpp` (`AUD-1B-02`):** `acldata.h`, `aggregatepermissionsmodel.h`, `aggregatepermissionswriter.h`, `aggregatepropertiesformat.h`, `aggregatepropertiesmodel.h`, `aggregatepropertiesscan.h`, `localfileoverwrite.h`, `localmergetransaction.h`.

Remaining `src/` classification (166 total files in `src/`):
- **12** pure Qt6 Core interface/controller headers whose `.cpp` files still use KF6/QtWidgets (Stages 2–4);
- **3** optional native-lib-only headers (`aclcontroller.h`, `aclprovider.h`, `archive-detection.h`);
- **8** QtGui/QtWidgets files without KF6 (portable Qt Widgets UI layer, Stages 3–5);
- **112** current KDE/KF6 integration files (direct or transitive via `browsercommon.h`, KIO, Solid, KFileMetaData, KConfig, KNotifications — Stages 2–6).

---

## 3. CMake Options, Compiler Discipline & Target Manifest

### 3.1 Core-Only Build (Zero ECM / Zero KF6 / Zero GIO)

```bash
cmake -S . -B /path/to/build-portable \
      -DCMAKE_BUILD_TYPE=Debug \
      -DTHISPC_BUILD_PORTABLE_CORE_ONLY=ON
cmake --build /path/to/build-portable -j"$(nproc)"
ctest --test-dir /path/to/build-portable --output-on-failure -V
```

- **CMake option:** `THISPC_BUILD_PORTABLE_CORE_ONLY` (default `OFF`), processed before `find_package(ECM ...)` and `find_package(KF6 ...)`.
- **Required Qt6 version in core-only mode:** `Qt6 >= 6.4.2` (`Core`, `Concurrent` only).
- **Target properties (`AUD-1B-06`):** `thispc-core` sets `POSITION_INDEPENDENT_CODE ON` (`-fPIC`) for future optional shared plugins, and in core-only mode applies target-scoped GCC/Clang warnings (`-Wall -Wextra -Wformat-security -Werror=return-type -Wnon-virtual-dtor -Woverloaded-virtual`) plus Qt strictness defines (`QT_NO_CAST_FROM_ASCII`, `QT_NO_CAST_FROM_BYTEARRAY`, `QT_NO_CAST_TO_ASCII`, `QT_NO_FOREACH`, `QT_NO_KEYWORDS`, `QT_NO_NARROWING_CONVERSIONS_IN_CONNECT`, `QT_NO_URL_CAST_FROM_STRING`).
- **Generated target manifest (`AUD-1B-07`):** CMake writes `${CMAKE_CURRENT_BINARY_DIR}/thispc-core-manifest.txt` using `file(GENERATE ...)` with real target properties (`SOURCES`, `LINK_LIBRARIES`, `INTERFACE_LINK_LIBRARIES`, `POSITION_INDEPENDENT_CODE`) consumed by `tests/check-portable-core-deps.py`.
- **CTest tests registered:**
  1. `thispc-core-smoke` — builds `tests/portable-core-smoke.cpp` + `tests/portable-core-headers-compile.cpp`, isolates `XDG_CONFIG_HOME` / `QSettings` into a temporary directory before constructing `QCoreApplication` (`AUD-1B-01`), tests exact `PortablePaths` contracts (`AUD-1B-04`), exercises an active barrier-controlled `ShutdownCoordinator` draining scenario (`AUD-1B-08`), plus `RemoteUrlHelper`, `SavedRemoteLocationsStore`, `applicationStyleSheet()`, `ChecksumJob`, `StorageScanJob`, `StorageAnalysisAccumulator`, `StorageTreemapLayout`, `DuplicateFinderJob`, `TransferBandwidthLimiter`, and the 8 header-only modules.
  2. `thispc-core-deps-gate` — runs `tests/check-portable-core-deps.py` in both `THISPC_BUILD_PORTABLE_CORE_ONLY=ON` (full check including `CMakeCache.txt`) and `OFF` (`--allow-kde-cache`, still enforcing target manifest, includes, static archive, `readelf -d` `DT_NEEDED`/`RPATH`/`RUNPATH`, and `ldd`). Isolated negative tests are runnable via `python3 tests/check-portable-core-deps.py --self-test`.

### 3.2 Full KDE Compatibility Build (Default)

```bash
cmake -S . -B /path/to/build-kde-compat \
      -DCMAKE_BUILD_TYPE=Debug \
      -DTHISPC_BUILD_PORTABLE_CORE_ONLY=OFF
cmake --build /path/to/build-kde-compat --target thispc thispc-view thispc-view-stage3c1-test thispc-core-smoke -j"$(nproc)"
ctest --test-dir /path/to/build-kde-compat --output-on-failure -V
```

---

## 4. Qt 6.4.2 Compatibility Baseline & XDG Path Semantics

- **Portable core minimum:** `QT_PORTABLE_CORE_MIN_VERSION = 6.4.2` (Ubuntu 24.04 LTS / Linux Mint 22 baseline).
- **Hardened `ThisPc::PortablePaths` (`src/portablepaths.h`, `src/portablepaths.cpp`, `AUD-1B-04`):**
  - Replaces direct Qt 6.7-only `QStandardPaths::StateLocation` / `GenericStateLocation` calls in `src/transferhistorymanager.h`.
  - Preserves exact production history path: `<XDG_STATE_HOME>/kio-thispc/thispc-view/operations_history.json` (or `$HOME/.local/state/kio-thispc/thispc-view/operations_history.json`).
  - Does **not** `.trimmed()` `XDG_STATE_HOME` (leading spaces fail the XDG leading-`/` check and fall back to `$HOME/.local/state`).
  - Rejects relative paths, `"/"`, `"/.local/state"`, `"/.qttest/state"`, and missing/invalid `HOME` (never falls back to root-level state directories).
  - Validates `fallbackSubdir` and metadata components against absolute paths, backslashes, NUL bytes, and `.`/`..` traversal segments; aligns empty-metadata fallback identically across Qt 6.4.2 and Qt 6.7+.
  - Preserves `QStandardPaths::isTestModeEnabled()` (`$HOME/.qttest/state`).
- **Fresh real Qt 6.4.2 verification (R1):** Ubuntu 24.04.5 LTS ephemeral Docker container (`ubuntu:24.04`, `--rm`, repo mounted `:ro`, `qt6-base-dev 6.4.2+dfsg-21.1build5`, `g++ 13.3.0`, `cmake 3.28.3`) compiles all 31/31 core files and runs `ctest` (`thispc-core-smoke` + `thispc-core-deps-gate`).
- **Remaining Qt > 6.4.2 APIs outside `thispc-core`:** `src/searchcontroller.h:539–540` (`QList::assign`, Qt 6.6+) remains in the KDE-compat target until Stage 4A.

---

## 5. Canonical Dependency Gate Status (Stage 0 / Stage 0A Definitions — `AUD-1B-03`)

The canonical architectural dependency gates approved in Stage 0 / Stage 0A are:

| Gate | Canonical Definition (Stage 0 / 0A) | Stage 1B-R1 Status | Completing Stages |
|---|---|---|---|
| **G1** | Main portable application executable has no mandatory KF6 or GIO runtime linkage. | **PENDING** (`thispc-view` still links directly to KF6/KIO/Solid/etc.) | Stages 2–5 |
| **G2** | Portable core-only runtime requires zero KF6 and zero GIO. | **PASS (after R1 verification)** — all 31/31 `thispc-core` files compiled and verified on CachyOS Qt 6.12 and Ubuntu 24.04 Qt 6.4.2 with 0 KF6/GIO/ECM/QtGui/QtWidgets dependencies. | Stage 1B / 1B-R1 |
| **G3** | GNOME/GIO application profile requires zero mandatory KF6. | **PENDING** (GIO backend profile not yet implemented) | Stages 2–6B |
| **G4** | KDE profile intentionally uses an optional KF6-based backend while the main executable remains free of mandatory KF6 linkage. | **PENDING** (optional KDE backend loading not yet implemented) | Stage 6A |

**Tracked separately from G1–G4:** Full `0.46.0` release acceptance (packaging/CI in Stage 7, cross-distro/cross-desktop VM acceptance on GNOME/Cinnamon/XFCE X11/Wayland in Stage 8, final security audit and owner release acceptance in Stage 9).

---

## 6. Verification Summary & Owner Acceptance

- **Independent Stage 1B-R1 delta audit:** `PASS — STAGE 1B READY FOR OWNER ACCEPTANCE` (**8/8** findings `AUD-1B-01`–`AUD-1B-08` resolved).
- **Real Ubuntu 24.04.5 LTS / Qt 6.4.2 core-only verification:** **PASS** (**31/31** compilation coverage, `ctest` 2/2 PASS, `readelf -d` 0 forbidden deps).
- **CachyOS core-only & ASan/UBSan/LSan builds:** **PASS** (2/2 PASS each, 0 leaks).
- **Dependency gate self-tests (`--self-test`):** **12/12 PASS** (on both CachyOS and Ubuntu 24.04 Qt 6.4.2).
- **KDE-compatible build & tests (`THISPC_BUILD_PORTABLE_CORE_ONLY=OFF`):** **PASS** (3/3 CTest PASS, 0 `DT_NEEDED` differences vs production `0.45.2`).
- **Targeted ASan/UBSan/LSan regression (`detect_leaks=1`):** **14/14 PASS** (0 leaks, `lifecycle_hygiene` 190/190 + 5 real `QApplication::exec()` subprocesses).
- **Full ASan/UBSan regression (`detect_leaks=0`):** **90/90 PASS**.
- **Owner manual GUI acceptance:** **4/4 PASS** (isolated GUI + Split View navigation, `stage1b-history-test.txt` transfer, XDG transfer history persistence across restart, and installed `0.45.2` launch/operation).

---

## 7. Known Limitations, Nonblocking Observations & Preserved Deferred Issues

- Full `thispc-view` GUI still requires ECM/KF6/KIO/Solid/KFileMetaData/KNotifications (`THISPC_BUILD_PORTABLE_CORE_ONLY=OFF`) until Stages 2–6.
- Deferred `AUD-1B-09`: `src/applicationstyle.cpp` temporarily remains in `thispc-core` and is scheduled to move to the portable UI layer in Stages 3–5.
- Nonblocking LOW observations retained from the independent Stage 1B-R1 delta audit:
  1. `src/portablepaths.cpp:166-171`: on Qt `>= 6.7.0`, if `XDG_STATE_HOME` is unset, `HOME` is empty or `"/"`, and metadata is non-empty, `QStandardPaths::StateLocation` returns `"/.local/state/<org>/<app>"`, bypassing the exact `isRootStatePath("/.local/state")` check (unreachable under normal Linux sessions with valid `HOME` and inapplicable to Qt 6.4.2).
  2. `tests/portable-core-smoke.cpp:79-89`: `tempBaseDir()` accepts a relative `THISPC_TEST_WORK_DIR` value (though it still creates an auto-removed `QTemporaryDir` isolated from `$HOME/.config`).
- Preserved pre-existing deferred items (unchanged by Stage 1B / R1):
  - `transfer_queue` and `operations` pre-existing LSan ownership findings (→ Stage 2);
  - intermittent pre-existing `trash` test flake (`empty-trash.cpp`, → future investigation);
  - historical `directory-refresh` pending contracts (`DR-058/059/062`);
  - `PreviewPane` / PDF worker-pool migration (→ Stage 5A).

## 8. Post-R1 & R2 Test-Infrastructure Security Remediation (`tests/transfer-queue.cpp`)

- Removed the historical `0.44.0` sandbox fallback (`~/Pobrane/Testy/ThisPC-0.44.0-Transfer-Queue/Stage3/sandbox`) and legacy path assertion from `tests/transfer-queue.cpp`.
- Resolved independent sandbox security audit findings `AUD-TQ-01` (HIGH), `AUD-TQ-02` (HIGH), and `AUD-TQ-03` (LOW):
  - **Ownership & containment model (`AUD-TQ-01`):** Every scenario write, rename, settings file (`thispc-stage6-settings-test.ini`), and `removeRecursively()` call (including Stage 7.19–7.36) is strictly confined to the unique, freshly created, test-owned `QTemporaryDir` (`<base>/run-XXXXXX`, bound to `root`). No scenario writes or deletes directly in `base` or the parent workspace; `temporary` cleans up only its own `run-XXXXXX` directory on exit. Existing non-empty directories without a UID-owned regular `.thispc-transfer-queue-sandbox` marker, Git repositories (any ancestor with `.git`), system roots, `$HOME`, `~/.config`/`~/.local`/`~/.cache`, and XDG user-data directories (`Documents`, `Desktop`, `Pictures`, `Music`, `Movies`, and exact `DownloadLocation`) are rejected.
  - **Pre-creation symlink & canonical-path checks (`AUD-TQ-02`):** `hasNoSymlinkInExistingHierarchy()` (prefix-walk `::lstat`) and `TransferReservation::resolveLocal()` validate `base` **before** `QDir().mkpath(base)` and `QTemporaryDir`, and re-verify `root` without masking symlinks via `canonicalFilePath()` (`QFileInfo(root).canonicalFilePath() == root` and `root.startsWith(base + '/')`). Residual limitation: standard Qt/POSIX path APIs cannot eliminate every concurrent adversarial filesystem race between validation and creation, so primary containment relies on confining all destructive operations inside the unique `mkdtemp`-owned `QTemporaryDir`.
  - **Regression coverage (`AUD-TQ-03`):** Added disposable synthetic-fixture self-checks covering valid `THISPC_TEST_WORK_DIR`, `THISPC_QUEUE_SANDBOX` precedence, missing-override temp fallback, relative/`.`/`..` paths, `/` and `/etc`, `$HOME` and user Documents, existing non-empty unmarked directories, root/parent symlinks, `.git` repo/build subdirectories, containment verification, and parent-workspace preservation on cleanup.
- Focused ASan/UBSan verification in isolated `Stage1B/r2-fixtures` passed on two consecutive runs (Run 1: `2477 checks passed`, Run 2: `2476 checks passed`; evidence log: `Stage1B/logs/r2-transfer-queue-sandbox-security.log`), leaving parent `keep-me.txt` untouched and zero leftover scenario artifacts in `r2-fixtures/transfer_queue/`.
- Independent Stage 1B-R2 security delta audit verdict: `PASS — STAGE 1B-R2 SECURITY ACCEPTED / READY FOR FINAL CHECKPOINT` (`AUD-TQ-01`, `AUD-TQ-02`, and `AUD-TQ-03` resolved; residual theoretical Qt/POSIX TOCTOU race between `lstat` and `mkpath`/`mkdtemp` retained as a documented LOW risk).
- Canonical Stage 0/0A dependency gates remain unchanged: **G2** PASS; **G1**, **G3**, and **G4** PENDING (Stages 2–6).
- Stage 1B Git checkpoint manifest remains 12 files total (6 modified tracked + 6 new untracked, excluding `release-artifacts/`), with zero production code modifications.
