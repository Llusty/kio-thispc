#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser(description="Build and verify ThisPC Release Candidate archives.")
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

    version = "0.43.0"
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

    new_files = [
        Path("src/adminretrycontract.h"),
        Path("src/adminretrycontroller.h"),
        Path("src/adminretryexecutor.h"),
        Path("tests/admin-retry.cpp"),
        Path("tests/admin-retry-stage2.cpp"),
        Path("tests/admin-retry-stage3.cpp"),
        Path("tests/admin-retry-stage4.cpp"),
        Path("tests/admin-retry-stage5.cpp"),
    ]

    forbidden_prefixes = ("release-artifacts/", "rc-artifacts/", "build/")
    all_files = set()
    for tf in tracked_files:
        rel_str = tf.as_posix()
        if not rel_str.startswith(forbidden_prefixes) and (repo_root / tf).is_file():
            all_files.add(tf)
    for nf in new_files:
        if (repo_root / nf).is_file():
            all_files.add(nf)

    print(f"Collected {len(all_files)} files for packaging.")

    # 2. Stage into a clean directory
    staging_base = work_root / "thispc-rc-staging"
    if staging_base.exists():
        shutil.rmtree(staging_base)
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
    verify_dir = work_root / "thispc-rc-verify"
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

    for req_file in new_files:
        assert (extracted_root / req_file).is_file(), f"Missing new 0.43.0 file in extracted archive: {req_file}"

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
        "## Verified 0.43.0 New Files Included (8/8)\n\n",
    ]
    for nf in new_files:
        completeness_lines.append(f"- `{archive_prefix}/{nf.as_posix()}` — PRESENT\n")
    completeness_file.write_text("".join(completeness_lines))

    print("SUCCESS: Release Candidate archives created and verified successfully.")
    shutil.rmtree(staging_base)
    if not args.keep_verify_dir:
        shutil.rmtree(verify_dir)

if __name__ == "__main__":
    main()
