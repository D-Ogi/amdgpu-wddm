/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/bc250_umd_private.h - the caps blob that bc250kmd hands to a user-mode driver.
 *
 * WHAT THIS IS
 *
 * On Linux, RADV learns what the GPU is by calling the amdgpu INFO ioctl a dozen times. On
 * Windows there is no ioctl: a WDDM user-mode driver asks dxgkrnl for KMTQAITYPE_UMDRIVERPRIVATE,
 * dxgkrnl turns that into DXGKQAITYPE_UMDRIVERPRIVATE on DxgkDdiQueryAdapterInfo, and whatever
 * bytes the miniport writes into pOutputData arrive verbatim in user mode. The shape of those
 * bytes is entirely ours to choose, and dxgkrnl neither inspects nor versions them. This header
 * is that shape.
 *
 * It is deliberately NOT a new information model. It is the Linux UAPI structures, by value, in
 * the order the kernel defines them, so that:
 *
 *   - the KMD fills the same fields from the same hardware registers amdgpu reads, and a
 *     disagreement with a Linux run on the same unit is a bug with an obvious diff;
 *   - the winsys can hand `&blob->device` straight to ac_identify_chip() and friends;
 *   - nobody has to invent, or argue about, a second set of names for the same 65 numbers.
 *
 * PROVENANCE
 *
 * The structures are not retyped here. They come from driver/contract/third_party/amdgpu_drm.h,
 * which is include/uapi/drm/amdgpu_drm.h imported byte for byte from Linux tag v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449, MIT-licensed, copyright the respective AMD and
 * Precision Insight / VA Linux / Tungsten Graphics holders. See third_party/PROVENANCE.md for the
 * import command and the checksum, and driver/README.md for the repository rule this follows
 * ("import AMD code, do not retype it"). This file, the shim in uapi-shim/ and everything under
 * test/ are ours and carry the repository's own licence.
 *
 * THE MESA TRAP
 *
 * Mesa built for Windows does not include the kernel header. src/amd/common/ac_linux_drm.h:16-321
 * declares its own copies of the same structures. Two of the three are NOT layout-compatible with
 * the kernel's, and a winsys that memcpy()s this blob into Mesa's types will silently read
 * garbage. Measured against Mesa main (P:\BC-250\ref\mesa, origin/main 3ae3d2e):
 *
 *   struct drm_amdgpu_info_device   65 members, identical names, order and widths. Compatible.
 *                                   448 bytes on both sides. Pass it through by pointer.
 *   struct drm_amdgpu_heap_info     kernel 4 members / 32 bytes (amdgpu_drm.h:1374);
 *                                   Mesa 1 member / 8 bytes (ac_linux_drm.h:287).
 *                                   drm_amdgpu_memory_info is therefore 96 bytes here and 24 in
 *                                   Mesa. MUST be translated field by field.
 *   struct drm_amdgpu_info_hw_ip    kernel 8 members / 40 bytes (amdgpu_drm.h:1540);
 *                                   Mesa 6 members / 24 bytes, no capabilities_flags, no
 *                                   userq_num_slots (ac_linux_drm.h:192).
 *                                   MUST be translated field by field.
 *
 * The blob carries the KERNEL shapes, because those are what our KMD reads off the hardware and
 * what a Linux capture can be diffed against. The translation belongs in the winsys, and
 * driver/contract/test/bc250_caps_mesa.c is the worked reference for it. The test asserts the
 * offsets on both sides, so the day Mesa or the kernel moves a field, the test says so.
 *
 * BOTH BUILD MODES
 *
 * The header compiles as plain user-mode C and with the WDK kernel flags the miniport uses.
 * Define BC250_CONTRACT_KERNEL (or BC250_SHIM_KERNEL, or let the WDK define _KERNEL_MODE) for the
 * kernel variant; uapi-shim/drm.h then takes the fixed-width typedefs from the compiler instead
 * of <stdint.h>, which the km CRT does not have. Compile with the shim on the include path:
 *     /I driver/contract /I driver/contract/uapi-shim
 *
 * WHAT IS IN HERE AND WHERE EACH NUMBER COMES FROM
 *
 * Column 2 is the line in third_party/amdgpu_drm.h that declares the member; column 3 is the line
 * in Mesa's src/amd/common/ac_gpu_info.c that reads it. "-" in column 3 means no Mesa code path
 * reads it on this chip and it is carried for fidelity with the ioctl only. Unit A's measured
 * values and their evidence IDs are in README.md and in test/bc250_caps_unitA.c; they are not in
 * this header, because the header is the contract and the values are evidence.
 *
 *   member                          amdgpu_drm.h   ac_gpu_info.c    note
 *   device.device_id                1433           663              -> info->pci_id
 *   device.chip_rev                 1435           934
 *   device.external_rev             1436           670,503,521..    the chip identity, with family
 *   device.pci_rev                  1438           664
 *   device.family                   1439           675              FAMILY_NV for Cyan Skillfish
 *   device.num_shader_engines       1440           1216
 *   device.num_shader_arrays_per_engine 1441       1217
 *   device.gpu_counter_freq         1443           1230             0 => Mesa warns, timestamps wrong
 *   device.max_engine_clock         1444           1214             kHz
 *   device.max_memory_clock         1445           1215             kHz
 *   device.cu_active_number         1447           -                Mesa recomputes from cu_bitmap
 *   device.cu_ao_mask               1449           -                the UAPI marks it INVALID
 *   device.cu_bitmap                1450           1252,1266,1268   all-zero => num_cu 0, and on
 *                                                                   GFX10_3+ a divide by zero at 1311
 *   device.enabled_rb_pipes_mask    1452           1219
 *   device.num_rb_pipes             1453           1225
 *   device.num_hw_gfx_contexts      1454           -
 *   device.pcie_gen                 1456           1662
 *   device.ids_flags                1457           357,566,1111,1197 FUSION/TMZ/PREEMPTION/TRUNC_COORD
 *   device.virtual_address_offset   1459           -                the winsys needs it, ac_ does not
 *   device.virtual_address_max      1461           563
 *   device.virtual_address_alignment 1463          564
 *   device.pte_fragment_size        1465           655
 *   device.gart_page_size           1466           656
 *   device.ce_ram_size              1468           -
 *   device.vram_type                1470           575              feeds ac_memory_ops_per_clock
 *   device.vram_bit_width           1472           576
 *   device.vce_harvest_config       1474           665
 *   device.gc_double_offchip_lds_buf 1476          -
 *   device.prim_buf_gpu_addr..param_buf_size 1478-1488 -            gfx9 NGG scratch, unused here
 *   device.wave_front_size          1490           -
 *   device.num_shader_visible_vgprs 1492           (3 sites)
 *   device.num_cu_per_sh            1494           1218
 *   device.num_tcc_blocks           1496           587
 *   device.gs_vgt_table_depth       1498           -
 *   device.gs_prim_buffer_depth     1500           -
 *   device.max_gs_waves_per_vgt     1502           -
 *   device.pcie_num_lanes           1504           1663
 *   device.cu_ao_bitmap             1506           -
 *   device.high_va_offset           1508           561
 *   device.high_va_max              1510           562
 *   device.pa_sc_tile_steering_override 1512       1224
 *   device.tcc_disabled_mask        1514           590
 *   device.min_engine_clock         1515           -
 *   device.min_memory_clock         1516           -
 *   device.tcp_cache_size           1518           603              GFX11+ only; ignored on gfx10.1
 *   device.num_sqc_per_wgp          1519           1244
 *   device.sqc_data_cache_size      1520           1243             KiB
 *   device.sqc_inst_cache_size      1521           1242             KiB
 *   device.gl1c_cache_size          1522           604              GFX11+ only; ignored on gfx10.1
 *   device.gl2c_cache_size          1523           605              GFX11+ only; ignored on gfx10.1
 *   device.mall_size                1524           606              GFX11+ only; ignored on gfx10.1
 *   device.enabled_rb_pipes_mask_hi 1526           1220
 *   device.shadow_size..csa_alignment 1528-1534    -                CP gfx shadowing, gfx11+
 *   device.userq_ip_mask            1536           1521             leave 0: we have no user queues
 *   memory.vram/cpu_accessible_vram/gtt 1396       555-557          .total_heap_size only
 *   hw_ip[].available_rings         1551           500,506          popcount => ip[].num_queues
 *   hw_ip[].hw_ip_version_major     1542           515              fallback when discovery is 0
 *   hw_ip[].hw_ip_version_minor     1543           516
 *   hw_ip[].ib_start_alignment      1547           540
 *   hw_ip[].ib_size_alignment       1549           541
 *   hw_ip[].ip_discovery_version    1553           510-513          (maj<<16)|(min<<8)|rev
 *   tiling.gb_addr_config           (not in UAPI)  457              amdgpu_gpu_info::gb_addr_cfg
 *   tiling.mc_arb_ramcfg            (not in UAPI)  456
 *   tiling.gb_tile_mode             (not in UAPI)  466,472          gfx6-8 only; zero on gfx10
 *   tiling.gb_macro_tile_mode       (not in UAPI)  474              gfx6-8 only; zero on gfx10
 *   firmware.me/pfp/mec version+feature (not in UAPI) 1546-1565     ac_query_gpu_info hard-fails
 *                                                                   if these queries fail
 *   kernel.drm_major/minor          (not in UAPI)  1493,1496        Mesa asserts 3 and >= 54
 *   kernel.address32_hi             (not in UAPI)  1598
 *   kernel.pci_*                    (not in UAPI)  1487             ac_drm_query_pci_bus_info
 *
 * The four members Mesa reads that are NOT in drm_amdgpu_info_device (gb_addr_config,
 * mc_arb_ramcfg and the two tile-mode arrays) come from the separate libdrm structure
 * `struct amdgpu_gpu_info` (ac_linux_drm.h:287, filled on Linux by amdgpu_query_gpu_info()).
 * There is no kernel UAPI structure for them, so they are carried as loose fields.
 *
 * VERSIONING RULE
 *
 * `magic`, `version` and `size` are the first three words and never move. A reader that does not
 * recognise magic, or that gets a size smaller than the offset of the field it wants, must fail
 * rather than guess. New fields are appended and the version is bumped; nothing is ever removed
 * or reordered, because an old UMD and a new KMD can meet on the same machine after an upgrade.
 */
#ifndef BC250_UMD_PRIVATE_H
#define BC250_UMD_PRIVATE_H

#include "amdgpu_drm.h"     /* third_party/, imported; pulls uapi-shim/drm.h */

#if defined(__cplusplus)
extern "C" {
#endif

/* "BC25" little endian. Any other value: not our blob, do not parse it. */
#define BC250_UMD_PRIVATE_MAGIC     0x35324342u

/* 2: version 1 plus max_submitted_ibs[], appended. See the versioning rule above. Nothing was
 * moved or removed, so a version-1 reader still finds every field it knows where it expects it;
 * it just stops short. Bumped while nothing consumes the blob yet, which is the cheapest moment. */
#define BC250_UMD_PRIVATE_VERSION   2u

/* Sized by the UAPI so a future IP type does not silently fall off the end. */
#define BC250_UMD_HW_IP_MAX         AMDGPU_HW_IP_NUM   /* 10, amdgpu_drm.h:948 */

/* bc250_umd_private::flags */
#define BC250_UMD_F_APU             0x00000001u  /* dGPU-style VRAM is carved out of system RAM */
#define BC250_UMD_F_ALL_VRAM_VISIBLE 0x00000002u /* cpu_accessible_vram == vram */
#define BC250_UMD_F_GOLDEN_GB_ADDR  0x00000004u  /* tiling.gb_addr_config is amdgpu's golden
                                                  * constant for this ASIC, not the register read.
                                                  * See README.md: on Cyan Skillfish amdgpu never
                                                  * reads GB_ADDR_CONFIG, and the two differ. */
#define BC250_UMD_F_UNMEASURED      0x00000008u  /* at least one field is a placeholder, not a
                                                  * value read from this unit. The KMD sets this
                                                  * until the wishlist in README.md is closed; a
                                                  * release build of the UMD should refuse it. */

/* The libdrm `struct amdgpu_gpu_info` subset that ac_fill_tiling_info() reads
 * (ac_gpu_info.c:454-476). Not a UAPI structure, so it is spelled out here. */
struct bc250_umd_tiling {
    __u32 gb_addr_config;               /* ac_gpu_info.c:457 -> info->gb_addr_config */
    __u32 mc_arb_ramcfg;                /* ac_gpu_info.c:456 */
    __u32 gb_tile_mode[32];             /* ac_gpu_info.c:472; gfx6-8 only, zero on gfx10 */
    __u32 gb_macro_tile_mode[16];       /* ac_gpu_info.c:474; gfx6-8 only, zero on gfx10 */
};

/* AMDGPU_INFO_FW_* answers. ac_query_gpu_info() returns AC_QUERY_GPU_INFO_FAIL outright if the
 * ME, MEC or PFP query fails (ac_gpu_info.c:1546-1565), so these are not optional. The rest are
 * carried because bc250kmd already knows them from the PSP load and because a version mismatch
 * against a Linux capture is the cheapest way to catch a wrong firmware blob. */
struct bc250_umd_firmware {
    __u32 me_version,   me_feature;
    __u32 pfp_version,  pfp_feature;
    __u32 ce_version,   ce_feature;
    __u32 mec_version,  mec_feature;
    __u32 mec2_version, mec2_feature;
    __u32 rlc_version,  rlc_feature;
    __u32 sdma_version, sdma_feature;
    __u32 smc_version;
    __u32 reserved;
};

/* What the DRM layer would have told the winsys and WDDM does not. */
struct bc250_umd_kernel {
    __u32 drm_major;                    /* ac_gpu_info.c:1493 asserts 3 */
    __u32 drm_minor;                    /* ac_gpu_info.c:1496 requires >= 54 */
    __u32 drm_patchlevel;
    __u32 address32_hi;                 /* ac_gpu_info.c:1598, amdgpu_sw_info_address32_hi */
    __u32 pci_domain;                   /* ac_gpu_info.c:1487, ac_drm_query_pci_bus_info */
    __u32 pci_bus;
    __u32 pci_dev;
    __u32 pci_func;
};

struct bc250_umd_private {
    /* --- envelope: these four words never move --- */
    __u32 magic;                        /* BC250_UMD_PRIVATE_MAGIC */
    __u32 version;                      /* BC250_UMD_PRIVATE_VERSION */
    __u32 size;                         /* sizeof(struct bc250_umd_private) as the KMD built it */
    __u32 flags;                        /* BC250_UMD_F_* */

    /* --- the imported UAPI structures, kernel shapes (see THE MESA TRAP above) --- */
    struct drm_amdgpu_info_device device;               /* AMDGPU_INFO_DEV_INFO */
    struct drm_amdgpu_memory_info memory;               /* AMDGPU_INFO_MEMORY */
    struct drm_amdgpu_info_hw_ip  hw_ip[BC250_UMD_HW_IP_MAX];  /* AMDGPU_INFO_HW_IP_INFO */
    __u32 hw_ip_mask;                   /* bit n set: hw_ip[n] was filled; others are zero */
    __u32 hw_ip_instances[BC250_UMD_HW_IP_MAX];         /* AMDGPU_INFO_HW_IP_COUNT */
    __u32 hw_ip_reserved;

    /* --- the rest --- */
    struct bc250_umd_tiling   tiling;
    struct bc250_umd_firmware firmware;
    struct bc250_umd_kernel   kernel;

    /* Version 1's reserved tail. Left exactly where and as big as it was, so a version-1 reader
     * that checks it is still zero keeps working. New fields go after it, never into it. */
    __u32 reserved[10];

    /* --- appended in version 2, strictly after version 1's last byte --- */
    /* AMDGPU_INFO_MAX_IBS (query 0x22), indexed by AMDGPU_HW_IP_*. The kernel computes each as
     * (ring->max_dw - emit_frame_size) / emit_ib_size, so it is a property of this ASIC's rings
     * and its firmware, not something a UMD can work out. ac_gpu_info.c:1698 reads it; if the
     * query fails it substitutes estimates (:1700-1716) that are WRONG for this part - it guesses
     * 124 for compute where unit A measures 125. Zero here means "not measured": a reader should
     * then fall back exactly as Mesa does rather than submit 0 IBs. */
    __u32 max_submitted_ibs[BC250_UMD_HW_IP_MAX];

    __u32 reserved_v2[6];
};

/* ---------------------------------------------------------------------------------------------
 * Compile-time checks. These are what stops a well-meant edit from silently changing the wire
 * format. They are plain C89 negative-array-size asserts so they work in /kernel mode too, where
 * <assert.h> is not available and the compiler is not required to be C11.
 * ------------------------------------------------------------------------------------------- */
#define BC250_CTASSERT3(cond, line) \
    typedef char bc250_ctassert_##line[(cond) ? 1 : -1]
#define BC250_CTASSERT2(cond, line) BC250_CTASSERT3(cond, line)
#define BC250_CTASSERT(cond)        BC250_CTASSERT2(cond, __LINE__)

#ifndef offsetof
#define offsetof(t, m) ((unsigned long)(unsigned long long)&(((t *)0)->m))
#endif

/* The imported structures must have the sizes the Linux ioctl has on x86-64, or the blob is not
 * a capture of the same wire format and a diff against a Linux run means nothing. */
BC250_CTASSERT(sizeof(struct drm_amdgpu_info_device) == 448);
BC250_CTASSERT(sizeof(struct drm_amdgpu_info_hw_ip) == 40);
BC250_CTASSERT(sizeof(struct drm_amdgpu_heap_info) == 32);
BC250_CTASSERT(sizeof(struct drm_amdgpu_memory_info) == 96);

/* A few interior offsets, because a wrong compiler packing setting would change these and not the
 * total size. cu_bitmap is the one that must be right or Mesa divides by zero. */
BC250_CTASSERT(offsetof(struct drm_amdgpu_info_device, cu_bitmap) == 56);
BC250_CTASSERT(offsetof(struct drm_amdgpu_info_device, ids_flags) == 136);
BC250_CTASSERT(offsetof(struct drm_amdgpu_info_device, tcc_disabled_mask) == 360);
BC250_CTASSERT(offsetof(struct drm_amdgpu_info_device, userq_ip_mask) == 436);

/* The envelope. A reader that only knows version 1 relies on these four being where it thinks. */
BC250_CTASSERT(offsetof(struct bc250_umd_private, magic)   == 0);
BC250_CTASSERT(offsetof(struct bc250_umd_private, version) == 4);
BC250_CTASSERT(offsetof(struct bc250_umd_private, size)    == 8);
BC250_CTASSERT(offsetof(struct bc250_umd_private, flags)   == 12);
BC250_CTASSERT(offsetof(struct bc250_umd_private, device)  == 16);

/* The blob as a whole. Change this number only together with the version.
 *
 * V1 is kept as a named constant, not as history for its own sake: a reader that only knows
 * version 1 must still be able to say "the first 1344 bytes are the shape I understand", and the
 * assert below is what guarantees version 2 only grew at the end. */
#define BC250_UMD_PRIVATE_SIZE_V1 1344
#define BC250_UMD_PRIVATE_SIZE_V2 1408
BC250_CTASSERT(sizeof(struct bc250_umd_private) == BC250_UMD_PRIVATE_SIZE_V2);
/* Version 2 grew only at the end: the first field it added starts exactly where version 1 stopped,
 * so every version-1 offset, including its reserved tail, is untouched. */
BC250_CTASSERT(offsetof(struct bc250_umd_private, max_submitted_ibs) == BC250_UMD_PRIVATE_SIZE_V1);

/* DxgkDdiQueryAdapterInfo copies the blob into a user-mode buffer. Keep it small enough that the
 * copy is never the reason a query fails, and a round multiple of 64 so it does not straddle more
 * cache lines than it must. */
BC250_CTASSERT(BC250_UMD_PRIVATE_SIZE_V2 % 64 == 0);
BC250_CTASSERT(BC250_UMD_PRIVATE_SIZE_V2 <= 4096);

#if defined(__cplusplus)
}
#endif

#endif /* BC250_UMD_PRIVATE_H */
