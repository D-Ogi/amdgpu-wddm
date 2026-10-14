#!/usr/bin/env python3
"""Build the INF-owned user-mode payload before Inf2Cat, never after signing.

The input slots are release-sources.json's existing, hash-pinned artifacts. Each
output basename is unique in the INF, including x86. No DLL bytes are rewritten.
PROVENANCE: MicrosoftDocs/windows-driver-docs (CC-BY-4.0),
develop/run-from-driver-store.md and display/wddm-2-1-features.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import struct
from pathlib import Path, PurePosixPath
from image_identity import image_digest

SCHEMA = 1
PREFIX = "payload/kmd/"
# (input slot, DriverStore member). Keep different RADV roles separate, even
# when two particular releases happen to contain identical bytes.
NATIVE = (
    ("desktop/bc250d3d_router.dll", "desktop/bc250d3d_router.dll"),
    ("desktop/bc250d3d.dll", "desktop/bc250d3d.dll"),
    ("desktop/bc250d3d_zink.dll", "desktop/bc250d3d_zink.dll"),
    ("desktop/amdgpu_wddm_radv.dll", "desktop/amdgpu_wddm_radv.dll"),
    ("d3d11/amdgpu_wddm_d3d11.dll", "d3d11/amdgpu_wddm_d3d11.dll"),
    ("d3d11/amdgpu_wddm_d3d11.config", "d3d11/amdgpu_wddm_d3d11.config"),
    ("d3d11/amdgpu_wddm_dxvk.dll", "d3d11/amdgpu_wddm_dxvk.dll"),
    ("d3d11/amdgpu_wddm_radv.dll", "d3d11/amdgpu_wddm_radv_d3d11.dll"),
    ("d3d12/amdgpu_wddm_d3d12.dll", "d3d12/amdgpu_wddm_d3d12.dll"),
    ("d3d12/amdgpu_wddm_vkd3d.dll", "d3d12/amdgpu_wddm_vkd3d.dll"),
    ("d3d12/amdgpu_wddm_radv.dll", "d3d12/amdgpu_wddm_radv_d3d12.dll"),
    ("vulkan/vulkan_radeon.dll", "vulkan/vulkan_radeon.dll"),
    ("vulkan/radeon_icd.json", "vulkan/radeon_icd.json"),
)
WOW = tuple(("wow64/" + src, "wow64/" + str(PurePosixPath(dst).with_name(
    PurePosixPath(dst).stem + "32" + PurePosixPath(dst).suffix)))
    for src, dst in NATIVE if src not in (
        "desktop/bc250d3d_zink.dll", "desktop/amdgpu_wddm_radv.dll"))


def relative(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]+(?:[./][A-Za-z0-9_-]+)*", value):
        raise ValueError(f"invalid package member: {value!r}")
    if any(p in ("", ".", "..") for p in value.split("/")):
        raise ValueError(f"invalid package member: {value!r}")
    if any(re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]", p.split('.')[0], re.I) for p in value.split('/')):
        raise ValueError(f"reserved Windows path: {value!r}")
    return value


def plain_path(root: Path, name: str) -> Path:
    """Refuse a redirected input or output ancestor before any staging write."""
    path = root
    for part in relative(name).split('/'):
        path = path / part
        if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
            raise ValueError(f"linked package path: {name}")
    return path


def members(sources: dict) -> list[dict]:
    """A closed set. Optional ICD architectures must be explicitly declared."""
    pairs = list(NATIVE + WOW)
    gl = sources.get("opengl_icd", {})
    arch = gl.get("architectures", ["x64"] if gl.get("register") else [])
    if len(arch) != len(set(arch)) or any(a not in ("x64", "x86") for a in arch):
        raise ValueError("invalid OpenGL architectures")
    if gl.get("register"):
        if not arch:
            raise ValueError("OpenGL registration requires an architecture")
        if "x64" in arch:
            pairs.append(("opengl/amdgpu_wddm_gl.dll", "opengl/amdgpu_wddm_gl.dll"))
        if "x86" in arch:
            pairs.append(("wow64/opengl/amdgpu_wddm_gl.dll", "wow64/opengl/amdgpu_wddm_gl32.dll"))
    if sources.get("mft_h264", {}).get("register"):
        pairs.append(("mft/amdgpu_wddm_mft_h264.dll", "mft/amdgpu_wddm_mft_h264.dll"))
    rows = [{"source": "payload/" + src, "member": relative(dst),
             "path": PREFIX + dst, "arch": "x86" if dst.startswith("wow64/") else "x64"}
            for src, dst in pairs]
    names = [PurePosixPath(row["member"]).name.lower() for row in rows]
    if len(names) != len(set(names)):
        raise ValueError("DriverStore member basenames must be unique")
    return rows


def registrations(rows: list[dict], sources: dict | None = None) -> dict:
    files = {r["member"] for r in rows}
    root = "%13%\\"
    result = {
        "AmdgpuDriverPackage": {"kind": "String", "value": "%13%"},
        "UserModeDriverName": {"kind": "MultiString", "value": ["<>", root + "desktop\\bc250d3d_router.dll",
            root + "desktop\\bc250d3d_router.dll", root + "d3d12\\amdgpu_wddm_d3d12.dll"]},
        "UserModeDriverNameWow": {"kind": "MultiString", "value": ["<>", root + "wow64\\desktop\\bc250d3d_router32.dll",
            root + "wow64\\desktop\\bc250d3d_router32.dll", root + "wow64\\d3d12\\amdgpu_wddm_d3d1232.dll"]},
        "VulkanDriverName": {"kind": "MultiString", "value": [root + "vulkan\\radeon_icd.json"]},
        "VulkanDriverNameWow": {"kind": "MultiString", "value": [root + "wow64\\vulkan\\radeon_icd32.json"]},
    }
    for suffix, member in (("", "opengl/amdgpu_wddm_gl.dll"), ("Wow", "wow64/opengl/amdgpu_wddm_gl32.dll")):
        if member in files:
            gl = (sources or {}).get("opengl_icd", {})
            kind = {"REG_MULTI_SZ": "MultiString", "REG_SZ": "String"}.get(gl.get("value_type", "REG_MULTI_SZ"))
            if not kind:
                raise ValueError("invalid OpenGL registry type")
            value = root + member.replace("/", "\\")
            result["OpenGLDriverName" + suffix] = {"kind": kind, "value": [value] if kind == "MultiString" else value}
            for name, default in (("Version", 1), ("Flags", 1)):
                value = gl.get(name.lower(), default)
                if type(value) is not int or not 0 <= value <= 0xffffffff:
                    raise ValueError("invalid OpenGL registry DWORD")
                result["OpenGL" + name + suffix] = {"kind": "DWord", "value": value}
    return result


def render_inf(text: str, rows: list[dict], sources: dict | None = None) -> str:
    if "[Amdgpu_UserModeDriver]" in text:
        raise ValueError("INF already contains the DriverStore UMD set")
    sections: dict[str, list[str]] = {}
    source = []
    for row in rows:
        p = PurePosixPath(row["member"])
        folder = str(p.parent).replace("/", "\\")
        section = "Amdgpu_Files_" + folder.replace("\\", "_")
        sections.setdefault(section, []).append(p.name)
        source.append(f"{p.name} = 1,{folder}")
    # Every copy section's source and destination subdirectories must match.
    dest = []
    for section in sections:
        folder = section.removeprefix("Amdgpu_Files_").replace("_", "\\")
        dest.append(f"{section} = 13,{folder}")
    def append_section(name: str, lines: list[str]) -> None:
        nonlocal text
        pattern = r"(?im)^\[" + re.escape(name) + r"\][ \t]*$"
        text, count = re.subn(pattern, lambda m: m[0] + "\n" + "\n".join(lines), text)
        if count != 1:
            raise ValueError(f"missing or duplicate INF section: {name}")
    text = text.replace("\r\n", "\n")
    append_section("SourceDisksFiles", source)
    append_section("DestinationDirs", dest)
    pattern = r"(?im)^CopyFiles\s*=\s*Bc250_Files\s*;@PLAIN-ONLY\s*$"
    text, count = re.subn(pattern, "CopyFiles = Bc250_Files, " + ", ".join(sections), text)
    if count != 1:
        raise ValueError("unexpected base INF CopyFiles contract")
    pattern = r"(?im)^AddReg\s*=\s*Bc250_PackageIdentity\s*;@PLAIN-ONLY\s*$"
    text, count = re.subn(pattern, "AddReg = Bc250_PackageIdentity, Amdgpu_UserModeDriver", text)
    if count != 1:
        raise ValueError("unexpected base INF AddReg contract")
    # Clear only this adapter's previous graphics registration before AddReg,
    # including an architecture which this package no longer supplies.
    text = text.replace("AddReg = Bc250_PackageIdentity, Amdgpu_UserModeDriver",
                        "DelReg = Amdgpu_PreviousUserModeDriver\nAddReg = Bc250_PackageIdentity, Amdgpu_UserModeDriver")
    old_names = ["UserModeDriverName", "UserModeDriverNameWow", "VulkanDriverName", "VulkanDriverNameWow"]
    old_names += [name + suffix for suffix in ("", "Wow") for name in ("OpenGLDriverName", "OpenGLVersion", "OpenGLFlags")]
    lines = [text.rstrip(), "", "[Amdgpu_PreviousUserModeDriver]",
             *[f"HKR,,{name}" for name in old_names], "", "[Amdgpu_UserModeDriver]"]
    for name, row in registrations(rows, sources).items():
        if row["kind"] == "DWord":
            value = f'0x00010001,{row["value"]}'
        else:
            values = row["value"] if row["kind"] == "MultiString" else [row["value"]]
            value = ("0x00010000" if row["kind"] == "MultiString" else "0x00000000") + "," + ",".join('"' + v + '"' for v in values)
        lines.append(f"HKR,,{name},{value}")
    for name, files in sections.items():
        lines += ["", f"[{name}]", *files]
    return "\n".join(lines) + "\n"


def signing_safe_config(data: bytes, engine: Path, icd: Path) -> bytes:
    """Preserve measured caps and verify the original full-file pins first."""
    if len(data) not in (108, 132):
        raise ValueError('invalid D3D11 configuration size')
    fields = list(struct.unpack('<' + ('11I' if len(data) == 108 else '17I') + '32s32s', data))
    if fields[:4] != [0x4334314d, 1 if len(data) == 108 else 2, len(data), 0]:
        raise ValueError('expected an original v1/v2 D3D11 configuration')
    for path, expected in zip((engine, icd), fields[-2:]):
        if hashlib.sha256(path.read_bytes()).digest() != expected:
            raise ValueError(f'D3D11 configuration does not pin {path.name}')
    caps = fields[:11] + ([0] * 6 if len(data) == 108 else fields[11:17])
    caps[1:3] = [3, 132]
    return struct.pack('<17I32s32s', *caps, bytes.fromhex(image_digest(engine)), bytes.fromhex(image_digest(icd)))


def stage(package: Path, sources: dict) -> dict:
    package = package.resolve(strict=True)
    rows = members(sources)
    indexed = {r["path"]: r for r in sources["files"]}
    if len(indexed) != len(sources["files"]):
        raise ValueError("duplicate release source slot")
    expected = {r["source"] for r in rows}
    # No unused PE may silently stay in Program Files while the manifest claims
    # DriverStore isolation. HIP has its separate runtime contract and stays out.
    adapter_dirs = {"desktop", "d3d11", "d3d12", "vulkan", "wow64", "opengl", "mft"}
    actual = {p for p in indexed if len(PurePosixPath(p).parts) > 2 and PurePosixPath(p).parts[1] in adapter_dirs}
    if actual != expected:
        raise ValueError(f"UMD payload mismatch: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}")
    json_bytes = {}
    for row in rows:
        src = plain_path(package, row["source"])
        pin = indexed[row["source"]]
        if src.is_symlink() or not src.is_file() or src.stat().st_size != pin["size"] or hashlib.sha256(src.read_bytes()).hexdigest().lower() != pin["sha256"].lower():
            raise ValueError(f"source changed before DriverStore staging: {row['source']}")
        if plain_path(package, row["path"]).exists():
            raise ValueError(f"DriverStore target already exists: {row['path']}")
        if src.suffix == '.json':
            data = json.loads(src.read_text(encoding='utf-8-sig'))
            if not isinstance(data, dict) or not isinstance(data.get('ICD'), dict):
                raise ValueError('invalid Vulkan ICD manifest')
            data['ICD']['library_path'] = '.\\' + ('vulkan_radeon32.dll' if row['arch'] == 'x86' else 'vulkan_radeon.dll')
            json_bytes[row['path']] = (json.dumps(data, indent=2) + '\n').encode('utf-8')
    # All source slots have passed their release pins before a config is read.
    # A signature replacement must not change caps or invalidate the loader's pins.
    for row in rows:
        if row['source'].endswith('.config'):
            src = package / row['source']
            json_bytes[row['path']] = signing_safe_config(src.read_bytes(),
                src.with_name('amdgpu_wddm_dxvk.dll'), src.with_name('amdgpu_wddm_radv.dll'))
    inf = plain_path(package, PREFIX + "bc250kmd.inf")
    generated = render_inf(inf.read_text(encoding="utf-8-sig"), rows, sources)
    for row in rows:
        dst = package / row["path"]
        dst.parent.mkdir(parents=True, exist_ok=True)
        if row['path'] in json_bytes:
            dst.write_bytes(json_bytes[row['path']])
        else:
            shutil.copyfile(package / row["source"], dst)
        row["size"] = dst.stat().st_size
        row["sha256"] = hashlib.sha256(dst.read_bytes()).hexdigest().upper()
    inf.write_bytes(generated.replace("\n", "\r\n").encode("utf-8"))
    # Only the exact source files this operation has validated. Never recurse
    # into a caller's directory to remove unrecognized or foreign payloads.
    for row in rows:
        (package / row["source"]).unlink()
    return {"schema": SCHEMA, "root": "payload/kmd", "members": rows, "registration": registrations(rows, sources)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--sources", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    result = stage(args.package, json.loads(args.sources.read_text(encoding="utf-8-sig")))
    args.report.write_bytes((json.dumps(result, indent=2) + "\n").encode("utf-8"))
    print(f"DriverStore: {len(result['members'])} user-mode members, INF owns adapter registration")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
