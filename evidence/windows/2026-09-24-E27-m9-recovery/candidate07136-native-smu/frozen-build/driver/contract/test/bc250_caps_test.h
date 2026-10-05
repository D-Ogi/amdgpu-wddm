/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/contract/test/bc250_caps_test.h - the interface between the three translation units of
 * the caps-blob parity test.
 *
 * The test is split because it has to be. The kernel's `struct drm_amdgpu_info_device` and Mesa's
 * Windows copy of it cannot both be declared in one translation unit, so:
 *
 *   bc250_caps_unitA.c   includes bc250_umd_private.h (kernel UAPI shapes) and builds the blob
 *                        out of unit A's measured values. Knows nothing about Mesa.
 *   bc250_caps_mesa.c    includes Mesa's ac_gpu_info.h (Mesa's Windows shapes), translates the
 *                        blob into them, and runs the real ac_identify_chip / ac_fill_* path.
 *                        Knows nothing about the kernel header.
 *   bc250_caps_test.c    main(). Calls one, then the other, checks the answers, and runs the
 *                        controls.
 *
 * The blob itself crosses the boundary as a `const void *` plus its size. That is exactly how it
 * crosses the real boundary too (DxgkDdiQueryAdapterInfo hands out bytes), so the split is not an
 * artefact of the test.
 */
#ifndef BC250_CAPS_TEST_H
#define BC250_CAPS_TEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------------
 * A field's place in a structure, as one side of the build sees it. Both sides emit one of these
 * tables from the same X-macro list in bc250_umd_private_fields.h; main() compares them.
 * ------------------------------------------------------------------------------------------- */
struct bc250_field {
    const char *name;
    uint32_t    offset;
    uint32_t    size;
};

/* ---------------------------------------------------------------------------------------------
 * Side A: the KMD's view.
 * ------------------------------------------------------------------------------------------- */

/* Build unit A's blob. Returns a pointer to a static blob and writes its size. `mutation` selects
 * a deliberate corruption so the test can prove it is able to fail; BC250_MUTATE_NONE is the
 * honest blob. */
enum bc250_mutation {
    BC250_MUTATE_NONE = 0,
    BC250_MUTATE_FAMILY,            /* family = FAMILY_NV3 : Mesa must not say GFX1013 */
    BC250_MUTATE_EXTERNAL_REV,      /* external_rev out of the GFX1013 range : no chip at all */
    BC250_MUTATE_GFX_IP_VERSION,    /* GC 10.3.0 : Mesa must say GFX10_3, not GFX10 */
    BC250_MUTATE_CU_BITMAP_ZERO,    /* all CUs off : num_cu 0, the divide-by-zero case */
    BC250_MUTATE_CU_BITMAP_FULL,    /* every CU on : 40 CUs, not the 24 this unit has */
    BC250_MUTATE_VRAM_SIZE,         /* 4 GB of VRAM : the heap translation must be visible */
    BC250_MUTATE_GB_ADDR_REGISTER,  /* the raw register instead of the constant the ioctl returns */
    BC250_MUTATE_COUNT
};

const void *bc250_unitA_blob(enum bc250_mutation mutation, uint32_t *size_out);
const char *bc250_mutation_name(enum bc250_mutation mutation);

/* ---------------------------------------------------------------------------------------------
 * The cross-check table.
 *
 * The blob's DEV_INFO, MEMORY, HW_IP and FW_VERSION content is decoded from the raw ioctl replies
 * in evidence/.../boot4-readonly-after-windows/info.txt, so nothing in it is a typed-in fact. The
 * facts we previously typed in are now these rows: an independent capture (gca_config.bin,
 * ip-discovery.bin, dmesg, fact M5) that must agree with the bytes the kernel returned.
 *
 * A passing row means two different mechanisms measured the same thing and got the same answer.
 * A failing row means a fact in docs/facts.md is wrong, or the dump is not from this unit.
 * ------------------------------------------------------------------------------------------- */
struct bc250_crosscheck {
    const char *field;      /* member name, for the report */
    uint32_t    offset;     /* offsetof() inside struct drm_amdgpu_info_device */
    uint32_t    size;       /* 4 or 8 */
    uint64_t    expected;   /* what the independent capture says it should be */
    const char *source;     /* which capture, named well enough to go and look */
};
unsigned bc250_unitA_crosschecks(const struct bc250_crosscheck **out);

/* Read `size` bytes at `offset` out of a byte image, little-endian, without alignment assumptions.
 * Cross-check offsets are relative to the device structure; add bc250_blob_device_offset(). */
uint64_t bc250_blob_read_field(const void *blob, uint32_t offset, uint32_t size);
uint32_t bc250_blob_device_offset(void);

/* Where things sit inside the blob, as offsetof() sees them on the KMD side. The Mesa side cannot
 * include bc250_umd_private.h, so it navigates the bytes with this instead of with magic numbers.
 * Every member below is a real offsetof(), never a literal. */
struct bc250_blob_layout {
    uint32_t total_size;
    uint32_t magic_off, version_off, size_off, flags_off;
    uint32_t device_off;
    uint32_t memory_off, heap_stride, heap_total_off;   /* heaps in order: vram, vis vram, gtt */
    uint32_t hw_ip_off, hw_ip_stride, hw_ip_count, hw_ip_mask_off, hw_ip_instances_off;
    uint32_t gb_addr_config_off, mc_arb_ramcfg_off, gb_tile_mode_off, gb_macro_tile_mode_off;
    uint32_t fw_me_version_off, fw_me_feature_off;
    uint32_t fw_pfp_version_off, fw_pfp_feature_off;
    uint32_t fw_mec_version_off, fw_mec_feature_off;
    uint32_t drm_major_off, drm_minor_off, drm_patchlevel_off, address32_hi_off;
    uint32_t pci_domain_off, pci_bus_off, pci_dev_off, pci_func_off;
    uint32_t max_submitted_ibs_off;     /* version 2 */
};
void bc250_kernel_blob_layout(struct bc250_blob_layout *out);

/* The flag bits, re-exported so the Mesa side can read them without the contract header. */
uint32_t bc250_flag_unmeasured(void);
uint32_t bc250_flag_golden_gb_addr(void);
uint32_t bc250_expected_magic(void);

/* The kernel-side field tables. */
unsigned bc250_kernel_device_fields(const struct bc250_field **out);
unsigned bc250_kernel_hw_ip_fields(const struct bc250_field **out);
uint32_t bc250_kernel_sizeof_device(void);
uint32_t bc250_kernel_sizeof_hw_ip(void);
uint32_t bc250_kernel_sizeof_memory(void);
uint32_t bc250_kernel_sizeof_heap(void);
uint32_t bc250_kernel_sizeof_blob(void);

/* ---------------------------------------------------------------------------------------------
 * Side B: Mesa's view.
 * ------------------------------------------------------------------------------------------- */

/* What the test asserts on. Filled by running the real ac_* path over the blob. Kept to plain
 * scalars so that bc250_caps_test.c never has to include a Mesa header. */
struct bc250_mesa_result {
    int      ok;                    /* the ac_ path ran to the end without refusing the chip */
    const char *failure;            /* why not, when ok == 0 */

    int      family;                /* enum radeon_family */
    const char *family_name;
    int      gfx_level;             /* enum amd_gfx_level */
    const char *gfx_level_name;

    uint32_t ip_gfx_major, ip_gfx_minor, ip_gfx_rev;
    uint32_t num_gfx_queues, num_compute_queues, num_sdma_queues;

    uint32_t pci_id, pci_rev_id, chip_rev;
    uint32_t max_se, num_se, max_sa_per_se, num_cu_per_sh, num_cu;
    uint32_t max_good_cu_per_sa, min_good_cu_per_sa;
    uint32_t num_rb, max_render_backends, num_tcc_blocks, max_tcc_blocks;
    uint32_t l2_cache_size, tcp_cache_size, l1_cache_size;
    uint32_t gb_addr_config, num_tile_pipes;
    uint32_t max_gpu_freq_mhz, memory_freq_mhz, memory_bus_width, vram_type;
    uint64_t vram_size_kb, vram_vis_size_kb, gart_size_kb, max_heap_size_kb;
    int      all_vram_visible, has_dedicated_vram, has_graphics;
    uint32_t clock_crystal_freq, pc_lines, pbb_max_alloc_count;
    uint32_t me_fw_feature, pfp_fw_feature, mec_fw_feature;
    uint64_t virtual_address_max, high_va_offset, high_va_max;
    uint32_t pte_fragment_size, gart_page_size;
    uint32_t sqc_inst_cache_size, sqc_scalar_cache_size, num_sqc_per_wgp;
};

/* Run Mesa's real ac_identify_chip / ac_fill_hw_ip_info / ac_fill_memory_info / ac_fill_hw_info /
 * ac_fill_tiling_info / ac_fill_feature_info / ac_fill_bug_info / ac_fill_tess_info /
 * ac_fill_compiler_info over the blob, in the order ac_query_gpu_info() calls them. */
void bc250_mesa_consume(const void *blob, uint32_t size, struct bc250_mesa_result *out);

/* Print the `struct radeon_info` that the last bc250_mesa_consume() derived, using Mesa's OWN
 * ac_print_gpu_info(). That is the same function RADV_DEBUG=info calls
 * (radv_physical_device.c:2903), so the output is directly comparable, line for line, with
 * evidence/.../E14-vulkan-compute-reference/radv-info.txt. test/compare_radv_info.py does exactly
 * that comparison. Using Mesa's printer rather than our own formatting is the whole point: a
 * field we forgot to mirror cannot hide, and neither can a field Mesa adds. */
void bc250_mesa_print_radeon_info(void);

/* The Mesa-side field tables, for the layout comparison. */
unsigned bc250_mesa_device_fields(const struct bc250_field **out);
unsigned bc250_mesa_hw_ip_fields(const struct bc250_field **out);
uint32_t bc250_mesa_sizeof_device(void);
uint32_t bc250_mesa_sizeof_hw_ip(void);
uint32_t bc250_mesa_sizeof_memory(void);
uint32_t bc250_mesa_sizeof_heap(void);

/* ---------------------------------------------------------------------------------------------
 * The stub layer (bc250_caps_stubs.c): which environment options Mesa consulted while deriving
 * the caps. They were all answered with the caller's own default.
 * ------------------------------------------------------------------------------------------- */
unsigned bc250_stub_options(const char *const **names);

#ifdef __cplusplus
}
#endif

#endif /* BC250_CAPS_TEST_H */
