"""Stage files on the lab for the arms that run clients which are not in the package, and prove the bytes.

    python stage.py --to <lab directory> <local file or directory>...

A file goes to <lab directory>\\<name>, a directory to <lab directory>\\<directory name>\\... with its tree.
Every file is hashed here and on the lab. A file the lab already holds with the same SHA-256 is not sent
again, so a large input (a model that an earlier session staged) costs one hash on the lab and no copy. After
the copy every file is hashed on the lab again, and one difference fails the stage.

It prints one line per file and the summary line
    stage: <n> files, <n> sent, <n> already there, <n> differ after the copy
and ends with 'stage OK' only when nothing differs. Exit 0 then, 1 otherwise.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import target as lab  # noqa: E402  (tools/win/target.py: the address, the ssh options, push)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def inventory(sources: list[Path], remote_dir: str) -> list[tuple[Path, Path, str]]:
    """(the source it belongs to, the local file, its lab path) for every file of every source."""
    files = []
    for source in sources:
        if source.is_dir():
            for path in sorted(p for p in source.rglob("*") if p.is_file()):
                relative = path.relative_to(source.parent).as_posix().replace("/", "\\")
                files.append((source, path, remote_dir.rstrip("\\") + "\\" + relative))
        elif source.is_file():
            files.append((source, source, remote_dir.rstrip("\\") + "\\" + source.name))
        else:
            raise SystemExit(f"stage: no such file or directory: {source}")
    return files


def lab_hashes(target, paths: list[str]) -> dict[str, str]:
    """The SHA-256 of each path on the lab, or 'ABSENT'. One ssh call for the whole list."""
    listing = ",".join("'" + p.replace("'", "''") + "'" for p in paths)
    script = (f"foreach ($p in @({listing})) {{ if (Test-Path -LiteralPath $p) {{ "
              f"'{{0}}|{{1}}' -f (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash, $p }} "
              f"else {{ 'ABSENT|' + $p }} }}")
    out = target.run(script, timeout=600) or ""
    found = {}
    for line in out.splitlines():
        if "|" in line:
            value, path = line.strip().split("|", 1)
            found[path.lower()] = value
    return {p: found.get(p.lower(), "UNREAD") for p in paths}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--to", required=True, help="the lab directory")
    parser.add_argument("sources", nargs="+", type=Path)
    args = parser.parse_args(argv)
    target = lab.Target()
    files = inventory(args.sources, args.to)
    local = {remote: sha256(path) for _, path, remote in files}
    there = lab_hashes(target, [remote for _, _, remote in files])
    to_send = sorted({source for source, _, remote in files if there[remote] != local[remote]}, key=str)
    for source in to_send:
        print(f"send {source}", flush=True)
        target.push([str(source)], args.to, timeout=1200)
    after = lab_hashes(target, [remote for _, _, remote in files]) if to_send else there
    differ = 0
    for _, path, remote in files:
        state = "ok" if after[remote] == local[remote] else f"DIFFERS (lab {after[remote][:8]})"
        differ += state != "ok"
        print(f"  {local[remote][:8]}  {path.stat().st_size:>11}  {remote}  {state}")
    sent = sum(1 for _, _, remote in files if there[remote] != local[remote])
    print(f"stage: {len(files)} files, {sent} sent, {len(files) - sent} already there, {differ} differ after the copy")
    if differ:
        print("stage FAILED")
        return 1
    print("stage OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
