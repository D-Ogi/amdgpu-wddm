#include <stdint.h>
#define main base_contract_main
#include "P:/BC-250/bc250-win/driver/kmd/test/umd_blob_test.c"
#undef main
#define BC250_ALLOC_MAGIC   0x41324342u /* "BC2A" */
#define BC250_HEAP_VRAM 0x4u
#define BC250_HEAP_GTT  0x2u
#define BC250_A_EXACT_VA 0x1u
#define RADEON_DOMAIN_VRAM 4
#define RADEON_DOMAIN_GTT 2
#define RADEON_FLAG_CPU_ACCESS (1 << 1)
#define RADEON_FLAG_NO_CPU_ACCESS (1 << 2)
#define RADEON_FLAG_GTT_WC (1 << 0)
struct bc250_alloc_blob {
   uint32_t magic, version, size, flags;
   uint64_t alloc_size;
   uint64_t phys_alignment;
   uint32_t preferred_heap;
   uint32_t reserved0;
   uint64_t gem_flags;
   uint64_t requested_va;
   uint64_t va_size;
   uint64_t va_flags;
   uint64_t va_offset;
   uint32_t metadata_size;
   uint32_t metadata[16];
   uint32_t reserved[11];
};

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
      struct bc250_alloc_blob blob;
      memset(&blob, 0, sizeof(blob));
      blob.magic = BC250_ALLOC_MAGIC;
      /* BC2A v2 makes cache intent explicit; v1 KMD allocations retain WC. */
      blob.version = 2;
      blob.size = sizeof(blob);
      blob.alloc_size = phys_size;
      blob.phys_alignment = phys_alignment;
      blob.preferred_heap = (initial_domain & RADEON_DOMAIN_VRAM) ? BC250_HEAP_VRAM : BC250_HEAP_GTT;
      /* AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED / NO_CPU_ACCESS / CPU_GTT_USWC,
       * matching radv_amdgpu_bo.c. Other GEM flags have no new WDDM policy here. */
      if (flags & RADEON_FLAG_CPU_ACCESS)
         blob.gem_flags |= 1ull << 0;
      if (flags & RADEON_FLAG_NO_CPU_ACCESS)
         blob.gem_flags |= 1ull << 1;
      if (flags & RADEON_FLAG_GTT_WC)
         blob.gem_flags |= 1ull << 2;
      blob.requested_va = address;
      blob.va_size = phys_size;
      if (address)
         blob.flags = BC250_A_EXACT_VA;
      _Static_assert(sizeof(blob) == 192, "BC2A is 192 bytes");
      _Static_assert(sizeof(blob) <= sizeof(alloc_pdata), "BC2A fits the allocation private buffer");
      memcpy(alloc_pdata, &blob, sizeof(blob));
      pdata_size = sizeof(blob);

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
 printf("actual Mesa producer roundtrip: %u cases, %d failures\n",cases,g_failures);
 return g_failures?1:0;
}
