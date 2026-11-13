#!/usr/bin/env python3
"""Development file swap: new user-mode driver files into the active driver package of the lab, and back.

A fix reaches the lab by a rebuild of the changed components and a swap of their files, with no release set and
no package. The swap changes user-mode files of the active bc250kmd package in the DriverStore and nothing else:
no pnputil, no device restart, no OS restart, no KMD change. A held file is renamed aside, so a running process
keeps its old image and a new process loads the new file.

    dev_swap.py plan    --file x64:dxvk=<path> [--file ...]     the checks and the records, no lab call
    dev_swap.py swap    --file x64:dxvk=<path> [--file ...] [--package bc250kmd.inf_<id>]
    dev_swap.py status
    dev_swap.py restore [--drop-stale]

--file takes <arch>:<role>=<path>. The arch is x64 or x86 (x86 is the WoW64 lane). The roles are d3d11-shell,
dxvk, d3d12-shell, vkd3d, d3d11-icd and d3d12-icd; 'icd' is both ICD roles, and 'd3d11' is the D3D11 shell. Every
role without a --file stays as it is installed. The D3D11 record (d3d11-record) is never an input: when the engine
or the ICD of an architecture changes, the tool writes that record from the installed record, with the image
digests of the engine and the ICD that are active after the swap. Before it writes, the installed record must pin
the installed engine and ICD (positive control); the lab script checks the same again before its first write.

The state lives on the lab (C:\\BC250\\dev-swap\\state): the installed bytes of every role that a swap changed,
with their SHA-256. A second swap on top of a swap is one step; restore puts the originals back and proves each one
by its SHA-256, from any host session and after a lab restart.

Each step prints one verdict line: '<STEP> PASS|FAIL|REFUSED <reason>'. Lab addresses are masked. Exit codes:
0 PASS, 1 FAIL, 2 REFUSED.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
for sub in ('tools/release', 'tools/packagecheck', 'tools/build', 'tools/win'):
    sys.path.insert(0, str(REPO / sub))

import check_package_v3  # noqa: E402  the v3 record layout
import driver_package  # noqa: E402  the DriverStore member names
import packagecheck  # noqa: E402  the PE reader
import versionstamp  # noqa: E402  the version resource reader and the record pin writer
from image_identity import image_digest  # noqa: E402

LAB_SCRIPT = HERE / 'lab' / 'dev-swap-lab.ps1'
LAB_BASE = 'C:\\BC250\\dev-swap'
MACHINES = {'x64': 0x8664, 'x86': 0x14C}
# The role, its input slot of driver_package.NATIVE (the x86 slot is the same below wow64/), and the exports that
# identify it. A shell and the router export the same entry point, so OriginalFilename decides where a version
# resource exists.
ROLES = (
    ('d3d11-shell', 'd3d11/amdgpu_wddm_d3d11.dll', ('OpenAdapter10_2',)),
    ('dxvk', 'd3d11/amdgpu_wddm_dxvk.dll', ('Bc250DxvkEngineGetFuncs',)),
    ('d3d11-record', 'd3d11/amdgpu_wddm_d3d11.config', ()),
    ('d3d11-icd', 'd3d11/amdgpu_wddm_radv.dll', ('vk_icdGetInstanceProcAddr', 'vkGetInstanceProcAddr')),
    ('d3d12-shell', 'd3d12/amdgpu_wddm_d3d12.dll', ('OpenAdapter12',)),
    ('vkd3d', 'd3d12/amdgpu_wddm_vkd3d.dll', ('Bc250Vkd3dEngineGetFuncs',)),
    ('d3d12-icd', 'd3d12/amdgpu_wddm_radv.dll', ('vk_icdGetInstanceProcAddr', 'vkGetInstanceProcAddr')),
)
ALIASES = {'icd': ('d3d11-icd', 'd3d12-icd'), 'd3d11': ('d3d11-shell',), 'd3d12': ('d3d12-shell',),
           'engine11': ('dxvk',), 'engine12': ('vkd3d',)}
RECORD = 'd3d11-record'
TIMEOUT = {'status': 240, 'swap': 300, 'restore': 300, 'push': 300}
IPV4 = re.compile(r'\b\d{1,3}(\.\d{1,3}){3}\b')
# The engines return their ABI version from the GetFuncs export at run time; the binaries carry it in no resource
# or section that can be read without loading them (driver/umd/dxvk/engine-modules.cpp, d3d12 engine-abi.json).
ABI_NOTE = ('ABI: neither the shells nor the engines carry the ABI version as data; the export is checked here, '
            'and the shell checks the version at the first device open')


def mask(text: str) -> str:
    return IPV4.sub('<ip>', text)


class Refusal(Exception):
    """One clear line: the reason the step does nothing."""


@dataclass(frozen=True)
class Role:
    key: str
    arch: str
    role: str
    name: str        # the DriverStore member name
    original: str    # the OriginalFilename of the build
    exports: tuple
    pe: bool


def role_table() -> list[Role]:
    """Every role of both architectures, with the member names of driver_package (one source)."""
    table = []
    for arch, members, prefix in (('x64', dict(driver_package.NATIVE), ''), ('x86', dict(driver_package.WOW), 'wow64/')):
        for role, slot, exports in ROLES:
            member = members[prefix + slot]
            table.append(Role(f'{arch}:{role}', arch, role, PurePosixPath(member).name,
                              PurePosixPath(slot).name, exports, role != RECORD))
    return table


def roles_b64(table: list[Role]) -> str:
    rows = [{'key': r.key, 'arch': r.arch, 'role': r.role, 'name': r.name, 'pe': r.pe} for r in table]
    return base64.b64encode(json.dumps(rows, separators=(',', ':')).encode('utf-8')).decode('ascii')


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def original_filename(data: bytes):
    """The OriginalFilename of the version resource, or None when the image has none."""
    try:
        image = versionstamp.Image(data)
        for kind, _, _, _, blob in versionstamp.read_resources(image):
            if kind == 16:
                return versionstamp.parse_version_resource(blob)[3].get('OriginalFilename')
    except (versionstamp.StampError, ValueError, IndexError) as exc:
        raise Refusal(f'the version resource cannot be read ({exc})') from None
    return None


@dataclass
class Input:
    role: Role
    path: Path
    sha256: str = ''
    digest: str = ''
    notes: list = field(default_factory=list)


def check_input(role: Role, path: Path) -> Input:
    """The file is a PE image of the role's architecture, with the role's export and OriginalFilename."""
    where = f'{role.key} {path.name}'
    if role.role == RECORD:
        raise Refusal(f'{role.key}: the record is written by the tool from the engine and the ICD, never an input')
    if not path.is_file():
        raise Refusal(f'{where}: no such file')
    image = packagecheck.PeImage(path)
    if not image.valid:
        raise Refusal(f'{where}: not a PE image ({"; ".join(image.problems) or "no PE header"})')
    if image.machine != MACHINES[role.arch]:
        other = {v: k for k, v in MACHINES.items()}.get(image.machine, f'machine 0x{image.machine:04x}')
        raise Refusal(f'{where}: the file is {other}, the role is {role.arch}')
    exports = set(image.exports or ())
    if not exports & set(role.exports):
        owners = sorted({r for r, _, ex in ROLES if exports & set(ex)})
        found = f'it exports the entry point of {", ".join(owners)}' if owners else 'it has none of the role exports'
        raise Refusal(f'{where}: wrong role, {found}; {role.role} needs {" or ".join(role.exports)}')
    item = Input(role, path)
    name = original_filename(image.data)
    if name is None:
        item.notes.append('no version resource: identified by its export only')
    elif name.lower() != role.original.lower():
        raise Refusal(f'{where}: wrong role, its OriginalFilename is {name}, {role.role} needs {role.original}')
    item.sha256 = sha256(path)
    item.digest = image_digest(path)
    return item


def parse_files(values: list[str], table: list[Role]) -> dict[str, Input]:
    by_key = {r.key: r for r in table}
    out: dict[str, Input] = {}
    for value in values or ():
        m = re.fullmatch(r'(x64|x86):([a-z0-9-]+)=(.+)', value.strip())
        if not m:
            raise Refusal(f'--file {value}: the form is <x64|x86>:<role>=<path>')
        arch, role, path = m.groups()
        for name in ALIASES.get(role, (role,)):
            key = f'{arch}:{name}'
            if key not in by_key:
                raise Refusal(f'--file {value}: no role {role} (roles: {", ".join(r for r, _, _ in ROLES)}, icd)')
            if key in out:
                raise Refusal(f'--file {value}: {key} is given twice')
            out[key] = check_input(by_key[key], Path(path))
    return out


def record_plan(new: dict[str, Input]) -> dict[str, dict]:
    """For each architecture whose engine or ICD changes: which pin comes from where."""
    plan = {}
    for arch in MACHINES:
        engine, icd = new.get(f'{arch}:dxvk'), new.get(f'{arch}:d3d11-icd')
        if engine or icd:
            plan[arch] = {'engine': engine.digest if engine else None, 'icd': icd.digest if icd else None}
    return plan


def write_records(new: dict[str, Input], status: dict) -> dict[str, bytes]:
    """The new D3D11 records, from the installed record and the image digests after the swap.

    The positive control comes first: the installed record must be a v3 record that pins the installed engine and
    the installed ICD. Only then are the pins changed, with versionstamp.repoint_m14c (the one pin writer); the
    capability bytes stay as they are."""
    records = {}
    roles = status.get('roles') or {}
    for arch, pins in record_plan(new).items():
        rec, eng, icd = (roles.get(f'{arch}:{r}') or {} for r in (RECORD, 'dxvk', 'd3d11-icd'))
        if not rec.get('hex'):
            raise Refusal(f'{arch}:{RECORD}: the lab sent no installed record')
        data = bytes.fromhex(rec['hex'])
        try:
            check_package_v3.fields(data)
        except ValueError as exc:
            raise Refusal(f'{arch}:{RECORD} installed: {exc}') from None
        have_e, have_i = versionstamp.m14c_pins(data)
        if have_e != str(eng.get('digest', '')).upper():
            raise Refusal(f'{arch}:{RECORD} installed pins engine {have_e[:8]}, the installed engine is '
                          f'{str(eng.get("digest"))[:8]} (positive control)')
        if have_i != str(icd.get('digest', '')).upper():
            raise Refusal(f'{arch}:{RECORD} installed pins ICD {have_i[:8]}, the installed ICD is '
                          f'{str(icd.get("digest"))[:8]} (positive control)')
        out = versionstamp.repoint_m14c(data, pins['engine'] or have_e, pins['icd'] or have_i)
        check_package_v3.fields(out)
        records[arch] = out
    return records


# ------------------------------------------------------------------ the lab

class Lab:
    """The lab through tools/win/target.py: a script run with a bound, and a push."""

    def __init__(self):
        if not os.environ.get('BC250_ROOT'):
            raise Refusal('BC250_ROOT is not set: the lab is reached only through tools/win/target.py with BC250_ROOT')
        import target
        self.target = target
        self.t = target.Target()

    def run(self, args: list[str], timeout: int):
        try:
            done = self.t.run_script(str(LAB_SCRIPT), args, timeout=timeout, remote_dir=LAB_BASE + '\\bin')
        except (self.target.TargetError, subprocess.TimeoutExpired) as exc:
            return 1, '', f'the lab call ended: {exc}'
        return done.returncode, done.stdout or '', done.stderr or ''

    def push(self, paths: list[Path], remote_dir: str):
        try:
            self.t.push([str(p) for p in paths], remote_dir, timeout=TIMEOUT['push'])
        except (self.target.TargetError, subprocess.TimeoutExpired) as exc:
            raise Refusal(f'the push to the lab failed: {exc}') from None


@dataclass
class LabResult:
    code: int
    lines: list
    facts: dict
    verdict: str
    reason: str


def call(lab, action: str, table, base: str, extra=()) -> LabResult:
    args = ['-Action', action, '-RolesB64', roles_b64(table), '-Base', base, *extra]
    code, out, err = lab.run(args, TIMEOUT[action])
    lines = [mask(l.rstrip('\r')) for l in out.splitlines()]
    facts, verdict, reason = {}, 'FAIL', 'the lab script gave no RESULT line'
    for line in lines:
        if line.startswith('DEVSWAP-JSON '):
            try:
                facts = json.loads(line[len('DEVSWAP-JSON '):])
            except ValueError as exc:
                reason = f'the lab facts are not JSON ({exc})'
        elif line.startswith('RESULT '):
            parts = line.split(' ', 2)
            verdict, reason = parts[1], parts[2] if len(parts) > 2 else ''
    if verdict == 'FAIL' and err.strip() and 'RESULT' not in out:
        reason += f'; {mask(err.strip().splitlines()[-1])}'
    return LabResult(code, lines, facts, verdict, reason)


def echo(lines):
    for line in lines:
        if not line.startswith('DEVSWAP-JSON '):
            print(f'  lab: {line}')


def role_lines(facts: dict, table) -> list[str]:
    out = []
    state = facts.get('state') or {}
    for r in table:
        f = (facts.get('roles') or {}).get(r.key) or {}
        if not f.get('found'):
            out.append(f'{r.key:18} absent ({f.get("problem", "not read")})')
            continue
        kept = f.get('kept_sha256')
        word = 'swapped' if kept and kept != f['sha256'] else 'installed'
        extra = f' original {kept[:8]}' if word == 'swapped' else ''
        if r.role == RECORD:
            pins = f.get('pins') or ['?', '?']
            eng = ((facts['roles'].get(f'{r.arch}:dxvk') or {}).get('digest') or '')
            icd = ((facts['roles'].get(f'{r.arch}:d3d11-icd') or {}).get('digest') or '')
            match = 'matches' if pins[0] == eng and pins[1] == icd else 'DOES NOT MATCH'
            extra += f' pins engine {pins[0][:8]} ICD {pins[1][:8]}, {match} the active engine and ICD'
        out.append(f'{r.key:18} {word:9} {f["sha256"][:8]}{extra}  {f["path"]}')
    if state:
        out.append(f'state: package {state.get("package")}, since {state.get("created")}, roles '
                   f'{", ".join(state.get("roles") or [])}{" (STALE: another package is active)" if state.get("stale") else ""}')
    else:
        out.append('state: none (everything is as installed)')
    return out


# ------------------------------------------------------------------ the steps

def verdict(step: str, word: str, reason: str) -> int:
    print(f'{step.upper()} {word} {mask(reason)}')
    return {'PASS': 0, 'REFUSED': 2}.get(word, 1)


def step_plan(args, table) -> int:
    new = parse_files(args.file, table)
    if not new:
        raise Refusal('no --file: nothing to swap')
    for key, item in new.items():
        print(f'{key:18} new {item.sha256[:8]} image {item.digest[:8]}  {item.path}')
        for note in item.notes:
            print(f'{"":18} note: {note}')
    for arch, pins in record_plan(new).items():
        print(f'{arch}:{RECORD:13} written at the swap: engine pin '
              f'{pins["engine"][:8] + " (new)" if pins["engine"] else "of the installed engine"}, ICD pin '
              f'{pins["icd"][:8] + " (new)" if pins["icd"] else "of the installed ICD"}; '
              f'capabilities of the installed record')
    print(ABI_NOTE)
    stays = [r.key for r in table if r.key not in new and not (r.role == RECORD and r.arch in record_plan(new))]
    print(f'stays installed: {", ".join(stays)}')
    return verdict('plan', 'PASS', f'{len(new)} files checked, {len(record_plan(new))} records to write, no lab call')


def lab_refusals(facts: dict, args):
    if not facts:
        raise Refusal('the lab sent no facts')
    if not facts.get('package'):
        raise Refusal(f'the active package is not known: {facts.get("package_problem")}')
    if args.package and facts['package'].lower() != args.package.lower():
        raise Refusal(f'the lab runs package {facts["package"]}, not the expected {args.package}')
    guard = facts.get('guard') or {}
    if guard.get('service') == 'Running':
        raise Refusal('the HLK campaign guard service runs (an HLK campaign is armed)')
    if guard.get('trip'):
        raise Refusal(f'the HLK campaign guard has a trip ({guard["trip"]})')
    state = facts.get('state') or {}
    if state.get('stale'):
        raise Refusal(f'the lab state belongs to package {state.get("package")}, the active package is '
                      f'{facts["package"]} (restore --drop-stale deletes it)')


def step_swap(args, table, lab, work: Path) -> int:
    new = parse_files(args.file, table)
    if not new:
        raise Refusal('no --file: nothing to swap')
    before = call(lab, 'status', table, args.lab_base)
    if before.verdict != 'PASS':
        echo(before.lines)
        return verdict('swap', 'FAIL', f'status before the swap: {before.reason}')
    lab_refusals(before.facts, args)
    records = write_records(new, before.facts)
    staged = Path(tempfile.mkdtemp(prefix='stage-', dir=work))
    try:
        rows = []
        by_key = {r.key: r for r in table}
        for key, item in new.items():
            shutil.copyfile(item.path, staged / by_key[key].name)
            rows.append({'key': key, 'file': by_key[key].name, 'sha256': item.sha256})
        for arch, data in records.items():
            r = by_key[f'{arch}:{RECORD}']
            (staged / r.name).write_bytes(data)
            rows.append({'key': r.key, 'file': r.name, 'sha256': hashlib.sha256(data).hexdigest().upper()})
            pins = versionstamp.m14c_pins(data)
            print(f'{r.key:18} new {rows[-1]["sha256"][:8]} pins engine {pins[0][:8]} ICD {pins[1][:8]}')
        (staged / 'manifest.json').write_text(json.dumps({'package': before.facts['package'], 'files': rows},
                                                         indent=1), encoding='utf-8')
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        in_dir = f'{args.lab_base}\\in-{stamp}'
        lab.push(sorted(staged.iterdir()), in_dir)
    finally:
        shutil.rmtree(staged, ignore_errors=True)
    done = call(lab, 'swap', table, args.lab_base, ['-InDir', in_dir])
    echo(done.lines)
    return verdict('swap', done.verdict, done.reason)


def step_status(args, table, lab) -> int:
    done = call(lab, 'status', table, args.lab_base)
    if done.verdict != 'PASS':
        echo(done.lines)
        return verdict('status', done.verdict, done.reason)
    f = done.facts
    print(f'package {f.get("package") or "unknown: " + str(f.get("package_problem"))}; guard service '
          f'{(f.get("guard") or {}).get("service") or "absent"}, trip {(f.get("guard") or {}).get("trip") or "none"}')
    for line in role_lines(f, table):
        print(line)
    swapped = [k for k, v in (f.get('roles') or {}).items() if v.get('kept_sha256') and v['kept_sha256'] != v.get('sha256')]
    return verdict('status', 'PASS', f'{len(swapped)} roles swapped' if swapped else 'every role as installed')


def step_restore(args, table, lab) -> int:
    extra = ['-DropStale'] if args.drop_stale else []
    done = call(lab, 'restore', table, args.lab_base, extra)
    echo(done.lines)
    return verdict('restore', done.verdict, done.reason)


def main(argv=None, lab=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(errors='replace')
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('step', choices=('plan', 'swap', 'status', 'restore'))
    p.add_argument('--file', action='append', default=[], help='<x64|x86>:<role>=<path>')
    p.add_argument('--package', help='refuse unless the lab runs this package (bc250kmd.inf_<id>)')
    p.add_argument('--drop-stale', action='store_true', help='restore: delete a state of another package')
    p.add_argument('--lab-base', default=LAB_BASE, help=argparse.SUPPRESS)
    p.add_argument('--work', type=Path, default=REPO / 'scratch' / 'dev-swap', help='host staging directory')
    args = p.parse_args(argv)
    table = role_table()
    try:
        if args.step == 'plan':
            return step_plan(args, table)
        if args.file and args.step != 'swap':
            raise Refusal(f'{args.step} takes no --file')
        lab = lab or Lab()
        if args.step == 'swap':
            args.work.mkdir(parents=True, exist_ok=True)
            return step_swap(args, table, lab, args.work)
        if args.step == 'status':
            return step_status(args, table, lab)
        return step_restore(args, table, lab)
    except Refusal as exc:
        return verdict(args.step, 'REFUSED', str(exc))


if __name__ == '__main__':
    sys.exit(main())
