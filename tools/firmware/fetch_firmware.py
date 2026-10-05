#!/usr/bin/env python3
"""Fetch, verify and push the BC-250's AMD PSP firmware files.

The eight files are linux-firmware's amdgpu/cyan_skillfish2_*.bin (the KMD's BC250_PSP_FIRMWARE_DIR reads
them from C:\\BC250\\firmware\\ on the target, driver/kmd/psp.c). The manifest next to this file
(cyan_skillfish2.json) pins the source commit, the download URLs, and every file's size and SHA-256; nothing
here trusts a downloaded or already-present file without checking both against it. This tool is Windows-native
Python standard library only (no pip install, no Linux needed anywhere) - "linux-firmware" is only the name of
the upstream project the blobs come from.

    python tools/firmware/fetch_firmware.py fetch [--out DIR]
        Download every manifest file into DIR (default: <BC250_ROOT>/ref/linux-firmware__WARN-AMD-blobs-never-
        commit/amdgpu), skipping any file already present with the right size and hash. A downloaded file that
        does not match the manifest is deleted and reported; the command then fails for that file but still
        tries the rest.

    python tools/firmware/fetch_firmware.py verify [DIR]
        Check DIR (same default as above) against the manifest, size and SHA-256 for every file. Exits 1 on
        any mismatch.

    python tools/firmware/fetch_firmware.py push [DIR]
        Verify DIR, then copy the files to the manifest's remote_dir on the lab target through
        tools/win/target.py. Files whose SHA-256 already matches on the target are left alone and reported
        "identical"; the rest are reported "copied".

BC250_ROOT sets the workspace root the default directories are computed from (default: the parent directory
of this repository); see tools/win/target.py for the same convention. The firmware blobs never enter this repository - see
tools/firmware/README.md.

Exit codes: 0 ok, 1 verification/download/push mismatch or failure, 2 usage error.
"""

import hashlib
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST_PATH = HERE / "cyan_skillfish2.json"
# Workspace root: BC250_ROOT from the environment, else the parent directory of this repository
# (HERE is <repo>/tools/firmware).
ROOT = Path(os.environ.get("BC250_ROOT", str(HERE.parents[2])))
DEFAULT_DIR = ROOT / "ref" / "linux-firmware__WARN-AMD-blobs-never-commit" / "amdgpu"


def load_manifest():
    with open(MANIFEST_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


def sha256_of(path, chunk_size=1024 * 1024):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk_size)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def check_file(path, entry):
    """None if `path` matches `entry`'s size and sha256, else a short mismatch reason."""
    if not path.is_file():
        return "missing"
    size = path.stat().st_size
    if size != entry["size"]:
        return f"size {size} != {entry['size']}"
    digest = sha256_of(path)
    if digest != entry["sha256"]:
        return f"sha256 {digest} != {entry['sha256']}"
    return None


def cmd_fetch(manifest, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    ok = True
    for entry in manifest["files"]:
        name = entry["name"]
        dest = out_dir / name
        reason = check_file(dest, entry)
        if reason is None:
            print(f"{name}: already present, hash matches")
            continue

        fetched = False
        last_error = None
        for template in manifest["url_templates"]:
            url = template.format(name=name, commit=manifest["commit"])
            try:
                with urllib.request.urlopen(url, timeout=60) as resp:
                    data = resp.read()
            except (urllib.error.URLError, OSError) as e:
                last_error = f"{url}: {e}"
                continue

            tmp = dest.with_name(dest.name + ".part")
            with open(tmp, "wb") as f:
                f.write(data)
            bad = check_file(tmp, entry)
            if bad is not None:
                tmp.unlink(missing_ok=True)
                last_error = f"{url}: downloaded but {bad}"
                continue

            tmp.replace(dest)
            print(f"{name}: downloaded from {url}")
            fetched = True
            break

        if not fetched:
            print(f"{name}: FAILED - {last_error}", file=sys.stderr)
            ok = False
    return ok


def cmd_verify(manifest, directory):
    ok = True
    for entry in manifest["files"]:
        reason = check_file(directory / entry["name"], entry)
        if reason is None:
            print(f"{entry['name']}: ok")
        else:
            print(f"{entry['name']}: MISMATCH - {reason}", file=sys.stderr)
            ok = False
    return ok


def cmd_push(manifest, directory):
    if not cmd_verify(manifest, directory):
        print("push: local directory failed verification, nothing copied", file=sys.stderr)
        return False

    sys.path.insert(0, str(HERE.parent / "win"))
    import target as target_mod  # tools/win/target.py

    remote_dir = manifest["remote_dir"]
    names = [entry["name"] for entry in manifest["files"]]
    quoted_names = ", ".join(f"'{n}'" for n in names)
    # One remote hash check for every file up front, so identical files are never rewritten.
    script = (
        f"$names = @({quoted_names})\n"
        f"$dir = '{remote_dir}'\n"
        "foreach ($n in $names) {\n"
        "    $p = Join-Path $dir $n\n"
        "    if (Test-Path $p) {\n"
        "        Write-Output ($n + ' ' + (Get-FileHash -Algorithm SHA256 -Path $p).Hash.ToLower())\n"
        "    } else {\n"
        "        Write-Output ($n + ' MISSING')\n"
        "    }\n"
        "}\n"
    )

    target = target_mod.Target()
    remote_hashes = {}
    for line in target.run(script).splitlines():
        parts = line.strip().split()
        if len(parts) == 2:
            remote_hashes[parts[0]] = parts[1]

    to_copy = []
    for entry in manifest["files"]:
        if remote_hashes.get(entry["name"]) == entry["sha256"]:
            print(f"{entry['name']}: identical")
        else:
            to_copy.append(directory / entry["name"])

    if to_copy:
        target.push([str(p) for p in to_copy], remote_dir)
        for p in to_copy:
            print(f"{p.name}: copied")
    return True


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, rest = argv[0], argv[1:]
    manifest = load_manifest()

    if cmd == "fetch":
        out_dir = DEFAULT_DIR
        if rest:
            if len(rest) == 2 and rest[0] == "--out":
                out_dir = Path(rest[1])
            else:
                sys.exit(2)
        sys.exit(0 if cmd_fetch(manifest, out_dir) else 1)
    elif cmd == "verify":
        if len(rest) > 1:
            sys.exit(2)
        directory = Path(rest[0]) if rest else DEFAULT_DIR
        sys.exit(0 if cmd_verify(manifest, directory) else 1)
    elif cmd == "push":
        if len(rest) > 1:
            sys.exit(2)
        directory = Path(rest[0]) if rest else DEFAULT_DIR
        sys.exit(0 if cmd_push(manifest, directory) else 1)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
