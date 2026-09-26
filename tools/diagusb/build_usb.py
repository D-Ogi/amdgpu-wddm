#!/usr/bin/env python3
"""Assemble the file tree of the BC-250 diagnostic USB stick (Alpine Linux, diskless mode).

The result is a directory. Copy its contents to an empty FAT32 partition and the stick boots on
UEFI machines (see write_usb.ps1). Nothing here needs admin rights or Linux.

    python tools/diagusb/build_usb.py --work <BC250_ROOT>\\scratch --net <BC250_ROOT>\\secrets\\net

Layout produced in <work>/usbroot:
    boot/ efi/ apks/            from the Alpine "standard" ISO (kernel has amdgpu + Cyan Skillfish firmware)
    boot/grub/grub.cfg          ours: boot menu with the diagnostic modes, amdgpu blacklisted at boot
    bc250diag.apkovl.tar.gz     overlay: tty1 runs /etc/bc250/launch instead of a login prompt
    bc250/                      payload (diag.py, qrshow.py, probes.json), python3 .apk files, vendored segno
    bc250/net/                  optional, copied from --net: Wi-Fi config and SSH keys (secrets, never in git)

Requires: 7z on PATH (ISO extraction), pip (to fetch the pure-Python 'segno' wheel).
"""

import argparse
import gzip
import hashlib
import io
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ALPINE_BRANCH = "v3.24"
ALPINE_VERSION = "3.24.2"
ISO_NAME = f"alpine-standard-{ALPINE_VERSION}-x86_64.iso"
MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
EXTRA_PACKAGES = ["python3", "pciutils",
                  # added 2026-09-22 (E21): KMS and Vulkan tools, scp, hex dumps
                  "libdrm-tests", "drm_info", "mesa-vulkan-ati", "vulkan-loader", "vulkan-tools",
                  "openssh-sftp-server", "xxd"]  # not on the ISO; installed from bc250/apks at boot
SEGNO_VERSION = "1.6.6"


def fetch(url, dest):
    if dest.exists() and dest.stat().st_size:
        return dest
    print(f"  fetching {url}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    tmp.replace(dest)
    return dest


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def get_iso(cache):
    base = f"{MIRROR}/{ALPINE_BRANCH}/releases/x86_64/{ISO_NAME}"
    iso = fetch(base, cache / ISO_NAME)
    expected = fetch(base + ".sha256", cache / (ISO_NAME + ".sha256")).read_text().split()[0]
    if sha256(iso) != expected:
        raise SystemExit(f"{iso}: SHA-256 mismatch, delete it and rerun")
    return iso


# ---- Alpine package closure -------------------------------------------------

def parse_apkindex(data):
    """APKINDEX text -> list of {P,V,D,p} records."""
    pkgs = []
    for block in data.split("\n\n"):
        rec = {}
        for line in block.splitlines():
            if len(line) > 2 and line[1] == ":":
                rec[line[0]] = line[2:]
        if "P" in rec:
            pkgs.append(rec)
    return pkgs


def strip_constraint(token):
    for sep in ("<", ">", "=", "~"):
        token = token.split(sep)[0]
    return token


def resolve(index, wanted):
    """Dependency closure of `wanted` as a list of index records."""
    by_name, provides = {}, {}
    for rec in index:
        by_name.setdefault(rec["P"], rec)
        for token in rec.get("p", "").split():
            provides.setdefault(strip_constraint(token), rec)
    done, queue = {}, list(wanted)
    while queue:
        token = queue.pop()
        if token.startswith("!"):
            continue
        name = strip_constraint(token)
        rec = by_name.get(name) or provides.get(name)
        if rec is None:
            raise SystemExit(f"cannot resolve dependency '{token}'")
        if rec["P"] not in done:
            done[rec["P"]] = rec
            queue.extend(rec.get("D", "").split())
    return list(done.values())


def get_packages(cache, dest):
    index = []
    for repo in ("main", "community"):
        url = f"{MIRROR}/{ALPINE_BRANCH}/{repo}/x86_64/APKINDEX.tar.gz"
        with urllib.request.urlopen(url) as r:
            with tarfile.open(fileobj=io.BytesIO(r.read()), mode="r:gz") as tar:
                for rec in parse_apkindex(tar.extractfile("APKINDEX").read().decode()):
                    rec["repo"] = repo
                    index.append(rec)
    closure = resolve(index, EXTRA_PACKAGES)
    dest.mkdir(parents=True, exist_ok=True)
    for old in dest.glob("*.apk"):
        old.unlink()
    for rec in closure:
        name = f"{rec['P']}-{rec['V']}.apk"
        src = fetch(f"{MIRROR}/{ALPINE_BRANCH}/{rec['repo']}/x86_64/{name}", cache / "apks" / name)
        shutil.copy2(src, dest / name)
    print(f"  {len(closure)} packages for {', '.join(EXTRA_PACKAGES)}")


def get_segno(cache, dest):
    wheels = cache / "wheels"
    if not list(wheels.glob(f"segno-{SEGNO_VERSION}-*.whl")):
        subprocess.run([sys.executable, "-m", "pip", "download", f"segno=={SEGNO_VERSION}", "--no-deps",
                        "-d", str(wheels), "-q"], check=True)
    wheel = next(wheels.glob(f"segno-{SEGNO_VERSION}-*.whl"))
    if dest.exists():
        shutil.rmtree(dest)
    with zipfile.ZipFile(wheel) as z:
        z.extractall(dest, [n for n in z.namelist() if n.startswith("segno/") or n.endswith("LICENSE")])


# ---- overlay ----------------------------------------------------------------

def build_apkovl(overlay_dir, out):
    """Alpine applies this tarball over / in RAM at boot. Everything root-owned; scripts executable."""
    def add(tar, name, data=b"", mode=0o644, isdir=False):
        info = tarfile.TarInfo(name)
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        info.mtime = 0
        if isdir:
            info.type, info.mode = tarfile.DIRTYPE, 0o755
            tar.addfile(info)
        else:
            info.size, info.mode = len(data), mode
            tar.addfile(info, io.BytesIO(data))

    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(overlay_dir.rglob("*")):
            rel = path.relative_to(overlay_dir).as_posix()
            if path.is_dir():
                add(tar, rel, isdir=True)
            else:
                data = path.read_bytes().replace(b"\r\n", b"\n")
                add(tar, rel, data, 0o755 if rel.startswith("etc/bc250/") else 0o644)
        add(tar, "etc/hostname", b"bc250diag\n")
        # Tells Alpine's init to enable its default boot services even though an overlay exists.
        add(tar, "etc/.default_boot_services")
    with gzip.GzipFile(out, "wb", mtime=0) as gz:
        gz.write(raw.getvalue())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", type=Path, required=True, help="working directory (cache + usbroot), outside the repo")
    ap.add_argument("--net", type=Path, help="secrets directory with wpa_supplicant.conf, authorized_keys, ssh host key")
    args = ap.parse_args()
    cache, root = args.work / "dl", args.work / "usbroot"

    print("[1/6] Alpine ISO")
    iso = get_iso(cache)
    print("[2/6] extracting ISO")
    if root.exists():
        shutil.rmtree(root)
    subprocess.run(["7z", "x", str(iso), f"-o{root}", "-y", "-bso0", "-bsp0"], check=True)
    shutil.rmtree(root / "[BOOT]", ignore_errors=True)
    print("[3/6] extra packages")
    get_packages(cache, root / "bc250" / "apks")
    print("[4/6] payload")
    subprocess.run([sys.executable, str(HERE / "gen_probes.py")], check=True)
    for f in (HERE / "payload" / "bc250").iterdir():
        if f.is_file():
            (root / "bc250" / f.name).write_bytes(f.read_bytes().replace(b"\r\n", b"\n"))
    get_segno(cache, root / "bc250" / "pylib")
    print("[5/6] overlay and boot menu")
    build_apkovl(HERE / "overlay", root / "bc250diag.apkovl.tar.gz")
    (root / "boot" / "grub" / "grub.cfg").write_bytes((HERE / "grub.cfg").read_bytes().replace(b"\r\n", b"\n"))
    print("[6/6] remote access")
    if args.net and (args.net / "authorized_keys").exists():
        shutil.copytree(args.net, root / "bc250" / "net")
        print(f"  enabled from {args.net}")
    else:
        print("  disabled (no --net directory with authorized_keys)")
    size = sum(f.stat().st_size for f in root.rglob("*") if f.is_file())
    print(f"done: {root} ({size / 2**20:.0f} MiB)")


if __name__ == "__main__":
    main()
