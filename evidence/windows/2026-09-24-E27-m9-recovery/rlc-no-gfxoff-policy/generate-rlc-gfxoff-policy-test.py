"""Compare actual shim RLC policy helpers with the original AMD helpers.
PROVENANCE: Linux amdgpu reference functions, AMD MIT; supplied source is retained
with experiment evidence. This is an MMIO model, not firmware execution.
"""
from pathlib import Path
import re
import sys

root, upstream, shared, output = map(Path, sys.argv[1:5])
def function(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        if source[end] == '{': depth += 1
        elif source[end] == '}': depth -= 1
        end += 1
    return source[start:end] + '\n'
original = upstream.read_text()
shim = (root / 'driver/shim/bc250_gfx.c').read_text()
feature = re.search(r'PP_GFXOFF_MASK\s*=\s*(0x[0-9a-fA-F]+)', shared.read_text()).group(1)
code = r'''
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "gc_10_1_0_offset.h"
#include "gc_10_1_0_sh_mask.h"
typedef unsigned u32;
struct amdgpu_device { int unused; };
static unsigned amdgpu_pp_feature_mask;
struct event { unsigned kind, reg, value; };
static struct event events[16];
static unsigned count, pg, control;
static void record(unsigned k,unsigned r,unsigned v){
 if(count<16){events[count].kind=k;events[count].reg=r;events[count].value=v;}
 count++;
}
static u32 rd(unsigned r){unsigned v=r==mmRLC_PG_CNTL?pg:control;record(1,r,v);return v;}
static void wr(unsigned r,unsigned v){record(2,r,v);if(r==mmRLC_PG_CNTL)pg=v;else control=v;}
#define RREG32_SOC15(ip,i,reg) ((void)adev,rd(reg))
#define WREG32_SOC15(ip,i,reg,val) ((void)adev,wr(reg,val))
#define WREG32_FIELD15(ip,i,reg,field,val) do { \
 u32 x=RREG32_SOC15(ip,i,mm##reg); \
 WREG32_SOC15(ip,i,mm##reg,(x & ~reg##__##field##_MASK) | ((val)<<reg##__##field##__SHIFT)); \
}while(0)
static void udelay(unsigned us){record(3,0,us);}
static void bc250_shim_udelay(unsigned us){record(3,0,us);}
'''
code += '#define PP_GFXOFF_MASK ' + feature + 'u\n'
for name in ['gfx_v10_0_rlc_smu_handshake_cntl', 'gfx_v10_0_rlc_start']:
    code += function(original, 'static void ' + name + '(')
for name in ['bc250_rlc_smu_handshake_cntl', 'bc250_rlc_start']:
    part = function(shim, 'static void ' + name + '(')
    if '--reverse-policy' in sys.argv:
        part = part.replace('if (!pp_gfxoff)', 'if (pp_gfxoff)')
    code += part
code += r'''
int main(void){
 struct amdgpu_device adev={0};
 unsigned policy,seed,bad=0,scenarios=0;
 for(policy=0;policy<2;policy++)for(seed=0;seed<2;seed++){
  struct event expected[16];unsigned n,expected_pg,expected_control;
  unsigned initial_pg=seed?0x13572468u:0u,initial_control=seed?0xa5000001u:0u;
  pg=initial_pg;control=initial_control;count=0;memset(events,0,sizeof(events));
  amdgpu_pp_feature_mask=policy?PP_GFXOFF_MASK:0;
  gfx_v10_0_rlc_start(&adev);
  n=count;expected_pg=pg;expected_control=control;memcpy(expected,events,sizeof(events));
  pg=initial_pg;control=initial_control;count=0;memset(events,0,sizeof(events));
  bc250_rlc_start(&adev,policy!=0);scenarios++;
  if(n>16 || count!=n || memcmp(events,expected,sizeof(events)) ||
     pg!=expected_pg || control!=expected_control){
   bad++;printf("FAIL policy=%u seed=%u accesses original=%u shim=%u\n",policy,seed,n,count);
  }
 }
 printf("%u AMD/shim policy scenarios, %u failures\n",scenarios,bad);return bad?1:0;
}
'''
output.write_text(code)

