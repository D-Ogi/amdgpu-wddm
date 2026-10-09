"""Which DDI shader model values a D3D12 runtime maps to an API shader model (static reading, no GPU).

Usage: python sm-map.py <dir> [<dir> ...]
Each <dir> holds a copy of a D3D12Core.dll and, when the Microsoft symbol server has its PDB, a symbols.json
([rva, public name] pairs). The copies stay outside the repository.

For a runtime with symbols the script prints the disassembly of ConvertShaderModelFromDDI, the function that turns
the driver's 1012 SHADER_MODELS list into D3D_SHADER_MODEL values, and the functions that hold the DDI release
values of 6_7 (0x60075) and 6_8 (0x60085) as immediates. For a runtime without symbols it prints only where these
immediates occur in the code section.
"""
import bisect
import json
import struct
import sys
from pathlib import Path

import capstone
import pefile

VALUES = {0x60065: '6_6 release', 0x60070: '6_7 experimental', 0x60075: '6_7 release', 0x60080: '6_8 experimental',
          0x60085: '6_8 release', 0x60090: '6_9 experimental'}


def version(pe):
    for info in getattr(pe, 'FileInfo', []) or []:
        for entry in info:
            for table in getattr(entry, 'StringTable', []):
                v = table.entries.get(b'FileVersion')
                if v:
                    return v.decode(errors='replace')
    return '?'


def main(directory):
    root = Path(directory)
    pe = pefile.PE(str(root / 'D3D12Core.dll'))
    print(f'== D3D12Core.dll FileVersion {version(pe)} SizeOfImage {pe.OPTIONAL_HEADER.SizeOfImage:#x}')
    text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
    code = text.get_data()
    symbols_file = root / 'symbols.json'
    symbols = sorted(json.loads(symbols_file.read_text())) if symbols_file.exists() else []
    addresses = [a for a, _ in symbols]

    def name_at(rva):
        i = bisect.bisect_right(addresses, rva) - 1
        return f'{symbols[i][1]}+{rva - symbols[i][0]:x}' if i >= 0 else '(no symbols)'

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    convert = [a for a, n in symbols if n.startswith('?ConvertShaderModelFromDDI@@')]
    if convert:
        start = convert[0]
        end = {e.struct.BeginAddress: e.struct.EndAddress for e in pe.DIRECTORY_ENTRY_EXCEPTION}.get(start, start + 0x100)
        print(f'-- {symbols[addresses.index(start)][1]} at {start:#x}')
        for ins in md.disasm(pe.get_data(start, end - start), start):
            note = ''
            for value, label in VALUES.items():
                if f'{value:#x}' in ins.op_str:
                    note = f'   ; {label}'
            print(f'   {ins.address:x}: {ins.mnemonic} {ins.op_str}{note}')
    else:
        print('-- ConvertShaderModelFromDDI: no public symbols for this build')
    # Every 32-bit little-endian occurrence of the values in the code section (immediates of mov/cmp/sub and the like).
    for value, label in VALUES.items():
        needle = struct.pack('<I', value)
        sites = []
        i = code.find(needle)
        while i >= 0:
            sites.append(text.VirtualAddress + i)
            i = code.find(needle, i + 1)
        shown = ', '.join(f'{s:x} {name_at(s)}' if symbols else f'{s:x}' for s in sites[:6])
        print(f'-- {value:#x} ({label}): {len(sites)} site(s) in .text{": " + shown if sites else ""}')


if __name__ == '__main__':
    for d in sys.argv[1:]:
        main(d)
