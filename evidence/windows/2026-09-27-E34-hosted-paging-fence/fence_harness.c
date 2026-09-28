/* BD-044 harness: drives the exact compiled radv_wddm2_bo.c object through a
 * scripted fake host dispatch and checks which paging fence value each CPU
 * wait receives. Linked once against the pre-fix object (negative control:
 * must report failures) and once against the fixed object (must pass).
 * Local scratch tool, not part of any repository.
 */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "winsys/wddm2/radv_wddm2_bo.h"
#include "winsys/wddm2/radv_wddm2_winsys.h"
#include "util/bc250_host_bootstrap.h"

#ifdef Status
#undef Status
#endif
#include <windows.h>
#include "d3dkmthk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PENDING ((int32_t)0x103)

/* Never reached: host.dispatch is set, so BC250_WDDM_CALL does not use the
 * native dispatch table. Defined here so the object links without the
 * Vulkan util library. */
struct vk_wddm2_dispatch_table;
struct vk_wddm2_dispatch_table *
vk_wddm2_dispatch_table_get(void)
{
   fprintf(stderr, "native dispatch table requested: harness setup error\n");
   abort();
}

struct script {
   uint64_t map_fence[8];
   int32_t map_status[8];
   unsigned map_count, map_next;
   uint64_t resident_fence;
   int32_t resident_status;
   uint64_t waits[8];
   unsigned wait_count;
   unsigned unexpected_op;
};

static struct script *cur;

static int32_t
fake_dispatch(void *userdata, uint32_t op, void *arg)
{
   struct script *s = userdata;
   switch (op) {
   case BC250_HOST_CreateAllocation2: {
      D3DKMT_CREATEALLOCATION *c = arg;
      c->pAllocationInfo2[0].hAllocation = 0x40;
      return 0;
   }
   case BC250_HOST_ReserveGpuVirtualAddress: {
      D3DDDI_RESERVEGPUVIRTUALADDRESS *r = arg;
      r->VirtualAddress = r->BaseAddress ? r->BaseAddress : r->MinimumAddress;
      return 0;
   }
   case BC250_HOST_MapGpuVirtualAddress: {
      D3DDDI_MAPGPUVIRTUALADDRESS *m = arg;
      if (s->map_next >= s->map_count) {
         s->unexpected_op = op;
         return (int32_t)0xC0000001;
      }
      m->VirtualAddress = m->BaseAddress ? m->BaseAddress : m->MinimumAddress;
      m->PagingFenceValue = s->map_fence[s->map_next];
      return s->map_status[s->map_next++];
   }
   case BC250_HOST_MakeResident: {
      D3DDDI_MAKERESIDENT *r = arg;
      r->PagingFenceValue = s->resident_fence;
      return s->resident_status;
   }
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w = arg;
      if (s->wait_count < 8)
         s->waits[s->wait_count] = w->FenceValueArray[0];
      s->wait_count++;
      return 0;
   }
   case BC250_HOST_QueryResourceInfoFromNtHandle: {
      D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *q = arg;
      q->NumAllocations = 1;
      q->TotalPrivateDriverDataSize = 32;
      q->ResourcePrivateDriverDataSize = 0;
      q->PrivateRuntimeDataSize = 0;
      return 0;
   }
   case BC250_HOST_OpenResourceFromNtHandle: {
      D3DKMT_OPENRESOURCEFROMNTHANDLE *o = arg;
      /* KMD LB7A v1 linear surface, 64x64 A8R8G8B8. */
      struct { uint32_t magic, version, width, height, pitch, format; uint64_t size; } surf = {
         0x4137424c, 1, 64, 64, 256, D3DDDIFMT_A8R8G8B8, 256 * 64,
      };
      memcpy(o->pTotalPrivateDriverDataBuffer, &surf, sizeof(surf));
      o->pOpenAllocationInfo2[0].hAllocation = 0x44;
      o->pOpenAllocationInfo2[0].pPrivateDriverData = o->pTotalPrivateDriverDataBuffer;
      o->pOpenAllocationInfo2[0].PrivateDriverDataSize = sizeof(surf);
      o->hResource = 0x48;
      return 0;
   }
   default:
      s->unexpected_op = op;
      return 0;
   }
}

static struct radv_wddm2_winsys *
make_ws(struct script *s)
{
   struct radv_wddm2_winsys *ws = calloc(1, sizeof(*ws));
   ws->bc250 = true;
   ws->host.userdata = s;
   ws->host.dispatch = fake_dispatch;
   ws->adapter_h = 0x10;
   ws->device_h = 0x20;
   ws->paging_queue_h = 0x30;
   ws->paging_fence_h = 0x34;
   ws->gpu_info.pte_fragment_size = 0x200000;
   ws->gpu_info.gart_page_size = 4096;
   radv_wddm2_bo_init_functions(ws);
   return ws;
}

static unsigned failures;

static void
check(const char *path, const char *name, VkResult r, struct script *s, unsigned wait_index,
      uint64_t expect)
{
   const bool ok = r == VK_SUCCESS && s->wait_count > wait_index &&
                   s->waits[wait_index] == expect && s->map_next == s->map_count;
   printf("%-7s %-34s result=%d waits=%u waited=%llu expected=%llu unexpected_op=%u  %s\n",
          path, name, (int)r, s->wait_count,
          (unsigned long long)(s->wait_count > wait_index ? s->waits[wait_index] : 0),
          (unsigned long long)expect, s->unexpected_op, ok ? "PASS" : "FAIL");
   if (!ok)
      failures++;
}

struct pair_case {
   const char *name;
   uint64_t map_fence;
   int32_t map_status;
   uint64_t resident_fence;
   int32_t resident_status;
   uint64_t expect;
};

static const struct pair_case pair_cases[] = {
   {"map>resident (10, 5 pending)", 10, PENDING, 5, PENDING, 10},
   {"map<resident (5, 10 pending)", 5, PENDING, 10, PENDING, 10},
   {"map=resident (7, 7 pending)", 7, PENDING, 7, PENDING, 7},
   {"map 9 pending, resident S_OK 0", 9, PENDING, 0, 0, 9},
   {"map 0 done, resident 4 pending", 0, 0, 4, PENDING, 4},
};

int
main(void)
{
   for (unsigned i = 0; i < sizeof(pair_cases) / sizeof(pair_cases[0]); i++) {
      const struct pair_case *c = &pair_cases[i];

      struct script s = {0};
      s.map_fence[0] = c->map_fence;
      s.map_status[0] = c->map_status;
      s.map_count = 1;
      s.resident_fence = c->resident_fence;
      s.resident_status = c->resident_status;
      struct radv_wddm2_winsys *ws = make_ws(&s);
      struct radeon_winsys_bo *bo = NULL;
      VkResult r = ws->base.buffer_create(&ws->base, 65536, 4096, RADEON_DOMAIN_GTT,
                                          RADEON_FLAG_NO_INTERPROCESS_SHARING, 0, 0, NULL, &bo);
      check("create", c->name, r, &s, 0, c->expect);

      struct script t = {0};
      t.map_fence[0] = c->map_fence;
      t.map_status[0] = c->map_status;
      t.map_count = 1;
      t.resident_fence = c->resident_fence;
      t.resident_status = c->resident_status;
      ws = make_ws(&t);
      bo = NULL;
      uint64_t alloc_size = 0;
      r = ws->base.buffer_from_handle(&ws->base, (void *)(uintptr_t)0x1234, 0, &bo, &alloc_size);
      check("import", c->name, r, &t, 0, c->expect);
   }

   /* Sparse alias: zero-map of the low view (own wait), then high reserve,
    * zero-map of the high view and one map per null-PRT chunk, one wait. */
   static const struct {
      const char *name;
      uint64_t low_zero, high_zero, chunk0, chunk1, expect;
   } sparse_cases[] = {
      {"last chunk done (9, 12, 0)", 3, 9, 12, 0, 12},
      {"chunk smaller (9, 4, 6)", 3, 9, 4, 6, 9},
      {"ascending (1, 2, 3)", 3, 1, 2, 3, 3},
   };
   for (unsigned i = 0; i < sizeof(sparse_cases) / sizeof(sparse_cases[0]); i++) {
      struct script s = {0};
      s.map_fence[0] = sparse_cases[i].low_zero;
      s.map_fence[1] = sparse_cases[i].high_zero;
      s.map_fence[2] = sparse_cases[i].chunk0;
      s.map_fence[3] = sparse_cases[i].chunk1;
      for (unsigned j = 0; j < 4; j++)
         s.map_status[j] = s.map_fence[j] ? PENDING : 0;
      s.map_count = 4;
      struct radv_wddm2_winsys *ws = make_ws(&s);
      struct radeon_winsys_bo null_bo = {.handle = 0x50, .size = 8 * 1024 * 1024};
      ws->null_prt.bo = &null_bo;
      struct radeon_winsys_bo *bo = NULL;
      VkResult r = ws->base.buffer_create(&ws->base, 16 * 1024 * 1024, 65536, RADEON_DOMAIN_VRAM,
                                          RADEON_FLAG_VIRTUAL | RADEON_FLAG_EMULATE_SPARSE_RESIDENCY,
                                          0, 0, NULL, &bo);
      check("sparse", sparse_cases[i].name, r, &s, 1, sparse_cases[i].expect);
   }

   printf("failures=%u\n", failures);
   return failures ? 1 : 0;
}
