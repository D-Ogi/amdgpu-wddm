"""Extract the actual Mesa BC250 producer and compose it with the KMD parser tests."""
import re
import sys
from pathlib import Path
repo,mesa,out=map(Path,sys.argv[1:4])
header=(mesa/'src/amd/vulkan/winsys/wddm2/radv_wddm2_bc250.h').read_text()
bo=(mesa/'src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c').read_text()
winsys=(mesa/'src/amd/vulkan/radv_radeon_winsys.h').read_text()
a=header.index('struct bc250_alloc_blob {');b=header.index('};',a)+2
struct=header[a:b]
defs=[]
for name in ('BC250_ALLOC_MAGIC','BC250_HEAP_VRAM','BC250_HEAP_GTT','BC250_A_EXACT_VA'):
    defs.append(re.search(r'^#define '+name+r'\s+[^\n]+',header,re.M).group(0))
for name in ('RADEON_DOMAIN_VRAM','RADEON_DOMAIN_GTT','RADEON_FLAG_CPU_ACCESS','RADEON_FLAG_NO_CPU_ACCESS','RADEON_FLAG_GTT_WC'):
    value=re.search(r'\b'+name+r'\s*=\s*([^,\n]+)',winsys).group(1)
    defs.append('#define '+name+' '+value)
a=bo.index('      struct bc250_alloc_blob blob;');b=bo.index('   } else {',a)
producer=bo[a:b]
if '--omit-cache-intent' in sys.argv:
    producer=producer.replace('blob.gem_flags |= 1ull <<','blob.gem_flags |= 0ull <<')
text='#include <stdint.h>\n#define main base_contract_main\n#include "'+(repo/'driver/kmd/test/umd_blob_test.c').as_posix()+'"\n#undef main\n'
text+='\n'.join(defs)+'\n'+struct+'\n'
text+='''
int main(void)
{
 unsigned heap,flags,offset,cases=0;
 if(base_contract_main())return 1;
 for(heap=0;heap<2;heap++)for(flags=0;flags<256;flags++) {
  uint8_t alloc_pdata[824]={0},unaligned[832];uint32_t pdata_size=0;
  uint64_t phys_size=8192,phys_alignment=4096,address=0x100004000ull;
  unsigned initial_domain=heap?RADEON_DOMAIN_VRAM:RADEON_DOMAIN_GTT;
  unsigned long long expected=0;
  struct umd_alloc_view view;
'''+producer+'''
  if(flags&RADEON_FLAG_CPU_ACCESS)expected|=AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
  if(flags&RADEON_FLAG_NO_CPU_ACCESS)expected|=AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
  if(flags&RADEON_FLAG_GTT_WC)expected|=AMDGPU_GEM_CREATE_CPU_GTT_USWC;
  CHECK(pdata_size==sizeof(struct bc250_umd_alloc_private));
  for(offset=0;offset<8;offset++) {
   memcpy(unaligned+offset,alloc_pdata,pdata_size);
   CHECK(UmdBlobParseAlloc(unaligned+offset,pdata_size,&view)==UMD_BLOB_OK);
   CHECK(view.cache_policy_valid && view.gem_flags==expected);
   CHECK(view.bytes==8192 && view.alignment==4096 && view.exact_va && view.requested_va==address);
   CHECK(UmdBlobAllocCpuCached(&view)==(!heap && !(flags&(RADEON_FLAG_NO_CPU_ACCESS|RADEON_FLAG_GTT_WC))));
   cases++;
  }
 }
 printf("actual Mesa producer roundtrip: %u cases, %d failures\\n",cases,g_failures);
 return g_failures?1:0;
}
'''
out.write_text(text)
