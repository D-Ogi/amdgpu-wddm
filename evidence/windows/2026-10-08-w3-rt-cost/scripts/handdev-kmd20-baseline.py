"""Re-pin lab-baseline.json to the hand deviation deployed on unit A since 2026-10-08 ~00:20Z (STATE.md):
KMD 0.7.216.20 (sys 7580A8F7, ABI 0x000700D8 = BC250_KMD_VERSION at 75420d23), router 93F707BB, D3D12 shell
BBB5803E, rest unchanged from the inc2 deviation. Every hash below was read on the lab by
scratch/w3-rt-cost/lab/identity.ps1 (identity-pre.txt). Keeps the previous file as
lab-baseline-before-handdev-kmd20.json. Usage: python handdev-kmd20-baseline.py [--apply]"""
import json
import shutil
import sys
from pathlib import Path

KIT = Path(__file__).resolve().parents[1] / 'm15' / 'native-caps001'
BASE = KIT / 'lab-baseline.json'
OLD_ROUTER = '7C8B7DCF506DA298EDC7DAD60282DDEBF07A4C5374A202D867CC9A91736C66B1'
NEW_ROUTER = '93F707BB6D01967034A586617BD854C98142645AF79577E2089FB6A2051DB52B'
OLD_SHELL = 'A3F8E2A8BFA2FC7177C454FED221533AF22A3BDB33E8A3F62446FCFC2D1E7B58'
NEW_SHELL = 'BBB5803E12AE918CF73F2B074074D669CF492BCD763D9EEDFA0B8BE32B0B063A'
NEW_SYS = '7580A8F78198941971B30761275BC46DD952A24C3B084379384BB023D10B4329'


def main():
    b = json.loads(BASE.read_text(encoding='utf-8'))
    assert b['kmd_version'] == '0.7.216.18' and b['kmd_abi'] == '0x000700D8', 'baseline is not the inc2 deviation'
    assert b['desktop']['registered_sha256'] == OLD_ROUTER
    assert b['d3d12']['accepted']['amdgpu_wddm_d3d12.dll'] == OLD_SHELL
    b['since'] = ('HAND DEVIATION 2026-10-08 (STATE.md): KMD 0.7.216.20 (integ/display-modes-scanout-formats 75420d23, '
                  'sys 7580A8F7) + router 93F707BB + D3D12 shell BBB5803E (m15/ascent-rt-additions 162468ec) over '
                  'tester.20, hashes read on the lab (scratch/w3-rt-cost/lab/identity-pre.txt); restore '
                  'lab-baseline-before-handdev-kmd20.json when the lab returns to the inc2 deviation. Before: ' + b['since'])
    b['kmd_version'] = '0.7.216.20'
    b['kmd_sys_sha256'] = NEW_SYS
    d = b['desktop']
    d['registered_sha256'] = NEW_ROUTER
    for route in d['dwm_routes'].values():
        route[:] = [NEW_ROUTER if h == OLD_ROUTER else h for h in route]
    b['d3d12']['accepted']['amdgpu_wddm_d3d12.dll'] = NEW_SHELL
    text = json.dumps(b, indent=2, ensure_ascii=False) + '\n'
    if '--apply' not in sys.argv:
        print(text)
        return
    shutil.copy2(BASE, KIT / 'lab-baseline-before-handdev-kmd20.json')
    BASE.write_text(text, encoding='utf-8')
    print('written', BASE)


if __name__ == '__main__':
    main()
