/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * driver/shim/include/amdgpu_ucode.h - the slice of amdgpu_ucode.h the PSP path needs: the headers
 * of the firmware files that go to the PSP on the BC-250 (amdgpu/cyan_skillfish2_*.bin).
 *
 *   [amdgpu] copied from drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h, MIT, kernel tag v6.18, commit
 *            7d0a66e4bb9081d75c82ec4957c50034cb0ea449. Copyright the respective AMD copyright
 *            holders (see driver/amdgpu-import/PROVENANCE.md).
 *
 * Only the four header versions the eight cyan_skillfish2 files carry are taken over: common,
 * gfx v1.0 (ce, pfp, me, mec, mec2), rlc v2.0 (rlc), sdma v1.0 (sdma, sdma1). The files are little
 * endian and so is every machine this driver runs on, so le32_to_cpu is the identity here.
 */
#ifndef BC250_SHIM_AMDGPU_UCODE_H
#define BC250_SHIM_AMDGPU_UCODE_H

#include "amdgpu.h"

#ifndef le32_to_cpu
#define le32_to_cpu(x) ((u32)(x))
#endif
#ifndef le16_to_cpu
#define le16_to_cpu(x) ((u16)(x))
#endif

/* [amdgpu] */
struct common_firmware_header {
	uint32_t size_bytes; /* size of the entire header+image(s) in bytes */
	uint32_t header_size_bytes; /* size of just the header in bytes */
	uint16_t header_version_major; /* header version */
	uint16_t header_version_minor; /* header version */
	uint16_t ip_version_major; /* IP version */
	uint16_t ip_version_minor; /* IP version */
	uint32_t ucode_version;
	uint32_t ucode_size_bytes; /* size of ucode in bytes */
	uint32_t ucode_array_offset_bytes; /* payload offset from the start of the header */
	uint32_t crc32;  /* crc32 checksum of the payload */
};

/* [amdgpu] version_major=1, version_minor=0 */
struct gfx_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t jt_offset; /* jt location */
	uint32_t jt_size;  /* size of jt */
};

/* [amdgpu] version_major=2, version_minor=0 */
struct rlc_firmware_header_v2_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t jt_offset; /* jt location */
	uint32_t jt_size;  /* size of jt */
	uint32_t save_and_restore_offset;
	uint32_t clear_state_descriptor_offset;
	uint32_t avail_scratch_ram_locations;
	uint32_t reg_restore_list_size;
	uint32_t reg_list_format_start;
	uint32_t reg_list_format_separate_start;
	uint32_t starting_offsets_start;
	uint32_t reg_list_format_size_bytes; /* size of reg list format array in bytes */
	uint32_t reg_list_format_array_offset_bytes; /* payload offset from the start of the header */
	uint32_t reg_list_size_bytes; /* size of reg list array in bytes */
	uint32_t reg_list_array_offset_bytes; /* payload offset from the start of the header */
	uint32_t reg_list_format_separate_size_bytes; /* size of reg list format array in bytes */
	uint32_t reg_list_format_separate_array_offset_bytes; /* payload offset from the start of the header */
	uint32_t reg_list_separate_size_bytes; /* size of reg list array in bytes */
	uint32_t reg_list_separate_array_offset_bytes; /* payload offset from the start of the header */
};

/* [amdgpu] version_major=1, version_minor=0 */
struct sdma_firmware_header_v1_0 {
	struct common_firmware_header header;
	uint32_t ucode_feature_version;
	uint32_t ucode_change_version;
	uint32_t jt_offset; /* jt location */
	uint32_t jt_size; /* size of jt */
};

#endif /* BC250_SHIM_AMDGPU_UCODE_H */
