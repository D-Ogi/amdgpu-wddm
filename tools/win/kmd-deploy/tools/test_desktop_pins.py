"""Host test of stage.py desktop_pins and the identity lines freeze generates from them (no lab, no files written).

    python tools/test_desktop_pins.py

Cases: the live lab-baseline.json; the installed release's desktop block, which registers the router; refusals for a
malformed block and for a baseline with no desktop block at all. Prints PASS and exits 0.
"""
import copy
import json
import re
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))
import stage  # noqa: E402
from kmdcommon import IDENTITY_KEYS, LAB_BASELINE  # noqa: E402

CPU = '4176D1DF5E93DB284A3FADF8E406AF98E4346966E15682015D7B5BA0ED7C8544'
ROUTER = '5BBEB7837AFA65F846B525019BFE6627C43C3BD07AA9EDC8D1CBA9746231DC71'
ZINK = 'E6B944CF6E1250E63920D7E4498D628B2CF97E53E143C69341EC8A689A6C69F7'
ICD = '66FE8F3178EA2D70D8804858E23434C7662E46AE119529C4DA6FAF7E53F57200'
INSTALL_ROOT = 'C:\\Program Files\\amdgpu-wddm'
# The release layout: every path below <InstallRoot>, as release-baseline.py derives it from the release manifest.
NO_DESKTOP = {'kmd_version': '0.7.216.20', 'kmd_sys_sha256': 'A' * 64, 'kmd_abi': '0x000700D8',
              'umd_path': INSTALL_ROOT + '\\desktop\\bc250d3d.dll', 'umd_sha256': CPU,
              'icd_path': INSTALL_ROOT + '\\vulkan\\vulkan_radeon.dll', 'icd_sha256': 'B' * 64}
ROUTED = dict(NO_DESKTOP, desktop={'registered_path': INSTALL_ROOT + '\\desktop\\bc250d3d_router.dll',
                                   'registered_sha256': ROUTER, 'switches': 1, 'default_route': 'gpu',
                                   'dwm_routes': {'cpu': [ROUTER, CPU], 'gpu': [ROUTER, ZINK, ICD]}})


def refused(baseline):
    try:
        stage.desktop_pins(baseline)
    except SystemExit as e:
        return str(e).startswith('REFUSED')
    return False


def identity_lines(baseline):
    candidate = {'revision': 183, 'commit': '8871602f' * 5, 'version': '0.7.183.1', 'abi': '0x000700B7'}
    rollback = {'revision': 182, 'commit': '6f68c980' * 5, 'version': '0.7.182.1', 'abi': '0x000700B6',
                'files': {'bc250kmd.sys': 'C' * 64, 'bc250kmd.inf': 'D' * 64, 'bc250kmd.cat': 'E' * 64}}
    text = stage.generated_identity(candidate, rollback, baseline, 'kmd183-deploy')
    return dict(re.findall(r"^\$(Kmd\w+)='([^']*)'\s*$", text, re.M))


def main():
    checks = []

    def check(name, ok):
        checks.append(name)
        if not ok:
            raise SystemExit('FAIL: ' + name)

    check('refused: no desktop block (the installed release always has one)', refused(NO_DESKTOP))
    routed = stage.desktop_pins(ROUTED)
    check('router block: switches 1, CPU route = router + CPU UMD sorted, router key',
          routed == {'switches': '1', 'modules': sorted([ROUTER, CPU]), 'router_key': 'SOFTWARE\\amdgpu-wddm\\DesktopRouter'})
    values = identity_lines(ROUTED)
    check('generated identity carries every IDENTITY_KEYS name', all(k in values for k in IDENTITY_KEYS))
    check('generated identity desktop lines',
          values['KmdDesktopSwitches'] == '1' and values['KmdDesktopModules'] == ','.join(sorted([ROUTER, CPU]))
          and values['KmdDesktopRouterKey'] == 'SOFTWARE\\amdgpu-wddm\\DesktopRouter')
    for name, change in [
            ('switches 2', lambda d: d['desktop'].__setitem__('switches', 2)),
            ('switches missing', lambda d: d['desktop'].pop('switches')),
            ('cpu route empty', lambda d: d['desktop']['dwm_routes'].__setitem__('cpu', [])),
            ('cpu route is the GPU set', lambda d: d['desktop']['dwm_routes'].__setitem__('cpu', [ROUTER, ZINK, ICD])),
            ('cpu route without the router', lambda d: d['desktop']['dwm_routes'].__setitem__('cpu', [CPU])),
            ('cpu route without the CPU UMD', lambda d: d['desktop']['dwm_routes'].__setitem__('cpu', [ROUTER])),
            ('registered hash not in the cpu route', lambda d: d['desktop'].__setitem__('registered_sha256', ZINK)),
            ('malformed hash', lambda d: d['desktop']['dwm_routes'].__setitem__('cpu', [ROUTER, 'xyz']))]:
        bad = copy.deepcopy(ROUTED)
        change(bad)
        check('refused: ' + name, refused(bad))
    live = json.loads(LAB_BASELINE.read_text(encoding='utf-8'))
    pins = stage.desktop_pins(live)
    check('live lab-baseline.json gives a well-formed pin set',
          pins['switches'] in ('0', '1') and live['umd_sha256'].upper() in pins['modules'])
    print(f'PASS: desktop pins, {len(checks)} checks (live baseline: switches {pins["switches"]}, '
          f'{len(pins["modules"])} module(s), router key {"set" if pins["router_key"] else "none"})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
