/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/test/bc250_caps_test.c - the caps-blob parity test.
 *
 * Question it answers: if bc250kmd hands a user-mode driver the blob defined in
 * driver/contract/bc250_umd_private.h, filled with what we have actually measured on unit A, does
 * Mesa's real ac_gpu_info.c recognise the chip and derive sane limits?
 *
 * It runs entirely on the development PC. No hardware, no driver, no lab.
 *
 * Structure:
 *   part 1  the wishlist: every field we have NOT measured, printed first so it cannot be missed
 *   part 2  layout parity between the kernel UAPI and Mesa's Windows copies of it
 *   part 3  the honest blob through the real ac_* path, with assertions
 *   part 4  the controls: eight deliberate corruptions, each of which MUST change the answer
 *
 * A test that cannot fail proves nothing, which is what part 4 is for. If part 3 passes and part
 * 4 reports that a mutation changed nothing, the test is broken, not the driver.
 */

#include "bc250_caps_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#if defined(_WIN32)
#include <windows.h>
#endif

extern const char *const bc250_unitA_wishlist[];

static int g_failures;
static int g_checks;

static void check(int cond, const char *what, const char *detail)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("  FAIL  %-44s %s\n", what, detail ? detail : "");
    }
}

static void check_u64(uint64_t got, uint64_t want, const char *what)
{
    char detail[128];
    snprintf(detail, sizeof(detail), "got %" PRIu64 " (0x%" PRIX64 "), want %" PRIu64
             " (0x%" PRIX64 ")", got, got, want, want);
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("  FAIL  %-44s %s\n", what, detail);
    }
}

/* ------------------------------------------------------------------------------------------- */

static void part1_wishlist(void)
{
    unsigned i;
    printf("========================================================================\n");
    printf(" 1. NOT MEASURED ON UNIT A - the wishlist\n");
    printf("========================================================================\n");
    printf("The blob's DEV_INFO, MEMORY, HW_IP and FW_VERSION content is decoded from the raw\n");
    printf("ioctl replies in evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-\n");
    printf("windows/info.txt, so what is left below is only what the INFO ioctl does not cover.\n");
    printf("BC250_UMD_F_UNMEASURED is set in the blob because of these.\n\n");
    for (i = 0; bc250_unitA_wishlist[i]; i++)
        printf("  %s\n", bc250_unitA_wishlist[i]);
    printf("\n");
}

/* ------------------------------------------------------------------------------------------- */

/* The decoded ioctl bytes against every independent capture we have of the same number.
 *
 * This part is the one that would have caught the nine things the previous version of this test
 * got wrong. Each row was, until the dump arrived, an INPUT: a number read out of gca_config.bin
 * or ip-discovery.bin or dmesg and typed into the blob. Now the blob comes from the kernel's own
 * reply and these are predictions about it. A failure here means docs/facts.md is wrong, or the
 * dump is not from unit A. */
static void part2b_crosschecks(void)
{
    const struct bc250_crosscheck *rows;
    unsigned n = bc250_unitA_crosschecks(&rows), i;
    uint32_t devoff = bc250_blob_device_offset();
    const void *blob;
    uint32_t size = 0;

    blob = bc250_unitA_blob(BC250_MUTATE_NONE, &size);

    printf("========================================================================\n");
    printf(" 2b. CROSS-CHECK - the decoded ioctl bytes against independent captures\n");
    printf("========================================================================\n");
    printf("Every row below used to be an input to this test. It is now a prediction about the\n");
    printf("raw AMDGPU_INFO reply. Two mechanisms, one unit, one answer each.\n\n");

    for (i = 0; i < n; i++) {
        uint64_t got = bc250_blob_read_field(blob, devoff + rows[i].offset, rows[i].size);
        int ok = (got == rows[i].expected);

        printf("  %-32s ioctl %-20" PRIu64 " (0x%" PRIX64 ")\n",
               rows[i].field, got, got);
        if (!ok)
            printf("      independent capture says %" PRIu64 " (0x%" PRIX64 ")\n",
                   rows[i].expected, rows[i].expected);
        printf("      %s\n", rows[i].source);
        check(ok, rows[i].field, ok ? NULL : "independent capture disagrees with the ioctl");
    }
    printf("\n");
}

/* ------------------------------------------------------------------------------------------- */

static void part2_layout(void)
{
    const struct bc250_field *k, *m;
    unsigned nk, nm, i;

    printf("========================================================================\n");
    printf(" 2. LAYOUT PARITY - kernel UAPI (third_party/amdgpu_drm.h) vs Mesa's\n");
    printf("    Windows copy (src/amd/common/ac_linux_drm.h)\n");
    printf("========================================================================\n");

    printf("  drm_amdgpu_info_device   kernel %4u B   mesa %4u B\n",
           bc250_kernel_sizeof_device(), bc250_mesa_sizeof_device());
    printf("  drm_amdgpu_info_hw_ip    kernel %4u B   mesa %4u B\n",
           bc250_kernel_sizeof_hw_ip(), bc250_mesa_sizeof_hw_ip());
    printf("  drm_amdgpu_heap_info     kernel %4u B   mesa %4u B\n",
           bc250_kernel_sizeof_heap(), bc250_mesa_sizeof_heap());
    printf("  drm_amdgpu_memory_info   kernel %4u B   mesa %4u B\n",
           bc250_kernel_sizeof_memory(), bc250_mesa_sizeof_memory());
    printf("  bc250_umd_private        %u B\n\n", bc250_kernel_sizeof_blob());

    /* drm_amdgpu_info_device is the structure the blob passes through by pointer, so this one
     * must match member for member. */
    nk = bc250_kernel_device_fields(&k);
    nm = bc250_mesa_device_fields(&m);
    check(nk == nm, "info_device: same number of members", NULL);
    check(bc250_kernel_sizeof_device() == bc250_mesa_sizeof_device(),
          "info_device: same sizeof", NULL);
    for (i = 0; i < (nk < nm ? nk : nm); i++) {
        char detail[160];
        snprintf(detail, sizeof(detail), "%s: kernel +%u/%u, mesa +%u/%u",
                 k[i].name, k[i].offset, k[i].size, m[i].offset, m[i].size);
        check(strcmp(k[i].name, m[i].name) == 0 && k[i].offset == m[i].offset &&
              k[i].size == m[i].size, "info_device member agrees", detail);
    }
    printf("  info_device: %u members compared, name/offset/size\n", i);

    /* hw_ip does NOT match: Mesa's copy omits capabilities_flags and userq_num_slots, which moves
     * every later member. Report it rather than assert it away. */
    nk = bc250_kernel_hw_ip_fields(&k);
    nm = bc250_mesa_hw_ip_fields(&m);
    printf("  hw_ip: kernel has %u members, mesa has %u. Per-member offsets:\n", nk, nm);
    for (i = 0; i < nm; i++) {
        unsigned j;
        uint32_t koff = 0xFFFFFFFFu;
        for (j = 0; j < nk; j++)
            if (strcmp(k[j].name, m[i].name) == 0)
                koff = k[j].offset;
        printf("      %-24s kernel +%-3u  mesa +%-3u  %s\n", m[i].name, koff, m[i].offset,
               koff == m[i].offset ? "same" : "DIFFERENT - must be translated");
        check(koff != 0xFFFFFFFFu, "hw_ip member exists on both sides", m[i].name);
    }
    printf("\n");
}

/* ------------------------------------------------------------------------------------------- */

static void print_result(const struct bc250_mesa_result *r)
{
    printf("    family              %s (%d)\n", r->family_name, r->family);
    printf("    gfx_level           %s (%d)\n", r->gfx_level_name, r->gfx_level);
    printf("    GFX IP              %u.%u.%u\n", r->ip_gfx_major, r->ip_gfx_minor, r->ip_gfx_rev);
    printf("    queues              gfx %u, compute %u, sdma %u\n",
           r->num_gfx_queues, r->num_compute_queues, r->num_sdma_queues);
    printf("    pci                 id 0x%04X rev 0x%02X chip_rev %u\n",
           r->pci_id, r->pci_rev_id, r->chip_rev);
    printf("    shader array        max_se %u num_se %u max_sa_per_se %u cu_per_sh %u\n",
           r->max_se, r->num_se, r->max_sa_per_se, r->num_cu_per_sh);
    printf("    CUs                 num_cu %u, good per SA min %u max %u\n",
           r->num_cu, r->min_good_cu_per_sa, r->max_good_cu_per_sa);
    printf("    render backends     num_rb %u, max %u\n", r->num_rb, r->max_render_backends);
    printf("    TCC                 %u of %u blocks\n", r->num_tcc_blocks, r->max_tcc_blocks);
    printf("    caches              tcp %u, l1 %u, l2 %u\n",
           r->tcp_cache_size, r->l1_cache_size, r->l2_cache_size);
    printf("    sqc                 inst %u, scalar %u, per_wgp %u\n",
           r->sqc_inst_cache_size, r->sqc_scalar_cache_size, r->num_sqc_per_wgp);
    printf("    tiling              gb_addr_config 0x%08X, num_tile_pipes %u\n",
           r->gb_addr_config, r->num_tile_pipes);
    printf("    clocks              gpu %u MHz, memory %u MHz, bus %u bit, vram_type %u\n",
           r->max_gpu_freq_mhz, r->memory_freq_mhz, r->memory_bus_width, r->vram_type);
    printf("    memory              vram %" PRIu64 " KiB, visible %" PRIu64 " KiB, gart %" PRIu64
           " KiB\n", r->vram_size_kb, r->vram_vis_size_kb, r->gart_size_kb);
    printf("                        max_heap %" PRIu64 " KiB, all_vram_visible %d, dedicated %d,"
           " graphics %d\n", r->max_heap_size_kb, r->all_vram_visible, r->has_dedicated_vram,
           r->has_graphics);
    printf("    VA                  max 0x%016" PRIX64 ", high 0x%016" PRIX64 "..0x%016" PRIX64
           "\n", r->virtual_address_max, r->high_va_offset, r->high_va_max);
    printf("                        pte_fragment %u, gart_page %u\n",
           r->pte_fragment_size, r->gart_page_size);
    printf("    misc                clock_crystal %u, pc_lines %u, pbb_max_alloc %u\n",
           r->clock_crystal_freq, r->pc_lines, r->pbb_max_alloc_count);
    printf("    firmware feature    me %u, pfp %u, mec %u\n",
           r->me_fw_feature, r->pfp_fw_feature, r->mec_fw_feature);
}

static void part3_honest(struct bc250_mesa_result *r)
{
    const void *blob;
    uint32_t size = 0;

    printf("========================================================================\n");
    printf(" 3. UNIT A AS MEASURED, THROUGH THE REAL ac_* PATH\n");
    printf("========================================================================\n");

    blob = bc250_unitA_blob(BC250_MUTATE_NONE, &size);
    printf("  blob %u bytes\n", size);
    /* This used to warn that Mesa was about to print "clock crystal frequency is 0". It no longer
     * does: gpu_counter_freq is measured now. Anything Mesa prints here is new and worth reading. */
    printf("  (Mesa should print nothing below. Anything it does print is a finding.)\n");
    bc250_mesa_consume(blob, size, r);

    if (!r->ok) {
        printf("\n  Mesa REFUSED the blob: %s\n\n", r->failure ? r->failure : "?");
        check(0, "ac_* path accepted unit A", r->failure);
        return;
    }
    printf("\n");
    print_result(r);
    printf("\n  assertions:\n");

    /* --- identity. Note the name: Mesa has no CHIP_CYAN_SKILLFISH. Cyan Skillfish is
     * CHIP_GFX1013 in enum radeon_family, reached through `case FAMILY_NV:` and
     * identify_chip(GFX1013) at ac_gpu_info.c:719-723, because external_rev 132 falls inside
     * AMDGPU_GFX1013_RANGE (0x82..0x86, addrlib/src/amdgpu_asic_addr.h:94). --- */
    check(strcmp(r->family_name, "CHIP_GFX1013") == 0,
          "family is CHIP_GFX1013 (Cyan Skillfish)", r->family_name);
    /* Mesa's gfx_level enum has no 10.1.3 rung. GC 10.1.3 lands on GFX10 (= gfx10.1) at
     * ac_gpu_info.c:791-792; the .3 survives in ip[AMD_IP_GFX].ver_rev. */
    check(strcmp(r->gfx_level_name, "GFX10") == 0,
          "gfx_level is GFX10 (gfx10.1)", r->gfx_level_name);
    check_u64(r->ip_gfx_major, 10, "GFX IP major is 10");
    check_u64(r->ip_gfx_minor, 1,  "GFX IP minor is 1");
    check_u64(r->ip_gfx_rev,   3,  "GFX IP revision is 3");
    check_u64(r->pci_id,     0x13FE, "pci_id is 0x13FE");
    check_u64(r->pci_rev_id, 0,      "pci_rev_id is 0");
    check_u64(r->chip_rev,   2,      "chip_rev is 2");

    /* --- geometry: two SEs, two SAs each, ten CUs per SA on paper, six alive. --- */
    check_u64(r->max_se, 2,        "max_se is 2");
    check_u64(r->num_se, 2,        "num_se is 2");
    check_u64(r->max_sa_per_se, 2, "max_sa_per_se is 2");
    check_u64(r->num_cu_per_sh, 10,"num_cu_per_sh is 10");
    /* 24, the number dmesg printed on four boots, arrived at independently: Mesa counted the bits
     * of the cu_bitmap we derived from CC_GC_SHADER_ARRAY_CONFIG. */
    check_u64(r->num_cu, 24,       "num_cu is 24");
    check_u64(r->min_good_cu_per_sa, 6, "min_good_cu_per_sa is 6");
    check_u64(r->max_good_cu_per_sa, 6, "max_good_cu_per_sa is 6");
    check_u64(r->num_rb, 16,            "num_rb is 16");
    check_u64(r->max_render_backends, 16, "max_render_backends is 16");
    check_u64(r->max_tcc_blocks, 16,    "max_tcc_blocks is 16");
    check_u64(r->num_tcc_blocks, 16,    "num_tcc_blocks is 16");

    /* --- the compute queues. This is not a bug in our blob. ac_gpu_info.c:501-504 drops
     * AMD_IP_COMPUTE outright when family is FAMILY_NV and external_rev is in the GFX1013 range,
     * with the comment "GFX1013 is known to have broken compute queue". RADV will therefore never
     * submit to the eight compute rings bc250kmd brings up, no matter what the blob says. The
     * test asserts it so that nobody spends a week wondering. --- */
    check_u64(r->num_gfx_queues, 1,     "one gfx queue");
    check_u64(r->num_compute_queues, 0, "zero compute queues (Mesa drops them on GFX1013)");
    check_u64(r->num_sdma_queues, 2,    "two SDMA queues");
    check(r->has_graphics, "has_graphics", NULL);

    /* --- caches. On gfx10.1 Mesa ignores the cache sizes in the blob and substitutes its own
     * (ac_gpu_info.c:607-614), then computes L2 from the TCC count. --- */
    check_u64(r->tcp_cache_size, 16384,   "tcp_cache_size is 16 KiB (Mesa's gfx10 constant)");
    check_u64(r->l1_cache_size, 128*1024, "l1_cache_size is 128 KiB (Mesa's gfx10 constant)");
    check_u64(r->l2_cache_size, 16*256*1024, "l2_cache_size is 4 MiB (16 TCC * 256 KiB)");
    /* Zero, and correct. The GC discovery table on this unit is version 1.1, and amdgpu only
     * fills these from version 1.2 upward, so RADV gets 0 on Linux here too. */
    check_u64(r->sqc_inst_cache_size, 0,   "sqc_inst_cache_size is 0 (GC table is v1.1)");
    check_u64(r->sqc_scalar_cache_size, 0, "sqc_scalar_cache_size is 0 (GC table is v1.1)");
    check_u64(r->num_sqc_per_wgp, 0,       "num_sqc_per_wgp is 0 (GC table is v1.1)");

    /* --- tiling. The golden constant, measured at the user-mode boundary: E14 radv-info.txt:186
     * shows Mesa 26.1.6 on unit A printing 0x00100044, because the READ_MMR_REG ioctl special-cases
     * this register and returns adev->gfx.config.gb_addr_config (nv.c:381-382) instead of doing an
     * RREG32. The raw register reads 0x00000044 (fact M46) and never reaches user mode. --- */
    check_u64(r->gb_addr_config, 0x00100044, "gb_addr_config is the golden 0x00100044");
    check_u64(r->num_tile_pipes, 16,         "num_tile_pipes is 16");

    /* --- clocks and memory --- */
    check_u64(r->max_gpu_freq_mhz, 2000, "max_gpu_freq_mhz is 2000");
    /* 0, and measured. DEV_INFO.max_memory_clock came back 0 even though the memory is plainly
     * running: E13b4's own SENSOR_GFX_MCLK reads 0x1C2 = 450 MHz and E01 pp_dpm_mclk lists a
     * 450 MHz level. amdgpu reports 0 here on this part, so Linux RADV computes its memory
     * bandwidth from 0 as well. Parity means carrying the 0, not the 450. */
    check_u64(r->memory_freq_mhz, 0,     "memory_freq_mhz is 0, as amdgpu reports it");
    check_u64(r->memory_bus_width, 1024, "memory_bus_width is 1024");
    check_u64(r->vram_type, 0,           "vram_type is UNKNOWN, as amdgpu reports it");
    check_u64(r->vram_size_kb, 8589934592ull / 1024, "vram_size_kb is 8 GiB");
    check_u64(r->vram_vis_size_kb, 8589934592ull / 1024, "vram_vis_size_kb is 8 GiB");
    check(r->all_vram_visible, "all_vram_visible", NULL);
    /* The GTT is sized from free system RAM at init, so it moves between boots: E13b4 measured
     * 0xEFA35000, E13b3 recorded 4020453376, E01 recorded 4020506624. The contract is pinned to
     * the boot every other byte in the blob came from. */
    check_u64(r->gart_size_kb, 4020457472ull / 1024, "gart_size_kb matches E13b4's MEMORY reply");
    /* APU: ids_flags has FUSION, so Mesa says the VRAM is not dedicated and clamps the largest
     * allocation to the GART size instead (ac_gpu_info.c:566-573). */
    check(!r->has_dedicated_vram, "has_dedicated_vram is false (FUSION)", NULL);
    check_u64(r->max_heap_size_kb, r->gart_size_kb, "max_heap_size_kb follows the GART");

    /* --- address space --- */
    check_u64(r->virtual_address_max, 0x0000800000000000ull, "virtual_address_max is the VA hole");
    check_u64(r->high_va_offset, 0xffff800000000000ull, "high_va_offset is the hole end");
    /* Measured, and it refuted a derivation. Computing it as AMDGPU_GMC_HOLE_END | (2^48 -
     * AMDGPU_VA_RESERVED_TOP) with mainline v6.18's RESERVED_TOP (trap 0x2000 + seq64 0x200000 +
     * CSA 0x200000 = 0x402000) gives 0xFFFFFFFFFFBFE000. The 6.18.52 kernel that answered returned
     * 0xFFFFFFFFFFBF0000, which implies 0x410000. The kernel is the authority on its own reserved
     * range; the derivation is gone. */
    check_u64(r->high_va_max, 0xFFFFFFFFFFBF0000ull, "high_va_max is the measured 0xFFFFFFFFFFBF0000");
    check_u64(r->pte_fragment_size, 512u * 4096u, "pte_fragment_size is 2 MiB (9-bit fragments)");
    check_u64(r->gart_page_size, 4096, "gart_page_size is 4 KiB");

    /* --- firmware. These are the numbers Collabora's wddm2 winsys had to invent; we have them
     * from the device. --- */
    check_u64(r->me_fw_feature, 32,  "ME firmware feature version is 32");
    check_u64(r->pfp_fw_feature, 32, "PFP firmware feature version is 32");
    check_u64(r->mec_fw_feature, 32, "MEC firmware feature version is 32");

    /* --- a wishlist entry that the ioctl dump settled. This used to assert 1, the value
     * ac_gpu_info.c:1231 substitutes after printing "clock crystal frequency is 0, timestamps will
     * be wrong", because the blob carried 0. DEV_INFO.gpu_counter_freq is 0x000186A0 = 100 MHz. --- */
    check_u64(r->clock_crystal_freq, 100000,
              "clock_crystal_freq is the measured 100000 kHz, not the fallback 1");

    /* --- and one Mesa constant that depends on getting the chip right --- */
    check_u64(r->pc_lines, 1024,            "pc_lines is 1024 (the GFX1013 case)");
    check_u64(r->pbb_max_alloc_count, 1024/3, "pbb_max_alloc_count is pc_lines/3");

    /* --- what Mesa asked the environment for. Every one was answered with the caller's own
     * default (bc250_caps_stubs.c), so nothing above depends on who ran this. If the list grows,
     * a new Mesa is gating a capability on a variable and somebody has to decide what bc250kmd's
     * answer should be. --- */
    {
        const char *const *opts;
        unsigned n = bc250_stub_options(&opts), i;
        printf("\n  environment options Mesa consulted (all answered with the default):\n");
        for (i = 0; i < n; i++)
            printf("      %s\n", opts[i]);
        if (!n)
            printf("      (none)\n");
        check(n == 1 && strcmp(opts[0], "AMD_IMAGE_OPCODES") == 0,
              "exactly AMD_IMAGE_OPCODES was consulted", NULL);
    }

    printf("\n");
}

/* ------------------------------------------------------------------------------------------- */

/* A mutation must change something. If it does not, the test is not measuring what it claims to.
 *
 * Each control runs in a child process, because some of them are supposed to take Mesa down and a
 * crash in one must not hide the other six. Mesa compiled here keeps its assert()s (Mesa's own
 * release builds define NDEBUG and lose them), and an all-zero CU bitmap makes ac_gpu_info.c:1311
 * divide by zero outright. Both are results worth reporting, not accidents to be stepped around.
 *
 * Child exit codes. 1 and 3 are deliberately not used: MSVC's abort() exits with 3, and a
 * mis-read of that as a result rather than as a death is exactly the kind of quiet wrong answer
 * this whole file exists to prevent.
 *   0   the answer changed        - the control did its job
 *   2   the answer did not change - the test is broken
 *   10  Mesa refused the blob     - also a change, and the expected outcome for one of them
 *   anything else                 - Mesa died on this input. Reported as such. */
#define BC250_CHILD_DIFFERENT 0
#define BC250_CHILD_SAME      2
#define BC250_CHILD_REFUSED   10

static const struct {
    enum bc250_mutation m;
    const char *expect;
} g_controls[] = {
    /* FAMILY_NV3 still contains a chip at external_rev 132, so Mesa identifies a Navi 3x and then
     * trips its own assert(0) at ac_gpu_info.c:1373, because that gfx11 family has no pc_lines
     * entry in the gfx9-to-gfx10 switch it reaches with our gfx10.1 IP version. A wrong family is
     * not politely refused; it desynchronises Mesa from itself. */
    { BC250_MUTATE_FAMILY,           "Mesa mis-identifies a Navi 3x and trips assert(0) at ac_gpu_info.c:1373" },
    { BC250_MUTATE_EXTERNAL_REV,     "refused: 0x90 is outside AMDGPU_GFX1013_RANGE" },
    { BC250_MUTATE_GFX_IP_VERSION,   "gfx_level becomes GFX10_3, not GFX10" },
    /* Measured, not assumed: on gfx10.1 this yields num_cu 0 and survives, because num_se comes
     * from max_se rather than from the bitmap. It is still a blob no KMD should ever emit. */
    { BC250_MUTATE_CU_BITMAP_ZERO,   "num_cu becomes 0 (survives on gfx10.1; would divide by zero on GFX10_3+)" },
    { BC250_MUTATE_CU_BITMAP_FULL,   "num_cu becomes 40, not 24" },
    { BC250_MUTATE_VRAM_SIZE,        "vram_size_kb halves" },
    /* The blob carries the golden constant, which is what READ_MMR_REG hands user mode
     * (nv.c:381-382). This control substitutes the raw register value - the mistake a Windows KMD
     * makes if it reads the hardware here instead of using the constant. */
    { BC250_MUTATE_GB_ADDR_REGISTER, "gb_addr_config becomes the raw register 0x00000044" },
};
#define BC250_N_CONTROLS (sizeof(g_controls) / sizeof(g_controls[0]))

/* The child: run one control and say whether it moved the needle. */
static int run_one_control(unsigned idx)
{
    struct bc250_mesa_result base, r;
    const void *blob;
    uint32_t size = 0;

    blob = bc250_unitA_blob(BC250_MUTATE_NONE, &size);
    bc250_mesa_consume(blob, size, &base);
    if (!base.ok) {
        printf("      (child) the honest blob did not get through; nothing to compare against\n");
        return BC250_CHILD_SAME;
    }

    blob = bc250_unitA_blob(g_controls[idx].m, &size);
    bc250_mesa_consume(blob, size, &r);

    if (!r.ok) {
        printf("      result: REFUSED - %s\n", r.failure ? r.failure : "?");
        return BC250_CHILD_REFUSED;
    }
    printf("      result: family %s, gfx_level %s, num_cu %u, vram %" PRIu64
           " KiB, gb_addr 0x%08X\n",
           r.family_name, r.gfx_level_name, r.num_cu, r.vram_size_kb, r.gb_addr_config);
    return (r.family != base.family || r.gfx_level != base.gfx_level ||
            r.num_cu != base.num_cu || r.vram_size_kb != base.vram_size_kb ||
            r.gb_addr_config != base.gb_addr_config)
           ? BC250_CHILD_DIFFERENT : BC250_CHILD_SAME;
}

static void part4_controls(const char *self)
{
    unsigned i;

    printf("========================================================================\n");
    printf(" 4. CONTROLS - each mutation must change the answer\n");
    printf("========================================================================\n");
    printf("Each runs in its own process. If any line says SAME, the test is not testing\n");
    printf("anything. A control that kills Mesa is a result, not a failure of the harness.\n\n");

    for (i = 0; i < BC250_N_CONTROLS; i++) {
        char cmd[1024];
        int rc;

        printf("  %-52s\n", bc250_mutation_name(g_controls[i].m));
        printf("      expect: %s\n", g_controls[i].expect);
        fflush(stdout);

        snprintf(cmd, sizeof(cmd), "\"\"%s\" --control %u\"", self, i);
        rc = system(cmd);

        switch (rc) {
        case BC250_CHILD_DIFFERENT:
            printf("      DIFFERENT - good\n");
            check(1, "control changed the answer", NULL);
            break;
        case BC250_CHILD_REFUSED:
            printf("      REFUSED - good, that is a change too\n");
            check(1, "control changed the answer", NULL);
            break;
        case BC250_CHILD_SAME:
            printf("      SAME - the test is broken\n");
            check(0, "control changed the answer", bc250_mutation_name(g_controls[i].m));
            break;
        default:
            /* Mesa died on this input: an assert(0), an abort() (exit 3) or a structured
             * exception. That is a legitimate control result and the loudest kind of change. */
            printf("      MESA DIED (child exit %d) - a change, and a loud one\n", rc);
            check(1, "control changed the answer", NULL);
            break;
        }
        printf("\n");
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------------------------------- */

/* A control is meant to be able to kill the process. Make sure that when it does, it dies quietly
 * into this console: no "abnormal program termination" message box, no Windows Error Reporting
 * dialog. Nothing on this PC gets to open a window. */
static void die_quietly(void)
{
#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}

int main(int argc, char **argv)
{
    struct bc250_mesa_result base;

    setvbuf(stdout, NULL, _IONBF, 0);   /* a crashing control must not swallow the report */
    die_quietly();

    /* Dump mode: derive the caps from the honest blob and print them with Mesa's own
     * ac_print_gpu_info(), in exactly the format RADV_DEBUG=info produces. Nothing else goes to
     * stdout, so test/compare_radv_info.py can diff this against the E14 capture directly. */
    if (argc == 2 && strcmp(argv[1], "--radeon-info") == 0) {
        struct bc250_mesa_result r;
        const void *blob;
        uint32_t size = 0;

        blob = bc250_unitA_blob(BC250_MUTATE_NONE, &size);
        bc250_mesa_consume(blob, size, &r);
        if (!r.ok) {
            fprintf(stderr, "bc250: Mesa refused the blob: %s\n", r.failure ? r.failure : "?");
            return 1;
        }
        bc250_mesa_print_radeon_info();
        return 0;
    }

    /* Child mode: one control, then out. See part4_controls(). */
    if (argc == 3 && strcmp(argv[1], "--control") == 0) {
        unsigned idx = (unsigned)strtoul(argv[2], NULL, 10);
        if (idx >= BC250_N_CONTROLS)
            return BC250_CHILD_SAME;
        return run_one_control(idx);
    }

    printf("\nbc250 caps-blob parity test\n");
    printf("driver/contract/bc250_umd_private.h against Mesa's real ac_gpu_info.c\n\n");

    part1_wishlist();
    part2_layout();
    part2b_crosschecks();
    part3_honest(&base);
    if (base.ok)
        part4_controls(argv[0]);
    else
        printf("4. CONTROLS skipped: the honest blob did not get through.\n\n");

    printf("========================================================================\n");
    printf(" %d checks, %d failures\n", g_checks, g_failures);
    printf("========================================================================\n\n");
    return g_failures ? 1 : 0;
}
