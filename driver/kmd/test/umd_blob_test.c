// Host test for driver/kmd/umd_blob.c. The reader does not include the contract header (that header
// pulls the Linux UAPI, which the kernel build must not pick up by accident from libdrm). This file
// includes both and checks that every constant and offset the reader hardcodes is the contract's.
#include <stdio.h>
#include <string.h>

#include "../umd_blob.h"
#include "../../contract/bc250_umd_submit.h"

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
            alloc.version=BC250_UMD_ALLOC_VERSION;
            memcpy(wire+offset,&alloc,sizeof(alloc));
            CHECK(UmdBlobParseAlloc(wire+offset,sizeof(alloc),&av)==UMD_BLOB_OK);
            CHECK(!av.cache_policy_valid && !UmdBlobAllocCpuCached(&av));
        }
        CHECK(UmdBlobParseAlloc(wire,sizeof(alloc)-1,&av)==UMD_BLOB_TOO_SMALL);
        CHECK(!av.gem_flags && !av.cache_policy_valid && !UmdBlobAllocCpuCached(&av));
        CHECK(!UmdBlobAllocCpuCached(NULL));
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
    FillContext(&ctx, AMDGPU_HW_IP_COMPUTE, 0);
    CHECK(UmdBlobParseContext(&ctx, sizeof(ctx), 0, &cv) == UMD_BLOB_BAD_IP);
    CHECK(cv.ip_type == 0);
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
    FillSubmit(&submit, 1);
    submit.ib[0].ip_type = AMDGPU_HW_IP_COMPUTE;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    FillSubmit(&submit, 2);
    submit.ib[1].ip_type = AMDGPU_HW_IP_DMA;
    CHECK(UmdBlobParseSubmit(&submit, sizeof(submit), &sv) == UMD_BLOB_BAD_IB);
    CHECK(sv.num_ibs == 0);
    FillSubmit(&submit, 1);
    submit.ip_type = AMDGPU_HW_IP_COMPUTE;
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
        alloc.flags = 0;
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
