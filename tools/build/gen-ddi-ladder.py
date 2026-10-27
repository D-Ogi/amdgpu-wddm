"""Generate typed member walks from the checked WDK, never from slot offsets."""
from pathlib import Path
import re
import argparse
import sys
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--header',type=Path,required=True)
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[2]/'driver/umd/d3d12/ddi-ladder.h')
parser.add_argument('--check',action='store_true')
args=parser.parse_args()
text=args.header.read_text()
if '#define D3D12DDI_BUILD_VERSION_0119 119' not in text:
    parser.error('the generator requires the WDK 0119 header')
tables={}
for m in re.finditer(r'typedef struct (D3D12DDI_(DEVICE_FUNCS_CORE|COMMAND_LIST_FUNCS_3D)_(\d+))\s*\{(.*?)\}\s*\w+;',text,re.S):
    v=int(m[3])
    if (m[2]=='DEVICE_FUNCS_CORE' and v>=88) or (m[2]=='COMMAND_LIST_FUNCS_3D' and v>=92):
        tables[m[1]]=(v,m[2],re.findall(r'\w+\s+(pfn\w+)\s*;',m[4]))
names=sorted(set(n for _,_,fs in tables.values() for n in fs))
lines=['// SPDX-License-Identifier: MIT','#pragma once','#include <windows.h>','#include <d3d12umddi.h>','#include <type_traits>',
       '// Typed member walks derived from WDK 10.0.28000.2526 d3d12umddi.h.',
       '// Each specialization names every member; sizeof gates detect additions in another header.',
       'namespace native12 {','template<class T> struct DdiMembers;']
for n in names: lines.append(f'struct DdiName_{n} {{ static constexpr const char* name() noexcept {{ return "{n}"; }} }};')
for name,(v,family,fields) in tables.items():
    if v>94: lines.append('#if defined(D3D12DDI_BUILD_VERSION_0119)')
    lines += [f'template<> struct DdiMembers<{name}> {{',f'    using T={name};',f'    static constexpr unsigned count={len(fields)};',
              '    static_assert(sizeof(T)==count*sizeof(void*));','    template<class F> static void each(F&& f) {']
    for n in fields: lines.append(f'        f(std::integral_constant<decltype(&T::{n}),&T::{n}>{{}},DdiName_{n}{{}});')
    lines+=['    }','};']
    if v>94: lines.append('#endif')
lines+=['template<class C,class L,unsigned V> struct DdiLayout { using Core=C; using List=L; static constexpr unsigned version=V; };',
        '// DDI build 0120. No public header defines it: WDK 10.0.28000 ends at 0119. A static analysis of D3D12Core',
        '// ("D3D12 DDI 0120: what the Agility runtime requires beyond 0119", 2026-10-25) found no table, slot or caps',
        '// payload change from 0119 to 0120. The runtime changes one answer: below 0120 it clamps the shader model of',
        '// caps type 1012 to 6_8 (the compare of the negotiated index with 0x50 at 0x1802907a0 in Agility 1.619.6 and at',
        '// 0x1801c5d79 in 1.721.3). So 0120 uses the 0119 layouts. Both cores list 0120 as 0xC005000780000.',
        '#if defined(D3D12DDI_BUILD_VERSION_0119)','inline constexpr unsigned kDdiBuild0120=120;',
        'inline constexpr UINT64 kDdiSupported0120=(UINT64{D3D12DDI_INTERFACE_VERSION_R8}<<32)|(UINT64{kDdiBuild0120}<<16);',
        'static_assert(kDdiSupported0120==0xC005000780000ull);',
        'inline constexpr unsigned kDdiDefault=122;',
        '// 0122 uses the measured 0119/0114 tables and the separately translated descriptor layouts.',
        'inline constexpr unsigned kDdiMax=kDdiDefault;',
        '#else','inline constexpr unsigned kDdiDefault=94;', 'inline constexpr unsigned kDdiMax=kDdiDefault;','#endif',
        'template<class F> HRESULT with_ddi_layout(unsigned version,F&& f) {','    switch(version) {']
for v in range(92,123):
    if v==95: lines.append('#if defined(D3D12DDI_BUILD_VERSION_0119)')
    core=max((ver,n) for n,(ver,f,_) in tables.items() if f=='DEVICE_FUNCS_CORE' and ver<=v)[1]
    lis=max((ver,n) for n,(ver,f,_) in tables.items() if f=='COMMAND_LIST_FUNCS_3D' and ver<=v)[1]
    lines.append(f'    case {v}: return f(DdiLayout<{core},{lis},{v}>{{}});')
lines+=['#endif','    default: return E_UNEXPECTED;','    }','}','}']
generated='\n'.join(lines)+'\n'
if args.check:
    if not args.output.exists() or args.output.read_text()!=generated:
        sys.exit('DDI member map differs from the supplied header; regenerate and review layouts')
    print('DDI typed member map matches WDK header')
else:
    args.output.write_text(generated)
