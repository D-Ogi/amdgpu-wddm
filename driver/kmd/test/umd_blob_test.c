// Host test for driver/kmd/umd_blob.c. The reader does not include the contract header (that header
// pulls the Linux UAPI, which the kernel build must not pick up by accident from libdrm). This file
// includes both and checks that every constant and offset the reader hardcodes is the contract's.
// It includes nvd.h too, for the one bound the packet sets rather than the contract: the IB length.
#include <stdio.h>
#include <string.h>

#include "../umd_blob.h"
#include "../../contract/bc250_umd_submit.h"
#include "../../amdgpu-import/nvd.h"

static int g_failures;

#define CHECK(cond) \
    do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void CheckContract(void)
{
    CHECK(UMD_BLOB_ALLOC_MAGIC == BC250_UMD_ALLOC_MAGIC);
    CHECK(UMD_BLOB_CONTEXT_MAGIC == BC250_UMD_CONTEXT_MAGIC);
    CHECK(UMD_BLOB_SUBMIT_MAGIC == BC250_UMD_SUBMIT_MAGIC);
    CHECK(UMD_BLOB_ALLOC_BYTES == BC250_UMD_ALLOC_SIZE_V1);
    CHECK(UMD_BLOB_CONTEXT_V1 == BC250_UMD_CONTEXT_SIZE_V1);
    CHECK(UMD_BLOB_CONTEXT_BYTES == BC250_UMD_CONTEXT_SIZE_V2);
    CHECK(UMD_BLOB_SUBMIT_BYTES == BC250_UMD_SUBMIT_SIZE_V1);
    CHECK(UMD_BLOB_IB_BYTES == sizeof(struct bc250_umd_ib));
    CHECK(UMD_BLOB_SUBMIT_MAX_IBS == BC250_UMD_SUBMIT_MAX_IBS);
    CHECK(UMD_BLOB_SUBMIT_PREFIX == offsetof(struct bc250_umd_submit_private, ib));
    CHECK(UMD_BLOB_HEAP_GTT == AMDGPU_GEM_DOMAIN_GTT);
    CHECK(UMD_BLOB_HEAP_VRAM == AMDGPU_GEM_DOMAIN_VRAM);
    CHECK(UMD_BLOB_IP_GFX == AMDGPU_HW_IP_GFX);
    CHECK(UMD_BLOB_IP_COMPUTE == AMDGPU_HW_IP_COMPUTE);
    CHECK(UMD_BLOB_IP_DMA == AMDGPU_HW_IP_DMA);
    CHECK(UMD_BLOB_A_EXACT_VA == BC250_UMD_A_EXACT_VA);
    CHECK(UMD_BLOB_A_SPARSE == BC250_UMD_A_SPARSE);
    CHECK(UMD_BLOB_A_USERPTR == BC250_UMD_A_USERPTR);
    // BD-127: the compute statement of a context blob, and the version from which it is read.
    CHECK(UMD_BLOB_C_COMPUTE == BC250_UMD_C_COMPUTE);
    CHECK(UMD_BLOB_CONTEXT_VERSION_COMPUTE == BC250_UMD_CONTEXT_VERSION_COMPUTE);
    CHECK(UMD_BLOB_CONTEXT_BYTES == BC250_UMD_CONTEXT_SIZE_V3);
    CHECK(UMD_BLOB_CONTEXT_VERSION_COMPUTE > BC250_UMD_CONTEXT_VERSION);

    CHECK(offsetof(struct bc250_umd_alloc_private, alloc_size) == 16);
    CHECK(offsetof(struct bc250_umd_alloc_private, phys_alignment) == 24);
    CHECK(offsetof(struct bc250_umd_alloc_private, preferred_heap) == 32);
    CHECK(offsetof(struct bc250_umd_alloc_private, requested_va) == 48);
    CHECK(offsetof(struct bc250_umd_alloc_private, gem_flags) == 40);
    CHECK(UMD_BLOB_GEM_GTT_USWC == AMDGPU_GEM_CREATE_CPU_GTT_USWC);
    CHECK(UMD_BLOB_GEM_NO_CPU_ACCESS == AMDGPU_GEM_CREATE_NO_CPU_ACCESS);
    CHECK(UMD_BLOB_GEM_DISCARDABLE == AMDGPU_GEM_CREATE_DISCARDABLE);
    CHECK(offsetof(struct bc250_umd_context_private, ip_type) == 16);
    CHECK(offsetof(struct bc250_umd_context_private, ip_instance) == 20);
    CHECK(offsetof(struct bc250_umd_context_private, ring) == 24);
    CHECK(offsetof(struct bc250_umd_context_private, priority) == 28);
    CHECK(offsetof(struct bc250_umd_context_private, node_ordinal) == UMD_BLOB_CONTEXT_V1);
    CHECK(offsetof(struct bc250_umd_alloc_private, scanout_width) == UMD_BLOB_ALLOC_SCANOUT_AT);
    CHECK(offsetof(struct bc250_umd_alloc_private, scanout_height) == UMD_BLOB_ALLOC_SCANOUT_AT + 4);
    CHECK(offsetof(struct bc250_umd_alloc_private, scanout_pitch) == UMD_BLOB_ALLOC_SCANOUT_AT + 8);
    CHECK(offsetof(struct bc250_umd_alloc_private, scanout_format) == UMD_BLOB_ALLOC_SCANOUT_AT + 12);
    CHECK(BC250_UMD_A_SCANOUT == UMD_BLOB_A_SCANOUT);
    CHECK(BC250_UMD_ALLOC_VERSION_SCANOUT == UMD_BLOB_ALLOC_VERSION_SCANOUT);
    CHECK(offsetof(struct bc250_umd_ib, va_start) == 0);
    CHECK(offsetof(struct bc250_umd_ib, ib_bytes) == 8);
    CHECK(offsetof(struct bc250_umd_ib, ip_type) == 12);
    CHECK(offsetof(struct bc250_umd_ib, ip_instance) == 16);
    CHECK(offsetof(struct bc250_umd_ib, ring) == 20);
    CHECK(sizeof(struct bc250_umd_alloc_private) == UMD_BLOB_ALLOC_BYTES);
    CHECK(sizeof(struct bc250_umd_context_private) == UMD_BLOB_CONTEXT_BYTES);
    CHECK(sizeof(struct bc250_umd_submit_private) == UMD_BLOB_SUBMIT_BYTES);
    // The IB length bound is the IB_SIZE field of the packet the ring writes: the bound survives the
    // field's own mask, and one dword more does not.
    CHECK(PACKET3_INDIRECT_BUFFER__IB_SIZE(UMD_BLOB_IB_MAX_DWORDS) == UMD_BLOB_IB_MAX_DWORDS);
    CHECK(PACKET3_INDIRECT_BUFFER__IB_SIZE(UMD_BLOB_IB_MAX_DWORDS + 1ul) != UMD_BLOB_IB_MAX_DWORDS + 1ul);
}

static void FillAlloc(struct bc250_umd_alloc_private* a, unsigned heap)
{
    memset(a, 0, sizeof(*a));
    a->magic = BC250_UMD_ALLOC_MAGIC;
    a->version = BC250_UMD_ALLOC_VERSION;
    a->size = sizeof(*a);
    a->alloc_size = 4096;
    a->phys_alignment = 4096;
    a->preferred_heap = heap;
}

static void FillContext(struct bc250_umd_context_private* c, unsigned ip, unsigned node)
{
    memset(c, 0, sizeof(*c));
    c->magic = BC250_UMD_CONTEXT_MAGIC;
    c->version = BC250_UMD_CONTEXT_VERSION;
    c->size = sizeof(*c);
    c->ip_type = ip;
    c->node_ordinal = node;
    c->priority = 2;
}

static unsigned SubmitWire(unsigned n)
{
    return (unsigned)(offsetof(struct bc250_umd_submit_private, ib) + n * sizeof(struct bc250_umd_ib));
}

static void FillSubmit(struct bc250_umd_submit_private* s, unsigned n)
{
    unsigned i;

    memset(s, 0, sizeof(*s));
    s->magic = BC250_UMD_SUBMIT_MAGIC;
    s->version = BC250_UMD_SUBMIT_VERSION;
    s->size = SubmitWire(n);
    s->ip_type = AMDGPU_HW_IP_GFX;
    s->num_ibs = n;
    s->fence_va = 0x1000;
    s->fence_value = 7;
    for (i = 0; i < n && i < BC250_UMD_SUBMIT_MAX_IBS; i++)
    {
        s->ib[i].va_start = 0x8000ull + (unsigned long long)i * 0x1000ull;
        s->ib[i].ib_bytes = 256;
        s->ib[i].ip_type = AMDGPU_HW_IP_GFX;
    }
}

int main(void)
{
    struct bc250_umd_alloc_private alloc;
    struct bc250_umd_context_private ctx;
    struct bc250_umd_submit_private submit;
    struct umd_alloc_view av;
    struct umd_context_view cv;
    struct umd_submit_view sv;
    unsigned char extra[sizeof(alloc) + 16];
    unsigned char gdi[32];

    CheckContract();

    // 1-2. VRAM and GTT, the two heaps M8 places.
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.flags = BC250_UMD_A_EXACT_VA;
    alloc.requested_va = 0x100000;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK);
    CHECK(av.bytes == 4096 && av.heap == UMD_BLOB_HEAP_VRAM && av.exact_va == 1);
    CHECK(av.requested_va == 0x100000 && av.alignment == 4096);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_GTT);
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK);
    CHECK(av.heap == UMD_BLOB_HEAP_GTT && av.exact_va == 0 && av.requested_va == 0);

    // Preserve the wire flags at every alignment; cache policy follows UAPI
    // intent, not unrelated allocation flags or truncated upper GEM bits.
    {
        unsigned heap,flags,offset;
        unsigned char wire[sizeof(alloc)+8];
        for(heap=0;heap<2;heap++)for(flags=0;flags<256;flags++)for(offset=0;offset<8;offset++) {
            int cached;
            FillAlloc(&alloc,heap?AMDGPU_GEM_DOMAIN_VRAM:AMDGPU_GEM_DOMAIN_GTT);
            alloc.version=BC250_UMD_ALLOC_VERSION_CACHE_POLICY;
            alloc.gem_flags=(1ull<<48)|flags;
            memcpy(wire+offset,&alloc,sizeof(alloc));
            CHECK(UmdBlobParseAlloc(wire+offset,sizeof(alloc),&av)==UMD_BLOB_OK);
            CHECK(av.gem_flags==alloc.gem_flags);
            cached=!heap && !(flags&(AMDGPU_GEM_CREATE_NO_CPU_ACCESS|AMDGPU_GEM_CREATE_CPU_GTT_USWC));
            CHECK(av.cache_policy_valid && UmdBlobAllocCpuCached(&av)==cached);
            // BD-210: no CPU view only where the creator said NO_CPU_ACCESS; Cached never without one.
            CHECK(UmdBlobAllocCpuVisible(&av)==!(flags&AMDGPU_GEM_CREATE_NO_CPU_ACCESS));
            CHECK(!cached || UmdBlobAllocCpuVisible(&av));
            alloc.version=BC250_UMD_ALLOC_VERSION;
            memcpy(wire+offset,&alloc,sizeof(alloc));
            CHECK(UmdBlobParseAlloc(wire+offset,sizeof(alloc),&av)==UMD_BLOB_OK);
            CHECK(!av.cache_policy_valid && !UmdBlobAllocCpuCached(&av));
            // A version 1 GEM word is not intent: the view stays.
            CHECK(UmdBlobAllocCpuVisible(&av));
        }
        CHECK(UmdBlobParseAlloc(wire,sizeof(alloc)-1,&av)==UMD_BLOB_TOO_SMALL);
        CHECK(!av.gem_flags && !av.cache_policy_valid && !UmdBlobAllocCpuCached(&av));
        CHECK(!UmdBlobAllocCpuCached(NULL));
        CHECK(UmdBlobAllocCpuVisible(NULL));
        printf("cache policy:4096 heap/flag/alignment cases passed if final verdict passes\n");
    }

    // 3-7. Heaps and flags this milestone does not implement, and a size of zero.
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_GDS);
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_HEAP);
    CHECK(av.bytes == 0);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM | AMDGPU_GEM_DOMAIN_GTT);
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_HEAP);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.alloc_size = 0;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SIZE);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.alloc_size = UMD_BLOB_ALLOC_MAX + 1ull;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SIZE);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.flags = BC250_UMD_A_SPARSE;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_FLAGS);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.flags = BC250_UMD_A_USERPTR;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_FLAGS);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.flags = BC250_UMD_A_EXACT_VA;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_VA);

    // M15.14, the scan-out request. The flag is read only at version 3, only out of VRAM, and only
    // with a geometry whose rows fit in the allocation the same blob asked for; everything else about
    // whether this surface may reach the display pipeline belongs to scanout_admit.h.
    {
        const unsigned long long frame = 1920ull * 4ull * 1200ull;
        FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
        alloc.version = BC250_UMD_ALLOC_VERSION_SCANOUT;
        alloc.alloc_size = frame;
        alloc.flags = BC250_UMD_A_SCANOUT;
        alloc.scanout_width = 1920; alloc.scanout_height = 1200;
        alloc.scanout_pitch = 1920 * 4; alloc.scanout_format = 21;      // D3DDDIFMT_A8R8G8B8
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK);
        CHECK(av.scanout == 1 && av.scanout_width == 1920 && av.scanout_height == 1200);
        CHECK(av.scanout_pitch == 1920 * 4 && av.scanout_format == 21);
        // The flag clear: the words are not read, whatever they hold.
        alloc.flags = 0;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK);
        CHECK(av.scanout == 0 && !av.scanout_width && !av.scanout_height && !av.scanout_pitch && !av.scanout_format);
        // A version 2 producer asking for scan-out is describing nothing: its tail is reserved.
        alloc.flags = BC250_UMD_A_SCANOUT;
        alloc.version = BC250_UMD_ALLOC_VERSION_CACHE_POLICY;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SCANOUT);
        alloc.version = BC250_UMD_ALLOC_VERSION_SCANOUT;
        // The aperture is not scanned out.
        alloc.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SCANOUT);
        alloc.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
        // Each of the four words on its own, and rows that do not fit.
        alloc.scanout_width = 0;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SCANOUT);
        alloc.scanout_width = 1920; alloc.scanout_height = 0;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SCANOUT);
        alloc.scanout_height = 1200; alloc.scanout_pitch = 0;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SCANOUT);
        alloc.scanout_pitch = 1920 * 4; alloc.scanout_format = 0;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SCANOUT);
        alloc.scanout_format = 21; alloc.alloc_size = frame - 4096;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SCANOUT);
        CHECK(av.scanout == 0 && av.bytes == 0);
        alloc.alloc_size = frame;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK);
        // A later version still reads the same four words at the same offsets.
        alloc.version = 99;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK && av.scanout == 1);
        CHECK(strcmp(UmdBlobStatusText(UMD_BLOB_BAD_SCANOUT), "unknown") != 0);
    }

    // 8. Envelope: wrong magic, a short buffer, version 0, a size word that does not cover v1,
    //    a buffer shorter than the size word, and a later version whose tail this reader ignores.
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.magic = BC250_UMD_CONTEXT_MAGIC;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_MAGIC);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    CHECK(UmdBlobParseAlloc(&alloc, UMD_BLOB_ALLOC_BYTES - 1, &av) == UMD_BLOB_TOO_SMALL);
    CHECK(UmdBlobParseAlloc(0, sizeof(alloc), &av) == UMD_BLOB_TOO_SMALL);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.version = 0;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_VERSION);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.size = UMD_BLOB_ALLOC_BYTES - 1;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SIZE);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    alloc.size = sizeof(alloc) + 8;
    CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_BAD_SIZE);
    FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
    memset(extra, 0, sizeof(extra));
    alloc.version = 99;
    alloc.size = sizeof(alloc) + 16;
    memcpy(extra, &alloc, sizeof(alloc));
    CHECK(UmdBlobParseAlloc(extra, sizeof(alloc) + 16, &av) == UMD_BLOB_OK);
    CHECK(av.heap == UMD_BLOB_HEAP_VRAM);

    // A GDI-sized buffer is not an allocation blob, and a real one is.
    memset(gdi, 0, sizeof(gdi));
    gdi[0] = 0x4C; gdi[1] = 0x42; gdi[2] = 0x37; gdi[3] = 0x41;   // "LB7A"
    CHECK(!UmdBlobIsAlloc(gdi, sizeof(gdi)));
    CHECK(UmdBlobIsAlloc(&alloc, sizeof(alloc)));
    CHECK(!UmdBlobIsAlloc(0, 4));
    CHECK(UmdBlobFirstWord(gdi, sizeof(gdi)) == 0x4137424Cul);

    // 9-11. Context: GFX on node 0, then the refusals.
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 0);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_OK);
    CHECK(cv.ip_type == UMD_BLOB_IP_GFX && cv.node_ordinal == 0 && cv.priority == 2);
    // COMPUTE runs on node 2 and on no other node; GFX runs on node 0 only.
    FillContext(&ctx, AMDGPU_HW_IP_COMPUTE, 0);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_NODE);
    CHECK(cv.ip_type == 0);
    FillContext(&ctx, AMDGPU_HW_IP_COMPUTE, UMD_BLOB_NODE_COMPUTE);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), UMD_BLOB_NODE_COMPUTE, &cv) == UMD_BLOB_OK);
    CHECK(cv.ip_type == UMD_BLOB_IP_COMPUTE && cv.node_ordinal == UMD_BLOB_NODE_COMPUTE);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_NODE);
    FillContext(&ctx, AMDGPU_HW_IP_GFX, UMD_BLOB_NODE_COMPUTE);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), UMD_BLOB_NODE_COMPUTE, &cv) == UMD_BLOB_BAD_NODE);
    FillContext(&ctx, AMDGPU_HW_IP_COMPUTE, 1);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 1, &cv) == UMD_BLOB_BAD_NODE);
    FillContext(&ctx, AMDGPU_HW_IP_COMPUTE, UMD_BLOB_NODE_COMPUTE);
    ctx.ring = 1;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), UMD_BLOB_NODE_COMPUTE, &cv) == UMD_BLOB_BAD_IP);
    CHECK(UmdBlobIpRunsOnNode(UMD_BLOB_IP_GFX, UMD_BLOB_NODE_3D) && UmdBlobIpRunsOnNode(UMD_BLOB_IP_COMPUTE, 2));
    CHECK(!UmdBlobIpRunsOnNode(UMD_BLOB_IP_GFX, 2) && !UmdBlobIpRunsOnNode(UMD_BLOB_IP_COMPUTE, 0));
    CHECK(!UmdBlobIpRunsOnNode(UMD_BLOB_IP_DMA, 1) && !UmdBlobIpRunsOnNode(UMD_BLOB_IP_COMPUTE, 1));
    FillContext(&ctx, AMDGPU_HW_IP_DMA, 0);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_IP);
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 0);
    ctx.ip_instance = 1;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_IP);
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 0);
    ctx.ring = 1;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_IP);
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 1);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 1, &cv) == UMD_BLOB_BAD_NODE);
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 1);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_NODE);
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 0);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 1, &cv) == UMD_BLOB_BAD_NODE);
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 0);
    ctx.version = 1;
    ctx.size = BC250_UMD_CONTEXT_SIZE_V1;
    CHECK(UmdBlobParseContext(&ctx, BC250_UMD_CONTEXT_SIZE_V1, 0, &cv) == UMD_BLOB_BAD_VERSION);
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 0);
    ctx.magic = BC250_UMD_ALLOC_MAGIC;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_MAGIC);

    // 11a. BD-127, the compute statement. A version 3 blob with the flag says compute; the same blob at
    // version 2 does not, so an older producer's flags word cannot turn the governor's compute set on. The
    // flag is never a refusal, the flags word is recorded as written, and a refused blob reports no compute.
    FillContext(&ctx, AMDGPU_HW_IP_GFX, 0);
    ctx.version = BC250_UMD_CONTEXT_VERSION_COMPUTE;
    ctx.flags = BC250_UMD_C_COMPUTE;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_OK);
    CHECK(cv.compute == 1 && cv.flags == BC250_UMD_C_COMPUTE);
    ctx.version = BC250_UMD_CONTEXT_VERSION;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_OK);
    CHECK(cv.compute == 0 && cv.flags == BC250_UMD_C_COMPUTE);
    // A version 3 blob without the flag, and one with a flag nothing defines: accepted, no compute.
    ctx.version = BC250_UMD_CONTEXT_VERSION_COMPUTE;
    ctx.flags = BC250_UMD_C_STABLE_PSTATE;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_OK && cv.compute == 0);
    ctx.flags = 0x80000000u;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_OK && cv.compute == 0);
    // A version above 3 keeps the meaning: everything this reader knows is where it expects it.
    ctx.version = BC250_UMD_CONTEXT_VERSION_COMPUTE + 7u;
    ctx.flags = BC250_UMD_C_COMPUTE | BC250_UMD_C_RESERVE_VMID;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_OK && cv.compute == 1);
    // And a refusal clears it, whatever the flag said.
    ctx.node_ordinal = 1;
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_NODE);
    CHECK(cv.compute == 0 && cv.flags == 0);

    // 12-16. One IB is runnable. Two IBs are a valid blob and not a single submission. The size word
    // is the used prefix, so sizeof(the whole struct) with one IB is a refusal.
    CHECK(SubmitWire(1) == UMD_BLOB_SUBMIT_PREFIX + UMD_BLOB_IB_BYTES);
    CHECK(SubmitWire(2) == UMD_BLOB_SUBMIT_PREFIX + 2 * UMD_BLOB_IB_BYTES);
    FillSubmit(&submit, 1);
    CHECK(UmdBlobParseSubmit(&submit, SubmitWire(1), &sv) == UMD_BLOB_OK);
    CHECK(sv.single_ib == 1 && sv.num_ibs == 1 && sv.ib_va == 0x8000ull && sv.ib_bytes == 256);
    CHECK(offsetof(struct bc250_umd_submit_private, fence_va) == 24 && offsetof(struct bc250_umd_submit_private, fence_value) == 32);
    CHECK(sv.fence_va == submit.fence_va && sv.fence_value == submit.fence_value);
    // A longer buffer than the size word is fine: the KMD's private-data slot is the whole struct.
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_OK);
    FillSubmit(&submit, 2);
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_OK);
    CHECK(sv.single_ib == 0 && sv.num_ibs == 2 && sv.ib_va == 0x8000ull);
    FillSubmit(&submit, 1);
    submit.num_ibs = 0;
    submit.size = SubmitWire(0);
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_SIZE);
    CHECK(sv.num_ibs == 0 && sv.ib_va == 0);
    CHECK(sv.fence_va == 0 && sv.fence_value == 0);
    FillSubmit(&submit, 1);
    submit.num_ibs = BC250_UMD_SUBMIT_MAX_IBS + 1;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_SIZE);
    FillSubmit(&submit, 1);
    submit.size = sizeof(submit);
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_SIZE);
    FillSubmit(&submit, 1);
    submit.ib[0].ib_bytes = 250;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    CHECK(sv.ib_bytes == 0);
    FillSubmit(&submit, 1);
    submit.ib[0].va_start = 0;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    FillSubmit(&submit, 1);
    submit.ib[0].va_start = 0x8002;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    FillSubmit(&submit, 1);
    submit.ib[0].ib_bytes = 0;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    // F-q44-mem-4: the longest IB the INDIRECT_BUFFER packet can carry is runnable, one dword more is
    // malformed private data, in any IB of the blob. Before the bound the second one parsed as OK, and the
    // ring's refusal of it closed node 0 for every process.
    FillSubmit(&submit, 1);
    submit.ib[0].ib_bytes = (unsigned)(UMD_BLOB_IB_MAX_DWORDS * 4ul);
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_OK);
    CHECK(sv.ib_bytes == UMD_BLOB_IB_MAX_DWORDS * 4ul);
    FillSubmit(&submit, 1);
    submit.ib[0].ib_bytes = (unsigned)(UMD_BLOB_IB_MAX_DWORDS * 4ul + 4ul);
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    CHECK(sv.ib_bytes == 0 && sv.single_ib == 0);
    FillSubmit(&submit, 1);
    submit.ib[0].ib_bytes = 0xFFFFFFFCu;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    FillSubmit(&submit, 2);
    submit.ib[1].ib_bytes = (unsigned)(UMD_BLOB_IB_MAX_DWORDS * 4ul + 4ul);
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    // The arithmetic of the path. The largest value of the 32-bit size field. A length whose dword count
    // passes the 20-bit mask as 1 (0x100001 & 0xFFFFF): a packet built from it would run a one-dword IB.
    // An IB count whose product with the entry size wraps 32 bits to the size of a one-IB blob
    // (0x08000001 * 32 = 2^32 + 32): only the count bound, checked before the product, refuses it.
    FillSubmit(&submit, 1);
    submit.ib[0].ib_bytes = 0xFFFFFFFFu;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    CHECK(sv.ib_bytes == 0 && sv.ib_va == 0);
    FillSubmit(&submit, 1);
    submit.ib[0].ib_bytes = (unsigned)((UMD_BLOB_IB_MAX_DWORDS + 2ul) * 4ul);
    CHECK(PACKET3_INDIRECT_BUFFER__IB_SIZE((UMD_BLOB_IB_MAX_DWORDS + 2ul)) == 1u);
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    FillSubmit(&submit, 1);
    submit.num_ibs = 0x08000001u;
    CHECK((unsigned)(UMD_BLOB_SUBMIT_PREFIX + submit.num_ibs * UMD_BLOB_IB_BYTES) == SubmitWire(1));
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_SIZE);
    CHECK(sv.num_ibs == 0 && sv.ib_va == 0);
    FillSubmit(&submit, 1);
    submit.num_ibs = 0xFFFFFFFFu;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_SIZE);
    FillSubmit(&submit, 1);
    submit.ib[0].ip_type = AMDGPU_HW_IP_COMPUTE;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    FillSubmit(&submit, 2);
    submit.ib[1].ip_type = AMDGPU_HW_IP_DMA;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    CHECK(sv.num_ibs == 0);
    // A COMPUTE blob whose IB says GFX: the IPs must agree, so it is a bad IB now, not a bad IP.
    FillSubmit(&submit, 1);
    submit.ip_type = AMDGPU_HW_IP_COMPUTE;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    CHECK(sv.ip_type == 0);
    // COMPUTE throughout is a submission for node 2; the view says which IP it is.
    submit.ib[0].ip_type = AMDGPU_HW_IP_COMPUTE;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_OK);
    CHECK(sv.ip_type == UMD_BLOB_IP_COMPUTE && sv.single_ib == 1);
    FillSubmit(&submit, 1);
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_OK && sv.ip_type == UMD_BLOB_IP_GFX);
    submit.ip_type = AMDGPU_HW_IP_DMA;
    submit.ib[0].ip_type = AMDGPU_HW_IP_DMA;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IP);
    FillSubmit(&submit, 1);
    submit.magic = BC250_UMD_ALLOC_MAGIC;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_MAGIC);
    CHECK(UmdBlobParseSubmit(0, 80, &sv) == UMD_BLOB_TOO_SMALL);
    FillSubmit(&submit, 1);
    CHECK(UmdBlobParseSubmit(&submit, SubmitWire(1) - 1, &sv) == UMD_BLOB_BAD_SIZE);

    // Memory manager stage 1c: the segments a parsed allocation may live in. Segment ids as wddm.c
    // has them: 1 local, 2 aperture; a set has bit (id - 1).
    {
        struct umd_placement pl;
        unsigned shared, heap, scan, discard;
        int second;
        for (shared = 0; shared < 2; shared++)
        for (heap = 0; heap < 2; heap++)
        for (scan = 0; scan < 2; scan++)
        for (discard = 0; discard < 2; discard++) {
            int want;
            memset(&av, 0, sizeof(av));
            av.heap = heap ? UMD_BLOB_HEAP_VRAM : UMD_BLOB_HEAP_GTT;
            av.scanout = (int)scan;
            av.gem_flags = (discard ? AMDGPU_GEM_CREATE_DISCARDABLE : 0) | AMDGPU_GEM_CREATE_CPU_GTT_USWC;
            second = UmdBlobPlacement(&av, 1, 2, (int)shared, &pl);
            if (!heap) {
                // GTT: the aperture alone, whatever the switch and the flags say.
                CHECK(!second && pl.preferred[0] == 2 && pl.preferred[1] == 0 && pl.supported == 0x2);
                continue;
            }
            want = shared && !scan && !discard;
            CHECK(second == want && pl.preferred[0] == 1);
            CHECK(pl.preferred[1] == (want ? 2ul : 0ul));
            CHECK(pl.supported == (want ? 0x3ul : 0x1ul));
            // Every preference is in the supported set (VidMm asserts otherwise).
            CHECK((pl.supported & (1ul << (pl.preferred[0] - 1))) != 0);
            CHECK(!pl.preferred[1] || (pl.supported & (1ul << (pl.preferred[1] - 1))) != 0);
        }
        // From the parser: a v3 VRAM blob that asked for scan-out stays local, the same blob
        // without the request may be demoted.
        FillAlloc(&alloc, AMDGPU_GEM_DOMAIN_VRAM);
        alloc.version = BC250_UMD_ALLOC_VERSION_SCANOUT;
        alloc.alloc_size = 1920ull * 4 * 1080;
        alloc.flags = BC250_UMD_A_SCANOUT;
        alloc.scanout_width = 1920; alloc.scanout_height = 1080; alloc.scanout_pitch = 1920 * 4;
        alloc.scanout_format = 87;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK && av.scanout == 1);
        CHECK(UmdBlobPlacement(&av, 1, 2, 1, &pl) == 0 && pl.supported == 0x1 && pl.preferred[1] == 0);
        // BD-210: a scan-out request keeps its CPU view even when it says NO_CPU_ACCESS; the same
        // blob without the request has none.
        alloc.gem_flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK && av.scanout == 1);
        CHECK(UmdBlobAllocCpuVisible(&av) && !UmdBlobAllocCpuCached(&av));
        alloc.flags = 0;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK && av.scanout == 0);
        CHECK(!UmdBlobAllocCpuVisible(&av));
        alloc.gem_flags = 0;
        CHECK(UmdBlobParseAlloc(&alloc, sizeof(alloc), &av) == UMD_BLOB_OK && av.scanout == 0);
        CHECK(UmdBlobPlacement(&av, 1, 2, 1, &pl) == 1 && pl.supported == 0x3 && pl.preferred[1] == 2);
        // No view: the local segment only. Ids this driver does not have: nothing at all.
        CHECK(UmdBlobPlacement(NULL, 1, 2, 1, &pl) == 0 && pl.preferred[0] == 1 && pl.supported == 0x1);
        CHECK(UmdBlobPlacement(&av, 0, 2, 1, &pl) == 0 && pl.supported == 0 && pl.preferred[0] == 0);
        CHECK(UmdBlobPlacement(&av, 1, 1, 1, &pl) == 0 && pl.supported == 0);
        CHECK(UmdBlobPlacement(&av, 1, 32, 1, &pl) == 0 && pl.supported == 0);
        CHECK(UmdBlobPlacement(&av, 1, 2, 1, NULL) == 0);
        printf("placement: 16 heap/scan-out/discardable/switch cases and the parser-fed ones\n");
    }

    CHECK(strcmp(UmdBlobStatusText(UMD_BLOB_OK), "ok") == 0);
    CHECK(strcmp(UmdBlobStatusText(UMD_BLOB_BAD_IB), "bad ib") == 0);
    CHECK(strcmp(UmdBlobStatusText(999), "unknown") == 0);

    if (g_failures == 0) { printf("umd_blob_test: all cases passed\n"); return 0; }
    fprintf(stderr, "umd_blob_test: %d case(s) failed\n", g_failures);
    return 1;
}
