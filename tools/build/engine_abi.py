"""Select one engine ABI pin before building or running against an engine DLL.

BD-167 ships two engine DLLs from one source line: the release flavour reads no diagnostic variable and
carries no statistics, the lab flavour keeps both. They are different builds, so each flavour pins its own
SHA-256 and its own build directory, and a pin of one flavour can never satisfy a stage of the other.

The selection has the semantics of tools/release/release_flavour.py, which selects the DriverStore payload
of release-sources.json: an unknown flavour, a pin already selected for the other flavour and an incomplete
pair are all refused, and nothing is selected by falling back to the other flavour's value.

A flavour whose DLL is not built yet pins PENDING. The selection then succeeds (the native library and its
host tests need no engine DLL) and only a run against the engine DLL is refused, by name.

The 32-bit (WoW64) engine is another build of the same commit. A flavour can pin it in an 'x86' object with the
three fields of the 64-bit DLL (engine_dll_dir, engine_dll_sha256, engine_dll_recipe). --arch x86 selects those
fields in place of the 64-bit ones, and is refused when the flavour pins no x86 DLL. The selection says which
architecture it holds ('arch'), so a pin of one architecture never satisfies a stage of the other.
"""
import argparse
import copy
import hashlib
import json
import sys
from pathlib import Path

FLAVOURS = ('release', 'lab')
ARCHES = ('x64', 'x86')
# The fields of one engine DLL, which the 'x86' object of a flavour replaces for --arch x86.
DLL_FIELDS = frozenset(('engine_dll_dir', 'engine_dll_sha256', 'engine_dll_recipe'))
# Written in engine-abi.json until the release build pins the digest of that flavour's DLL.
PENDING = 'pending'


def _digest_of(value: str, what: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f'{what} is not a SHA-256')
    low = value.casefold()
    if low == PENDING:
        return low
    if len(low) != 64 or low.strip('0123456789abcdef'):
        raise ValueError(f'{what} is not a SHA-256')
    return low


def select_engine_abi(pin: dict, flavour: str = 'release', arch: str = 'x64') -> dict:
    """Return the pin of one flavour and architecture: the shared fields plus that flavour's own (for x86, those of
    its 'x86' object), 'flavour' and 'arch'."""
    if flavour not in FLAVOURS:
        raise ValueError('unknown engine flavour')
    if arch not in ARCHES:
        raise ValueError('unknown engine architecture')
    declared = pin.get('flavour')
    if declared is not None and declared != flavour:
        raise ValueError('engine pin belongs to a different flavour')
    if 'flavours' not in pin:
        # Already selected (idempotent, as release_flavour.py is), or a pin from before the two flavours.
        if declared == flavour:
            if pin.get('arch', 'x64') != arch:
                raise ValueError('engine pin belongs to a different architecture')
            return copy.deepcopy(pin)
        if pin.get('schema', 0) >= 2:
            raise ValueError('every engine pin needs a release and a lab flavour')
        if flavour != 'release':
            raise ValueError('legacy engine pins do not contain a lab build')
        if arch != 'x64':
            raise ValueError('legacy engine pins do not contain an x86 build')
        return dict(copy.deepcopy(pin), flavour=flavour)
    flavours = pin['flavours']
    if not isinstance(flavours, dict) or set(flavours) != set(FLAVOURS):
        raise ValueError('every engine pin needs a release and a lab flavour')
    slots = [frozenset(f) if isinstance(f, dict) else None for f in flavours.values()]
    if None in slots or slots[0] != slots[1] or not slots[0]:
        raise ValueError('the two engine flavours pin different fields')
    for name in sorted(flavours):
        for key in sorted(flavours[name]):
            if key in pin:
                raise ValueError(f'{key} is pinned both per flavour and shared')
    # A lab DLL must never pass a release stage, and an x86 DLL never an x64 one, so no two DLLs of the pin can have
    # the same digest.
    digests = [_digest_of(flavours[n].get('engine_dll_sha256'), f'{n} engine_dll_sha256') for n in FLAVOURS]
    for name in FLAVOURS:
        x86 = flavours[name].get('x86')
        if x86 is None:
            continue
        if not isinstance(x86, dict) or frozenset(x86) != DLL_FIELDS:
            raise ValueError(f'the x86 engine DLL of {name} needs exactly {", ".join(sorted(DLL_FIELDS))}')
        digests.append(_digest_of(x86.get('engine_dll_sha256'), f'{name} x86 engine_dll_sha256'))
    real = [d for d in digests if d != PENDING]
    if len(set(real)) != len(real):
        raise ValueError('two engine DLLs of the pin (flavour or architecture) cannot have the same SHA-256')
    result = {k: v for k, v in copy.deepcopy(pin).items() if k != 'flavours'}
    own = copy.deepcopy(flavours[flavour])
    x86 = own.pop('x86', None)
    result.update(own)
    if arch == 'x86':
        if x86 is None:
            raise ValueError(f'engine-abi.json pins no x86 engine DLL for {flavour}')
        result.update(x86)
    result['flavour'] = flavour
    result['arch'] = arch
    return result


def check_engine_dll(selected: dict, digest: str) -> str:
    """Raise unless digest is the engine DLL the selected flavour pins."""
    flavour = selected.get('flavour')
    if flavour not in FLAVOURS:
        raise ValueError('engine pin was not selected for a flavour')
    pinned = _digest_of(selected.get('engine_dll_sha256'), f'{flavour} engine_dll_sha256')
    got = _digest_of(digest, 'engine DLL SHA-256')
    if pinned == PENDING:
        raise ValueError(f'engine-abi.json pins no {flavour} engine DLL yet (build it and pin its SHA-256)')
    if got != pinned:
        raise ValueError(f'engine DLL has SHA-256 {got}; the {flavour} flavour pins {pinned}')
    return got


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pin', required=True, type=Path)
    parser.add_argument('--flavour', choices=FLAVOURS, default='release')
    parser.add_argument('--arch', choices=ARCHES, default='x64')
    parser.add_argument('--out', type=Path, help='write the selected pin here instead of stdout')
    parser.add_argument('--engine-dll', type=Path, help='check this DLL against the selected pin')
    args = parser.parse_args()
    # One line on stderr, not a traceback: the build script shows it as the reason it stopped.
    try:
        selected = select_engine_abi(json.loads(args.pin.read_text(encoding='utf-8-sig')), args.flavour, args.arch)
        if args.engine_dll:
            print(check_engine_dll(selected, hashlib.sha256(args.engine_dll.read_bytes()).hexdigest()))
            return 0
    except ValueError as error:
        print(f'{args.pin}: {error}', file=sys.stderr)
        return 2
    text = json.dumps(selected, indent=2) + '\n'
    if args.out:
        args.out.write_text(text, encoding='utf-8')
    else:
        print(text, end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
