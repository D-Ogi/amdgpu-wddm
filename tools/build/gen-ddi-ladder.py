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
        '#if defined(D3D12DDI_BUILD_VERSION_0119)','inline constexpr unsigned kDdiMax=119;','#else','inline constexpr unsigned kDdiMax=94;','#endif',
        'template<class F> HRESULT with_ddi_layout(unsigned version,F&& f) {','    switch(version) {']
for v in range(92,120):
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
