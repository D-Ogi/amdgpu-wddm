/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/test/bc250_caps_mesa.c - the winsys side of the parity test.
 *
 * This translation unit includes Mesa's own headers and NOT bc250_umd_private.h, because Mesa's
 * Windows build declares its own `struct drm_amdgpu_info_device` (ac_linux_drm.h:16-321) and the
 * two declarations cannot coexist. It navigates the blob with the offsets the KMD side reports
 * through struct bc250_blob_layout, exactly as a real winsys would have to.
 *
 * It calls Mesa's real functions, unmodified, from <BC250_ROOT>\ref\mesa. Nothing about
 * ac_gpu_info.c is forked, patched or re-implemented here; the only thing this file adds is the
 * translation from the blob's kernel-shaped structures into Mesa's Windows-shaped ones, which is
 * the piece a WDDM winsys will have to carry anyway. Treat the code below as the reference for
 * that translation.
 *
 * The call order follows ac_query_gpu_info() (ac_gpu_info.c:1463-1643) exactly, because the order
 * matters: ac_fill_hw_ip_info() must run before ac_identify_chip(), which reads
 * info->ip[AMD_IP_GFX].ver_major/ver_minor to pick the gfx level (ac_gpu_info.c:779-805).
 *
 * Two steps of ac_query_gpu_info() are deliberately not reproduced and are named here rather than
 * quietly dropped:
 *   - ac_drm_query_pci_bus_info() and the syncobj/DRM-version checks. They talk to a DRM device
 *     node. The blob carries what they would have returned and this file writes it straight into
 *     struct radeon_info.
 *   - ac_fill_video_info(). It is static inside ac_gpu_info.c and queries VCN capabilities over
 *     the same device node. Unit A exposes no video ring (E01 debugfs-rings.txt), so on Linux the
 *     VCN path at ac_gpu_info.c:1567 is not entered either.
 */

#include "bc250_caps_test.h"
#include "bc250_umd_private_fields.h"

/* Mesa's ac_linux_drm.h defines about ninety `static inline` stubs for Windows, every one of them
 * ignoring its parameters (src/util/u_stub.h turns the declarations into empty bodies). At /W4
 * that is ninety C4100s, and this file is built with /WX like the rest of our code. Scope the
 * exemption to the include so our own code keeps the full warning level. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4100)  /* unreferenced formal parameter */
#pragma warning(disable: 4018)  /* signed/unsigned mismatch, ac_gpu_info.h inline helpers */
#pragma warning(disable: 4820)  /* padding added after a struct member */
#endif
#include "ac_gpu_info.h"
#include "ac_linux_drm.h"
#include "amd_family.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------------------------
 * Reading the blob. Little endian both sides; the KMD and the UMD are the same machine.
 * ------------------------------------------------------------------------------------------- */
static uint32_t rd32(const void *blob, uint32_t off)
{
    uint32_t v;
    memcpy(&v, (const char *)blob + off, sizeof(v));
    return v;
}

static uint64_t rd64(const void *blob, uint32_t off)
{
    uint64_t v;
    memcpy(&v, (const char *)blob + off, sizeof(v));
    return v;
}

/* Find a field's offset inside the blob's kernel-shaped drm_amdgpu_info_hw_ip by name. The table
 * comes from the KMD side and was built from the X-macro list both sides share, so a rename on
 * either side turns into a miss here instead of a silent misread. */
static uint32_t hw_ip_field_off(const struct bc250_field *t, unsigned n, const char *name)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (strcmp(t[i].name, name) == 0)
            return t[i].offset;
    abort();    /* the field list and the structure have diverged; there is nothing to salvage */
}

/* ---------------------------------------------------------------------------------------------
 * Names for the two enums the test reports on. Mesa has no public stringifier for either.
 * ------------------------------------------------------------------------------------------- */
static const char *family_name(enum radeon_family f)
{
    switch (f) {
    case CHIP_UNKNOWN:   return "CHIP_UNKNOWN";
    case CHIP_GFX1013:   return "CHIP_GFX1013";     /* Cyan Skillfish. Mesa has no CHIP_CYAN_* */
    case CHIP_NAVI10:    return "CHIP_NAVI10";
    case CHIP_NAVI12:    return "CHIP_NAVI12";
    case CHIP_NAVI14:    return "CHIP_NAVI14";
    case CHIP_NAVI21:    return "CHIP_NAVI21";
    case CHIP_NAVI22:    return "CHIP_NAVI22";
    case CHIP_NAVI23:    return "CHIP_NAVI23";
    case CHIP_NAVI24:    return "CHIP_NAVI24";
    case CHIP_NAVI31:    return "CHIP_NAVI31";
    case CHIP_NAVI32:    return "CHIP_NAVI32";
    case CHIP_NAVI33:    return "CHIP_NAVI33";
    case CHIP_VANGOGH:   return "CHIP_VANGOGH";
    default:             return "CHIP_<other>";
    }
}

static const char *gfx_level_name(enum amd_gfx_level l)
{
    switch (l) {
    case GFX6:     return "GFX6";
    case GFX7:     return "GFX7";
    case GFX8:     return "GFX8";
    case GFX9:     return "GFX9";
    case GFX10:    return "GFX10";       /* gfx10.1, which is what GC 10.1.3 lands on */
    case GFX10_3:  return "GFX10_3";
    case GFX11:    return "GFX11";
    case GFX11_5:  return "GFX11_5";
    case GFX12:    return "GFX12";
    default:       return "GFX<other>";
    }
}

/* ---------------------------------------------------------------------------------------------
 * The translation, then the real ac_* path.
 * ------------------------------------------------------------------------------------------- */
/* The oldest blob this winsys can work with. See the version check in bc250_mesa_consume(). */
#define BC250_WINSYS_MIN_BLOB_VERSION 2u

/* The last successfully derived radeon_info, kept so bc250_mesa_print_radeon_info() can hand it
 * to Mesa's own printer without running the whole path a second time. */
static struct radeon_info g_last_info;
static int g_last_info_valid;

void bc250_mesa_consume(const void *blob, uint32_t size, struct bc250_mesa_result *out)
{
    struct bc250_blob_layout L;
    struct radeon_info info;
    struct drm_amdgpu_info_device dev;
    struct drm_amdgpu_memory_info meminfo;
    struct amdgpu_gpu_info amdinfo;
    const struct bc250_field *hw_fields;
    unsigned n_hw_fields;
    uint32_t off_major, off_minor, off_ib_start, off_ib_size, off_rings, off_disc;
    uint32_t hw_ip_mask;
    unsigned ip_type;

    memset(out, 0, sizeof(*out));
    memset(&info, 0, sizeof(info));
    memset(&dev, 0, sizeof(dev));
    memset(&meminfo, 0, sizeof(meminfo));
    memset(&amdinfo, 0, sizeof(amdinfo));

    bc250_kernel_blob_layout(&L);
    n_hw_fields = bc250_kernel_hw_ip_fields(&hw_fields);

    /* Envelope. A winsys that skips these checks deserves what it gets. */
    if (size < L.total_size) { out->failure = "blob shorter than the layout says"; return; }
    if (rd32(blob, L.magic_off) != bc250_expected_magic()) { out->failure = "bad magic"; return; }
    /* The versioning rule (bc250_umd_private.h) is that fields are only ever appended and the
     * version is bumped. So a NEWER blob than this winsys knows is usable - the tail it does not
     * understand is simply not read - while an OLDER one is not, because a field it relies on may
     * be absent. This winsys reads max_submitted_ibs, which arrived in version 2.
     *
     * Until version 2 this check was `!= 1`, which would have refused every future blob and made
     * the version word pointless. Bumping the version is what caught it. */
    if (rd32(blob, L.version_off) < BC250_WINSYS_MIN_BLOB_VERSION) {
        out->failure = "blob older than this winsys requires"; return;
    }
    if (rd32(blob, L.size_off) < L.total_size) { out->failure = "size field disagrees"; return; }

    /* --- drm_amdgpu_info_device: the one structure that IS layout-compatible ---------------
     * Mesa's copy (ac_linux_drm.h) has the same 65 members in the same order and widths as the
     * kernel's. main() proves that with the offset tables before this runs; here we only refuse
     * to proceed if the sizes disagree, because a memcpy would then be nonsense. */
    if (sizeof(dev) != bc250_kernel_sizeof_device()) {
        out->failure = "drm_amdgpu_info_device size differs between Mesa and the kernel UAPI";
        return;
    }
    memcpy(&dev, (const char *)blob + L.device_off, sizeof(dev));

    /* --- drm_amdgpu_memory_info: NOT layout-compatible -------------------------------------
     * The kernel's drm_amdgpu_heap_info is four u64; Mesa's Windows copy is one. Read the three
     * total_heap_size values out of the kernel-shaped heaps by hand. */
    meminfo.vram.total_heap_size =
        rd64(blob, L.memory_off + 0 * L.heap_stride + L.heap_total_off);
    meminfo.cpu_accessible_vram.total_heap_size =
        rd64(blob, L.memory_off + 1 * L.heap_stride + L.heap_total_off);
    meminfo.gtt.total_heap_size =
        rd64(blob, L.memory_off + 2 * L.heap_stride + L.heap_total_off);

    /* --- struct amdgpu_gpu_info: the four members ac_fill_tiling_info() reads --------------- */
    amdinfo.gb_addr_cfg    = rd32(blob, L.gb_addr_config_off);
    amdinfo.mc_arb_ramcfg  = rd32(blob, L.mc_arb_ramcfg_off);
    memcpy(amdinfo.gb_tile_mode, (const char *)blob + L.gb_tile_mode_off,
           sizeof(amdinfo.gb_tile_mode));
    memcpy(amdinfo.gb_macro_tile_mode, (const char *)blob + L.gb_macro_tile_mode_off,
           sizeof(amdinfo.gb_macro_tile_mode));

    /* --- what the DRM device node would have supplied --------------------------------------- */
    info.drm_major      = rd32(blob, L.drm_major_off);
    info.drm_minor      = rd32(blob, L.drm_minor_off);
    info.drm_patchlevel = rd32(blob, L.drm_patchlevel_off);
    info.is_amdgpu      = true;
    info.address32_hi   = rd32(blob, L.address32_hi_off);
    info.pci.domain     = rd32(blob, L.pci_domain_off);
    info.pci.bus        = rd32(blob, L.pci_bus_off);
    info.pci.dev        = rd32(blob, L.pci_dev_off);
    info.pci.func       = rd32(blob, L.pci_func_off);
    info.pci.valid      = true;

    /* Mesa's own gate, ac_gpu_info.c:1493-1501. Reproduced because a blob that claims an old
     * UAPI must be refused here rather than half-consumed. */
    if (info.drm_major != 3) { out->failure = "drm_major is not 3"; return; }
    if (info.drm_minor < 54) { out->failure = "drm_minor below 54"; return; }

    /* We expose no user queues; ac_fill_hw_ip_info() reads this at ac_gpu_info.c:495. */
    info.userq_ip_mask = 0;

    /* --- ac_fill_hw_ip_info(), once per IP, before ac_identify_chip() ----------------------- */
    off_major    = hw_ip_field_off(hw_fields, n_hw_fields, "hw_ip_version_major");
    off_minor    = hw_ip_field_off(hw_fields, n_hw_fields, "hw_ip_version_minor");
    off_ib_start = hw_ip_field_off(hw_fields, n_hw_fields, "ib_start_alignment");
    off_ib_size  = hw_ip_field_off(hw_fields, n_hw_fields, "ib_size_alignment");
    off_rings    = hw_ip_field_off(hw_fields, n_hw_fields, "available_rings");
    off_disc     = hw_ip_field_off(hw_fields, n_hw_fields, "ip_discovery_version");
    hw_ip_mask   = rd32(blob, L.hw_ip_mask_off);

    for (ip_type = 0; ip_type < AMD_NUM_IP_TYPES && ip_type < L.hw_ip_count; ip_type++) {
        struct drm_amdgpu_info_hw_ip ip_info;
        uint32_t base;

        if (!(hw_ip_mask & (1u << ip_type)))
            continue;   /* the KMD did not fill this IP; on Linux the query would have failed */

        base = L.hw_ip_off + ip_type * L.hw_ip_stride;
        memset(&ip_info, 0, sizeof(ip_info));
        ip_info.hw_ip_version_major = rd32(blob, base + off_major);
        ip_info.hw_ip_version_minor = rd32(blob, base + off_minor);
        ip_info.ib_start_alignment  = rd32(blob, base + off_ib_start);
        ip_info.ib_size_alignment   = rd32(blob, base + off_ib_size);
        ip_info.available_rings     = rd32(blob, base + off_rings);
        ip_info.ip_discovery_version = rd32(blob, base + off_disc);

        if (ac_fill_hw_ip_info(&info, &dev, ip_type, &ip_info))
            info.ip[ip_type].num_instances =
                rd32(blob, L.hw_ip_instances_off + ip_type * 4u);
        /* A false return is not an error. For this chip it is the expected answer for
         * AMD_IP_COMPUTE: ac_gpu_info.c:501-504 drops the compute IP outright on GFX1013.
         * main() asserts that, because it is a fact about what RADV will and will not submit to. */
    }

    /* --- max_submitted_ibs, ac_gpu_info.c:1698 ---------------------------------------------- */
    /* On Linux this is one AMDGPU_INFO_MAX_IBS query straight into the array. Mesa tolerates a
     * failure and substitutes estimates at :1700-1716; we have the measured values, so the blob
     * carries them and the fallback never runs. */
    {
        unsigned i;
        for (i = 0; i < AMD_NUM_IP_TYPES && i < L.hw_ip_count; i++)
            info.max_submitted_ibs[i] = rd32(blob, L.max_submitted_ibs_off + i * 4u);
    }

    /* --- firmware versions, ac_gpu_info.c:1546-1565 ----------------------------------------- */
    info.me_fw_version  = rd32(blob, L.fw_me_version_off);
    info.me_fw_feature  = rd32(blob, L.fw_me_feature_off);
    info.pfp_fw_version = rd32(blob, L.fw_pfp_version_off);
    info.pfp_fw_feature = rd32(blob, L.fw_pfp_feature_off);
    info.mec_fw_version = rd32(blob, L.fw_mec_version_off);
    info.mec_fw_feature = rd32(blob, L.fw_mec_feature_off);

    /* --- the real thing --------------------------------------------------------------------- */
    if (!ac_identify_chip(&info, &dev)) {
        out->failure = "ac_identify_chip refused the device (AC_QUERY_GPU_INFO_UNIMPLEMENTED_HW)";
        out->family = info.family;
        out->family_name = family_name(info.family);
        return;
    }

    ac_fill_memory_info(&info, &dev, &meminfo);
    ac_fill_hw_info(&info, &dev);
    ac_fill_tiling_info(&info, &amdinfo);
    ac_fill_feature_info(&info, &dev);
    ac_fill_bug_info(&info);
    ac_fill_tess_info(&info);
    ac_fill_compiler_info(&info, &dev, false);

    /* --- ac_query_gpu_info()'s inline epilogue, ac_gpu_info.c:1629-1691 and :1808-1815 --------
     *
     * These assignments are NOT in any ac_fill_* function; they sit in the body of
     * ac_query_gpu_info() between the calls. Calling only the ac_fill_* set therefore leaves
     * them at zero, which is what the radv-info.txt comparison caught: max_gflops, pcie_gen,
     * pcie_num_lanes and pcie_bandwidth were all 0 here and non-zero on the device.
     *
     * Reproduced line for line, with the citation, and only the branches our gfx level takes.
     * The GFX11-and-later attribute/pos/prim ring block at :1717-1806 is skipped because it is
     * gated on gfx_level >= GFX11; the comparison would show it if that were wrong. */

    /* :1621, ac_drm_query_has_vm_always_valid(). Not a device property and not an ioctl: for a
     * non-virtio device that function's whole body is `info->has_vm_always_valid = true`
     * (ac_linux_drm.c:1120-1129). The radv-info.txt comparison caught this sitting at 0. */
    info.has_vm_always_valid = true;

    /* :1642. GFX10_3 and up only, so false here. Spelled out rather than left zero. */
    info.discardable_allows_big_page = info.gfx_level >= GFX10_3 && info.gfx_level < GFX12 &&
                                       info.has_dedicated_vram;
    /* :1646-1647 */
    info.scratch_wavesize_granularity_shift = info.gfx_level >= GFX11 ? 8 : 10;
    info.scratch_wavesize_granularity = 1u << info.scratch_wavesize_granularity_shift;
    /* :1655-1656 */
    info.max_scratch_waves = MAX2(32 * info.max_good_cu_per_sa * info.max_sa_per_se * info.num_se,
                                  32 /* max_waves_per_tg: 1024 threads in Wave32 */);
    /* :1657-1658 */
    info.has_scratch_base_registers = info.gfx_level >= GFX11 ||
                                      (!info.has_graphics && info.family >= CHIP_GFX940);
    /* :1659 */
    info.max_gflops = (info.gfx_level >= GFX11 ? 256 : 128) * info.num_cu *
                      info.max_gpu_freq_mhz / 1000;
    /* :1660. memory_freq_mhz_effective is 0 on this part, so this is 0 as well - see the
     * max_memory_clock note in bc250_caps_unitA.c. */
    info.memory_bandwidth_gbps = DIV_ROUND_UP(info.memory_freq_mhz_effective *
                                              info.memory_bus_width / 8, 1000);
    /* :1662-1663. Straight out of DEV_INFO, which we have measured. */
    info.pcie_gen = dev.pcie_gen;
    info.pcie_num_lanes = dev.pcie_num_lanes;
    /* :1665-1666 */
    info.instr_prefetch_distance = !info.has_graphics && info.family >= CHIP_MI200 ? 16 :
                                   info.gfx_level >= GFX10 ? 3 : 0;
    /* :1669-1691, the PCIe generation table. Only the Gen4 arm is reached here; the others are
     * carried so a different measurement does not silently produce 0. */
    switch (info.pcie_gen) {
    case 1: info.pcie_bandwidth_mbps = (unsigned)(info.pcie_num_lanes * 0.25 * 1024); break;
    case 2: info.pcie_bandwidth_mbps = (unsigned)(info.pcie_num_lanes * 0.5 * 1024); break;
    case 3: info.pcie_bandwidth_mbps = (unsigned)(info.pcie_num_lanes * 0.985 * 1024); break;
    case 4: info.pcie_bandwidth_mbps = (unsigned)(info.pcie_num_lanes * 1.970 * 1024); break;
    case 5: info.pcie_bandwidth_mbps = (unsigned)(info.pcie_num_lanes * 3.940 * 1024); break;
    case 6: info.pcie_bandwidth_mbps = (unsigned)(info.pcie_num_lanes * 7.563 * 1024); break;
    case 7: info.pcie_bandwidth_mbps = (unsigned)(info.pcie_num_lanes * 15.125 * 1024); break;
    default: break;
    }
    /* :1808, set_custom_cu_en_mask(). That function is static in ac_gpu_info.c, so it cannot be
     * called from here; its first statement is `info->spi_cu_en = ~0` and it then returns
     * immediately unless AMD_CU_MASK is set in the environment (:80-84). Our option layer never
     * returns a value (bc250_caps_stubs.c), so the early return is the only reachable path and
     * this one assignment is the whole of its effect.
     *
     * Worth knowing for the winsys: the real ac_query_gpu_info() consults AMD_CU_MASK here. The
     * test's "options Mesa consulted" assertion does not list it only because the static function
     * is out of reach, not because Mesa would not ask. */
    info.spi_cu_en = ~0u;
    /* :1810-1811. gfx9 and up take the first arm; ac_get_raster_config() is gfx8-and-below. */
    if (info.gfx_level >= GFX9)
        info.se_tile_repeat = 32 * info.max_se;

    /* --- report ----------------------------------------------------------------------------- */
    out->ok = 1;
    out->family = info.family;
    out->family_name = family_name(info.family);
    out->gfx_level = info.gfx_level;
    out->gfx_level_name = gfx_level_name(info.gfx_level);

    out->ip_gfx_major = info.ip[AMD_IP_GFX].ver_major;
    out->ip_gfx_minor = info.ip[AMD_IP_GFX].ver_minor;
    out->ip_gfx_rev   = info.ip[AMD_IP_GFX].ver_rev;
    out->num_gfx_queues     = info.ip[AMD_IP_GFX].num_queues;
    out->num_compute_queues = info.ip[AMD_IP_COMPUTE].num_queues;
    out->num_sdma_queues    = info.ip[AMD_IP_SDMA].num_queues;

    out->pci_id     = info.pci_id;
    out->pci_rev_id = info.pci_rev_id;
    out->chip_rev   = (uint32_t)info.chip_rev;

    out->max_se             = info.max_se;
    out->num_se             = info.num_se;
    out->max_sa_per_se      = info.max_sa_per_se;
    out->num_cu_per_sh      = info.num_cu_per_sh;
    out->num_cu             = info.num_cu;
    out->max_good_cu_per_sa = info.max_good_cu_per_sa;
    out->min_good_cu_per_sa = info.min_good_cu_per_sa;

    out->num_rb              = info.num_rb;
    out->max_render_backends = info.max_render_backends;
    out->num_tcc_blocks      = info.num_tcc_blocks;
    out->max_tcc_blocks      = info.max_tcc_blocks;

    out->l2_cache_size  = info.l2_cache_size;
    out->tcp_cache_size = info.tcp_cache_size;
    out->l1_cache_size  = info.l1_cache_size;

    out->gb_addr_config = info.gb_addr_config;
    out->num_tile_pipes = info.num_tile_pipes;

    out->max_gpu_freq_mhz = info.max_gpu_freq_mhz;
    out->memory_freq_mhz  = info.memory_freq_mhz;
    out->memory_bus_width = info.memory_bus_width;
    out->vram_type        = info.vram_type;

    out->vram_size_kb     = info.vram_size_kb;
    out->vram_vis_size_kb = info.vram_vis_size_kb;
    out->gart_size_kb     = info.gart_size_kb;
    out->max_heap_size_kb = info.max_heap_size_kb;
    out->all_vram_visible   = info.all_vram_visible;
    out->has_dedicated_vram = info.has_dedicated_vram;
    out->has_graphics       = info.has_graphics;

    out->clock_crystal_freq  = info.clock_crystal_freq;
    out->pc_lines            = info.pc_lines;
    out->pbb_max_alloc_count = info.pbb_max_alloc_count;

    out->me_fw_feature  = info.me_fw_feature;
    out->pfp_fw_feature = info.pfp_fw_feature;
    out->mec_fw_feature = info.mec_fw_feature;

    out->virtual_address_max = info.virtual_address_max;
    out->high_va_offset      = info.high_va_offset;
    out->high_va_max         = info.high_va_max;
    out->pte_fragment_size   = info.pte_fragment_size;
    out->gart_page_size      = info.gart_page_size;

    out->sqc_inst_cache_size   = info.sqc_inst_cache_size;
    out->sqc_scalar_cache_size = info.sqc_scalar_cache_size;
    out->num_sqc_per_wgp       = info.num_sqc_per_wgp;

    g_last_info = info;
    g_last_info_valid = 1;
}

void bc250_mesa_print_radeon_info(void)
{
    if (!g_last_info_valid) {
        fprintf(stderr, "bc250: no radeon_info to print; bc250_mesa_consume() did not succeed\n");
        return;
    }
    /* fd = -1: ac_print_gpu_info only uses it to readlink("/proc/self/fd/N") for the
     * dev_filename line. There is no such path here, the readlink stub fails, and the line is
     * simply omitted - which is correct, because the device node is a property of the Linux
     * capture and not of the contract. compare_radv_info.py ignores that key. */
    ac_print_gpu_info(stdout, &g_last_info, -1);
}

/* ---------------------------------------------------------------------------------------------
 * The Mesa-side field tables, built from the same X-macro list the KMD side uses.
 * ------------------------------------------------------------------------------------------- */
#define BC250_ROW(member) \
    { #member, (uint32_t)offsetof(struct drm_amdgpu_info_device, member), \
      (uint32_t)sizeof(((struct drm_amdgpu_info_device *)0)->member) },
static const struct bc250_field g_device_fields[] = { BC250_DEVICE_FIELD_LIST(BC250_ROW) };
#undef BC250_ROW

/* Mesa's Windows drm_amdgpu_info_hw_ip has six of the kernel's eight members, so only those six
 * can be listed. The two it lacks are named here so the omission is visible in the source. */
#define BC250_MESA_HW_IP_FIELD_LIST(X) \
    X(hw_ip_version_major)             \
    X(hw_ip_version_minor)             \
    /* capabilities_flags: absent in Mesa's Windows copy */ \
    X(ib_start_alignment)              \
    X(ib_size_alignment)               \
    X(available_rings)                 \
    X(ip_discovery_version)            \
    /* userq_num_slots: absent in Mesa's Windows copy */

#define BC250_ROW(member) \
    { #member, (uint32_t)offsetof(struct drm_amdgpu_info_hw_ip, member), \
      (uint32_t)sizeof(((struct drm_amdgpu_info_hw_ip *)0)->member) },
static const struct bc250_field g_hw_ip_fields[] = { BC250_MESA_HW_IP_FIELD_LIST(BC250_ROW) };
#undef BC250_ROW

unsigned bc250_mesa_device_fields(const struct bc250_field **out)
{
    *out = g_device_fields;
    return (unsigned)(sizeof(g_device_fields) / sizeof(g_device_fields[0]));
}

unsigned bc250_mesa_hw_ip_fields(const struct bc250_field **out)
{
    *out = g_hw_ip_fields;
    return (unsigned)(sizeof(g_hw_ip_fields) / sizeof(g_hw_ip_fields[0]));
}

uint32_t bc250_mesa_sizeof_device(void) { return (uint32_t)sizeof(struct drm_amdgpu_info_device); }
uint32_t bc250_mesa_sizeof_hw_ip(void)  { return (uint32_t)sizeof(struct drm_amdgpu_info_hw_ip); }
uint32_t bc250_mesa_sizeof_memory(void) { return (uint32_t)sizeof(struct drm_amdgpu_memory_info); }
uint32_t bc250_mesa_sizeof_heap(void)   { return (uint32_t)sizeof(struct drm_amdgpu_heap_info); }
