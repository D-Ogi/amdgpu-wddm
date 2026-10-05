/*
 * Copyright © 2020 Valve Corporation
 *
 * based on amdgpu winsys.
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2022 Collabora, Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* Windows headers conflict with Wayland and XLib headers */
#undef VK_USE_PLATFORM_WAYLAND_KHR
#undef VK_USE_PLATFORM_XLIB_KHR
#undef VK_USE_PLATFORM_XLIB_XRANDR_EXT

#include "radv_wddm2_bo.h"
#include "radv_wddm2_bc250.h"
#include "radv_wddm2_cs.h"
#include "util/u_memory.h"

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "wsl/winadapter.h"
#endif
#include "d3dkmthk.h"

static const bool all_resident = true;

static struct util_vma_heap *
radv_wddm2_bo_heap(struct radv_wddm2_winsys *ws, enum radeon_bo_flag flags)
{
   if (flags & RADEON_FLAG_32BIT)
      return &ws->_32bit_heap;
   else if (flags & RADEON_FLAG_REPLAYABLE)
      return &ws->replay_heap;
   else
      return &ws->heap;
}

static uint64_t
radv_wddm2_bo_va_alloc(struct radv_wddm2_winsys *ws, enum radeon_bo_flag flags,
                       uint64_t size, uint32_t alignment)
{
   struct util_vma_heap *heap = radv_wddm2_bo_heap(ws, flags);

   simple_mtx_lock(&ws->heap_mtx);
   uint64_t addr = util_vma_heap_alloc(heap, size, alignment);
   simple_mtx_unlock(&ws->heap_mtx);

   return addr;
}

static void
radv_wddm2_bo_va_free(struct radv_wddm2_winsys *ws, enum radeon_bo_flag flags,
                      uint64_t addr, uint64_t size)
{
   struct util_vma_heap *heap = radv_wddm2_bo_heap(ws, flags);

   simple_mtx_lock(&ws->heap_mtx);
   util_vma_heap_free(heap, addr, size);
   simple_mtx_unlock(&ws->heap_mtx);
}

#pragma pack(push, 4)
struct create_alloc_pdata {
   uint32_t adapter_id;
   uint32_t _dw1;
   uint32_t flags; // 0x80
   uint32_t checksum;
   uint32_t reserved[11];
   uint32_t pdata_size;
};

struct alloc_header {
   uint32_t entry_size[4];
   uint32_t adapter_id;
   uint32_t checksum;
   uint32_t reserved[9];
   uint32_t num_entries;
};

struct alloc_bo_info {
   uint32_t section_size;
   uint32_t _dw1;
   uint32_t flags;
   uint32_t size;
   uint32_t alignment;
   uint32_t priority;
   uint8_t heaps[4];
   uint32_t _unknown[16];
   uint32_t flags2;
   uint64_t phys_size;
   uint64_t va_addr;
   uint64_t va_size;
   uint32_t reserved[2];
};

struct alloc_surf {
   uint32_t section_size;
   uint32_t flags;
   uint32_t swizzle_mode; // linear = 0x20
   uint32_t resource_type;
   uint32_t format;
   uint32_t width;
   uint32_t height;
   uint32_t width_in_texels;
   uint32_t height_in_texels;
   uint32_t depth;
   uint32_t slice_size;
   uint32_t _unknown0[16];
   uint32_t width2;
   uint32_t height2;
   uint32_t depth2;
   uint32_t size;
   uint32_t _unknown1[41];
};

struct alloc_metadata  {
   uint32_t section_size;
   uint32_t _pad;
   uint64_t va_addr;
   uint64_t va_size;
   uint32_t _unknown0[45];
   uint32_t mtype;
   uint32_t flags;
   uint32_t mall_policy;
   uint32_t mall_range[2];
   uint32_t _unknown2[13];
};

struct alloc_entry {
   uint32_t entry_size;
   uint32_t enabled_sections;
   struct alloc_bo_info bo_info;
   uint32_t num_planes;
   uint32_t _unknown0[2];
   uint16_t samples;
   uint16_t mask;
   uint32_t mip_levels;
   uint32_t layers;
   uint32_t _unknown1[4];
   uint32_t metadata_offset;
   uint32_t _unknown2[2];
   uint32_t version;
   uint32_t _unknown3;
};
#pragma pack(pop)

static inline uint32_t
calculate_checksum(const uint32_t *data, size_t dword_count)
{
   uint32_t acc[8] = {0};

   for (size_t i = 0; i < dword_count; i++)
      acc[i % 8] += i ^ data[i];
   return acc[0] + acc[1] + acc[2] + acc[3] + acc[4] + acc[5] + acc[6] + acc[7];
}

enum alloc_entry_section {
   ALLOC_SECTION_METADATA = 0x1,
   ALLOC_SECTION_SURF = 0xc,
};

enum alloc_flags {
   ALLOC_FLAG_NOT_VIRTUAL = 0x2000,
   ALLOC_FLAG_UDMA_BUFFER = 0x40000,
   ALLOC_FLAG_HOST_ALLOCATED = 0x4000000,
   ALLOC_FLAG_CPU_VISIBLE = 0x20000000,
   ALLOC_FLAG_SHARED = 0x80000000,
};

enum alloc_heap {
   ALLOC_HEAP_LOCAL = 0,
   ALLOC_HEAP_INVISIBLE = 1,
   ALLOC_HEAP_GART_USWC = 2,
   ALLOC_HEAP_GART_CACHEABLE = 3,
};

#define ADD_HEAP(heap) \
   do { \
      bo_info->flags |= 1 << ALLOC_HEAP_##heap; \
      bo_info->heaps[heap_count++] = ALLOC_HEAP_##heap + 1; \
   } while (0)

static void
fill_alloc_heaps(struct radv_wddm2_winsys *ws, enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                 bool host_allocated, struct alloc_bo_info *bo_info)
{
   uint32_t heap_count = 0;

   if (initial_domain & RADEON_DOMAIN_VRAM) {
      assert(!host_allocated);
      if (!(flags & RADEON_FLAG_CPU_ACCESS))
         ADD_HEAP(INVISIBLE);
      if (!(flags & RADEON_FLAG_NO_CPU_ACCESS))
         ADD_HEAP(LOCAL);
   }

   if (initial_domain & RADEON_DOMAIN_GTT || initial_domain == 0) {
      if (!(flags & RADEON_FLAG_GTT_WC))
         ADD_HEAP(GART_CACHEABLE);
      if (!host_allocated)
         ADD_HEAP(GART_USWC);
   }
}

#define ADD_SECTION(section) \
   do { \
      section = (struct alloc_##section *)pdata; \
      pdata += sizeof(struct alloc_##section); \
      entry_size += sizeof(struct alloc_##section); \
   } while (0)

#define ADD_OPT_SECTION(section, type) \
   do { \
      entry->enabled_sections |= ALLOC_SECTION_##type; \
      ADD_SECTION(section); \
      section->section_size = sizeof(struct alloc_##section); \
   } while (0)

static uint32_t
fill_alloc_pdata(struct radv_wddm2_winsys *ws, uint64_t size, unsigned alignment,
                 enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                 unsigned priority, uint64_t address, void *cpu_ptr, uint8_t *pdata)
{
   struct alloc_header *header = (struct alloc_header *)pdata;
   struct alloc_entry *entry;
   struct alloc_surf *surf;
   struct alloc_metadata *metadata;
   uint32_t entry_size = 0;

   pdata += sizeof(struct alloc_header);
   ADD_SECTION(entry);

   // BO info
   entry->bo_info.section_size = sizeof(struct alloc_bo_info);
   entry->bo_info.flags = (flags & RADEON_FLAG_VIRTUAL) ? 0 : ALLOC_FLAG_NOT_VIRTUAL;
   if (!(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING))
      entry->bo_info.flags |= ALLOC_FLAG_SHARED;
   if (!(flags & RADEON_FLAG_NO_CPU_ACCESS))
      entry->bo_info.flags |= ALLOC_FLAG_CPU_VISIBLE;
   if (cpu_ptr)
      entry->bo_info.flags |= ALLOC_FLAG_HOST_ALLOCATED;
   entry->bo_info.size = size;
   entry->bo_info.alignment = alignment;
   entry->bo_info.priority = 0x5;
   if (cpu_ptr)
      entry->bo_info.priority |= 0x800;
   entry->bo_info.phys_size = size;
   entry->bo_info.va_size = size;
   entry->bo_info.va_addr = address;
   entry->bo_info.flags2 = 0x1000000;
   fill_alloc_heaps(ws, initial_domain, flags, cpu_ptr != NULL, &entry->bo_info);

   if (!(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING)) {
      entry->num_planes = 1;
      entry->mip_levels = 1;
      entry->layers = 1;
      entry->samples = 1;

      ADD_OPT_SECTION(surf, SURF);
      surf->flags = 0x20000; // typed?
      surf->swizzle_mode = 0x20; // linear
      surf->resource_type = 1; // 1D
      surf->width = size;
      surf->height = 1;
      surf->width_in_texels = size;
      surf->height_in_texels = 1;
      surf->depth = 1;
      surf->slice_size = size;
      surf->width2 = size;
      surf->height2 = 1;
      surf->depth2 = 1;
      surf->size = size;
   }

   // Metadata
   entry->metadata_offset = entry_size;
   ADD_OPT_SECTION(metadata, METADATA);
   metadata->va_addr = address;
   metadata->va_size = size;
   if (flags & RADEON_FLAG_NO_CPU_ACCESS)
      metadata->flags |= 0x2000; // no CPU access
   if (flags & RADEON_FLAG_GL2_BYPASS) {
      metadata->mtype = 0x4; // L2_UNCACHED;
      metadata->mall_policy = 0x1; // never
   }

   entry->version = 0x9;
   entry->entry_size = entry_size;

   header->entry_size[0] = entry_size;
   header->adapter_id = 0;
   header->num_entries = 1;
   header->checksum = calculate_checksum((const uint32_t *)header, sizeof(*header) / 4);

   return entry_size + sizeof(*header);
}

static uint64_t
radv_wddm2_get_optimal_vm_alignment(struct radv_wddm2_winsys *ws, uint64_t size, unsigned alignment)
{
   uint64_t vm_alignment = alignment;

   /* Increase the VM alignment for faster address translation. */
   if (size >= ws->gpu_info.pte_fragment_size)
      vm_alignment = MAX2(vm_alignment, ws->gpu_info.pte_fragment_size);

   /* Gfx9: Increase the VM alignment to the most significant bit set
    * in the size for faster address translation.
    */
   if (ws->gpu_info.gfx_level >= GFX9) {
      unsigned msb = util_last_bit64(size); /* 0 = no bit is set */
      uint64_t msb_alignment = msb ? 1ull << (msb - 1) : 0;

      vm_alignment = MAX2(vm_alignment, msb_alignment);
   }
   return vm_alignment;
}

static uint64_t
radv_wddm2_reserve_va_range(struct radv_wddm2_winsys *ws, uint64_t size, unsigned alignment,
                            enum radeon_bo_flag flags)
{
   D3DGPU_VIRTUAL_ADDRESS min;
   NTSTATUS status;
   
   if (flags & RADEON_FLAG_32BIT)
      min = RADV_WDDM2_32BIT_HEAP_START;
   else if (flags & RADEON_FLAG_REPLAYABLE)
      min = RADV_WDDM2_REPLAY_HEAP_START;
   else
      min = RADV_WDDM2_HEAP_START;

   D3DDDI_RESERVEGPUVIRTUALADDRESS reserve = {
      .hPagingQueue = ws->paging_queue_h,
      .MinimumAddress = min,
      .MaximumAddress = min + (1ull << 32),
      .Size = size,
   };
   status = WDDM2_DISPATCH(ReserveGpuVirtualAddress(&reserve));
   if (!NT_SUCCESS(status))
      return 0;

   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .BaseAddress = reserve.VirtualAddress,
      .SizeInPages = size / 4096,
      .Protection = {
         .Zero = 1,
      },
   };
   status = WDDM2_DISPATCH(MapGpuVirtualAddress(&map));
   if (!NT_SUCCESS(status))
      return 0;

   return reserve.VirtualAddress;
}

static VkResult
radv_wddm2_virtual_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                             enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                             unsigned priority, uint64_t address, struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   VkResult result;

   /* Courtesy for users using NULL to check if they need to destroy the BO. */
   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = initial_domain;
   bo->ws = ws;
   bo->flags = flags;
   bo->base.is_virtual = true;
   bo->base.va = radv_wddm2_reserve_va_range(ws, size, alignment, flags);
   if (bo->base.va == 0) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_reserve;
   }

   *out_bo = &bo->base;
   return VK_SUCCESS;

error_va_reserve:
   FREE(bo);
   return result;
}

static VkResult
radv_wddm2_bo_create_internal(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                              enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                              unsigned priority, uint64_t address, void *cpu_ptr,
                              struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   NTSTATUS status;
   VkResult result;

   /* Courtesy for users using NULL to check if they need to destroy the BO. */
   *out_bo = NULL;

   /* The contract has no userptr allocation. A normal BO would be the wrong object. */
   if (ws->bc250 && cpu_ptr)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = initial_domain;
   bo->ws = ws;
   bo->flags = flags;

   uint32_t phys_alignment = MAX2(alignment, 0x1000);
   if (initial_domain & RADEON_DOMAIN_VRAM) {
      if (size >= 0x10000)
         phys_alignment = MAX2(phys_alignment, 0x10000);
      if (size >= 0x40000)
         phys_alignment = MAX2(phys_alignment, 0x40000);
   }

   uint32_t virt_alignment = phys_alignment;
   if (size >= ws->gpu_info.pte_fragment_size)
      virt_alignment = MAX2(virt_alignment, ws->gpu_info.pte_fragment_size);
   const uint64_t phys_size = align64(size, phys_alignment);

   uint8_t alloc_pdata[824] = {0};
   uint32_t pdata_size;
   if (ws->bc250) {
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
   } else {
      pdata_size = fill_alloc_pdata(ws, phys_size, phys_alignment, initial_domain,
                                    flags, priority, address, cpu_ptr, alloc_pdata);
   }

   D3DDDI_ALLOCATIONINFO2 alloc_info = {
      .pSystemMem = cpu_ptr,
      .pPrivateDriverData = alloc_pdata,
      .PrivateDriverDataSize = pdata_size,
      .VidPnSourceId = 0xffffffff,
      .Priority = D3DDDI_ALLOCATIONPRIORITY_NORMAL,
   };

   struct create_alloc_pdata create_pdata = {
      .adapter_id = 0,
      .flags = 0x80,
      .pdata_size = pdata_size,
   };
   create_pdata.checksum =
      calculate_checksum((const uint32_t *)&create_pdata, sizeof(create_pdata) / 4);

   D3DKMT_CREATEALLOCATION create = {
      .hDevice = ws->device_h,
      .pPrivateDriverData = &create_pdata,
      .PrivateDriverDataSize = sizeof(create_pdata),
      .NumAllocations = 1,
      .pAllocationInfo2 = &alloc_info,
      .Flags = {
         .CreateResource = 1,
         .CreateShared = !(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING),
         .NonSecure = 1,
      }
   };

#if 0
   fprintf(stdout, "pArgs (%zi bytes):\n", sizeof(create));
   print_hex_data(stdout, &create, sizeof(create));
   fprintf(stdout, "pPrivateDriverData (%zi bytes):\n", sizeof(create_pdata));
   print_hex_data(stdout, &create_pdata, sizeof(create_pdata));
   fprintf(stdout, "child (%zi bytes):\n", sizeof(alloc_info));
   print_hex_data(stdout, &alloc_info, sizeof(alloc_info));
   fprintf(stdout, "child.pPrivateDriverData (%i bytes):\n", pdata_size);
   print_hex_data(stdout, alloc_pdata, pdata_size);
   fflush(stdout);
#endif

   /*bo->base.va = radv_wddm2_bo_va_alloc(ws, flags, phys_size, virt_alignment);
   if (bo->base.va == 0) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_create;
   }*/

   status = WDDM2_DISPATCH(CreateAllocation2(&create));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "CreateAllocation2 failed 0x%X\n", status);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_ptr_alloc;
   }

   bo->base.obj_id = alloc_info.hAllocation;
   bo->base.size = phys_size;
   bo->base.handle = alloc_info.hAllocation;
   fprintf(stderr, "allocation handle=0x%x\n", bo->base.handle);

   const D3DKMT_DESTROYALLOCATION2 destroy = {
      .hDevice = ws->device_h,
      .phAllocationList = &bo->base.handle,
      .AllocationCount = 1,
   };

   D3DGPU_VIRTUAL_ADDRESS min = flags & RADEON_FLAG_32BIT ? RADV_WDDM2_32BIT_HEAP_START : RADV_WDDM2_HEAP_START;
   if (flags & RADEON_FLAG_REPLAYABLE)
      min = RADV_WDDM2_REPLAY_HEAP_START;
   D3DGPU_VIRTUAL_ADDRESS max = flags & RADEON_FLAG_32BIT ? RADV_WDDM2_HEAP_START : RADV_WDDM2_REPLAY_HEAP_START;
   if (flags & RADEON_FLAG_REPLAYABLE)
      max = RADV_WDDM2_REPLAY_HEAP_START + (4ull << 32);
   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .BaseAddress = address, //0, //bo->base.va == 0x100002000 ? 0 : bo->base.va,
      .MinimumAddress = min,
      .MaximumAddress = max,
      .hAllocation = bo->base.handle,
      .SizeInPages = phys_size / 4096,
      .Protection = {
         .Write = !(flags & RADEON_FLAG_READ_ONLY),
      },
   };
   status = WDDM2_DISPATCH(MapGpuVirtualAddress(&map));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "mapping 0x%" PRIx64 " failed: 0x%X\n", bo->base.va, status);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }
   bo->base.va = map.VirtualAddress;

   uint64_t paging_fence_value = map.PagingFenceValue;

   if (all_resident) {
      D3DDDI_MAKERESIDENT make_resident = {
         .hPagingQueue = ws->paging_queue_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags = {
            .MustSucceed = 1,
         },
      };
      status = WDDM2_DISPATCH(MakeResident(&make_resident));
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "MakeResident failed\n");
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto error_va_alloc;
      }

      paging_fence_value = make_resident.PagingFenceValue;
   }

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &ws->paging_fence_h,
      .FenceValueArray = &paging_fence_value,
   };
   status = WDDM2_DISPATCH(WaitForSynchronizationObjectFromCpu(&wait));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "WaitForSynchronizationObjectFromCpu failed\n");
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }

   if (ws->debug_all_bos)
      radv_winsys_bo_list_add(&ws->global_bo_list, &bo->base);
   if (ws->debug_log_bos)
      radv_winsys_log_bo(&ws->bo_log, &bo->base, false);

   *out_bo = (struct radeon_winsys_bo *)bo;
   return VK_SUCCESS;

error_va_alloc:
   //radv_wddm2_bo_va_free(ws, flags, bo->base.va, bo->base.size);

   fprintf(stderr, "destroy allocation\n");
   status = WDDM2_DISPATCH(DestroyAllocation2(&destroy));
   assert(NT_SUCCESS(status));

error_ptr_alloc:
   fprintf(stderr, "free va\n");
   FREE(bo);
   return result;
}

static VkResult
radv_wddm2_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                     enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags,
                     unsigned priority, uint64_t address, struct radv_image *image,
                     struct radeon_winsys_bo **out_bo)
{
   if (flags & RADEON_FLAG_VIRTUAL)
      return radv_wddm2_virtual_bo_create(_ws, size, alignment, initial_domain, flags,
                                          priority, address, out_bo);
   return radv_wddm2_bo_create_internal(_ws, size, alignment, initial_domain, flags, priority, address, NULL, out_bo);
}

static VkResult
radv_wddm2_bo_from_ptr(struct radeon_winsys *_ws, void *pointer, uint64_t size, unsigned priority,
                       struct radeon_winsys_bo **out_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   enum radeon_bo_domain initial_domain = RADEON_DOMAIN_GTT;
   enum radeon_bo_flag flags = RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_CPU_ACCESS;
   unsigned alignment = ws->gpu_info.gart_page_size;

   return radv_wddm2_bo_create_internal(_ws, size, alignment, initial_domain, flags, priority, 0, pointer, out_bo);
}

static bool
radv_wddm2_bo_get_handle(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, void **handle)
{
   return false;
}

static VkResult
radv_wddm2_bo_from_handle(struct radeon_winsys *_ws, void *handle, unsigned priority,
                           struct radeon_winsys_bo **out_bo, uint64_t *alloc_size)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo;
   NTSTATUS status;
   VkResult result;

   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_wddm2_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->base.initial_domain = RADEON_DOMAIN_VRAM;
   bo->ws = ws;
   bo->flags = 0;

   /* Query resource info to determine private data sizes */
   D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query_info = {
      .hDevice = ws->device_h,
      .hNtHandle = (HANDLE)handle,
   };
   status = WDDM2_DISPATCH(QueryResourceInfoFromNtHandle(&query_info));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "QueryResourceInfoFromNtHandle failed 0x%X\n", status);
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error_alloc;
   }

   /* Allocate buffer for private driver data */
   void *pdata = calloc(1, query_info.TotalPrivateDriverDataSize + query_info.PrivateRuntimeDataSize +
                           query_info.NumAllocations * sizeof(D3DDDI_OPENALLOCATIONINFO2));
   if (!pdata) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto error_alloc;
   }
   void *res_pdata = (uint8_t *)pdata + query_info.TotalPrivateDriverDataSize;
   void *runtime_data = (uint8_t *)res_pdata + query_info.ResourcePrivateDriverDataSize;
   D3DDDI_OPENALLOCATIONINFO2 *alloc_info = (D3DDDI_OPENALLOCATIONINFO2 *)((uint8_t *)res_pdata + query_info.PrivateRuntimeDataSize);

   D3DKMT_OPENRESOURCEFROMNTHANDLE open_resource = {
      .hDevice = ws->device_h,
      .hNtHandle = (HANDLE)handle,
      .NumAllocations = query_info.NumAllocations,
      .pOpenAllocationInfo2 = alloc_info,
      .TotalPrivateDriverDataBufferSize = query_info.TotalPrivateDriverDataSize,
      .pTotalPrivateDriverDataBuffer = pdata,
      .ResourcePrivateDriverDataSize = query_info.ResourcePrivateDriverDataSize,
      .pResourcePrivateDriverData = res_pdata,
      .PrivateRuntimeDataSize = query_info.PrivateRuntimeDataSize,
      .pPrivateRuntimeData = runtime_data,
   };
   status = WDDM2_DISPATCH(OpenResourceFromNtHandle(&open_resource));
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error_import;
   }

   struct alloc_entry *entry = (struct alloc_entry *)((uint8_t *) pdata + sizeof(struct alloc_header));
   struct alloc_surf *surf = (struct alloc_surf *)((uint8_t *)entry + sizeof(struct alloc_entry));

   bo->base.obj_id = bo->base.handle = alloc_info[0].hAllocation;
   bo->base.size = entry->bo_info.phys_size;

   bo->md.u.gfx9.swizzle_mode = surf->swizzle_mode;
   bo->md.metadata_type = RADEON_METADATA_TYPE_KMW;
   bo->md.kmw.pitch_bytes = surf->width;
   bo->md.kmw.surf_size = surf->slice_size;

   /* Map the opened allocation into GPU virtual address space */
   D3DDDI_MAPGPUVIRTUALADDRESS map = {
      .hPagingQueue = ws->paging_queue_h,
      .MinimumAddress = RADV_WDDM2_HEAP_START,
      .MaximumAddress = RADV_WDDM2_REPLAY_HEAP_START,
      .hAllocation = bo->base.handle,
      .Protection = {
         .Write = 1,
      },
   };
   status = WDDM2_DISPATCH(MapGpuVirtualAddress(&map));
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_import;
   }
   bo->base.va = map.VirtualAddress;

   if (alloc_size)
      *alloc_size = bo->base.size;

   /* Make the allocation resident */
   D3DDDI_MAKERESIDENT make_resident = {
      .hPagingQueue = ws->paging_queue_h,
      .NumAllocations = 1,
      .AllocationList = &bo->base.handle,
      .Flags = {
         .MustSucceed = 1,
      },
   };
   status = WDDM2_DISPATCH(MakeResident(&make_resident));
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_map;
   }

   /* Wait for the paging operation to complete */
   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = ws->device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &ws->paging_fence_h,
      .FenceValueArray = &make_resident.PagingFenceValue,
   };
   status = WDDM2_DISPATCH(WaitForSynchronizationObjectFromCpu(&wait));
   if (!NT_SUCCESS(status)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_map;
   }

   free(pdata);
   *out_bo = &bo->base;
   return VK_SUCCESS;

error_map:
   {
      const D3DKMT_FREEGPUVIRTUALADDRESS unmap = {
         .hAdapter = ws->adapter_h,
         .BaseAddress = bo->base.va,
         .Size = bo->base.size,
      };
      WDDM2_DISPATCH(FreeGpuVirtualAddress(&unmap));
   }
error_import:
   {
      const D3DKMT_DESTROYALLOCATION2 destroy = {
         .hDevice = ws->device_h,
         .phAllocationList = &bo->base.handle,
         .AllocationCount = 1,
      };
      WDDM2_DISPATCH(DestroyAllocation2(&destroy));
      free(pdata);
   }
error_alloc:
   FREE(bo);
   return result;
}

static bool
radv_wddm2_bo_get_flags_from_handle(struct radeon_winsys *_ws, void *handle,
                                    enum radeon_bo_domain *domains,
                                    enum radeon_bo_flag *flags)
{
   *domains = RADEON_DOMAIN_VRAM;
   *flags = RADEON_FLAG_CPU_ACCESS;

   return true;
}

static void
radv_wddm2_bo_get_metadata(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                           struct radeon_bo_metadata *md)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);

   memcpy(md, &bo->md, sizeof(*md));
}

static void *
radv_wddm2_bo_map(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                  bool use_fixed_addr, void *fixed_addr)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   ASSERTED NTSTATUS status;

   if (bo->map && !fixed_addr)
      return bo->map;

   if (bo->flags & RADEON_FLAG_NO_CPU_ACCESS) {
      fprintf(stderr, "attempt to map non-CPU-accessible BO\n");
      return NULL;
   }

   D3DKMT_LOCK2 lock = {
      .hDevice = bo->ws->device_h,
      .hAllocation = bo->base.handle,
   };
   status = WDDM2_DISPATCH(Lock2(&lock));
   if (!NT_SUCCESS(status))
      return NULL;

   bo->map = lock.pData;

   return lock.pData;
}

static void
radv_wddm2_bo_unmap(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, bool replace)
{
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   ASSERTED NTSTATUS status;

   if (bo->map == NULL)
      return;

   const D3DKMT_UNLOCK2 unlock = {
      .hDevice = bo->ws->device_h,
      .hAllocation = bo->base.handle,
   };
   status = WDDM2_DISPATCH(Unlock2(&unlock));
   assert(NT_SUCCESS(status));
   
   bo->map = NULL;
}

static VkResult
radv_wddm2_bo_make_resident(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                            bool resident)
{
   if (all_resident)
      return VK_SUCCESS;

   //fprintf(stderr, "make resident \n");
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   NTSTATUS status;

   if (resident) {
      D3DDDI_MAKERESIDENT make_resident = {
         .hPagingQueue = ws->paging_queue_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags = {
            .MustSucceed = 1,
         },
      };
      status = D3DKMTMakeResident(&make_resident);
      if (!NT_SUCCESS(status))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;

      const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
         .hDevice = ws->device_h,
         .ObjectCount = 1,
         .ObjectHandleArray = &ws->paging_fence_h,
         .FenceValueArray = &make_resident.PagingFenceValue,
      };
      status = D3DKMTWaitForSynchronizationObjectFromCpu(&wait);
      if (!NT_SUCCESS(status))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   } else {
      D3DKMT_EVICT evict = {
         .hDevice = ws->device_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags.EvictOnlyIfNecessary = false,
      };
      status = WDDM2_DISPATCH(Evict(&evict));
      if (!NT_SUCCESS(status))
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   return VK_SUCCESS;
}

static void
radv_wddm2_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_bo *bo = radv_wddm2_bo(_bo);
   ASSERTED NTSTATUS status;

   if (all_resident && !bo->base.is_virtual) {
      D3DKMT_EVICT evict = {
         .hDevice = ws->device_h,
         .NumAllocations = 1,
         .AllocationList = &bo->base.handle,
         .Flags.EvictOnlyIfNecessary = false,
      };
      status = WDDM2_DISPATCH(Evict(&evict));
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "*****  Evict failed\n");
         return;
      }
   }

   radv_wddm2_bo_unmap(_ws, _bo, false);

   const D3DKMT_FREEGPUVIRTUALADDRESS unmap = {
      .hAdapter = ws->adapter_h,
      .BaseAddress = bo->base.va,
      .Size = bo->base.size,
   };
   status = WDDM2_DISPATCH(FreeGpuVirtualAddress(&unmap));

   if (!bo->base.is_virtual) {
      const D3DKMT_DESTROYALLOCATION2 destroy = {
         .hDevice = ws->device_h,
         .phAllocationList = &bo->base.handle,
         .AllocationCount = 1,
      };
      status = WDDM2_DISPATCH(DestroyAllocation2(&destroy));
      //assert(NT_SUCCESS(status));

      if (ws->debug_all_bos)
         radv_winsys_bo_list_del(&ws->global_bo_list, &bo->base);
      if (ws->debug_log_bos)
         radv_winsys_log_bo(&ws->bo_log, &bo->base, true);
   }

   //radv_wddm2_bo_va_free(ws, bo->flags, bo->base.va, bo->base.size);

   FREE(bo);
}
static VkResult
radv_wddm2_virtual_bo_map(struct radv_wddm2_winsys *ws, struct radv_wddm2_ctx *ctx,
                            enum amd_ip_type ip_type, struct radv_wddm2_bo *parent,
                            uint64_t offset, uint64_t size, struct radv_wddm2_bo *bo,
                            uint64_t bo_offset)
{
   D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op = {
      .OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT,
      .MapProtect = {
         .BaseAddress = parent->base.va + offset,
         .SizeInBytes = size,
         .hAllocation = bo->base.handle,
         .AllocationOffsetInBytes = bo_offset,
         .AllocationSizeInBytes = size,
         .Protection = {
            .Write = 1,
         },
         .DriverProtection = 1,
      },
   };

   const D3DKMT_UPDATEGPUVIRTUALADDRESS update = {
      .hDevice = ws->device_h,
      .hContext = ctx->per_ip[ip_type].queue.context_h,
      .hFenceObject = ctx->per_ip[ip_type].queue.vm_fence.handle,
      .FenceValue = ctx->per_ip[ip_type].queue.vm_fence.wait_value,
      .Operations = &op,
      .NumOperations = 1,
   };

   NTSTATUS status = WDDM2_DISPATCH(UpdateGpuVirtualAddress(&update));
   if (!NT_SUCCESS(status)) {
      fprintf(stdout, "mapping 0x%" PRIx64 " failed: 0x%X\n", parent->base.va + offset, status);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   return VK_SUCCESS;
}

static VkResult
radv_wddm2_virtual_bo_unmap(struct radv_wddm2_winsys *ws, struct radv_wddm2_ctx *ctx,
                            enum amd_ip_type ip_type, struct radv_wddm2_bo *parent,
                            uint64_t offset, uint64_t size)
{
   D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op = {
      .OperationType = D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP,
      .Unmap = {
         .BaseAddress = parent->base.va + offset,
         .SizeInBytes = size,
         .Protection = {
            .Zero = 1,
         }
      },
   };

   const D3DKMT_UPDATEGPUVIRTUALADDRESS update = {
      .hDevice = ws->device_h,
      .hContext = ctx->per_ip[ip_type].queue.context_h,
      .hFenceObject = ctx->per_ip[ip_type].queue.vm_fence.handle,
      .FenceValue = ctx->per_ip[ip_type].queue.vm_fence.wait_value,
      .Operations = &op,
      .NumOperations = 1,
   };

   NTSTATUS status = WDDM2_DISPATCH(UpdateGpuVirtualAddress(&update));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "mapping 0x%" PRIx64 " failed: 0x%X\n", parent->base.va + offset, status);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   return VK_SUCCESS;
}

static VkResult
radv_wddm2_bo_virtual_bind(struct radeon_winsys *_ws, struct radeon_winsys_ctx *_ctx, enum amd_ip_type ip_type,
                           struct radeon_winsys_bo *_parent, uint64_t offset, uint64_t size,
                           struct radeon_winsys_bo *_bo, uint64_t bo_offset)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);
   struct radv_wddm2_ctx *ctx = (struct radv_wddm2_ctx *)_ctx;
   struct radv_wddm2_bo *parent = (struct radv_wddm2_bo *)_parent;
   struct radv_wddm2_bo *bo = (struct radv_wddm2_bo *)_bo;
   VkResult ret;

   assert(parent->base.is_virtual);
   assert(!bo || !bo->base.is_virtual);

   if (bo) {
      ret = radv_wddm2_virtual_bo_map(ws, ctx, ip_type, parent, offset, size, bo, bo_offset);
   } else {
      ret = radv_wddm2_virtual_bo_unmap(ws, ctx, ip_type, parent, offset, size);
   }

   return ret;
}

static void
radv_wddm2_dump_bo_log(struct radeon_winsys *_ws, FILE *file)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (!ws->debug_log_bos)
      return;

   radv_winsys_dump_bo_log(&ws->bo_log, file);
}

static void
radv_wddm2_dump_bo_ranges(struct radeon_winsys *_ws, FILE *file)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   fprintf(stderr, "dump bo ranges\n");
   if (ws->debug_all_bos)
      radv_winsys_dump_bo_ranges(&ws->global_bo_list, file);
   else
      fprintf(file, "  To get BO VA ranges, please specify RADV_DEBUG=allbos\n");
}

void
radv_wddm2_bo_init_functions(struct radv_wddm2_winsys *ws)
{
   ws->base.buffer_create = radv_wddm2_bo_create;
   ws->base.buffer_destroy = radv_wddm2_bo_destroy;
   ws->base.buffer_map = radv_wddm2_bo_map;
   ws->base.buffer_unmap = radv_wddm2_bo_unmap;
   ws->base.buffer_make_resident = radv_wddm2_bo_make_resident;
   ws->base.buffer_from_ptr = radv_wddm2_bo_from_ptr;
   ws->base.buffer_get_handle = radv_wddm2_bo_get_handle;
   ws->base.buffer_from_handle = radv_wddm2_bo_from_handle;
   ws->base.buffer_get_flags_from_handle = radv_wddm2_bo_get_flags_from_handle;
   ws->base.buffer_get_metadata = radv_wddm2_bo_get_metadata;
   ws->base.buffer_virtual_bind = radv_wddm2_bo_virtual_bind;
   ws->base.dump_bo_ranges = radv_wddm2_dump_bo_ranges;
   ws->base.dump_bo_log = radv_wddm2_dump_bo_log;
}
