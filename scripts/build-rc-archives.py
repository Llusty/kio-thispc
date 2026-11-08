#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

def main():
    parser = argparse.ArgumentParser(description="Build and verify ThisPC Release Candidate archives.")
    parser.add_argument("--version", default="",
                        help="Target release version (default: parsed from CMakeLists.txt or THISPC_VERSION env).")
    parser.add_argument("--output-dir", default=os.environ.get("THISPC_RC_OUTPUT_DIR", ""),
                        help="Output directory for RC archives and manifests.")
    parser.add_argument("--work-dir", default=os.environ.get("THISPC_RC_WORK_DIR", ""),
                        help="Working directory for staging and verification builds.")
    parser.add_argument("--rc-tag", default="rc1",
                        help="Release candidate suffix for archive filenames (default: rc1).")
    parser.add_argument("--keep-verify-dir", action="store_true",
                        help="Retain the extracted verification build directory.")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    rc_dir = Path(args.output_dir).resolve() if args.output_dir else (repo_root / "rc-artifacts")
    rc_dir.mkdir(parents=True, exist_ok=True)
    work_root = Path(args.work_dir).resolve() if args.work_dir else Path("/tmp")
    work_root.mkdir(parents=True, exist_ok=True)

    cmake_text = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    cmake_ver_match = re.search(r"^project\(kio-thispc VERSION ([^ ]+)", cmake_text, re.MULTILINE)
    default_version = cmake_ver_match.group(1) if cmake_ver_match else "0.45.0"
    version = args.version or os.environ.get("THISPC_VERSION", default_version)

    rc_suffix = f"-{args.rc_tag}" if args.rc_tag else "-rc"
    archive_prefix = f"kio-thispc-{version}"
    tar_name = f"kio-thispc-{version}{rc_suffix}.tar.gz"
    zip_name = f"kio-thispc-{version}{rc_suffix}.zip"
    tar_path = rc_dir / tar_name
    zip_path = rc_dir / zip_name

    # 1. Collect files to include (inherits GIT_INDEX_FILE when set)
    result = subprocess.run(
        ["git", "ls-files"],
        cwd=repo_root,
        check=True,
        text=True,
        capture_output=True,
    )
    tracked_files = [Path(line.strip()) for line in result.stdout.splitlines() if line.strip()]

    required_core_files = [
        Path("CMakeLists.txt"),
        Path("install.sh"),
        Path("uninstall.sh"),
        Path("src/thispcview.cpp"),
        Path("src/operationmanager.h"),
        Path("src/transferqueuecontroller.h"),
        Path("src/localfileoverwrite.h"),
        Path("docs/SECURITY_AUDIT_045.md"),
    ]

    forbidden_prefixes = ("release-artifacts/", "rc-artifacts/", "build/")
    all_files = set()
    for tf in tracked_files:
        rel_str = tf.as_posix()
        if not rel_str.startswith(forbidden_prefixes) and (repo_root / tf).is_file():
            all_files.add(tf)

    print(f"Collected {len(all_files)} files for packaging.")

    # 2. Stage into a clean temporary directory to prevent symlink/collision attacks
    staging_temp = tempfile.TemporaryDirectory(prefix="thispc-rc-staging-", dir=work_root)
    staging_base = Path(staging_temp.name)
    staging_target = staging_base / archive_prefix
    staging_target.mkdir(parents=True, exist_ok=True)

    sorted_files = sorted(all_files)
    for rel_path in sorted_files:
        src = repo_root / rel_path
        dst = staging_target / rel_path
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

    # Write source file list manifest
    source_list_file = rc_dir / "SOURCE_FILES.txt"
    source_list_file.write_text("".join(f"{archive_prefix}/{p.as_posix()}\n" for p in sorted_files))

    # 3. Create tar.gz archive
    if tar_path.exists():
        tar_path.unlink()
    print(f"Creating {tar_path}...")
    subprocess.run(
        ["tar", "-czf", str(tar_path), archive_prefix],
        cwd=staging_base,
        check=True,
    )

    # 4. Create zip archive
    if zip_path.exists():
        zip_path.unlink()
    print(f"Creating {zip_path}...")
    subprocess.run(
        ["zip", "-r", "-q", str(zip_path), archive_prefix],
        cwd=staging_base,
        check=True,
    )

    # 5. Compute SHA256 sums
    sums_file = rc_dir / "SHA256SUMS"
    sha_lines = []
    for archive in (tar_path, zip_path):
        h = hashlib.sha256()
        with open(archive, "rb") as f:
            while chunk := f.read(65536):
                h.update(chunk)
        digest = h.hexdigest()
        sha_lines.append(f"{digest}  {archive.name}\n")

    sums_file.write_text("".join(sha_lines))
    print(f"Wrote SHA256SUMS:\n{sums_file.read_text()}")

    # 6. Verify extraction and build
    verify_temp = None
    if not args.keep_verify_dir:
        verify_temp = tempfile.TemporaryDirectory(prefix="thispc-rc-verify-", dir=work_root)
        verify_dir = Path(verify_temp.name)
    else:
        verify_dir = work_root / f"thispc-rc-verify-{version}"
        if verify_dir.exists():
            shutil.rmtree(verify_dir)
        verify_dir.mkdir(parents=True, exist_ok=True)

    print("Extracting tarball for verification build...")
    subprocess.run(
        ["tar", "-xzf", str(tar_path), "-C", str(verify_dir)],
        check=True,
    )

    extracted_root = verify_dir / archive_prefix
    assert extracted_root.is_dir(), f"Extracted directory {extracted_root} not found"

    for req_file in required_core_files:
        assert (extracted_root / req_file).is_file(), f"Missing core file in extracted archive: {req_file}"

    print("Configuring extracted build...")
    subprocess.run(
        ["cmake", "-S", str(extracted_root), "-B", str(extracted_root / "build"), "-DCMAKE_BUILD_TYPE=Release"],
        check=True,
    )

    print("Compiling extracted build...")
    subprocess.run(
        ["cmake", "--build", str(extracted_root / "build"), "-j", str(os.cpu_count() or 4)],
        check=True,
    )

    # Check CLI version
    binary = extracted_root / "build/bin/thispc-view"
    ver_out = subprocess.run([str(binary), "--version"], check=True, capture_output=True, text=True).stdout
    print(f"Extracted binary version check: {ver_out.strip()}")
    assert ver_out.strip() == f"thispc-view {version}", f"Unexpected version: {ver_out}"

    # Run version-cli.py in extracted tree
    subprocess.run(
        [sys.executable, str(extracted_root / "tests/version-cli.py")],
        check=True,
    )

    # Write completeness report
    completeness_file = rc_dir / "COMPLETENESS_REPORT.md"
    completeness_lines = [
        f"# ThisPC {version} ({args.rc_tag.upper()}) — Source Archive Completeness Report\n\n",
        f"- **Version:** `{version}`\n",
        f"- **Archive Prefix:** `{archive_prefix}/`\n",
        f"- **Total Packaged Files:** `{len(sorted_files)}`\n",
        f"- **Tarball:** `{tar_name}` (`{tar_path.stat().st_size}` bytes)\n",
        f"- **Zip Archive:** `{zip_name}` (`{zip_path.stat().st_size}` bytes)\n",
        f"- **Extracted Independent Build:** `PASS`\n",
        f"- **Extracted CLI `--version`:** `{ver_out.strip()}` (`PASS`)\n\n",
        f"## Verified Core Files Included ({len(required_core_files)}/{len(required_core_files)})\n\n",
    ]
    for rf in required_core_files:
        completeness_lines.append(f"- `{archive_prefix}/{rf.as_posix()}` — PRESENT\n")
    completeness_file.write_text("".join(completeness_lines))

    print("SUCCESS: Release Candidate archives created and verified successfully.")
    staging_temp.cleanup()
    if verify_temp is not None:
        verify_temp.cleanup()

if __name__ == "__main__":
    main()
