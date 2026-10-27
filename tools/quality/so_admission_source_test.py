"""Generate a host test from the production SO admission clauses, never a copied implementation.

Specification: DirectX-Specs PartialGraphicsPrograms.md (f2917d), lines 97-98:
SO is forbidden in partial definitions and permitted on the final linked program.
This focused branch test does not replace the installed-package rendering tests.
"""
import argparse
import hashlib
import re
from pathlib import Path


def block(text, marker):
    if text.count(marker) != 1:
        raise ValueError('Admission extraction anchor changed: ' + marker)
    start = text.index('{', text.index(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start + 1:end - 1]


def extract(text):
    linked = block(text, 'HRESULT compile_linked_exports(GenericStateObject& object,')
    compile_body = block(text, 'HRESULT compile(GenericStateObject& object,')
    marker = 'case D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:'
    if linked.count(marker) != 1:
        raise ValueError('Expected exactly one final-link Stream Output admission case')
    start = linked.index(marker)
    end = linked.index('            break;', start) + len('            break;')
    final = linked[start:end]
    loops = {}
    for name in ('preraster', 'pixel'):
        scope = block(compile_body, '    if (partial_' + name + ') {')
        loops[name] = block(scope, 'switch (static_cast<UINT>(entry.first))')
    return final, loops


def generate(source):
    final, loops = extract(source.read_text(encoding='utf-8'))
    symbols = sorted(set(re.findall(r'D3D12DDI_STATE_SUBOBJECT_TYPE_\w+', final + ''.join(loops.values()))))
    enums = ','.join(name + '=' + str(100 + i) for i, name in enumerate(symbols))
    prefix = '''#include <cstdint>
#include <cstdio>
#include <vector>
using UINT=unsigned; using HRESULT=int32_t;
constexpr HRESULT E_INVALIDARG=(HRESULT)0x80070057, E_NOINTERFACE=(HRESULT)0x80004002;
constexpr UINT D3D12_SO_NO_RASTERIZED_STREAM=~0u, D3D12_SO_BUFFER_MAX_STRIDE_IN_BYTES=2048;
enum { ENUMS };
struct State { std::vector<UINT> strides; UINT value; };
HRESULT final_admission(UINT minor,bool has_preraster,bool linked_mesh,const State* state) {
struct Device { struct { UINT AbiVersion; } funcs; } device{{65536u|minor}};
struct { Device* device; } object{&device};
switch(D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) { FINAL }
return 0;
}
HRESULT partial_preraster_admission(UINT kind,bool partial_combined) { switch(kind) { PRE } return 0; }
HRESULT partial_pixel_admission(UINT kind) { switch(kind) { PIX } return 0; }
'''
    code = prefix.replace('ENUMS', enums).replace('FINAL', final).replace('PRE', loops['preraster']).replace('PIX', loops['pixel'])
    code += '''int main() {
unsigned checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\\n",__LINE__,#x); } } while(0)
State s{{16},0};
CHECK(final_admission(10,true,false,&s)==0);
CHECK(final_admission(9,true,false,&s)==E_NOINTERFACE);
CHECK(final_admission(0,true,false,&s)==E_NOINTERFACE);
CHECK(final_admission(11,true,false,&s)==0);
CHECK(final_admission(10,false,false,&s)==E_INVALIDARG);
CHECK(final_admission(10,true,true,&s)==E_INVALIDARG);
for(UINT stream=0;stream<4;++stream){s.value=stream;CHECK(final_admission(10,true,false,&s)==0);}
s.value=~0u;CHECK(final_admission(10,true,false,&s)==0);
s.value=4;CHECK(final_admission(10,true,false,&s)==E_INVALIDARG);
s.value=0;s.strides={2048};CHECK(final_admission(10,true,false,&s)==0);
s.strides={2052};CHECK(final_admission(10,true,false,&s)==E_INVALIDARG);
s.strides={15};CHECK(final_admission(10,true,false,&s)==E_INVALIDARG);
s.strides={4,4,4,4,4};CHECK(final_admission(10,true,false,&s)==E_INVALIDARG);
s.strides={4,4,4,4};CHECK(final_admission(10,true,false,&s)==0);
CHECK(partial_preraster_admission(D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT,false)==E_INVALIDARG);
CHECK(partial_preraster_admission(D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT,true)==E_INVALIDARG);
CHECK(partial_pixel_admission(D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT)==E_INVALIDARG);
CHECK(partial_preraster_admission(D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,false)==0);
CHECK(partial_pixel_admission(D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY)==0);
std::printf("SO actual-source admission: %u checks, %u failures\\n",checks,failures);
return failures ? 1 : 0;
}
'''
    return '// Source SHA256: ' + hashlib.sha256(source.read_bytes()).hexdigest() + '\n' + code


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(generate(args.source), encoding='utf-8')
