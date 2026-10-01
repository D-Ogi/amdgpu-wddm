// monfence_host_test - development-PC checks for monfence, no D3DKMT call, no GPU.
//
// What can be proven without the lab, is proven here, against the sources and not against a retyped number:
//   1. every PM4 constant monfence_packets.h restates equals the vendored Linux original (nvd.h, navi10_enum.h);
//   2. the RELEASE_MEM monfence emits is the KMD's own fence packet (driver/shim/bc250_gfx.c:1544-1561, re-expanded
//      here from the same macros), and with the KMD's selectors it reproduces Linux amdgpu's fence on unit A word for
//      word ("c0064900 06603514 22000000", evidence/linux/2026-09-21-E13-reference-2/boot1-full/rings-after-ib/
//      amdgpu_ring_gfx_0.0.0.txt:326); INT_SEL 3 gives RADV's dword 2 ("23000000", evidence/windows/
//      2026-09-23-E27-m9-inference/ops-gpu-003/suite.err:40);
//   3. the DMA_DATA readback is the E27 packet (experiments/E27-m9-inference/gpu-residency-probe.c:202-203);
//   4. the three private blobs monfence sends pass the KMD's own reader (driver/kmd/umd_blob.c), with the cache
//      policy the client relies on (version 1: write-combined, not CPU-cached);
//   5. fence values are monotonic per fence, change both halves at every step and never collide across fences;
//   6. exit codes are distinct and never 1; the IH client ids the lab runner matches are the AMD ones.
// Exit 0 when all pass, else the number of failed checks.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../../driver/amdgpu-import/nvd.h"
#include "../../../third_party/linux-amdgpu/navi10_enum.h"
#include "../../../third_party/linux-amdgpu/soc15_ih_clientid.h"
#include "../../../driver/kmd/umd_blob.h"
#include "monfence_blobs.h"
#include "monfence_packets.h"

static int g_Failed, g_Checks;

static void Check(int ok, const char* what, unsigned long long got, unsigned long long want)
{
    g_Checks++;
    if (ok) return;
    g_Failed++;
    printf("FAIL %s: got 0x%llX, want 0x%llX\n", what, got, want);
}

#define EQ(what, got, want) Check((unsigned long long)(got) == (unsigned long long)(want), what, \
                                  (unsigned long long)(got), (unsigned long long)(want))
// For packet dwords: nvd.h's PACKET3() is a signed int expression (3 << 30), so compare the 32-bit words.
#define EQ32(what, got, want) EQ(what, (uint32_t)(got), (uint32_t)(want))

// bc250_gfx_emit_fence()'s two selector dwords, expanded here from the same nvd.h macros in the same order, so a
// change in the shim's packet shows up as a review diff against this copy and in the E13 equality below.
static uint32_t ShimDw1(void)
{
    return (PACKET3_RELEASE_MEM_GCR_SEQ | PACKET3_RELEASE_MEM_GCR_GL2_WB | PACKET3_RELEASE_MEM_GCR_GLM_INV |
            PACKET3_RELEASE_MEM_GCR_GLM_WB | PACKET3_RELEASE_MEM_CACHE_POLICY(3) |
            PACKET3_RELEASE_MEM_EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) | PACKET3_RELEASE_MEM_EVENT_INDEX(5));
}

static uint32_t ShimDw2(int write64bit, int int_sel)
{
    return (uint32_t)(PACKET3_RELEASE_MEM_DATA_SEL(write64bit ? 2 : 1) | PACKET3_RELEASE_MEM_INT_SEL(int_sel ? 2 : 0));
}

static void Packets(void)
{
    uint32_t dw[8];
    unsigned n;

    EQ("MF_PACKET_TYPE3", MF_PACKET_TYPE3, PACKET_TYPE3);
    EQ("MF_PACKET3_NOP", MF_PACKET3_NOP, PACKET3_NOP);
    EQ("MF_PACKET3_RELEASE_MEM", MF_PACKET3_RELEASE_MEM, PACKET3_RELEASE_MEM);
    EQ("MF_PACKET3_DMA_DATA", MF_PACKET3_DMA_DATA, PACKET3_DMA_DATA);
    EQ32("MF_PACKET3(RELEASE_MEM,6)", MF_PACKET3(MF_PACKET3_RELEASE_MEM, 6), PACKET3(PACKET3_RELEASE_MEM, 6));
    EQ32("MF_PACKET3(DMA_DATA,5)", MF_PACKET3(MF_PACKET3_DMA_DATA, 5), PACKET3(PACKET3_DMA_DATA, 5));
    EQ32("MF_PACKET3(NOP,6)", MF_PACKET3(MF_PACKET3_NOP, 6), PACKET3(PACKET3_NOP, 6));
    EQ("GLM_WB", MF_RM_GCR_GLM_WB, PACKET3_RELEASE_MEM_GCR_GLM_WB);
    EQ("GLM_INV", MF_RM_GCR_GLM_INV, PACKET3_RELEASE_MEM_GCR_GLM_INV);
    EQ("GL2_WB", MF_RM_GCR_GL2_WB, PACKET3_RELEASE_MEM_GCR_GL2_WB);
    EQ("GCR_SEQ", MF_RM_GCR_SEQ, PACKET3_RELEASE_MEM_GCR_SEQ);
    EQ("CACHE_POLICY(3)", MF_RM_CACHE_POLICY(3), PACKET3_RELEASE_MEM_CACHE_POLICY(3));
    EQ("EVENT_TYPE(x)", MF_RM_EVENT_TYPE(0x14), PACKET3_RELEASE_MEM_EVENT_TYPE(0x14));
    EQ("EVENT_INDEX(5)", MF_RM_EVENT_INDEX(5), PACKET3_RELEASE_MEM_EVENT_INDEX(5));
    EQ("DATA_SEL(2)", MF_RM_DATA_SEL(2), PACKET3_RELEASE_MEM_DATA_SEL(2));
    EQ("INT_SEL(3)", MF_RM_INT_SEL(3), PACKET3_RELEASE_MEM_INT_SEL(3));
    EQ("DST_SEL(0)", MF_RM_DST_SEL(0), PACKET3_RELEASE_MEM_DST_SEL(0));
    EQ("CACHE_FLUSH_AND_INV_TS_EVENT", MF_EVENT_CACHE_FLUSH_AND_INV_TS, CACHE_FLUSH_AND_INV_TS_EVENT);
    EQ("DMA_DATA_CP_SYNC", MF_DMA_DATA_CP_SYNC, (uint32_t)PACKET3_DMA_DATA_CP_SYNC);

    // The KMD's fence packet.
    EQ("dw1 == shim", mf_release_mem_dw1(), ShimDw1());
    EQ("dw1 == E13 Linux ring word", mf_release_mem_dw1(), 0x06603514u);

    // KMD selectors (32-bit, INT_SEL 2): Linux amdgpu's fence on unit A, word for word.
    n = mf_emit_release_mem(dw, 0x401080ull, 0x1234ull, MF_DATA_SEL_32, MF_INT_SEL_IRQ_CONFIRM);
    EQ("emit 32/2 count", n, 8);
    EQ("emit 32/2 dw0 == E13", dw[0], 0xC0064900u);
    EQ("emit 32/2 dw1 == E13", dw[1], 0x06603514u);
    EQ("emit 32/2 dw2 == E13", dw[2], 0x22000000u);
    EQ("emit 32/2 dw2 == shim", dw[2], ShimDw2(0, 1));

    // What monfence writes: 64-bit, INT_SEL 0.
    n = mf_emit_release_mem(dw, 0x0000000004120040ull, 0x000BC002A5000021ull, MF_DATA_SEL_64, MF_INT_SEL_NONE);
    EQ("emit 64/0 count", n, 8);
    EQ32("emit 64/0 dw0", dw[0], PACKET3(PACKET3_RELEASE_MEM, 6));
    EQ("emit 64/0 dw1", dw[1], ShimDw1());
    EQ("emit 64/0 dw2 == shim(64, no int)", dw[2], ShimDw2(1, 0));
    EQ("emit 64/0 addr lo", dw[3], 0x04120040u);
    EQ("emit 64/0 addr hi", dw[4], 0u);
    EQ("emit 64/0 data lo", dw[5], 0xA5000021u);
    EQ("emit 64/0 data hi", dw[6], 0x000BC002u);
    EQ("emit 64/0 dw7", dw[7], 0u);

    // INT_SEL 3: RADV's dword 2 with DATA_SEL 1, and the 64-bit form monfence uses.
    n = mf_emit_release_mem(dw, 0x1000ull, 1, MF_DATA_SEL_32, MF_INT_SEL_DATA_CONFIRM);
    EQ("emit 32/3 dw2 == RADV E27", dw[2], 0x23000000u);
    n = mf_emit_release_mem(dw, 0x1000ull, 1, MF_DATA_SEL_64, MF_INT_SEL_DATA_CONFIRM);
    EQ("emit 64/3 dw2", dw[2], (uint32_t)(PACKET3_RELEASE_MEM_DATA_SEL(2) | PACKET3_RELEASE_MEM_INT_SEL(3)));

    // Refusals: the shim's alignment rule and the 48-bit VA.
    EQ("emit 64 misaligned refused", mf_emit_release_mem(dw, 0x1004ull, 1, MF_DATA_SEL_64, 0), 0);
    EQ("emit 32 misaligned refused", mf_emit_release_mem(dw, 0x1002ull, 1, MF_DATA_SEL_32, 0), 0);
    EQ("emit va>48 bits refused", mf_emit_release_mem(dw, 1ull << 48, 1, MF_DATA_SEL_64, 0), 0);
    EQ("emit INT_SEL 4 refused", mf_emit_release_mem(dw, 0x1000ull, 1, MF_DATA_SEL_64, 4), 0);

    // E27 readback.
    n = mf_emit_dma_copy(dw, 0x4120040ull, 0x200100ull, 8);
    EQ("dma count", n, 8);
    EQ32("dma dw0 == E27", dw[0], PACKET3(PACKET3_DMA_DATA, 5));
    EQ("dma dw1 == E27", dw[1], (uint32_t)PACKET3_DMA_DATA_CP_SYNC);
    EQ("dma src lo", dw[2], 0x04120040u);
    EQ("dma dst lo", dw[4], 0x00200100u);
    EQ("dma bytes", dw[6], 8u);
    EQ("dma pad", dw[7], MF_CP_NOP);

    // NOP: count = total - 2 (gfx_v10_0.c:9505), one header plus seven ignored dwords.
    n = mf_emit_nop(dw);
    EQ("nop count", n, 8);
    EQ32("nop dw0", dw[0], PACKET3(PACKET3_NOP, 6));
    EQ("nop header count field + 2 == 8", CP_PACKET_GET_COUNT(dw[0]) + 2u, MF_IB_DWORDS);
}

static void Blobs(void)
{
    struct bc250_umd_alloc_private a;
    struct bc250_umd_context_private c;
    struct bc250_umd_submit_private s;
    struct umd_alloc_view av;
    struct umd_context_view cv;
    struct umd_submit_view sv;
    uint32_t size;

    EQ("alloc blob size", sizeof(a), UMD_BLOB_ALLOC_BYTES);
    EQ("context blob size", sizeof(c), UMD_BLOB_CONTEXT_BYTES);
    EQ("submit blob size", sizeof(s), UMD_BLOB_SUBMIT_BYTES);
    EQ("GTT domain", AMDGPU_GEM_DOMAIN_GTT, UMD_BLOB_HEAP_GTT);
    EQ("GFX ip", AMDGPU_HW_IP_GFX, UMD_BLOB_IP_GFX);

    mf_build_alloc_blob(&a, 0x10000ull, AMDGPU_GEM_DOMAIN_GTT);
    EQ("alloc parse", UmdBlobParseAlloc(&a, sizeof(a), &av), UMD_BLOB_OK);
    EQ("alloc bytes", av.bytes, 0x10000ull);
    EQ("alloc heap", av.heap, UMD_BLOB_HEAP_GTT);
    EQ("alloc exact va", av.exact_va, 0);
    EQ("alloc v1 not CPU-cached (WC backing, non-snooped PTEs)", UmdBlobAllocCpuCached(&av), 0);

    mf_build_context_blob(&c);
    EQ("context parse node 0", UmdBlobParseContext(&c, sizeof(c), 0, &cv), UMD_BLOB_OK);
    EQ("context ip", cv.ip_type, UMD_BLOB_IP_GFX);
    EQ("context refused on node 1", UmdBlobParseContext(&c, sizeof(c), 1, &cv), UMD_BLOB_BAD_NODE);

    size = mf_build_submit_blob(&s, 0x0000000004200040ull, 32, 0x4120040ull, 7);
    EQ("submit size word", size, UMD_BLOB_SUBMIT_PREFIX + UMD_BLOB_IB_BYTES);
    EQ("submit parse", UmdBlobParseSubmit(&s, size, &sv), UMD_BLOB_OK);
    EQ("submit single ib", sv.single_ib, 1);
    EQ("submit ib va", sv.ib_va, 0x0000000004200040ull);
    EQ("submit ib bytes", sv.ib_bytes, 32);
}

static void Values(void)
{
    unsigned f, g, s;
    int monotonic = 1, halves = 1, disjoint = 1;

    for (f = 0; f < MF_FENCE_LIMIT; f++)
        for (s = 1; s < MF_STEP_LIMIT; s++)
        {
            uint64_t a = mf_value(f, s - 1), b = mf_value(f, s);
            if (b <= a) monotonic = 0;
            if ((uint32_t)a == (uint32_t)b || (uint32_t)(a >> 32) == (uint32_t)(b >> 32)) halves = 0;
        }
    // Ranges: fence f's values are [mf_value(f, 0), mf_value(f, 255)]; no two ranges may overlap, and no value of
    // one fence may equal a torn mix (hi of one step, lo of another) that is a valid value elsewhere.
    for (f = 0; f < MF_FENCE_LIMIT; f++)
        for (g = 0; g < MF_FENCE_LIMIT; g++)
            if (f != g && mf_value(f, 0) <= mf_value(g, MF_STEP_LIMIT - 1) && mf_value(g, 0) <= mf_value(f, MF_STEP_LIMIT - 1))
                disjoint = 0;
    EQ("values monotonic per fence", monotonic, 1);
    EQ("both halves change at every step", halves, 1);
    EQ("fence ranges disjoint", disjoint, 1);
    EQ("value below 2^63", mf_value(MF_FENCE_LIMIT - 1, MF_STEP_LIMIT - 1) >> 63, 0);
    EQ("value nonzero", mf_value(0, 0) != 0, 1);
}

static void Codes(void)
{
    static const int codes[] = {
        MF_EXIT_PASS, MF_EXIT_ARGS, MF_EXIT_SETUP, MF_EXIT_WATCHDOG, MF_EXIT_NO_GPU_VA, MF_EXIT_PACKET_CONTROL,
        MF_EXIT_READ_CONTROL, MF_EXIT_A_VALUE, MF_EXIT_B_WAKE, MF_EXIT_C_ORDER, MF_EXIT_C_RELEASE,
        MF_EXIT_UNEXPECTED_VALUE, MF_EXIT_LATENCY, MF_EXIT_MT_CONTROL, MF_EXIT_MT_A, MF_EXIT_MT_B, MF_EXIT_MT_C_ORDER,
        MF_EXIT_MT_C_RELEASE, MF_EXIT_MT_SYNC, MF_EXIT_MT_UNEXPECTED, MF_EXIT_RESCUE };
    unsigned i, j;
    int distinct = 1, no_one = 1;

    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
    {
        if (codes[i] == 1) no_one = 0;
        for (j = i + 1; j < sizeof(codes) / sizeof(codes[0]); j++)
            if (codes[i] == codes[j]) distinct = 0;
    }
    EQ("exit codes distinct", distinct, 1);
    EQ("exit code 1 unused", no_one, 1);
    EQ("watchdog exit is kmtprobe's 4", MF_EXIT_WATCHDOG, 4);
    // run-lab.ps1 counts IH vectors of these (client, source 0) pairs as VM faults; it states them in decimal.
    EQ("UTCL2 client id (gfxhub faults)", SOC15_IH_CLIENTID_UTCL2, 27);
    EQ("VMC client id (mmhub faults)", SOC15_IH_CLIENTID_VMC, 18);
}

int main(void)
{
    Packets();
    Blobs();
    Values();
    Codes();
    printf("monfence_host_test: %d of %d checks passed, release_mem dw1 0x%08X\n", g_Checks - g_Failed, g_Checks,
           mf_release_mem_dw1());
    return g_Failed;
}
