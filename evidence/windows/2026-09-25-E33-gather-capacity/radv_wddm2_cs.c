/*
 * Copyright © 2020 Valve Corporation
 *
 * based on amdgpu winsys.
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include "radv_wddm2_cs.h"
#include "sid.h"
#include "radv_wddm2_bc250.h"
#include "radv_wddm2_bo.h"
#include "radv_winsys_cs.h"
#include "vk_async_event.h"
#include "vk_util.h"
#include "vk_wddm2_monitored_fence.h"
#include "util/macros.h"
#include "util/set.h"
#include "util/u_memory.h"
#include <stdlib.h>
#include "ac_debug.h"
#include "radv_cs.h"

#include "amd/common/sid.h"

/* Xlib headers conflict with DXGI headers */
#ifdef Status
#undef Status
#endif

/* Windows headers need to be included dead last because they have lots of
 * #defines which may mess with other included headers.
 */

#ifdef _WIN32
#include <windows.h>
#include <immintrin.h>
#else
#include "wsl/winadapter.h"
#endif

#include <assert.h>
#include <d3dkmthk.h>

struct PACKED create_context_private_data {
   uint32_t header_size;
   uint32_t flags;
   uint32_t reserved[11];
   uint32_t version;
   uint32_t dw13;
   uint32_t dw14;
   uint32_t section_size;
   uint32_t reserved2[89];
};
static_assert(sizeof(struct create_context_private_data) == 0x40 + 0x168, "This struct has no holes");

struct PACKED create_queue_private_data {
   uint32_t header_size;
   uint32_t reserved[3];
   uint32_t flags;
   uint32_t queue_id;
   uint32_t parent_queue_id;
   uint32_t reserved2[9];
};
static_assert(sizeof(struct create_queue_private_data) == 0x40, "This struct has no holes");

struct submit_pdd_writer {
   uint32_t num_entries;
   uint8_t *buffer;
   unsigned buffer_size;
   unsigned offset;
   uint8_t inline_data[256];
};

struct submit_pdd_header {
   uint32_t version;
   uint32_t num_entries;
};

struct submit_pdd_entry {
   uint32_t type;
   uint32_t size;
};

struct submit_pdd_gfx_ib {
   struct submit_pdd_entry base;
   uint32_t len;
   uint32_t flags;
   uint32_t _dw_2;
   uint32_t ip_type;
   uint32_t addr_lo;
   uint32_t addr_hi;
   uint32_t _dw_5;
   uint32_t _dw_7;
   uint32_t _dw_8;
   uint32_t _dw_9;
   uint32_t _dw_a;
   uint32_t _dw_b;
   uint32_t _dw_c;
   uint32_t _dw_d;
};
static_assert(sizeof(struct submit_pdd_gfx_ib) == 64, "This struct has no holes");

struct submit_pdd_gang_queue {
   struct submit_pdd_entry base;
   uint32_t always_1;
   uint32_t queue_id;
   uint32_t num_ibs;
   uint32_t _dw3;
};

struct submit_pdd_gang_ib {
   uint32_t len;
   uint32_t flags;
   uint32_t addr_lo;
   uint32_t addr_hi;
};

enum submit_pdd_entry_type {
   SUBMIT_PDD_ENTRY_GFX_IB = 0,
   SUBMIT_PDD_ENTRY_GANG_QUEUE = 6,
};

static void
submit_pdd_writer_init(struct submit_pdd_writer *writer)
{
   writer->num_entries = 0;
   writer->buffer = writer->inline_data;
   writer->buffer_size = sizeof(writer->inline_data);
   writer->offset = sizeof(struct submit_pdd_header);
}

static void
submit_pdd_writer_finalize(struct submit_pdd_writer *writer)
{
   struct submit_pdd_header *header = (struct submit_pdd_header *)writer->buffer;
   header->version = 0x9;
   header->num_entries = writer->num_entries;
}

static void
submit_pdd_writer_destroy(struct submit_pdd_writer *writer)
{
   if (writer->buffer != writer->inline_data)
      free(writer->buffer);
}

static void *
submit_pdd_writer_reserve(struct submit_pdd_writer *writer, unsigned size)
{
   if (writer->offset + size > writer->buffer_size) {
      uint8_t *old_buffer = writer->buffer;
      writer->buffer_size = MAX2(writer->buffer_size * 2, writer->offset + size);
      writer->buffer = malloc(writer->buffer_size);
      if (!writer->buffer)
         return NULL;
      memcpy(writer->buffer, old_buffer, writer->offset);
      if (old_buffer != writer->inline_data)
         free(old_buffer);
   }
   void *ptr = writer->buffer + writer->offset;
   memset(ptr, 0, size);
   writer->offset += size;
   return ptr;
}

static bool vk_wddm2_fence_wait(uint32_t device_h, struct vk_wddm2_fence *fence);

static void
radv_wddm2_queue_destroy(struct radv_wddm2_queue *queue)
{
   radv_wddm2_sparse_groups_clear(queue);
   util_dynarray_fini(&queue->sparse_ops);
   if (queue->bc250_progress.handle) {
      if (queue->bc250_progress.wait_value)
         vk_wddm2_fence_wait(queue->bc250_ws->device_h, &queue->bc250_progress);
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy_progress = {
         .hSyncObject = queue->bc250_progress.handle,
      };
      WDDM2_DISPATCH(DestroySynchronizationObject(&destroy_progress));
      queue->bc250_progress.handle = 0;
   }
   if (queue->bc250_gather) {
      queue->bc250_ws->base.buffer_destroy(&queue->bc250_ws->base, queue->bc250_gather);
      queue->bc250_gather = NULL;
      queue->bc250_gather_map = NULL;
   }
   if (queue->vm_fence.handle) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy_fence = {
         .hSyncObject = queue->vm_fence.handle,
      };
      WDDM2_DISPATCH(DestroySynchronizationObject(&destroy_fence));
      queue->vm_fence.handle = 0;
   }
   if (queue->context_h) {
      D3DKMT_DESTROYCONTEXT context_destroy = {
         .hContext = queue->context_h,
      };
      WDDM2_DISPATCH(DestroyContext(&context_destroy));
      queue->context_h = 0;
   }
   if (queue->handle) {
      D3DKMT_DESTROYHWQUEUE queue_destroy = {
         .hHwQueue = queue->handle,
      };
      WDDM2_DISPATCH(DestroyHwQueue(&queue_destroy));
      queue->handle = 0;
   }
}

static VkResult
radv_wddm2_queue_init(struct radv_wddm2_winsys *ws, enum amd_ip_type hw_ip,
                      enum radeon_ctx_priority priority, struct radv_wddm2_queue *parent,
                      struct radv_wddm2_queue *queue)
{
   NTSTATUS status;
   uint32_t node;

   queue->hw_ip = hw_ip;
   util_dynarray_init(&queue->sparse_ops, NULL);

   switch (hw_ip) {
   case AMD_IP_GFX:
      node = debug_get_num_option("RADV_DXGI_3D_NODE", 0);
      break;
   case AMD_IP_COMPUTE:
      node = debug_get_num_option("RADV_DXGI_COMPUTE_NODE", 2);
      break;
   default:
      /* Not supported */
      return VK_SUCCESS;
   }

   struct create_context_private_data create_context_data = {
      .header_size = 0x40,
      .flags = 0x100000,
      .dw13 = 0x200000,
      .section_size = 0x168,
   };
   struct bc250_context_blob bc250_ctx = {0};
   void *private_data = &create_context_data;
   UINT private_size = sizeof(create_context_data);

   if (ws->bc250) {
      /* GFX1013 has no compute queue (fact M50). Node 1 is not a UMD queue. */
      if (hw_ip != AMD_IP_GFX)
         return VK_SUCCESS;
      node = 0;
      bc250_ctx.magic = BC250_CONTEXT_MAGIC;
      bc250_ctx.version = 2;
      bc250_ctx.size = sizeof(bc250_ctx);
      _Static_assert(sizeof(bc250_ctx) == 80, "BC2C version 2 is 80 bytes");
      bc250_ctx.ip_type = BC250_IP_GFX;
      bc250_ctx.node_ordinal = 0;
      private_data = &bc250_ctx;
      private_size = sizeof(bc250_ctx);
   } else if (hw_ip == AMD_IP_GFX) {
      create_context_data.flags |= 0x40000;
      create_context_data.version = 9;
   }

   D3DKMT_CREATECONTEXTVIRTUAL create_context = {
      .hDevice = ws->device_h,
      .NodeOrdinal = node,
      .EngineAffinity = 1, // TODO
      .Flags = {
         .HwQueueSupported = !ws->bc250,
      },
      .pPrivateDriverData = private_data,
      .PrivateDriverDataSize = private_size,
      .ClientHint = D3DKMT_CLIENTHINT_VULKAN,
   };

   status = WDDM2_DISPATCH(CreateContextVirtual(&create_context));
   if (!NT_SUCCESS(status)) {
      fprintf(stderr, "Create context failed 0x%X for IP %i and device 0x%x\n", status, hw_ip, ws->device_h);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   queue->context_h = create_context.hContext;

   /* bc250kmd has no hardware-queue DDIs. SubmitCommand is the packet path. */
   if (ws->bc250) {
      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create_progress = {
         .hDevice = ws->device_h,
         .Info = {
            .Type = D3DDDI_MONITORED_FENCE,
            .MonitoredFence = { .InitialFenceValue = 0, .EngineAffinity = 1 },
         },
      };
      queue->bc250_ws = ws;
      status = WDDM2_DISPATCH(CreateSynchronizationObject2(&create_progress));
      if (!NT_SUCCESS(status))
         goto failed;
      queue->bc250_progress.handle = create_progress.hSyncObject;
      queue->bc250_progress.value_map = create_progress.Info.MonitoredFence.FenceValueCPUVirtualAddress;
      /* Packet submission also needs a distinct companion-paging fence.
       * The render progress fence protects CPU-repacked IBs and cannot share
       * ownership with UpdateGpuVirtualAddress increments. */
      create_progress.hSyncObject = 0;
      create_progress.Info.MonitoredFence.FenceValueCPUVirtualAddress = NULL;
      create_progress.Info.MonitoredFence.FenceValueGPUVirtualAddress = 0;
      status = WDDM2_DISPATCH(CreateSynchronizationObject2(&create_progress));
      if (!NT_SUCCESS(status))
         goto failed;
      queue->vm_fence.handle = create_progress.hSyncObject;
      queue->vm_fence.value_map = create_progress.Info.MonitoredFence.FenceValueCPUVirtualAddress;
      return VK_SUCCESS;
   }

   if (hw_ip == AMD_IP_GFX || hw_ip == AMD_IP_COMPUTE) {
      struct create_queue_private_data create_queue_pdd = {
         .header_size = sizeof(struct create_queue_private_data),
         .flags = 0x21000,
      };

      if (parent) {
         create_queue_pdd.flags |= 0x80000;
         create_queue_pdd.parent_queue_id = parent->queue_id;
      }

      D3DKMT_CREATEHWQUEUE create_queue = {
         .hHwContext = queue->context_h,
         .pPrivateDriverData = &create_queue_pdd,
         .PrivateDriverDataSize = sizeof(create_queue_pdd),
      };
      status = WDDM2_DISPATCH(CreateHwQueue(&create_queue));
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "Create queue failed 0x%X for IP %i and device 0x%x\n", status, hw_ip, ws->device_h);
         goto failed;
      }
      queue->handle = create_queue.hHwQueue;
      queue->queue_id = create_queue_pdd.queue_id;

      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create_sync = {
         .hDevice = ws->device_h,
         .Info = {
            .Type = D3DDDI_MONITORED_FENCE,
            .MonitoredFence = {
               .EngineAffinity = 1,
            },
         }
      };
      status = WDDM2_DISPATCH(CreateSynchronizationObject2(&create_sync));
      if (unlikely(!NT_SUCCESS(status))) {
         fprintf(stderr, "CreateSynchronizationObject2 failed with NTSTATUS 0x%x\n", status);
         goto failed;
      }
      queue->vm_fence.handle = create_sync.hSyncObject;
   }

   return VK_SUCCESS;

failed:
   radv_wddm2_queue_destroy(queue);
   return VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult
radv_wddm2_ctx_create(struct radeon_winsys *_ws, enum radeon_ctx_priority priority,
                      struct radeon_winsys_ctx **rctx)
{
   VkResult result;
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(_ws);

   if (priority < RADEON_CTX_PRIORITY_LOW || priority > RADEON_CTX_PRIORITY_HIGH)
      return VK_ERROR_NOT_PERMITTED;

   struct radv_wddm2_ctx *ctx = CALLOC_STRUCT(radv_wddm2_ctx);
   if (!ctx)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   ctx->ws = ws;

   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++) {
      result = radv_wddm2_queue_init(ws, ip, priority, NULL, &ctx->per_ip[ip].queue);
      if (result != VK_SUCCESS)
         goto fail_contexts;
   }

   *rctx = (struct radeon_winsys_ctx *)ctx;
   return VK_SUCCESS;

fail_contexts:
   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_queue_destroy(&ctx->per_ip[ip].queue);

   FREE(ctx);
   return result;
}

static void
radv_wddm2_ctx_destroy(struct radeon_winsys_ctx *rwctx)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);

   for (uint32_t ip = 0; ip < AMD_NUM_IP_TYPES; ip++)
      radv_wddm2_queue_destroy(&ctx->per_ip[ip].queue);
   radv_wddm2_queue_destroy(&ctx->ace_queue);

   FREE(ctx);
}

static bool
vk_wddm2_fence_wait(uint32_t device_h, struct vk_wddm2_fence *fence)
{
   VkResult result;
   NTSTATUS status;
   HANDLE async_event = 0;

   /* Quick poll all the fences ourselves.  We may not have to call into the
    * kernel at all.
    */
   if (p_atomic_read(fence->value_map) >= fence->wait_value)
      return true;

   result = vk_async_event_create(&async_event);
   if (unlikely(result != VK_SUCCESS))
      return false;

   const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {
      .hDevice = device_h,
      .ObjectCount = 1,
      .ObjectHandleArray = &fence->handle,
      .FenceValueArray = &fence->wait_value,
      .hAsyncEvent = async_event,
   };
   status = WDDM2_DISPATCH(WaitForSynchronizationObjectFromCpu(&wait));

   if (unlikely(!NT_SUCCESS(status))) {
      vk_async_event_close(async_event);
      fprintf(stderr, "fence wait failed: 0x%X\n", status);
      return false;
   }

   result = vk_async_event_wait(async_event, 10000000000ull);
   vk_async_event_close(async_event);
   if (result != VK_SUCCESS)
      fprintf(stderr, "async wait event: 0x%x\n", result);

   D3DKMT_GETDEVICESTATE get_state = {
      .hDevice = device_h,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };

   if (get_state.ExecutionState == D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT) {
      get_state.StateType = D3DKMT_DEVICESTATE_PAGE_FAULT;
      status = WDDM2_DISPATCH(GetDeviceState(&get_state));
      D3DKMT_DEVICEPAGEFAULT_STATE fault = get_state.PageFaultState;

      fprintf(stderr, "faulted VA: 0x%" PRIx64 ", error: 0x%x (vendor specific: %i), flags: %i, stage: %i\n",
             fault.FaultedVirtualAddress, fault.FaultErrorCode.GeneralErrorCode,
             fault.FaultErrorCode.DeviceSpecificCode, fault.PageFaultFlags, fault.FaultedPipelineStage);
      return false;
   }

   return result == VK_SUCCESS;
}

static bool
radv_wddm2_ctx_wait_idle(struct radeon_winsys_ctx *rwctx, enum amd_ip_type ip_type, int ring_index)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(rwctx);
   bool ret = true;

   if (ctx->per_ip[ip_type].last_submission.handle)
      ret = vk_wddm2_fence_wait(ctx->ws->device_h, &ctx->per_ip[ip_type].last_submission);

   return ret;
}

struct radv_wddm2_cs {
   struct radv_winsys_cs base;
   struct set *buffers;
};

static inline struct radv_wddm2_cs *
radv_wddm2_cs(struct ac_cmdbuf *base)
{
   return (struct radv_wddm2_cs *)base;
}

static enum radeon_bo_domain
radv_wddm2_cs_domain(const struct radeon_winsys *_ws)
{
   return RADEON_DOMAIN_GTT;
}

static struct ac_cmdbuf *
radv_wddm2_cs_create(struct radeon_winsys *rws, enum amd_ip_type ip_type, bool is_secondary)
{
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(rws);
   struct radv_wddm2_cs *cs;

   cs = calloc(1, sizeof(struct radv_wddm2_cs));
   if (!cs)
      return NULL;

   cs->buffers = _mesa_pointer_set_create(NULL);

   VkResult result = radv_winsys_cs_init(&cs->base, rws, ip_type, is_secondary, ws->chain_ib);
   if (result != VK_SUCCESS) {
      _mesa_set_destroy(cs->buffers, NULL);
      free(cs);
      return NULL;
   }

   _mesa_set_add(cs->buffers, radv_wddm2_bo(cs->base.ib_buffer));

   return &cs->base.base;
}

static unsigned
radv_wddm2_cs_num_ibs(UNUSED const struct radv_wddm2_cs *cs)
{
   return 1;
}

static uint32_t
radv_wddm2_cs_translate_ip_type(enum amd_ip_type ip_type)
{
   return (ip_type == AMD_IP_SDMA) ? 0 : 1;
}

static void
radv_wddm2_cs_destroy(struct ac_cmdbuf *_cs)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);

   radv_winsys_cs_destroy(&cs->base);
   _mesa_set_destroy(cs->buffers, NULL);

   FREE(cs);
}

static void
radv_wddm2_cs_reset(struct ac_cmdbuf *_cs)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);

   radv_winsys_cs_reset(&cs->base);
   _mesa_set_clear(cs->buffers, NULL);
}

static void
radv_wddm2_cs_add_buffer(struct ac_cmdbuf *_cs, struct radeon_winsys_bo *_bo)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);
   _mesa_set_add(cs->buffers, radv_wddm2_bo(_bo));
}

static void
radv_wddm2_cs_execute_secondary(struct ac_cmdbuf *_parent, struct ac_cmdbuf *_child,
                                bool allow_ib2)
{
   radv_winsys_cs_execute_secondary(radv_winsys_cs(_parent), radv_winsys_cs(_child), allow_ib2);
}

static void
radv_wddm2_get_cpu_addr(void *_cs, uint64_t addr, struct ac_addr_info *info)
{
   struct radv_wddm2_cs *cs = radv_wddm2_cs(_cs);
   struct radv_wddm2_winsys *ws = radv_wddm2_winsys(cs->base.ws);

   memset(info, 0, sizeof(struct ac_addr_info));

   if (ws->debug_log_bos) {
      bool destroyed = false;

      if (radv_winsys_bo_log_find(&ws->bo_log, addr, &destroyed))
         info->use_after_free = destroyed;
   }

   if (info->use_after_free)
      return;

   info->valid = !ws->debug_all_bos;

   if (radv_winsys_cs_get_cpu_addr(&cs->base, addr, &info->cpu_addr)) {
      info->valid = true;
      return;
   }

   if (radv_winsys_bo_list_get_cpu_addr(&ws->global_bo_list, &ws->base, addr, &info->cpu_addr)) {
      info->valid = true;
      return;
   }
}

#define WRITE_STRUCT(pdd, type, item) \
   for (struct submit_pdd_##type *item = submit_pdd_writer_reserve(pdd, sizeof(struct submit_pdd_##type)); \
        item != NULL; \
        item = NULL)

#define WRITE_ENTRY(pdd, TYPE, typ, entry) \
   for (struct submit_pdd_##typ *entry = submit_pdd_writer_reserve(pdd, sizeof(struct submit_pdd_##typ)); \
        entry != NULL && \
        (((struct submit_pdd_entry *) entry)->size = sizeof(struct submit_pdd_##typ)); \
        ((struct submit_pdd_entry *) entry)->type = SUBMIT_PDD_ENTRY_##TYPE, \
        (pdd)->num_entries++, \
        entry = NULL)

static void
radv_wddm2_submit_add_cs(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                         struct radv_winsys_cs *cs, enum radv_cs_dump_type type, bool is_gang,
                         struct radv_winsys_ib *first)
{
   const unsigned num_ib_buffers = cs->chain_ib ? 1 : cs->num_ib_buffers;
   uint32_t flags = 0;

   if (type == RADV_CS_DUMP_TYPE_PREAMBLE_IBS)
      flags = 0xc;
   else if (type == RADV_CS_DUMP_TYPE_POSTAMBLE_IBS)
      flags = 0x104;

   if (ctx->ws->dump_ibs)
      ctx->ws->base.cs_dump(&cs->base, stderr, NULL, 0, type);

   for  (unsigned i = 0; i < num_ib_buffers; i++) {
      struct radv_winsys_ib ib = cs->ib_buffers[i];

      if (first->va == 0)
         *first = ib;

      if (is_gang) {
         WRITE_STRUCT(pdd, gang_ib, gang_ib) {
            gang_ib->len = ib.cdw * 4;
            gang_ib->flags = flags;
            gang_ib->addr_lo = ib.va;
            gang_ib->addr_hi = ib.va >> 32;
         }
      } else {
         WRITE_ENTRY(pdd, GFX_IB, gfx_ib, entry) {
            fprintf(stderr, "Adding IB with VA 0x%" PRIx64 " and length %u bytes\n", ib.va, ib.cdw * 4);
            entry->len = ib.cdw * 4;
            entry->flags = flags;
            entry->ip_type = radv_wddm2_cs_translate_ip_type(cs->hw_ip);
            entry->addr_lo = ib.va;
            entry->addr_hi = ib.va >> 32;
         }
      }
   }
}

static unsigned
radv_wddm2_count_submitted_ibs(const struct radv_winsys_submit_info *submit, unsigned hw_ip)
{
   unsigned num_ibs = 0;

   for (unsigned i = 0; i < submit->initial_preamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->initial_preamble_cs[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   for (unsigned i = 0; i < submit->cs_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->cs_array[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   for (unsigned i = 0; i < submit->postamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->postamble_cs[i]);
      if (cs->hw_ip == hw_ip)
         num_ibs += cs->chain_ib ? 1 : cs->num_ib_buffers;
   }

   return num_ibs;
}

static void
radv_wddm2_cs_submit_add_ibs(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                             const struct radv_winsys_submit_info *submit, unsigned hw_ip,
                             struct radv_winsys_ib *first_ib)
{
   for (unsigned i = 0; i < submit->initial_preamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->initial_preamble_cs[i]);
      assert(cs->num_ib_buffers == 1);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_PREAMBLE_IBS,
                               submit->is_gang, first_ib);
   }

   for (unsigned i = 0; i < submit->cs_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->cs_array[i]);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_MAIN_IBS,
                               submit->is_gang, first_ib);
   }

   for (unsigned i = 0; i < submit->postamble_count; i++) {
      struct radv_winsys_cs *cs = radv_winsys_cs(submit->postamble_cs[i]);
      assert(cs->num_ib_buffers == 1);

      if (cs->hw_ip != hw_ip)
         continue;

      radv_wddm2_submit_add_cs(ctx, pdd, cs, RADV_CS_DUMP_TYPE_POSTAMBLE_IBS,
                               submit->is_gang, first_ib);
   }
}

static void
radv_wddm2_submit_add_queue(struct radv_wddm2_ctx *ctx, struct submit_pdd_writer *pdd,
                             const struct radv_winsys_submit_info *submit, struct radv_wddm2_queue *queue,
                             struct radv_winsys_ib *first_ib)
{
   WRITE_ENTRY(pdd, GANG_QUEUE, gang_queue, entry) {
      entry->num_ibs = radv_wddm2_count_submitted_ibs(submit, queue->hw_ip);
      entry->base.size += entry->num_ibs * sizeof(struct submit_pdd_gang_ib);
      entry->always_1 = 1;
      entry->queue_id = queue->queue_id;
   }

   radv_wddm2_cs_submit_add_ibs(ctx, pdd, submit, queue->hw_ip, first_ib);
}

/* A chained CS is one IB: the CP follows the rest. An unchained CS is every piece, in order.
 * Returns false when the caller's 16-IB cap cannot hold them. Silent truncation would run a prefix.
 */
static bool
bc250_collect_cs(struct radv_winsys_cs *cs, struct radv_winsys_ib *out, unsigned *n, unsigned cap)
{
   unsigned count = cs->chain_ib ? 1 : cs->num_ib_buffers;
   unsigned i;
   if (*n + count > cap) {
      fprintf(stderr, "bc250: gather IB limit used=%u add=%u cap=%u\n", *n, count, cap);
      return false;
   }
   for (i = 0; i < count; i++)
      out[(*n)++] = cs->ib_buffers[i];
   return true;
}

static bool
bc250_collect_array(struct ac_cmdbuf **arr, unsigned count, struct radv_winsys_ib *out, unsigned *n, unsigned cap)
{
   unsigned i;
   for (i = 0; i < count; i++) {
      if (!bc250_collect_cs(radv_winsys_cs(arr[i]), out, n, cap))
         return false;
   }
   return true;
}

/* One IB on the gfx ring. Several command streams are copied into one gather BO: the KMD
 * runs a single IB and does not half-run a list.
 */
static NTSTATUS
radv_wddm2_bc250_submit(struct radv_wddm2_ctx *ctx, struct radv_wddm2_queue *queue,
                        const struct radv_winsys_submit_info *submit)
{
   struct radv_wddm2_winsys *ws = ctx->ws;
   struct radv_winsys_ib ibs[16];
   struct bc250_submit_blob blob;
   unsigned n = 0, i;
   uint64_t total = 0;
   uint64_t va;
   uint32_t bytes;
   NTSTATUS status;
   D3DKMT_SUBMITCOMMAND cmd;

   /* Same order as the amdgpu winsys: initial preamble on the first CS, the continue
    * preamble on every later CS in this submit, postamble once at the end.
    */
   for (i = 0; i < submit->cs_count; i++) {
      struct ac_cmdbuf **preambles = i ? submit->continue_preamble_cs : submit->initial_preamble_cs;
      unsigned preamble_count = i ? submit->continue_preamble_count : submit->initial_preamble_count;
      if (!bc250_collect_array(preambles, preamble_count, ibs, &n, 16) ||
          !bc250_collect_cs(radv_winsys_cs(submit->cs_array[i]), ibs, &n, 16))
         return STATUS_INVALID_PARAMETER;
   }
   if (!bc250_collect_array(submit->postamble_cs, submit->postamble_count, ibs, &n, 16))
      return STATUS_INVALID_PARAMETER;
   if (n == 0)
      return STATUS_SUCCESS;

   if (n == 1) {
      va = ibs[0].va;
      bytes = ibs[0].cdw * 4;
   } else {
      uint8_t *dst;
      for (i = 0; i < n; i++)
         total += ibs[i].cdw;
      /* Match the GFX INDIRECT_BUFFER IB_SIZE field, not an arbitrary 1 MiB
       * staging limit. The caller retired this queue's previous gather first. */
      if (total == 0 || total > G_3F3_IB_SIZE(UINT32_MAX)) {
         fprintf(stderr, "bc250: gather exceeds IB_SIZE field: %" PRIu64 " dwords\n", total);
         return STATUS_INVALID_PARAMETER;
      }
      bytes = (uint32_t)(total * 4);
      if (!queue->bc250_gather || queue->bc250_gather->size < bytes) {
         struct radeon_winsys_bo *replacement = NULL;
         uint64_t capacity = align64(bytes, 4096);
         VkResult result = ws->base.buffer_create(&ws->base, capacity, 4096, RADEON_DOMAIN_GTT,
                                                  RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING,
                                                  0, 0, NULL, &replacement);
         if (result != VK_SUCCESS)
            return STATUS_NO_MEMORY;
         uint8_t *mapping = ws->base.buffer_map(&ws->base, replacement, false, NULL);
         if (!mapping) {
            ws->base.buffer_destroy(&ws->base, replacement);
            return STATUS_NO_MEMORY;
         }
         if (queue->bc250_gather)
            ws->base.buffer_destroy(&ws->base, queue->bc250_gather);
         queue->bc250_gather = replacement;
         queue->bc250_gather_map = mapping;
         fprintf(stderr, "bc250: gather capacity=%" PRIu64 " bytes required=%u ibs=%u\n", capacity, bytes, n);
      }
      dst = queue->bc250_gather_map;
      for (i = 0; i < n; i++) {
         uint8_t *src = NULL;
         uint64_t off = 0;
         if (!ibs[i].bo)
            return STATUS_INVALID_PARAMETER;
         if (ibs[i].va >= ibs[i].bo->va)
            off = ibs[i].va - ibs[i].bo->va;
         if (off + (uint64_t)ibs[i].cdw * 4 > ibs[i].bo->size)
            return STATUS_INVALID_PARAMETER;
         src = ws->base.buffer_map(&ws->base, ibs[i].bo, false, NULL);
         if (!src)
            return STATUS_INVALID_PARAMETER;
         memcpy(dst, src + off, ibs[i].cdw * 4);
         dst += ibs[i].cdw * 4;
      }
      va = queue->bc250_gather->va;
   }

   /* BC250_IB_DWORDS cuts the IB the CP executes. 5 is CONTEXT_CONTROL plus
    * CLEAR_STATE, a packet boundary. The ring fence still follows the IB, so a
    * timeout here means the CP did not finish that prefix. Unset, the whole IB runs. */
   {
      const char *cap_env = getenv("BC250_IB_DWORDS");
      if (cap_env && cap_env[0]) {
         unsigned long cap = strtoul(cap_env, NULL, 0);
         if (cap > 0 && cap < 0x100000ul && cap * 4ul < bytes) {
            fprintf(stderr, "bc250: IB clamped to %lu dwords (%u were ready)\n", cap, bytes / 4u);
            bytes = (uint32_t)(cap * 4ul);
         }
      }
   }

   memset(&blob, 0, sizeof(blob));
   _Static_assert(offsetof(struct bc250_submit_blob, ib) == 40, "BC2S prefix is 40 bytes");
   _Static_assert(sizeof(struct bc250_ib_blob) == 32, "BC2S ib is 32 bytes");
   blob.magic = BC250_SUBMIT_MAGIC;
   blob.version = 1;
   blob.num_ibs = 1;
   blob.ip_type = BC250_IP_GFX;
   blob.size = 40 + sizeof(struct bc250_ib_blob);
   blob.ib[0].va_start = va;
   blob.ib[0].ib_bytes = bytes;
   blob.ib[0].ip_type = BC250_IP_GFX;

   memset(&cmd, 0, sizeof(cmd));
   cmd.Commands = va;
   cmd.CommandLength = bytes;
   cmd.pPrivateDriverData = &blob;
   cmd.PrivateDriverDataSize = blob.size;
   cmd.BroadcastContextCount = 1;
   cmd.BroadcastContext[0] = queue->context_h;
   /* Write-combined stores sit in the WC buffers until a fence. The syscall drains them
    * too; this one makes the order obvious.
    */
   _mm_sfence();
   status = WDDM2_DISPATCH(SubmitCommand(&cmd));
   if (!NT_SUCCESS(status))
      fprintf(stderr, "bc250: SubmitCommand 0x%X (%u ibs packed, %u bytes)\n", status, n, bytes);
   else if (ws->bc250_trace_submits) {
      const uint32_t *dw = NULL;
      if (n == 1 && ibs[0].bo)
         dw = (const uint32_t *)((uint8_t *)ws->base.buffer_map(&ws->base, ibs[0].bo, false, NULL) +
                                 (ibs[0].va - ibs[0].bo->va));
      else
         dw = (const uint32_t *)queue->bc250_gather_map;
      fprintf(stderr, "bc250: SubmitCommand ib 0x%" PRIx64 " %u bytes (from %u) %08x %08x %08x %08x\n",
              va, bytes, n,
              dw ? dw[0] : 0, dw && bytes >= 8 ? dw[1] : 0,
              dw && bytes >= 12 ? dw[2] : 0, dw && bytes >= 16 ? dw[3] : 0);
      /* Slices of the unclamped preamble and the main CS. 163 is the first
       * ACQUIRE_MEM, 176 the main IB, 216 the dispatch. */
      if (dw && bytes >= 176u * 4u) {
         fprintf(stderr, "bc250: IB +158 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[158], dw[159], dw[160], dw[161], dw[162], dw[163], dw[164], dw[165]);
         fprintf(stderr, "bc250: IB +166 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[166], dw[167], dw[168], dw[169], dw[170], dw[171], dw[172], dw[173]);
         fprintf(stderr, "bc250: IB +174 %08x %08x\n", dw[174], dw[175]);
      }
      if (dw && bytes >= 216u * 4u) {
         fprintf(stderr, "bc250: IB +176 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[176], dw[177], dw[178], dw[179], dw[180], dw[181], dw[182], dw[183]);
         fprintf(stderr, "bc250: IB +184 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[184], dw[185], dw[186], dw[187], dw[188], dw[189], dw[190], dw[191]);
         fprintf(stderr, "bc250: IB +192 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[192], dw[193], dw[194], dw[195], dw[196], dw[197], dw[198], dw[199]);
         fprintf(stderr, "bc250: IB +200 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[200], dw[201], dw[202], dw[203], dw[204], dw[205], dw[206], dw[207]);
         fprintf(stderr, "bc250: IB +208 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[208], dw[209], dw[210], dw[211], dw[212], dw[213], dw[214], dw[215]);
      }
      if (dw && bytes >= 240u * 4u) {
         fprintf(stderr, "bc250: IB +216 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[216], dw[217], dw[218], dw[219], dw[220], dw[221], dw[222], dw[223]);
         fprintf(stderr, "bc250: IB +224 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[224], dw[225], dw[226], dw[227], dw[228], dw[229], dw[230], dw[231]);
         fprintf(stderr, "bc250: IB +232 %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 dw[232], dw[233], dw[234], dw[235], dw[236], dw[237], dw[238], dw[239]);
      }
   }
   return status;
}

static VkResult
radv_wddm2_cs_submit(struct radeon_winsys_ctx *_ctx,
                     const struct radv_winsys_submit_info *submit,
                     uint32_t wait_count, const struct vk_sync_wait *waits,
                     uint32_t signal_count, const struct vk_sync_signal *signals)
{
   struct radv_wddm2_ctx *ctx = radv_wddm2_ctx(_ctx);
   struct radv_wddm2_queue *queue = &ctx->per_ip[submit->ip_type].queue;
   struct radv_wddm2_queue *ace_queue = &ctx->ace_queue;
   NTSTATUS status;

   assert(queue->context_h != 0 && "Unsupported IP type");

   if (submit->is_gang && ace_queue->handle == 0) {
      assert(submit->ip_type == AMD_IP_GFX);
      radv_wddm2_queue_init(ctx->ws, AMD_IP_COMPUTE, 0, queue, ace_queue);
      if (ace_queue->handle == 0)
         return VK_ERROR_DEVICE_LOST;
   }

   if (wait_count > 0) {
      STACK_ARRAY(D3DKMT_HANDLE, handles, wait_count);
      STACK_ARRAY(uint64_t, values, wait_count);

      for (uint32_t i = 0; i < wait_count; i++) {
         handles[i] = vk_sync_as_wddm2_monitored_fence(waits[i].sync)->handle;
         values[i] = waits[i].wait_value;
      }

      if (queue->handle) {
         D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE wait = {
            .hHwQueue = queue->handle,
            .ObjectCount = wait_count,
            .ObjectHandleArray = handles,
            .FenceValueArray = values,
         };
         status = WDDM2_DISPATCH(SubmitWaitForSyncObjectsToHwQueue(&wait));
      } else {
         D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait = {
            .hContext = queue->context_h,
            .ObjectCount = wait_count,
            .ObjectHandleArray = handles,
            .MonitoredFenceValueArray = values,
         };
         status = WDDM2_DISPATCH(WaitForSynchronizationObjectFromGpu(&wait));
      }

      STACK_ARRAY_FINISH(handles);
      STACK_ARRAY_FINISH(values);

      assert(NT_SUCCESS(status));
      if (!NT_SUCCESS(status))
         return VK_ERROR_DEVICE_LOST;
   }

   if (submit->cs_count > 0 && ctx->ws->bc250) {
      /* Application signals can be absent from an intermediate submit. They
       * cannot protect a CPU-rewritten IB. Retire our own previous queue IB
       * before packing the next, including submissions with no app signals.
       * A separate BO/fence per queue prevents cross-context overwrite. */
      uint64_t pending_value = queue->bc250_progress.wait_value;
      uint64_t observed = p_atomic_read(queue->bc250_progress.value_map);
      /* Keep one queue identity witness, without formatting every hot-path submit. */
      if (ctx->ws->bc250_trace_submits || pending_value == 0)
         fprintf(stderr, "bc250: progress before submit previous=%" PRIu64 " observed=%" PRIu64
                         " waits=%u signals=%u\n", pending_value, observed, wait_count, signal_count);
      if (pending_value && !vk_wddm2_fence_wait(ctx->ws->device_h, &queue->bc250_progress))
         return VK_ERROR_DEVICE_LOST;
      status = radv_wddm2_bc250_submit(ctx, queue, submit);
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "bc250: native submit failed NTSTATUS=0x%X cs_count=%u\n", status, submit->cs_count);
         return VK_ERROR_DEVICE_LOST;
      }
      uint64_t next_value = queue->bc250_progress.wait_value + 1;
      D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 progress_signal = {
         .ObjectCount = 1,
         .ObjectHandleArray = &queue->bc250_progress.handle,
         .BroadcastContextCount = 1,
         .BroadcastContextArray = &queue->context_h,
         .MonitoredFenceValueArray = &next_value,
      };
      status = WDDM2_DISPATCH(SignalSynchronizationObjectFromGpu2(&progress_signal));
      if (!NT_SUCCESS(status))
         return VK_ERROR_DEVICE_LOST;
      queue->bc250_progress.wait_value = next_value;
   } else if (submit->cs_count > 0) {
      struct radv_winsys_ib first_ib = {};
      struct submit_pdd_writer pdd;

      submit_pdd_writer_init(&pdd);
      if (submit->is_gang) {
         radv_wddm2_submit_add_queue(ctx, &pdd, submit, queue, &first_ib);
         radv_wddm2_submit_add_queue(ctx, &pdd, submit, ace_queue, &first_ib);
      } else {
         radv_wddm2_cs_submit_add_ibs(ctx, &pdd, submit, submit->ip_type, &first_ib);
      }
      submit_pdd_writer_finalize(&pdd);

      print_hex_data(stderr, pdd.buffer, pdd.offset);

      if (queue->handle) {
         static uint32_t submit_count = 0;
         submit_count++;
         D3DKMT_SUBMITCOMMANDTOHWQUEUE wddm2_submit = {
            .hHwQueue = queue->handle,
            .HwQueueProgressFenceId = submit_count,
            .CommandBuffer = first_ib.va,
            .CommandLength = first_ib.cdw * 4,
            .pPrivateDriverData = pdd.buffer,
            .PrivateDriverDataSize = pdd.offset,
         };
         status = WDDM2_DISPATCH(SubmitCommandToHwQueue(&wddm2_submit));
      } else {
         D3DKMT_SUBMITCOMMAND wddm2_submit = {
            .Commands = first_ib.va,
            .CommandLength = first_ib.cdw * 4,
            .pPrivateDriverData = pdd.buffer,
            .PrivateDriverDataSize = pdd.offset,
            .BroadcastContextCount = 1,
            .BroadcastContext[0] = queue->context_h,
         };
         status = WDDM2_DISPATCH(SubmitCommand(&wddm2_submit));          
      }
      if (!NT_SUCCESS(status)) {
         fprintf(stderr, "SubmitCommand: VK_ERROR_DEVICE_LOST\n");
         return VK_ERROR_DEVICE_LOST;
      }
   }

   if (signal_count > 0) {
      STACK_ARRAY(D3DKMT_HANDLE, handles, signal_count);
      STACK_ARRAY(uint64_t, values, signal_count);

      for (uint32_t i = 0; i < signal_count; i++) {
         handles[i] = vk_sync_as_wddm2_monitored_fence(signals[i].sync)->handle;
         values[i] = signals[i].signal_value;
      }

      if (queue->handle) {
         D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE signal = {
            .Flags = {
               .AllowFenceRewind = 1,
            },
            .BroadcastHwQueueCount = 1,
            .BroadcastHwQueueArray = &queue->handle,
            .ObjectCount = signal_count,
            .ObjectHandleArray = handles,
            .FenceValueArray = values,
         };
         status = WDDM2_DISPATCH(SubmitSignalSyncObjectsToHwQueue(&signal));
      } else {
         D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal = {
            .ObjectCount = signal_count,
            .ObjectHandleArray = handles,
            .BroadcastContextCount = 1,
            .BroadcastContextArray = &queue->context_h,
            .MonitoredFenceValueArray = values,
         };
         status = WDDM2_DISPATCH(SignalSynchronizationObjectFromGpu2(&signal));
      }

      STACK_ARRAY_FINISH(handles);
      STACK_ARRAY_FINISH(values);

      assert(NT_SUCCESS(status));
      if (!NT_SUCCESS(status))
         return VK_ERROR_DEVICE_LOST;

      struct vk_wddm2_monitored_fence *fence = vk_sync_as_wddm2_monitored_fence(signals[0].sync);
      ctx->per_ip[submit->ip_type].last_submission.handle = fence->handle;
      ctx->per_ip[submit->ip_type].last_submission.wait_value = signals[0].signal_value;
      ctx->per_ip[submit->ip_type].last_submission.value_map = fence->value_map;
   }   

   return VK_SUCCESS;
}

void
radv_wddm2_cs_init_functions(struct radv_wddm2_winsys *ws)
{
   ws->base.ctx_create = radv_wddm2_ctx_create;
   ws->base.ctx_destroy = radv_wddm2_ctx_destroy;
   ws->base.ctx_wait_idle = radv_wddm2_ctx_wait_idle;
   ws->base.cs_domain = radv_wddm2_cs_domain;
   ws->base.cs_create = radv_wddm2_cs_create;
   ws->base.cs_finalize = radv_winsys_cs_finalize;
   ws->base.cs_reset = radv_wddm2_cs_reset;
   ws->base.cs_chain = radv_winsys_cs_chain;
   ws->base.cs_unchain = radv_winsys_cs_unchain;
   ws->base.cs_destroy = radv_wddm2_cs_destroy;
   ws->base.cs_grow = radv_winsys_cs_grow;
   ws->base.cs_reset = radv_wddm2_cs_reset;
   ws->base.cs_add_buffer = radv_wddm2_cs_add_buffer;
   ws->base.cs_submit = radv_wddm2_cs_submit;
   ws->base.cs_execute_secondary = radv_wddm2_cs_execute_secondary;
   ws->base.cs_execute_ib = radv_winsys_cs_execute_ib;
   ws->base.cs_chain_dgc_ib = radv_winsys_cs_chain_dgc_ib;
   ws->base.cs_dump = radv_winsys_cs_dump;
   ws->base.cs_get_cpu_addr = radv_wddm2_get_cpu_addr;
   ws->base.cs_annotate = radv_winsys_cs_annotate;
   ws->base.cs_pad = radv_winsys_cs_pad;
}
