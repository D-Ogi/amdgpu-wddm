#!/usr/bin/env python3
"""The release side of Nagi self-update, stage 1 (docs/design/self-update.md).

Three commands, each on an unpacked package root (the files\\ folder that manifest.json describes):

  identity --package <files>
      The update identity of each component kind, as JSON on stdout. build-release.ps1 writes it into manifest.json
      as "update_components", so that every installed release names the content it installed.
  packs --package <files> --release <version> --out <dir>
      The apps pack nagi-apps-<release>.zip (payload/control/**), deterministic: sorted entries, one constant
      timestamp, deflate. Prints the pack row as JSON.
  manifest --package <files> --release-zip <zip> --apps-pack <zip> --min-installed <version> --out <file>
      The update manifest nagi-update.json (schema nagi.update/1). The signature is a separate step
      (build-release.ps1, signtool /p7), and installer/nagi-update.ps1 verifies it.

The identity of a file is its Authenticode digest when it is a PE image (the digest leaves the signature out, so a
signature of another key or a new time stamp is not a change), and its SHA-256 otherwise. The identity of a
component is the SHA-256 of its sorted "path<TAB>digest" lines. Files that each release changes by design are not
part of any identity: the INF and the catalog (BD-150 stamps the release into the INF), the certificate, the
documents, the licence texts and the compatibility record.
"""
import argparse
import hashlib
import io
import json
import re
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from provenance import authenticode_digest  # noqa: E402

SCHEMA = "nagi.update/1"
MANIFEST_ASSET = "nagi-update.json"
SIGNATURE_ASSET = "nagi-update.json.p7s"
STATE_SCHEMA = 2                      # the state catalog header (state-package.ps1): "AMDST02\0", schema 2
ZIP_TIME = (2026, 1, 1, 0, 0, 0)      # one constant timestamp: the same files give the same pack bytes
RELEASE = re.compile(r"^\d{1,6}\.\d{1,6}\.\d{1,6}\.\d{1,6}(-tester\.\d{1,6}(v([2-9]|[1-9]\d{1,2}))?)?$")
PE_SUFFIXES = (".exe", ".dll", ".sys")
APPS_PREFIX = "payload/control/"


def kind_of(path):
    """The component kind of one package path, or None when the path is in no identity."""
    p = path.replace("\\", "/")
    if p.startswith(APPS_PREFIX):
        return "apps"
    if "/" not in p and (p.lower().endswith(".md") or p in ("NOTICE", "compatibility.json", "manifest.json")):
        return None
    if p.startswith("licenses/") or p.startswith("payload/cert/"):
        return None
    if p.startswith("payload/kmd/") and p.lower().endswith((".inf", ".cat")):
        return None
    return "driver"


def file_digest(path):
    if path.suffix.lower() in PE_SUFFIXES:
        return authenticode_digest(path)
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def content_id(rows):
    """rows: (path, digest) pairs. The id of their sorted set; the empty set has the id of the empty input."""
    h = hashlib.sha256()
    for path, digest in sorted(rows, key=lambda r: r[0]):
        h.update(f"{path}\t{digest}\n".encode("utf-8"))
    return h.hexdigest().upper()


def read_manifest(package):
    return json.loads((Path(package) / "manifest.json").read_text(encoding="utf-8-sig"))


def state_identity(package):
    """(schema, catalog id) of payload/kmd/amdgpu-state.bin, as Test-DriverStatePackage reads them."""
    data = (Path(package) / "payload" / "kmd" / "amdgpu-state.bin").read_bytes()
    if len(data) < 80 or data[:8] != b"AMDST02\0":
        raise ValueError("amdgpu-state.bin: invalid state catalog header")
    schema = int.from_bytes(data[8:12], "little")
    catalog = data[16:80].decode("ascii")
    if not re.fullmatch(r"[0-9a-f]{64}", catalog):
        raise ValueError("amdgpu-state.bin: invalid state catalog identity")
    return schema, catalog


def identity(package, files=None):
    """The update identity of each kind. files: the package paths (default: the 'files' list of manifest.json)."""
    package = Path(package)
    if files is None and (package / "manifest.json").is_file():
        files = [f["path"] for f in read_manifest(package)["files"]]
    if files is None:
        # build-release.ps1 asks before it writes manifest.json: every file under the package root.
        files = [p.relative_to(package).as_posix() for p in sorted(package.rglob("*")) if p.is_file()]
    rows = {"driver": [], "apps": []}
    for path in files:
        kind = kind_of(path)
        if kind:
            rows[kind].append((path.replace("\\", "/"), file_digest(package / path)))
    if not rows["driver"]:
        raise ValueError("the package has no driver file")
    out = {}
    for kind, r in rows.items():
        out[kind] = {"content_id": content_id(r), "files": len(r)}
    if (package / "payload" / "kmd" / "amdgpu-state.bin").is_file():
        out["driver"]["state_schema"], out["driver"]["catalog_id"] = state_identity(package)
    return out


def apps_files(package, files):
    """The files of the apps pack: (pack path, package path), sorted by pack path."""
    out = []
    for path in files:
        p = path.replace("\\", "/")
        if p.startswith(APPS_PREFIX):
            out.append((p[len(APPS_PREFIX):], p))
    return sorted(out)


def write_pack(package, entries, target):
    """A deterministic zip of (pack path, package path) entries. Returns the bytes."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, source in entries:
            if name.startswith("/") or ".." in name.split("/") or "\\" in name:
                raise ValueError(f"pack path {name} is not a plain relative path")
            info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, (Path(package) / source).read_bytes())
    data = buf.getvalue()
    Path(target).write_bytes(data)
    return data


def make_packs(package, release, out):
    if not RELEASE.fullmatch(release):
        raise ValueError(f"release {release} is not of the tag grammar")
    m = read_manifest(package)
    entries = apps_files(package, [f["path"] for f in m["files"]])
    if not entries:
        raise ValueError("the package has no payload/control file")
    Path(out).mkdir(parents=True, exist_ok=True)
    target = Path(out) / f"nagi-apps-{release}.zip"
    data = write_pack(package, entries, target)
    return {"asset": target.name, "path": str(target), "sha256": hashlib.sha256(data).hexdigest().upper(), "size": len(data),
            "files": [{"path": name, "sha256": sha256_file(Path(package) / source), "size": (Path(package) / source).stat().st_size}
                      for name, source in entries]}


def make_manifest(package, release_zip, apps_pack, min_installed, built_utc=None):
    m = read_manifest(package)
    release = m["release"]
    for v, what in ((release, "release"), (min_installed, "min_installed")):
        if not RELEASE.fullmatch(v or ""):
            raise ValueError(f"{what} {v} is not of the tag grammar")
    ids = m.get("update_components") or identity(package)
    schema, catalog = state_identity(package)
    kmd_abi = m["kmd_abi"]
    kmd = next((c for c in m.get("components", []) if c.get("package_path") == "payload/kmd/bc250kmd.sys"), None)
    zip_path, pack_path = Path(release_zip), Path(apps_pack)
    expected_zip = f"amdgpu-wddm-{release}.zip" if "-tester." not in release else f"amdgpu-wddm-tester-{release}.zip"
    if zip_path.name != expected_zip:
        raise ValueError(f"the driver asset is {zip_path.name}, the release asset name is {expected_zip}")
    if pack_path.name != f"nagi-apps-{release}.zip":
        raise ValueError(f"the apps pack is {pack_path.name}, not nagi-apps-{release}.zip")
    with zipfile.ZipFile(pack_path) as z:
        pack_files = sorted(z.namelist())
        pack_rows = [{"path": n, "sha256": hashlib.sha256(z.read(n)).hexdigest().upper(), "size": z.getinfo(n).file_size} for n in pack_files]
    expected = [name for name, _ in apps_files(package, [f["path"] for f in m["files"]])]
    if pack_files != expected:
        raise ValueError("the apps pack does not hold exactly the payload/control files of the package")
    return {
        "schema": SCHEMA,
        "product": "Nagi",
        "release": release,
        "flavour": m.get("flavour"),
        "built_utc": built_utc or m.get("built_utc"),
        "min_installed": min_installed,
        "components": [
            {"name": "driver", "kind": "driver", "version": release, "asset": zip_path.name,
             "sha256": sha256_file(zip_path), "size": zip_path.stat().st_size, "restart": True,
             "content_id": ids["driver"]["content_id"],
             "driver": {"kmd_version": m.get("kmd_version"), "kmd_abi": kmd_abi,
                        "kmd_sha256": kmd.get("sha256") if kmd else None,
                        "state_schema": schema, "catalog_id": catalog,
                        "package_manifest_sha256": sha256_file(Path(package) / "manifest.json")}},
            {"name": "apps", "kind": "apps", "version": release, "asset": pack_path.name,
             "sha256": sha256_file(pack_path), "size": pack_path.stat().st_size, "restart": False,
             "content_id": ids["apps"]["content_id"],
             "apps": {"kmd_abis": [kmd_abi], "state_schemas": [schema], "files": pack_rows}},
        ],
    }


def dump(obj):
    return json.dumps(obj, indent=1, ensure_ascii=False) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)
    i = sub.add_parser("identity")
    i.add_argument("--package", required=True)
    p = sub.add_parser("packs")
    p.add_argument("--package", required=True)
    p.add_argument("--release", required=True)
    p.add_argument("--out", required=True)
    m = sub.add_parser("manifest")
    m.add_argument("--package", required=True)
    m.add_argument("--release-zip", required=True)
    m.add_argument("--apps-pack", required=True)
    m.add_argument("--min-installed", required=True)
    m.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    try:
        if a.command == "identity":
            sys.stdout.write(dump(identity(a.package)))
        elif a.command == "packs":
            sys.stdout.write(dump(make_packs(a.package, a.release, a.out)))
        else:
            text = dump(make_manifest(a.package, a.release_zip, a.apps_pack, a.min_installed))
            Path(a.out).write_bytes(text.encode("utf-8"))
            sys.stdout.write(f"{a.out} SHA256 {hashlib.sha256(text.encode('utf-8')).hexdigest().upper()}\n")
    except (ValueError, KeyError, OSError) as e:
        sys.stderr.write(f"nagi_update: {e}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
