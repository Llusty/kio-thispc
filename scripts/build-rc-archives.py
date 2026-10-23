#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

def main():
    repo_root = Path(__file__).resolve().parents[1]
    rc_dir = repo_root / "rc-artifacts"
    rc_dir.mkdir(parents=True, exist_ok=True)

    version = "0.42.0"
    archive_prefix = f"kio-thispc-{version}"
    tar_name = f"kio-thispc-{version}-rc.tar.gz"
    zip_name = f"kio-thispc-{version}-rc.zip"
    tar_path = rc_dir / tar_name
    zip_path = rc_dir / zip_name

    # 1. Collect files to include
    result = subprocess.run(
        ["git", "ls-files"],
        cwd=repo_root,
        check=True,
        text=True,
        capture_output=True,
    )
    tracked_files = [Path(line.strip()) for line in result.stdout.splitlines() if line.strip()]

    new_files = [
        Path("docs/DIRECTORY_REFRESH_CONTRACT_STAGE1.md"),
        Path("docs/MTP_PERMANENT_DELETE_CONTRACT_STAGE2.md"),
        Path("docs/SMART_DELETE_FALLBACK_STAGE2_3.md"),
        Path("docs/RELEASE_CHECKLIST_040_DRAFT.md"),
        Path("docs/RELEASE_NOTES_040_DRAFT.md"),
        Path("src/directorywatchcontroller.cpp"),
        Path("src/directorywatchcontroller.h"),
        Path("tests/directory-refresh-contract-spec.h"),
        Path("tests/directory-refresh.cpp"),
        Path("tests/mtp-permanent-delete-contract-spec.h"),
        Path("tests/mtp-permanent-delete.cpp"),
        Path("tests/run-directory-refresh-tests.py"),
        Path("tests/run-mtp-permanent-delete-tests.py"),
        Path("tests/run-smart-delete-fallback-tests.py"),
        Path("tests/smart-delete-fallback-contract-spec.h"),
        Path("tests/smart-delete-fallback.cpp"),
    ]

    all_files = set(tracked_files)
    for nf in new_files:
        if (repo_root / nf).is_file():
            all_files.add(nf)

    print(f"Collected {len(all_files)} files for packaging.")

    # 2. Stage into a clean directory
    staging_base = Path("/tmp/thispc-rc-staging")
    if staging_base.exists():
        shutil.rmtree(staging_base)
    staging_target = staging_base / archive_prefix
    staging_target.mkdir(parents=True, exist_ok=True)

    for rel_path in sorted(all_files):
        src = repo_root / rel_path
        dst = staging_target / rel_path
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)

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

    # 6. Verify extraction and build in /tmp
    verify_dir = Path("/tmp/thispc-rc-verify")
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

    print("SUCCESS: Release Candidate archives created and verified successfully.")
    # Clean up staging and verify dirs
    shutil.rmtree(staging_base)
    shutil.rmtree(verify_dir)

if __name__ == "__main__":
    main()
