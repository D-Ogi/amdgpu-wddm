#!/usr/bin/env python3
"""Strict env/config IO for X3; Linux CLI touches only the verified diagnostic USB.

Import is pure. Offline tests call functions on temporary fixture directories.
The CLI accepts no alternative mount or device path and never reboots.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

HEADER = b"# GRUB Environment Block\n"
STICK = Path("/media/usb")
ENV_SIZE = 1024
DISK_SIZE = 64160400896


def read_env(data):
    if len(data) != ENV_SIZE or not data.startswith(HEADER):
        raise ValueError("invalid grubenv size/header")
    if any(c != 10 and not 32 <= c <= 126 for c in data):
        raise ValueError("non-ASCII/control byte in grubenv")
    rest = data[len(HEADER):]
    lines, names = [], set()
    while not (rest and set(rest) == {35}):
        m = re.match(rb"([A-Za-z_][A-Za-z0-9_]*)=([^\r\n]*)\n", rest)
        if not m or m[1] in names:
            raise ValueError("invalid/duplicate grubenv variable or padding")
        if m[1] == b"bc250_try" and m[2] not in (b"1", b"2", b"3"):
            raise ValueError("invalid bc250_try")
        names.add(m[1])
        lines.append(m[0])
        rest = rest[m.end():]
    return lines


def clear_env(data):
    content = HEADER + b"".join(x for x in read_env(data) if not x.startswith(b"bc250_try="))
    if len(content) >= ENV_SIZE:
        raise ValueError("no grubenv padding space")
    return content + b"#" * (ENV_SIZE - len(content))


def switch_config(data):
    text = data.decode("ascii")
    if len(re.findall(r"^set default=[0-6]$", text, re.M)) != 1:
        raise ValueError("expected exactly one valid default line")
    entries = re.findall(r'^menuentry "([^"\n]+)"', text, re.M)
    if len(entries) != 7 or entries[4] != "BC-250 reference: network only, no GPU probe" or entries[6] != "Windows (NVMe bootmgfw.efi, X3 start counter)":
        raise ValueError("unexpected recovery/Windows entry layout")
    if "--set=win_esp" not in text or "if load_env " not in text or "if save_env " not in text:
        raise ValueError("X3 cfg is not the corrected revision; install/verify it first")
    return re.sub(r"^set default=[0-6]$", "set default=6", text, flags=re.M).encode("ascii")


def corrected_config(old, head, entry):
    text = old.decode("ascii")
    starts = list(re.finditer(r'^menuentry "([^"\n]+)"[^\n]*\{', text, re.M))
    if len(starts) not in (6, 7) or starts[4][1] != "BC-250 reference: network only, no GPU probe":
        raise ValueError("unexpected menu layout")
    if len(starts) == 7 and starts[6][1] != "Windows (NVMe bootmgfw.efi, X3 start counter)":
        raise ValueError("unexpected seventh menu entry")
    defaults = re.findall(r"^set default=([0-6])$", text, re.M)
    timeouts = re.findall(r"^set timeout=(\d+)$", text, re.M)
    if len(defaults) != 1 or len(timeouts) != 1:
        raise ValueError("ambiguous default/timeout")
    # Keep the six existing entry bodies; normalize only the separator after them.
    end = starts[6].start() if len(starts) == 7 else len(text)
    body = text[starts[0].start():end].rstrip("\n") + "\n"
    out = ("set timeout=" + timeouts[0] + "\nset default=" + defaults[0] + "\n" +
           head.decode("ascii") + "\n" + body + entry.decode("ascii").lstrip("\n")).encode("ascii")
    switch_config(out)  # structure + corrected-logic admission
    return out


def durable_new(path, data):
    with path.open("xb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    if path.read_bytes() != data:
        raise OSError("candidate readback mismatch")


def backup(path, data):
    dest = path.with_name(path.name + ".x3-backup-" + hashlib.sha256(data).hexdigest()[:16])
    if dest.exists():
        if dest.read_bytes() != data:
            raise OSError("existing backup differs")
    else:
        durable_new(dest, data)
    return dest


def prepare_switch(stick):
    g = stick / "boot/grub"
    old_cfg = (g / "grub.cfg").read_bytes()
    old_env = (g / "grubenv").read_bytes()
    cfg, env = switch_config(old_cfg), clear_env(old_env)
    # All validation before candidate/backup writes; preserve every unrelated env variable.
    backup(g / "grub.cfg", old_cfg)
    backup(g / "grubenv", old_env)
    durable_new(g / "grub.cfg.x3-new", cfg)
    durable_new(g / "grubenv.x3-new", env)


def verify_switch(stick):
    g = stick / "boot/grub"
    data = (g / "grub.cfg").read_bytes()
    if switch_config(data) != data:
        raise ValueError("default was not set to 6")
    data = (g / "grubenv").read_bytes()
    if clear_env(data) != data:
        raise ValueError("counter was not cleared")


def prepare_upgrade(stick, sources):
    g = stick / "boot/grub"
    old = (g / "grub.cfg").read_bytes()
    env = (g / "grubenv").read_bytes()
    read_env(env)  # Never repair a corrupt counter silently.
    new = corrected_config(old, (sources / "x3-head.cfg").read_bytes(), (sources / "x3-entry.cfg").read_bytes())
    backup(g / "grub.cfg", old)
    backup(g / "grubenv", env)
    durable_new(g / "grub.cfg.x3-new", new)


def verify_upgrade(stick, sources):
    g = stick / "boot/grub"
    actual = (g / "grub.cfg").read_bytes()
    if corrected_config(actual, (sources / "x3-head.cfg").read_bytes(), (sources / "x3-entry.cfg").read_bytes()) != actual:
        raise ValueError("corrected config readback differs")
    read_env((g / "grubenv").read_bytes())


def validate_stick():
    # Bind the mount used for writes to the exact partition checked for identity.
    mounts = [line.split() for line in Path("/proc/mounts").read_text().splitlines()]
    found = [m for m in mounts if m[1] == str(STICK)]
    if len(found) != 1 or found[0][0] != "/dev/sda1" or found[0][2] != "vfat":
        raise ValueError("unexpected diagnostic mount/device/filesystem")
    text = subprocess.run(["blkid", "/dev/sda1"], check=True, capture_output=True, text=True).stdout
    if 'LABEL="BC250DIAG"' not in text or 'TYPE="vfat"' not in text:
        raise ValueError("stick label/type mismatch")
    disk = Path("/sys/class/block/sda")
    if int((disk / "size").read_text()) * 512 != DISK_SIZE or not re.search(r"/usb\d+/", str(disk.resolve())):
        raise ValueError("stick USB bus/size mismatch")
    if not (STICK / "boot/grub/bc250diag.id").is_file() or not (STICK / "efi/boot/bootx64.efi").is_file():
        raise ValueError("active X3 loader/marker missing")
    if (STICK / "efi/microsoft").exists():
        raise ValueError("diagnostic stick carries a Windows loader directory")


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("validate", "prepare", "verify", "upgrade", "verify-upgrade"):
        raise ValueError("usage: x3-files.py validate|prepare|verify|upgrade|verify-upgrade")
    validate_stick()
    if sys.argv[1] == "prepare":
        prepare_switch(STICK)
    elif sys.argv[1] == "verify":
        verify_switch(STICK)
    elif sys.argv[1] == "upgrade":
        prepare_upgrade(STICK, Path(__file__).resolve().parent)
    elif sys.argv[1] == "verify-upgrade":
        verify_upgrade(STICK, Path(__file__).resolve().parent)
    print("X3 " + sys.argv[1] + " OK")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print("X3 refused: " + str(exc), file=sys.stderr)
        sys.exit(1)
