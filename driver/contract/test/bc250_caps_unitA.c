/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/test/bc250_caps_unitA.c - unit A's caps blob, decoded from the raw ioctl replies.
 *
 * WHAT CHANGED, AND WHY IT MATTERS
 *
 * The first version of this file assembled `struct drm_amdgpu_info_device` field by field out of
 * docs/facts.md, debugfs blobs and dmesg. That was the best we could do without a dump of the
 * ioctl itself. We now have the dump, so the direction is reversed:
 *
 *   INPUT      the raw AMDGPU_INFO replies, as little-endian u32 words, copied verbatim from
 *              evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-windows/info.txt
 *              and memcpy()d into the UAPI structures of third_party/amdgpu_drm.h. Not one field
 *              of DEV_INFO, MEMORY, HW_IP_INFO or FW_VERSION is typed out by hand below.
 *   ASSERTION  the numbers that used to be the input are now the cross-check table at the bottom
 *              of this file (bc250_unitA_crosschecks). gca_config.bin, ip-discovery.bin, dmesg and
 *              fact M5 must AGREE with the bytes the kernel returned. main() fails if they do not.
 *
 * That is a strictly stronger test. Before, a wrong fact produced a wrong blob and nothing noticed.
 * Now a wrong fact is a failed check, and the blob is right either way.
 *
 * Decoding the dump refuted nine things this file previously claimed. All nine are listed in
 * README.md; the corrections are in the bytes below, so they cannot drift back.
 *
 * WHAT IS STILL NOT FROM THE IOCTL
 *
 *   tiling.gb_addr_config   READ_MMR_REG, recorded by name in the same directory's state.txt.
 *   tiling.mc_arb_ramcfg    not read at all on this family; see fill_tiling().
 *   kernel.*                the DRM version and address32_hi are not AMDGPU_INFO queries.
 *
 * Tags on those remaining hand-written values:
 *   [M] measured on unit A, the comment names the evidence file.
 *   [D] derived from an [M] value by arithmetic we can point at in named source.
 *   [K] a constant of the kernel or of Mesa, not of the chip.
 *   [W] WISHLIST: not measured. BC250_UMD_F_UNMEASURED is set and bc250_unitA_wishlist[] says
 *       exactly how to settle it. These are the only guesses left in this file.
 *
 * Evidence directories under bc250-win/evidence/:
 *   E13b4  linux/2026-09-21-E13-reference-2/boot4-readonly-after-windows/  (info.txt, state.txt)
 *          info.txt is the raw ioctl dump; amdgpu 6.18.52, read-only queries only.
 *   E13b3  linux/2026-09-21-E13-reference-2/boot3-readonly/                (state.txt, dmesg.txt)
 *   E01    linux/2026-09-21-E01-recon/ and .../2026-09-21-E01-recon-binaries/
 *          gca_config.bin, ip-discovery.bin, gpu-sysfs.txt, lspci-gpu-vvv.txt, debugfs-rings.txt
 *   E03    linux/2026-09-21-E03-init-trace/dmesg.txt
 */

#include "bc250_umd_private.h"
#include "bc250_umd_private_fields.h"
#include "bc250_caps_test.h"

#include <string.h>

/* ---------------------------------------------------------------------------------------------
 * The raw ioctl replies.
 *
 * Each array is one line of E13b4 info.txt, verbatim: little-endian u32 words with the trailing
 * zero words cut off by the dumper. decode() pads the remainder back with zeros, which is exactly
 * what the kernel's copy_to_user() left there, because amdgpu memsets the reply structure first
 * (amdgpu_kms.c: `struct drm_amdgpu_info_device *dev_info; ... kzalloc`).
 *
 * DO NOT "tidy" these numbers. They are a measurement. The named constants that used to stand in
 * for them are in the cross-check table instead.
 * ------------------------------------------------------------------------------------------- */

/* info.txt: "AMDGPU_INFO_DEV_INFO: 93 words" -> struct drm_amdgpu_info_device (448 bytes). */
static const __u32 g_dev_info_words[] = {
    0x000013feu, 0x00000002u, 0x00000084u, 0x00000000u, 0x0000008fu, 0x00000002u, 0x00000002u,
    0x000186a0u, 0x001e8480u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000018u, 0x3f3f3f3fu,
    0x0000003fu, 0x0000003fu, 0x00000000u, 0x00000000u, 0x0000003fu, 0x0000003fu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x0000ffffu, 0x00000010u, 0x00000008u, 0x00000004u, 0x00000011u,
    0x00000000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00008000u, 0x00001000u, 0x00200000u,
    0x00001000u, 0x00010000u, 0x00000000u, 0x00000400u, 0x00000000u, 0x00000001u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000020u, 0x00000400u, 0x0000000au,
    0x00000010u, 0x00000020u, 0x00000700u, 0x00000020u, 0x00000010u, 0x0000003fu, 0x0000003fu,
    0x00000000u, 0x00000000u, 0x0000003fu, 0x0000003fu, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0xffff8000u, 0xffbf0000u, 0xffffffffu, 0x00122000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x000f4240u
};

/* info.txt: "AMDGPU_INFO_MEMORY: 23 words" -> struct drm_amdgpu_memory_info (96 bytes), three
 * struct drm_amdgpu_heap_info in the order vram, cpu_accessible_vram, gtt. */
static const __u32 g_memory_words[] = {
    0x00000000u, 0x00000002u, 0xfea16000u, 0x00000001u, 0x00df3000u, 0x00000000u, 0x7ef90800u,
    0x00000001u, 0x00000000u, 0x00000002u, 0xfea16000u, 0x00000001u, 0x00df3000u, 0x00000000u,
    0x7ef90800u, 0x00000001u, 0xefa35000u, 0x00000000u, 0xeeda2000u, 0x00000000u, 0x00c93000u,
    0x00000000u, 0xb3239800u
};

/* info.txt: "AMDGPU_INFO_HW_IP_INFO <ip> instance 0: 8 words" -> struct drm_amdgpu_info_hw_ip.
 * Every other IP type answered with a single zero word (UVD, VCE, UVD_ENC, VCN_DEC, VCN_ENC,
 * VCN_JPEG, VPE) or with ERROR 22 on HW_IP_COUNT, so they stay all-zero in the blob. */
static const __u32 g_hw_ip_gfx_words[] = {
    0x0000000au, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000020u, 0x00000020u, 0x00000001u,
    0x000a0103u
};
static const __u32 g_hw_ip_compute_words[] = {
    0x0000000au, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000020u, 0x00000020u, 0x0000000fu,
    0x000a0103u
};
static const __u32 g_hw_ip_dma_words[] = {
    0x00000005u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000100u, 0x00000004u, 0x00000003u,
    0x00050001u
};

/* info.txt: "AMDGPU_INFO_HW_IP_COUNT <ip>". GFX and COMPUTE returned 1, DMA returned 2; every
 * other IP type returned ERROR 22 (EINVAL), which is amdgpu saying the IP does not exist here. */
#define BC250_HW_IP_COUNT_GFX       1u
#define BC250_HW_IP_COUNT_COMPUTE   1u
#define BC250_HW_IP_COUNT_DMA       2u

/* info.txt: "AMDGPU_INFO_FW_VERSION <type> index 0", as {version, feature} word pairs. Types that
 * returned a single zero word (VCE, UVD, GMC, SOS, ASD, VCN, DMCU, DMCUB, TOC, CAP, RLCP, RLCV,
 * MES, MES_KIQ, IMU, VPE) carry no firmware on this part; FW_TA answered ERROR 22. */
static const __u32 g_fw_me_words[]   = { 0x00000063u, 0x00000020u };
static const __u32 g_fw_pfp_words[]  = { 0x00000094u, 0x00000020u };
static const __u32 g_fw_ce_words[]   = { 0x00000025u, 0x00000020u };
static const __u32 g_fw_rlc_words[]  = { 0x0000000du };
static const __u32 g_fw_mec_words[]  = { 0x00000090u, 0x00000020u };
static const __u32 g_fw_sdma_words[] = { 0x00000034u, 0x00000032u };
static const __u32 g_fw_smc_words[]  = { 0x00580600u };

/* info.txt: "AMDGPU_INFO_MAX_IBS: 10 words", indexed by AMDGPU_HW_IP_*. The kernel derives each
 * as (ring->max_dw - emit_frame_size) / emit_ib_size, so these are properties of this ASIC's
 * rings and firmware. E14 radv-info.txt:113-115 prints the same three that matter: GFX 192,
 * COMPUTE 125, SDMA 49.
 *
 * COMPUTE is the interesting one: 125 measured against the 124 Mesa guesses at ac_gpu_info.c:1713
 * when the query fails. One IB of difference, and the guess is the low one, so a UMD running on
 * the fallback simply leaves a slot unused - but it is exactly the kind of number nobody would
 * ever think to check. */
static const __u32 g_max_ibs_words[] = {
    0x000000c0u, 0x0000007du, 0x00000031u, 0x00000031u, 0x00000031u,
    0x00000031u, 0x00000031u, 0x00000031u, 0x00000010u, 0x00000031u
};

/* Copy a reply into its structure, zero-padding the words the dumper cut. Returns the structure
 * so the fill functions read as one statement. */
static void decode(void *dst, size_t dst_size, const __u32 *words, size_t nwords)
{
    memset(dst, 0, dst_size);
    /* A reply longer than the structure would mean the header and the kernel disagree about the
     * shape, which is the one thing this whole test exists to catch. Truncate rather than smash
     * the stack; part 2's size checks report the disagreement. */
    if (nwords * sizeof(__u32) > dst_size)
        nwords = dst_size / sizeof(__u32);
    memcpy(dst, words, nwords * sizeof(__u32));
}

#define BC250_DECODE(dst, words) decode((dst), sizeof(*(dst)), (words), \
                                        sizeof(words) / sizeof((words)[0]))

/* ---------------------------------------------------------------------------------------------
 * Kernel constants still needed for the values the INFO ioctl does not carry.
 * ------------------------------------------------------------------------------------------- */
/* drivers/gpu/drm/amd/amdgpu/amdgpu_object.h */
#define BC250_GPU_PAGE_SIZE     4096u

/* ---------------------------------------------------------------------------------------------
 * The wishlist: what we still have no measured value for.
 *
 * It is three rows now. Everything else on this list was settled by info.txt - gpu_counter_freq,
 * tcc_disabled_mask, pa_sc_tile_steering_override, both unknown ids_flags bits, pcie_gen and
 * pcie_num_lanes all came back as measured values, and four of the six turned out to differ from
 * what this file used to assume.
 * ------------------------------------------------------------------------------------------- */
const char *const bc250_unitA_wishlist[] = {
    "(empty)                          Every field of the blob is measured on unit A.",
    "",
    "Closed by the E13b4 raw ioctl dump (info.txt):",
    "  gpu_counter_freq, tcc_disabled_mask, pa_sc_tile_steering_override, both unknown",
    "  ids_flags bits, pcie_gen, pcie_num_lanes, ce_ram_size, high_va_max, max_memory_clock,",
    "  max_submitted_ibs[] (query 0x22 - it was in the committed dump all along; I had been",
    "  reading a stale copy and asked for a query we already had).",
    "",
    "Closed by the E14 RADV_DEBUG=info dump (radv-info.txt, Mesa 26.1.6 on unit A):",
    "  kernel.drm_minor      line 103, drm = 3.64.0. We had claimed 3.63 from the v6.18 tag.",
    "  kernel.address32_hi   0xffff8000. Not an ioctl - ac_drm computes it in user space - but",
    "                        RADV prints the result, so the number to arrive at is known.",
    "  tiling.gb_addr_config line 186, 0x00100044. See fill_tiling(): this one decided a",
    "                        question two source readings had answered both ways.",
    "",
    "BC250_UMD_F_UNMEASURED is therefore CLEAR. If a field ever goes back on this list, set it",
    "again: a release UMD should refuse a blob that admits to guessing.",
    "",
    "Still worth re-running after any Mesa bump, as a check on the output rather than an input:",
    "  RADV_DEBUG=info vulkaninfo --summary   on unit A. test/compare_radv_info.py diffs it",
    "  against what this test derives, field by field.",
    NULL
};

/* ---------------------------------------------------------------------------------------------
 * The blob.
 * ------------------------------------------------------------------------------------------- */
static struct bc250_umd_private g_blob;

/*
 * The measured DEV_INFO reply, unedited. The commentary explains what the bytes mean and where
 * they will be felt; it does not supply any of them.
 *
 * Reading of note, all now measured rather than assumed:
 *
 *   gpu_counter_freq       0x000186A0 = 100000 kHz = 100 MHz. This file used to carry 0, which
 *                          made ac_gpu_info.c:1231 print "clock crystal frequency is 0,
 *                          timestamps will be wrong" and substitute 1. Real value, real fix.
 *   max_memory_clock       0. Not a dumper artefact: E01 gpu-sysfs.txt pp_dpm_mclk shows one DPM
 *                          level at 450 MHz and E13b4's own SENSOR_GFX_MCLK reads 0x1C2 = 450 MHz,
 *                          so the memory really is running - amdgpu simply reports 0 in DEV_INFO
 *                          on this part. Mesa divides by 1000 and gets memory_freq_mhz 0, and that
 *                          is what RADV sees on Linux today. Parity means carrying the 0.
 *   pa_sc_tile_steering_override 0x00122000, not the 0 this file assumed. amdgpu_kms.c fills it
 *                          for family >= AMDGPU_FAMILY_NV from adev->gfx.config; it is non-zero
 *                          here and reaches addrlib, so the old assumption was not harmless.
 *   ids_flags              0x11 = FUSION | GANG_SUBMIT. PREEMPTION (0x02) and TMZ (0x04) are
 *                          clear, which confirms E03 dmesg's "TMZ ... feature disabled" line from
 *                          the other side, and CONFORMANT_TRUNC_COORD (0x08) is clear as gfx10.1
 *                          requires.
 *   ce_ram_size            0x10000 = 64 KiB, not the 0 this file asserted on the theory that
 *                          gfx10 has no CE RAM. amdgpu reports 64 KiB. Mesa does not read it.
 *   high_va_max            0xFFFFFFFFFFBF0000. This file computed 0xFFFFFFFFFFBFE000 from
 *                          AMDGPU_VA_RESERVED_TOP = trap + seq64 + CSA = 0x402000 at mainline
 *                          v6.18. The measurement implies 0x410000 on the 6.18.52 kernel that
 *                          answered. The kernel is the authority on its own reserved range, so
 *                          the derivation is gone and the byte stays.
 *   cu_bitmap / cu_ao_mask exactly the 0x3F per shader array and 0x3F3F3F3F this file derived
 *                          from fact M5's CC_GC_SHADER_ARRAY_CONFIG. Byte-identical. See the
 *                          cross-check table - that derivation is now confirmed, not assumed.
 */
static void fill_device(struct drm_amdgpu_info_device *d)
{
    BC250_DECODE(d, g_dev_info_words);
}

/*
 * The measured MEMORY reply.
 *
 * vram.total_heap_size and cpu_accessible_vram.total_heap_size are the same 0x200000000 = 8 GiB,
 * which is the measurement behind BC250_UMD_F_ALL_VRAM_VISIBLE and behind Mesa's
 * info->all_vram_visible. E13b4's VRAM_GTT reply says it twice more: vram_size and
 * vram_cpu_accessible_size are both 0x1FEA16000. No BAR limit on this part.
 *
 * gtt.total_heap_size is 0xEFA35000 = 4020457472. E13b3's state.txt recorded 4020453376 and E01
 * recorded 4020506624 on earlier boots. All three are right: amdgpu sizes the GTT from free system
 * RAM at init, so it moves between boots. The contract is pinned to the boot4 figure because that
 * is the boot every other byte in this file came from.
 *
 * usable_heap_size, heap_usage and max_allocation are instantaneous ttm numbers, not properties of
 * the part. ac_gpu_info.c:554 says outright that usable_heap_size "can be random and can't be
 * relied on" and reads only total_heap_size. They are carried so that a diff against the capture
 * is exact.
 */
static void fill_memory(struct drm_amdgpu_memory_info *m)
{
    BC250_DECODE(m, g_memory_words);
}

/*
 * The measured HW_IP_INFO replies.
 *
 * Both GFX and COMPUTE report ip_discovery_version 0x000A0103. Mesa takes ver_major/minor/rev
 * straight from that field when it is non-zero (ac_gpu_info.c:509-513), so the ioctl states
 * gfx 10.1.3 outright - it is no longer an inference from E01's IP discovery table. DMA reports
 * 0x00050001, SDMA 5.0.1. The hw_ip_version_major/minor fields say 10.0 and 5.0, which are the
 * amdgpu IP block's own version numbers; Mesa ignores them on this path and so do we.
 *
 * COMPUTE's available_rings is 0x0F, four queues - not the 0xFF this file assumed from E01
 * debugfs-rings.txt, which lists eight comp_* ring files. amdgpu advertises a ring here only if
 * its scheduler is ready, so the debugfs file list and the ioctl are answering different
 * questions. It changes nothing downstream: ac_gpu_info.c:501-504 drops AMD_IP_COMPUTE entirely
 * for FAMILY_NV in the GFX1013 range ("GFX1013 is known to have broken compute queue"), so RADV
 * sees zero compute queues whether the blob says four or eight. The blob carries four because
 * four is what was measured.
 *
 * ib_start_alignment / ib_size_alignment come back 32/32 for GFX and COMPUTE and 256/4 for DMA,
 * which is what this file already had. Mesa raises all of them to at least 256 at :540 anyway.
 */
static void fill_hw_ip(struct bc250_umd_private *b)
{
    BC250_DECODE(&b->hw_ip[AMDGPU_HW_IP_GFX],     g_hw_ip_gfx_words);
    BC250_DECODE(&b->hw_ip[AMDGPU_HW_IP_COMPUTE], g_hw_ip_compute_words);
    BC250_DECODE(&b->hw_ip[AMDGPU_HW_IP_DMA],     g_hw_ip_dma_words);

    b->hw_ip_mask = (1u << AMDGPU_HW_IP_GFX) | (1u << AMDGPU_HW_IP_COMPUTE) |
                    (1u << AMDGPU_HW_IP_DMA);
    b->hw_ip_instances[AMDGPU_HW_IP_GFX]     = BC250_HW_IP_COUNT_GFX;
    b->hw_ip_instances[AMDGPU_HW_IP_COMPUTE] = BC250_HW_IP_COUNT_COMPUTE;
    b->hw_ip_instances[AMDGPU_HW_IP_DMA]     = BC250_HW_IP_COUNT_DMA;

    /* Every video IP answered HW_IP_COUNT with ERROR 22 and HW_IP_INFO with a single zero word,
     * so available_rings stays 0 for all of them. That keeps ac_query_gpu_info() out of the VCN,
     * VCE and UVD firmware queries at ac_gpu_info.c:1566-1596, which is the behaviour we want:
     * E13b4's FW_VERSION replies for VCN, VCE and UVD are all 0 too. */
}

static void fill_tiling(struct bc250_umd_tiling *t)
{
    memset(t, 0, sizeof(*t));
    /* [M] 0x00100044, the golden constant - MEASURED at the user-mode boundary, not inferred.
     * E14 radv-info.txt:186, Mesa 26.1.6 on unit A: "GB_ADDR_CONFIG: 0x00100044". That line is
     * Mesa printing what READ_MMR_REG returned to it.
     *
     * Two values exist for this register on this part, and the difference is not academic:
     *
     *   0x00000044  what an RREG32 returns. Fact M46, and E13b4 state.txt, which reads through
     *               debugfs amdgpu_regs2 - a raw register read that never reaches user mode.
     *   0x00100044  what the AMDGPU_INFO_READ_MMR_REG ioctl returns, and therefore what every
     *               user-mode driver on Linux sees. Fact M50.
     *
     * The ioctl does NOT do an RREG32 for this register. The path is amdgpu_info ->
     * amdgpu_asic_read_register -> nv_read_register -> nv_get_register_value, and
     * nv.c:381-382 reads:
     *
     *     if (reg_offset == SOC15_REG_OFFSET(GC, 0, mmGB_ADDR_CONFIG))
     *             return adev->gfx.config.gb_addr_config;
     *
     * an explicit special case that returns the cached value instead of touching the hardware.
     * It is reached because nv.c:354 lists mmGB_ADDR_CONFIG in nv_allowed_read_registers with
     * grbm_indexed unset, so nv_read_register (nv.c:402-404) passes indexed = false and
     * nv_get_register_value takes the else branch. The cached value is the golden constant
     * CYAN_SKILLFISH_GB_ADDR_CONFIG_GOLDEN (gfx_v10_0.c:3675), assigned at gfx_v10_0.c:4619 in
     * the IP_VERSION(10,1,3) arm of gfx_v10_0_gpu_early_init() without reading the register.
     *
     * I got this backwards once, between two reports, by assuming READ_MMR_REG implies a real
     * register read. It does not on this family. The measurement settled it; the source reading
     * above explains the measurement, which is the only reason the story is closed.
     *
     * FOR THE WINDOWS KMD: do NOT fill this field from a register read. Use the constant, as
     * nv.c does. A driver that reads the hardware here hands addrlib 0x00000044 and diverges
     * from Linux in surface layout. BC250_UMD_F_GOLDEN_GB_ADDR records that the constant is what
     * is in the blob, and BC250_MUTATE_GB_ADDR_REGISTER shows what the register value would do. */
    t->gb_addr_config = 0x00100044u;
    /* [K] Zero because Mesa never fills it on this family, not because the register is zero.
     * ac_linux_drm.c:705-748 gates the mc_arb_ramcfg, gb_tile_mode, gb_macro_tile_mode,
     * backend_disable and pa_sc_raster_cfg reads behind `family_id < AMDGPU_FAMILY_AI` (141). Our
     * family is 143, so amdinfo.mc_arb_ramcfg is left at its initialised 0 and Mesa copies that 0
     * through at ac_gpu_info.c:456. E13b4 state.txt agrees from the other side: the registers it
     * does list under this gate (CC_RB_BACKEND_DISABLE, GC_USER_RB_BACKEND_DISABLE,
     * PA_SC_RASTER_CONFIG, PA_SC_RASTER_CONFIG_1) all read 0 as well. */
    t->mc_arb_ramcfg = 0u;
    /* [K] gb_tile_mode and gb_macro_tile_mode are gfx6-to-gfx8 tiling tables, left zero by the
     * same gate. Mesa only memcpy()s them (ac_gpu_info.c:472-475); the gfx9+ branch at :458 uses
     * gb_addr_config instead. Zero is correct, not missing. */
}

/*
 * The measured FW_VERSION replies, as {version, feature} pairs.
 *
 * All seven match what E13b3 state.txt's amdgpu_firmware_info block reported on the previous boot,
 * to the digit. That is the strongest kind of agreement in this file: a debugfs text dump and a
 * raw ioctl on two different boots returning the same numbers.
 *
 * The feature versions matter more than they look. ac_query_gpu_info() refuses the device outright
 * if the ME, MEC or PFP firmware query fails (ac_gpu_info.c:1546-1565), and RADV gates workarounds
 * on the feature version. A WDDM winsys has no ioctl to ask; this blob is where it must come from.
 */
static void fill_firmware(struct bc250_umd_firmware *f)
{
    memset(f, 0, sizeof(*f));
    f->me_version   = g_fw_me_words[0];   f->me_feature   = g_fw_me_words[1];
    f->pfp_version  = g_fw_pfp_words[0];  f->pfp_feature  = g_fw_pfp_words[1];
    f->ce_version   = g_fw_ce_words[0];   f->ce_feature   = g_fw_ce_words[1];
    f->rlc_version  = g_fw_rlc_words[0];  f->rlc_feature  = 0u;   /* reply was one word */
    f->mec_version  = g_fw_mec_words[0];  f->mec_feature  = g_fw_mec_words[1];
    /* [M] FW_GFX_MEC index 1 returned the same pair as index 0. */
    f->mec2_version = g_fw_mec_words[0];  f->mec2_feature = g_fw_mec_words[1];
    f->sdma_version = g_fw_sdma_words[0]; f->sdma_feature = g_fw_sdma_words[1];
    f->smc_version  = g_fw_smc_words[0];  /* 0x00580600, SMC 88.6.0 */
}

static void fill_kernel(struct bc250_umd_kernel *k)
{
    memset(k, 0, sizeof(*k));
    /* [M] E14 radv-info.txt:103 "drm = 3.64.0". Not an AMDGPU_INFO query - Mesa gets it from
     * DRM_IOCTL_VERSION - so info.txt could never have contained it. We claimed 3.63 from the
     * v6.18 tag; the device says 3.64. ac_gpu_info.c:1493 requires major 3 and :1496 refuses
     * below 3.54, so both would have passed, which is exactly why it needed measuring rather
     * than asserting. */
    k->drm_major = 3u;
    k->drm_minor = 64u;
    k->drm_patchlevel = 0u;
    /* [M] E14 radv-info.txt "address32_hi = 0xffff8000". Still not an ioctl - ac_drm computes it
     * in user space from the VA range - but RADV prints the result, so the number our winsys has
     * to arrive at is now known rather than guessed. Note it equals the high word of
     * high_va_offset (0xffff800000000000), which is where the computation lands. */
    k->address32_hi = 0xffff8000u;
    /* [M] E03 dmesg.txt and E01 lspci-gpu-vvv.txt: the GPU is 0000:01:00.0. */
    k->pci_domain = 0u;
    k->pci_bus    = 1u;
    k->pci_dev    = 0u;
    k->pci_func   = 0u;
}

const char *bc250_mutation_name(enum bc250_mutation m)
{
    switch (m) {
    case BC250_MUTATE_NONE:             return "none (unit A as measured)";
    case BC250_MUTATE_FAMILY:           return "family := FAMILY_NV3";
    case BC250_MUTATE_EXTERNAL_REV:     return "external_rev := 0x90 (outside the GFX1013 range)";
    case BC250_MUTATE_GFX_IP_VERSION:   return "GC IP version := 10.3.0";
    case BC250_MUTATE_CU_BITMAP_ZERO:   return "cu_bitmap := all zero";
    case BC250_MUTATE_CU_BITMAP_FULL:   return "cu_bitmap := 0x3FF (all 10 CUs per SA)";
    case BC250_MUTATE_VRAM_SIZE:        return "vram.total_heap_size := 4 GiB";
    case BC250_MUTATE_GB_ADDR_REGISTER: return "gb_addr_config := 0x00000044 (the raw register, fact M46)";
    default:                            return "?";
    }
}

const void *bc250_unitA_blob(enum bc250_mutation mutation, uint32_t *size_out)
{
    struct bc250_umd_private *b = &g_blob;

    memset(b, 0, sizeof(*b));
    b->magic   = BC250_UMD_PRIVATE_MAGIC;
    b->version = BC250_UMD_PRIVATE_VERSION;
    b->size    = (__u32)sizeof(*b);
    /* Every field is measured now, so BC250_UMD_F_UNMEASURED is clear. BC250_UMD_F_GOLDEN_GB_ADDR
     * is set: the blob carries the golden constant, which is what the READ_MMR_REG ioctl hands
     * user mode on Linux. See fill_tiling(). */
    b->flags   = BC250_UMD_F_APU | BC250_UMD_F_ALL_VRAM_VISIBLE | BC250_UMD_F_GOLDEN_GB_ADDR;

    fill_device(&b->device);
    fill_memory(&b->memory);
    fill_hw_ip(b);
    BC250_DECODE(&b->max_submitted_ibs, g_max_ibs_words);
    fill_tiling(&b->tiling);
    fill_firmware(&b->firmware);
    fill_kernel(&b->kernel);

    switch (mutation) {
    case BC250_MUTATE_NONE:
        break;
    case BC250_MUTATE_FAMILY:
        b->device.family = 0x91u;                  /* FAMILY_NV3, amdgpu_asic_addr.h:29 */
        break;
    case BC250_MUTATE_EXTERNAL_REV:
        b->device.external_rev = 0x90u;            /* past AMDGPU_GFX1013_RANGE 0x82..0x86 */
        break;
    case BC250_MUTATE_GFX_IP_VERSION:
        b->hw_ip[AMDGPU_HW_IP_GFX].ip_discovery_version = (10u << 16) | (3u << 8) | 0u;
        b->hw_ip[AMDGPU_HW_IP_COMPUTE].ip_discovery_version = (10u << 16) | (3u << 8) | 0u;
        break;
    case BC250_MUTATE_CU_BITMAP_ZERO:
        memset(b->device.cu_bitmap, 0, sizeof(b->device.cu_bitmap));
        b->device.cu_active_number = 0u;
        break;
    case BC250_MUTATE_CU_BITMAP_FULL: {
        unsigned se, sa;
        for (se = 0; se < 2; se++)
            for (sa = 0; sa < 2; sa++)
                b->device.cu_bitmap[se][sa] = 0x3FFu;
        b->device.cu_active_number = 40u;
        break;
    }
    case BC250_MUTATE_VRAM_SIZE:
        b->memory.vram.total_heap_size = 4294967296ull;
        b->memory.cpu_accessible_vram.total_heap_size = 4294967296ull;
        break;
    case BC250_MUTATE_GB_ADDR_REGISTER:
        /* What an RREG32 of GB_ADDR_CONFIG returns (fact M46, E13b4 state.txt via debugfs
         * amdgpu_regs2) - the value a Windows KMD would pick up if it read the hardware instead
         * of using the constant nv.c:381-382 hands out. */
        b->tiling.gb_addr_config = 0x00000044u;
        b->flags &= ~(__u32)BC250_UMD_F_GOLDEN_GB_ADDR;
        break;
    default:
        break;
    }

    if (size_out)
        *size_out = (uint32_t)sizeof(*b);
    return b;
}

/* ---------------------------------------------------------------------------------------------
 * The cross-check table: independent evidence against the decoded ioctl bytes.
 *
 * Every row is a number this file used to SUPPLY and now only CLAIMS. main() reads the field out
 * of the decoded blob and fails if it differs. A row passing means two separate captures, taken by
 * different mechanisms, agree about unit A.
 *
 * Deliberately absent: gtt.total_heap_size (it moves between boots, see fill_memory) and
 * max_memory_clock (E01 pp_dpm_mclk and E13b4 SENSOR_GFX_MCLK both say 450 MHz while DEV_INFO
 * says 0 - a real divergence inside amdgpu, documented in README.md, not a check).
 * ------------------------------------------------------------------------------------------- */
#define DEVOFF(m)  ((uint32_t)offsetof(struct drm_amdgpu_info_device, m))
#define DEVSZ(m)   ((uint32_t)sizeof(((struct drm_amdgpu_info_device *)0)->m))
#define DEVROW(m, expect, src) { #m, DEVOFF(m), DEVSZ(m), (uint64_t)(expect), (src) }

static const struct bc250_crosscheck g_crosschecks[] = {
    /* --- identity --- */
    DEVROW(device_id, 0x13FEu,
           "E03 dmesg.txt:793 \"CYAN_SKILLFISH 0x1002:0x13FE\"; E01 lspci \"[1002:13fe]\""),
    DEVROW(chip_rev, 2u,          "E01 gca_config.bin dword 24 (adev->rev_id)"),
    DEVROW(external_rev, 132u,    "E01 gca_config.bin dword 28 (external_rev_id); 0x84 is inside "
                                  "AMDGPU_GFX1013_RANGE 0x82..0x86"),
    DEVROW(pci_rev, 0u,           "E01 gca_config.bin dword 30; E03 dmesg trailing 0x00; lspci "
                                  "prints no \"(rev xx)\""),
    DEVROW(family, 0x8Fu,         "E01 gca_config.bin dword 27 (adev->family) = 143 = FAMILY_NV"),

    /* --- shader array geometry: three captures agree on each of these --- */
    DEVROW(num_shader_engines, 2u,
           "E01 gca_config.bin dword 1; E01 ip-discovery.bin gc_num_se; E03/E13 dmesg \"SE 2\""),
    DEVROW(num_shader_arrays_per_engine, 2u,
           "E01 gca_config.bin dword 4; ip-discovery gc_num_sa_per_se; dmesg \"SH per SE 2\""),
    DEVROW(num_cu_per_sh, 10u,
           "E01 gca_config.bin dword 3; ip-discovery 2*(gc_num_wgp0_per_sa 3 + gc_num_wgp1_per_sa "
           "2); dmesg \"CU per SH 10\""),
    DEVROW(cu_active_number, 24u, "E03 and E13 dmesg \"active_cu_number 24\", four boots"),

    /* --- THE derivation. Fact M5 measured CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000. Run through
     * gfx_v10_0_get_wgp_active_bitmap_per_sh() (INACTIVE_WGPS = bits 31:16 = 0xFFF8, inverted ->
     * WGPs 0,1,2 live) and gfx_v10_0_get_cu_active_bitmap_per_sh() (each live WGP becomes two CUs)
     * that predicts 0x3F per shader array. The ioctl returns 0x3F. The prediction was right, and
     * this row is the proof. 0x3F is 6 CUs; times the four SAs is the 24 dmesg printed. --- */
    { "cu_bitmap[0][0]", DEVOFF(cu_bitmap[0][0]), 4u, 0x3Fu,
      "fact M5 CC_GC_SHADER_ARRAY_CONFIG 0xFFF80000 through gfx_v10_0_get_cu_active_bitmap_per_sh()" },
    { "cu_bitmap[0][1]", DEVOFF(cu_bitmap[0][1]), 4u, 0x3Fu, "fact M5, as above" },
    { "cu_bitmap[1][0]", DEVOFF(cu_bitmap[1][0]), 4u, 0x3Fu, "fact M5, as above" },
    { "cu_bitmap[1][1]", DEVOFF(cu_bitmap[1][1]), 4u, 0x3Fu, "fact M5, as above" },
    DEVROW(cu_ao_mask, 0x3F3F3F3Fu, "the same four bitmaps, packed by gfx_v10_0_get_cu_info()"),

    /* --- render backends and caches --- */
    DEVROW(enabled_rb_pipes_mask, 0xFFFFu,
           "E01 gca_config.bin dword 15 (backend_enable_mask), built by gfx_v10_0_setup_rb() from "
           "CC_RB_BACKEND_DISABLE per SE/SA: all 16 RBs live"),
    DEVROW(num_rb_pipes, 16u,
           "E01 gca_config.bin dword 5 (8 per SE) * dword 1 (2 SEs); ip-discovery gc_num_rb_per_se"),
    DEVROW(num_tcc_blocks, 16u,
           "E01 gca_config.bin dword 6 (max_texture_channel_caches); ip-discovery gc_num_gl2c"),

    /* --- the rest of the gfx config --- */
    DEVROW(num_hw_gfx_contexts, 8u,      "E01 gca_config.bin dword 9 (max_hw_contexts)"),
    DEVROW(num_shader_visible_vgprs, 1024u,
           "E01 gca_config.bin dword 7; ip-discovery gc_num_gprs"),
    DEVROW(wave_front_size, 32u,         "E01 ip-discovery.bin gc_wave_size"),
    DEVROW(gs_vgt_table_depth, 32u,      "E01 ip-discovery.bin gc_gs_table_depth"),
    DEVROW(gs_prim_buffer_depth, 1792u,  "E01 ip-discovery.bin gc_gsprim_buff_depth = 0x700"),
    DEVROW(max_gs_waves_per_vgt, 32u,    "E01 gca_config.bin dword 8 (max_gs_threads)"),
    DEVROW(gc_double_offchip_lds_buf, 1u,"E01 ip-discovery.bin gc_double_offchip_lds_buffer"),

    /* --- memory --- */
    DEVROW(vram_bit_width, 1024u,        "E03 dmesg.txt \"RAM width 1024bits\""),
    DEVROW(vram_type, 0u,
           "E03 dmesg.txt \"RAM width 1024bits UNKNOWN\" = AMDGPU_VRAM_TYPE_UNKNOWN; amdgpu could "
           "not identify the memory on this part"),
    DEVROW(gart_page_size, BC250_GPU_PAGE_SIZE,
           "E13b3 state.txt:349 \"default_page_size: 4KiB\""),
    DEVROW(pte_fragment_size, 512u * BC250_GPU_PAGE_SIZE,
           "E03 dmesg.txt:809 \"fragment size is 9-bit\": (1 << 9) * AMDGPU_GPU_PAGE_SIZE"),
    DEVROW(virtual_address_max, 0x0000800000000000ull,
           "E03 dmesg.txt:809 \"vm size is 262144 GB\" = 2^48, clamped to AMDGPU_GMC_HOLE_START"),

    /* --- clocks --- */
    DEVROW(max_engine_clock, 2000000ull,
           "E01 gpu-sysfs.txt pp_dpm_sclk top level 2000Mhz, * 10 for kHz; pp_od_clk_voltage "
           "OD_RANGE SCLK 1000..2000Mhz agrees"),
    DEVROW(min_engine_clock, 1000000ull, "E01 gpu-sysfs.txt pp_dpm_sclk level 0, 1000Mhz"),

    /* --- flags --- */
    DEVROW(ids_flags, 0x11u,
           "E01 gca_config.bin dword 33 (adev->flags & AMD_IS_APU) = 1 gives the FUSION bit; E03 "
           "dmesg \"TMZ ... feature disabled\" gives the clear TMZ bit; GANG_SUBMIT measured"),
};

unsigned bc250_unitA_crosschecks(const struct bc250_crosscheck **out)
{
    *out = g_crosschecks;
    return (unsigned)(sizeof(g_crosschecks) / sizeof(g_crosschecks[0]));
}

uint64_t bc250_blob_read_field(const void *blob, uint32_t offset, uint32_t size)
{
    const unsigned char *p = (const unsigned char *)blob + offset;
    uint64_t v = 0;
    uint32_t i;
    /* Little-endian by construction: the blob is a byte image and x86/ARM64 Windows are both LE.
     * Assembled a byte at a time rather than cast, so an unaligned offset cannot trap. */
    for (i = 0; i < size && i < sizeof(v); i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

#undef DEVROW
#undef DEVSZ
#undef DEVOFF

/* ---------------------------------------------------------------------------------------------
 * The kernel-side field tables, built from the shared X-macro list.
 * ------------------------------------------------------------------------------------------- */
#define BC250_ROW(member) \
    { #member, (uint32_t)offsetof(struct drm_amdgpu_info_device, member), \
      (uint32_t)sizeof(((struct drm_amdgpu_info_device *)0)->member) },
static const struct bc250_field g_device_fields[] = { BC250_DEVICE_FIELD_LIST(BC250_ROW) };
#undef BC250_ROW

#define BC250_ROW(member) \
    { #member, (uint32_t)offsetof(struct drm_amdgpu_info_hw_ip, member), \
      (uint32_t)sizeof(((struct drm_amdgpu_info_hw_ip *)0)->member) },
static const struct bc250_field g_hw_ip_fields[] = { BC250_HW_IP_FIELD_LIST(BC250_ROW) };
#undef BC250_ROW

unsigned bc250_kernel_device_fields(const struct bc250_field **out)
{
    *out = g_device_fields;
    return (unsigned)(sizeof(g_device_fields) / sizeof(g_device_fields[0]));
}

unsigned bc250_kernel_hw_ip_fields(const struct bc250_field **out)
{
    *out = g_hw_ip_fields;
    return (unsigned)(sizeof(g_hw_ip_fields) / sizeof(g_hw_ip_fields[0]));
}

void bc250_kernel_blob_layout(struct bc250_blob_layout *o)
{
#define OFF(m) ((uint32_t)offsetof(struct bc250_umd_private, m))
    o->total_size          = (uint32_t)sizeof(struct bc250_umd_private);
    o->magic_off           = OFF(magic);
    o->version_off         = OFF(version);
    o->size_off            = OFF(size);
    o->flags_off           = OFF(flags);
    o->device_off          = OFF(device);
    o->memory_off          = OFF(memory);
    o->heap_stride         = (uint32_t)sizeof(struct drm_amdgpu_heap_info);
    o->heap_total_off      = (uint32_t)offsetof(struct drm_amdgpu_heap_info, total_heap_size);
    o->hw_ip_off           = OFF(hw_ip);
    o->hw_ip_stride        = (uint32_t)sizeof(struct drm_amdgpu_info_hw_ip);
    o->hw_ip_count         = BC250_UMD_HW_IP_MAX;
    o->hw_ip_mask_off      = OFF(hw_ip_mask);
    o->hw_ip_instances_off = OFF(hw_ip_instances);
    o->gb_addr_config_off  = OFF(tiling.gb_addr_config);
    o->mc_arb_ramcfg_off   = OFF(tiling.mc_arb_ramcfg);
    o->gb_tile_mode_off    = OFF(tiling.gb_tile_mode);
    o->gb_macro_tile_mode_off = OFF(tiling.gb_macro_tile_mode);
    o->fw_me_version_off   = OFF(firmware.me_version);
    o->fw_me_feature_off   = OFF(firmware.me_feature);
    o->fw_pfp_version_off  = OFF(firmware.pfp_version);
    o->fw_pfp_feature_off  = OFF(firmware.pfp_feature);
    o->fw_mec_version_off  = OFF(firmware.mec_version);
    o->fw_mec_feature_off  = OFF(firmware.mec_feature);
    o->drm_major_off       = OFF(kernel.drm_major);
    o->drm_minor_off       = OFF(kernel.drm_minor);
    o->drm_patchlevel_off  = OFF(kernel.drm_patchlevel);
    o->address32_hi_off    = OFF(kernel.address32_hi);
    o->pci_domain_off      = OFF(kernel.pci_domain);
    o->pci_bus_off         = OFF(kernel.pci_bus);
    o->pci_dev_off         = OFF(kernel.pci_dev);
    o->pci_func_off        = OFF(kernel.pci_func);
    o->max_submitted_ibs_off = OFF(max_submitted_ibs);
#undef OFF
}

uint32_t bc250_blob_device_offset(void)   { return (uint32_t)offsetof(struct bc250_umd_private, device); }

uint32_t bc250_flag_unmeasured(void)     { return BC250_UMD_F_UNMEASURED; }
uint32_t bc250_flag_golden_gb_addr(void) { return BC250_UMD_F_GOLDEN_GB_ADDR; }
uint32_t bc250_expected_magic(void)      { return BC250_UMD_PRIVATE_MAGIC; }

uint32_t bc250_kernel_sizeof_device(void) { return (uint32_t)sizeof(struct drm_amdgpu_info_device); }
uint32_t bc250_kernel_sizeof_hw_ip(void)  { return (uint32_t)sizeof(struct drm_amdgpu_info_hw_ip); }
uint32_t bc250_kernel_sizeof_memory(void) { return (uint32_t)sizeof(struct drm_amdgpu_memory_info); }
uint32_t bc250_kernel_sizeof_heap(void)   { return (uint32_t)sizeof(struct drm_amdgpu_heap_info); }
uint32_t bc250_kernel_sizeof_blob(void)   { return (uint32_t)sizeof(struct bc250_umd_private); }
