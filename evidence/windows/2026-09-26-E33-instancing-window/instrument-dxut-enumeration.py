from pathlib import Path
p=Path('scratch/m12/directx-sdk-src/C++/DXUT/Core/DXUTenum.cpp');s=p.read_text();s='#include <cstdio>\n'+s;s=s.replace('continue;', '{ fprintf(stderr, "BC250 DXUT enum skip line=%d\\n", __LINE__); fflush(stderr); continue; }');p.write_text(s)
p=Path('scratch/m12/directx-sdk-src/C++/DXUT/Core/DXUT.cpp');s=p.read_text();s='#include <cstdio>\n'+s;old='    switch( hr )\n    {\n        case DXUTERR_NODIRECT3D:';assert old in s;s=s.replace(old,'    fprintf(stderr, "BC250 DXUT error HRESULT=%08lx\\n", (unsigned long)hr); fflush(stderr);\n'+old);p.write_text(s)
