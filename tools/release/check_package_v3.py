#!/usr/bin/env python3
"""The D3D11 configuration records of a staged release package (BD-166, v3).

The D3D11 shell refuses an engine or an ICD that is not the one its configuration record pins, before it
loads them: b15 shipped a new ICD with the old record and every D3D11 device failed with ERROR_CRC. The
record therefore has to be checked in the package that ships, not only where it was written.

Version 3 is exactly 132 bytes, little-endian '<17I32s32s': the magic 0x4334314d ('M14C'), the version 3,
the size 132, a reserved zero, then the thirteen capability DWORDs, then the engine and the ICD pin. Each
pin is the Windows catalog image identity of that file (image_identity.image_digest,
CryptCATAdminCalcHashFromFileHandle2 with SHA256), not a full-file SHA-256, so signing a file afterwards
leaves the pin valid while any change of the image itself breaks it. This is code identity and not signer
trust: the signature and catalog gates stay where they are.

The DriverStore renames the members of a lane (amdgpu_wddm_radv.dll becomes amdgpu_wddm_radv_d3d11.dll, and
the whole x86 lane takes a 32 suffix), so the files to check cannot be found by name. They come from
driver_package.members(release_flavour.select_sources(sources, flavour)), which maps each input slot to its
one member path. The mapping is followed in that direction only: a slot names its member, a member never
chooses a sibling. Both architectures are required, so a package without the x86 record fails instead of
passing unnoticed, and a record that the member set does not name fails as well.

With --workspace, each record is also rebuilt from the lane inputs that release-sources.json pins, through
driver_package.signing_safe_config, and compared byte for byte. That catches an edited capability as well as
an edited pin: the capabilities are measured once, on the hardware, and no later step may change them.
Without it the capabilities are not checked, and the gate says so. Asking for the rebuild and not being able
to do it is a failure, never a quiet pass: a lane input the workspace does not hold, a source row that is
absent or names no file, and bytes that are not the pinned ones each fail.

prestage_problems() is the same contract one step earlier, for a package whose lane files are still in place
and whose records are the v1/v2 originals that pin them by full-file SHA-256. Which records must exist comes
from the same source mapping, so a missing architecture fails there too, and the pins are read at the record
version's own offsets (v1 at 44 and 76, v2 at 68 and 100).

    check_package_v3.py --package <files dir> --sources <release-sources.json> [--flavour lab]
                        [--workspace <workspace root>]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path, PurePosixPath

import driver_package
from image_identity import image_digest
from release_flavour import select_sources

MAGIC = 0x4334314D
VERSION = 3
SIZE = 132
LAYOUT = '<17I32s32s'
CAPABILITIES = ('maximum', 'doubles', 'compute', 'logic_op', 'tile_based', 'pixel_precision',
                'other_precision', 'tiled_tier', 'typed_uav_loads', 'rovs', 'stencil_ref',
                'conservative_tier', 'vp_rt_index')
ENGINE, ICD = 17, 18
# One record per architecture and the two images it pins, by input slot of driver_package.
LANES = (
    ('x64', 'payload/d3d11/amdgpu_wddm_d3d11.config',
     'payload/d3d11/amdgpu_wddm_dxvk.dll', 'payload/d3d11/amdgpu_wddm_radv.dll'),
    ('x86', 'payload/wow64/d3d11/amdgpu_wddm_d3d11.config',
     'payload/wow64/d3d11/amdgpu_wddm_dxvk.dll', 'payload/wow64/d3d11/amdgpu_wddm_radv.dll'),
)
# Every name a D3D11 configuration record can have in a package, staged or not.
RECORD_NAMES = ('amdgpu_wddm_d3d11.config', 'amdgpu_wddm_d3d1132.config')


def fields(data: bytes) -> list:
    """The record's fields, or ValueError naming the first thing that is not a v3 record."""
    if len(data) != SIZE:
        raise ValueError(f'{len(data)} bytes, a v3 record is {SIZE}')
    values = list(struct.unpack(LAYOUT, data))
    if values[0] != MAGIC:
        raise ValueError(f'magic 0x{values[0]:08x}, a v3 record has 0x{MAGIC:08x}')
    if values[1] != VERSION:
        raise ValueError(f'version {values[1]}, the release ships version {VERSION}')
    if values[2] != SIZE:
        raise ValueError(f'the size field says {values[2]}, a v3 record says {SIZE}')
    if values[3] != 0:
        raise ValueError(f'the reserved field is {values[3]}, a v3 record has 0')
    return values


def capability_differences(got: bytes, want: bytes) -> list[str]:
    """Which capability DWORDs of two records differ, by name."""
    if len(got) != SIZE or len(want) != SIZE:
        return []
    a, b = struct.unpack(LAYOUT, got), struct.unpack(LAYOUT, want)
    return [f'{name} {x} instead of {y}' for name, x, y in zip(CAPABILITIES, a[4:ENGINE], b[4:ENGINE]) if x != y]


def _lane_inputs(pins: dict, slots: tuple, workspace: Path, arch: str, log) -> tuple[dict, list[str]]:
    """The pinned lane inputs of one record in the workspace, with their pins verified.

    A rebuild was asked for, so every missing piece is a problem and never a quiet return to the mode that
    does not check capabilities: an absent row, a row without a workspace source, a file the workspace does
    not hold and a file whose bytes are not the pinned ones all fail here. Only the absence of --workspace
    turns the capability comparison off, and the caller says so in the log."""
    inputs, problems = {}, []
    for slot in slots:
        pin = pins.get(slot)
        if pin is None:
            problems.append(f'{slot}: release-sources.json has no row for this lane input, so the {arch} '
                            f'record cannot be rebuilt')
            continue
        if not pin.get('source'):
            problems.append(f'{slot}: the release-sources.json row names no workspace source, so the {arch} '
                            f'record cannot be rebuilt')
            continue
        path = workspace / PurePosixPath(pin['source'])
        if not path.is_file():
            problems.append(f'{pin["source"]}: the workspace {workspace} does not hold this pinned lane '
                            f'input, so the {arch} record cannot be rebuilt')
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest.lower() != str(pin.get('sha256', '')).lower():
            problems.append(f'{pin["source"]}: the workspace file is {digest[:8].upper()}, '
                            f'release-sources.json pins {str(pin.get("sha256"))[:8].upper()}')
            continue
        inputs[slot] = path
    return inputs, problems


def _rebuilt(package: Path, rows: dict, pins: dict, arch: str, slots: tuple, workspace, log) -> list[str]:
    """The record as the pinned lane inputs produce it, against the record in the package.

    The comparison is byte for byte, pins included: driver_package.stage refuses a lane input whose bytes are
    not the ones release-sources.json pins, so a release that stamps a version resource into the engine or
    the ICD has to stamp before it writes those pins. What the rebuild adds to the pin check above is the
    thirteen capability DWORDs, which are measured once, on the hardware, and which no later step may
    change."""
    if workspace is None:
        log(f'  {arch}: capabilities not checked, because --workspace was not given')
        return []
    cfg_slot, engine_slot, icd_slot = slots
    inputs, problems = _lane_inputs(pins, slots, Path(workspace), arch, log)
    if problems:
        return problems
    try:
        want = driver_package.signing_safe_config(inputs[cfg_slot].read_bytes(),
                                                  inputs[engine_slot], inputs[icd_slot])
    except (OSError, ValueError) as exc:
        return [f'{pins[cfg_slot]["source"]}: {exc}']
    got = (package / rows[cfg_slot]['path']).read_bytes()
    if got == want:
        log(f'  {arch}: the record is the one its pinned lane inputs produce')
        return []
    differences = capability_differences(got, want)
    detail = '; '.join(differences) if differences else 'the record differs in its header or its pins'
    return [f'{rows[cfg_slot]["path"]}: not the record of its pinned lane inputs ({detail})']


def problems(package, sources: dict, flavour: str = 'release', workspace=None, log=print) -> list[str]:
    """Everything wrong with the D3D11 configuration records of a staged package, in one flavour."""
    package = Path(package)
    selected = select_sources(sources, flavour)
    rows = {row['source']: row for row in driver_package.members(selected)}
    pins = {row['path']: row for row in selected['files']}
    found = []
    claimed = {rows[slot]['path'] for _, *slots in LANES for slot in slots if slot in rows}
    for arch, *slots in LANES:
        cfg_slot = slots[0]
        absent = [slot for slot in slots if slot not in rows]
        if absent:
            found.append(f'{arch}: the DriverStore member set names no ' + ', '.join(absent))
            continue
        paths, missing = {}, []
        for slot in slots:
            paths[slot] = driver_package.plain_path(package, rows[slot]['path'])
            if not paths[slot].is_file():
                missing.append(rows[slot]['path'])
        if missing:
            found.append(f'{arch}: the package has no ' + ', '.join(missing))
            continue
        try:
            record = fields(paths[cfg_slot].read_bytes())
        except ValueError as exc:
            found.append(f'{rows[cfg_slot]["path"]}: {exc}')
            continue
        for slot, at, which in ((slots[1], ENGINE, 'engine'), (slots[2], ICD, 'ICD')):
            want = bytes.fromhex(image_digest(paths[slot]))
            if record[at] != want:
                found.append(f'{rows[cfg_slot]["path"]}: pins the {which} '
                             f'{record[at].hex().upper()[:8]}, but {rows[slot]["path"]} has the image '
                             f'identity {want.hex().upper()[:8]}')
        found += _rebuilt(package, rows, pins, arch, tuple(slots), workspace, log)
        log(f'  {arch}: {rows[cfg_slot]["path"]} pins its engine and its ICD by image identity')
    found += _strays(package, claimed)
    return found


def _strays(package: Path, claimed: set) -> list[str]:
    """Every D3D11 configuration record in the package that the member set does not name."""
    out = []
    for path in sorted(package.rglob('*')):
        if path.is_file() and path.name.lower() in RECORD_NAMES:
            member = path.relative_to(package).as_posix()
            if member not in claimed:
                out.append(f'{member}: a D3D11 configuration record that the member set does not name')
    return out


def original_pins(data: bytes) -> tuple[str, str]:
    """The engine and ICD pin of an original v1 or v2 record, at that version's own offsets.

    A v1 record is 108 bytes with eleven DWORDs and its pins at 44 and 76; a v2 record is 132 bytes with
    seventeen and its pins at 68 and 100. Reading v2's offsets in both rejects a well-formed v1 pair, so the
    layout is taken from the size and the whole header is validated, exactly as
    driver_package.signing_safe_config does it for the staging step."""
    if len(data) not in (108, 132):
        raise ValueError(f'{len(data)} bytes, an original record is 108 (v1) or 132 (v2)')
    version = 1 if len(data) == 108 else 2
    values = struct.unpack('<' + ('11I' if version == 1 else '17I') + '32s32s', data)
    if values[0] != MAGIC:
        raise ValueError(f'magic 0x{values[0]:08x}, a record has 0x{MAGIC:08x}')
    if values[1] != version:
        raise ValueError(f'version {values[1]}, a {len(data)}-byte record is version {version}')
    if values[2] != len(data):
        raise ValueError(f'the size field says {values[2]}, the record is {len(data)} bytes')
    if values[3] != 0:
        raise ValueError(f'the reserved field is {values[3]}, a record has 0')
    return values[-2].hex().upper(), values[-1].hex().upper()


def prestage_problems(package, sources: dict, flavour: str = 'release', log=print) -> list[str]:
    """The same records before staging: beside their lane files, pinning them by full-file SHA-256.

    Which records must exist comes from the selected source mapping, not from what the package happens to
    carry, so a package with no x86 record or with no record at all fails here as it does after staging. The
    header is validated in full and the pins are read at the record version's own offsets."""
    package = Path(package)
    selected = select_sources(sources, flavour)
    rows = {row['source']: row for row in driver_package.members(selected)}
    found, claimed = [], set()
    for arch, *slots in LANES:
        cfg_slot, engine_slot, icd_slot = slots
        absent = [slot for slot in slots if slot not in rows]
        if absent:
            found.append(f'{arch}: the {flavour} source mapping names no ' + ', '.join(absent))
            continue
        paths = {slot: driver_package.plain_path(package, slot) for slot in slots}
        missing = [slot for slot in slots if not paths[slot].is_file()]
        if missing:
            found.append(f'{arch}: the package has no ' + ', '.join(missing))
            continue
        claimed.add(paths[cfg_slot].relative_to(package).as_posix())
        try:
            record = paths[cfg_slot].read_bytes()
            pins = original_pins(record)
        except (OSError, ValueError) as exc:
            found.append(f'{cfg_slot}: {exc}')
            continue
        for slot, pin, which in ((engine_slot, pins[0], 'engine'), (icd_slot, pins[1], 'ICD')):
            have = hashlib.sha256(paths[slot].read_bytes()).hexdigest().upper()
            if have != pin:
                found.append(f'{cfg_slot}: pins the {which} {pin[:8]}, but {slot} is {have[:8]}')
        log(f'  {arch}: {cfg_slot} pins its engine and its ICD by full-file SHA-256')
    found += _strays(package, claimed)
    return found


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--package', required=True, help="the package's files directory")
    ap.add_argument('--sources', required=True, help='release-sources.json')
    ap.add_argument('--flavour', choices=('release', 'lab'), default='release')
    ap.add_argument('--workspace', help='the workspace root that holds the pinned lane inputs')
    args = ap.parse_args(argv)
    try:
        sources = json.loads(Path(args.sources).read_text(encoding='utf-8-sig'))
        found = problems(args.package, sources, args.flavour, args.workspace)
    except (OSError, ValueError) as exc:
        print(f'check_package_v3: {exc}', file=sys.stderr)
        return 1
    for line in found:
        print('FAIL ' + line)
    print('D3D11 configuration records: ' + ('FAIL' if found else 'PASS'))
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
