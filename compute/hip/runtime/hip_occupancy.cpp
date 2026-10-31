// hip_occupancy.cpp - how many workgroups of one kernel a compute unit holds at once.
//
// hipOccupancyMaxActiveBlocksPerMultiprocessor (hip_device.cpp) asks this file. The model is
// pure, so tests/host/test_hip_occupancy.cpp checks it against numbers computed by hand from
// the same LLVM sources, with no device.
//
// Three limits, each from a primary source:
//
//   1. Wave slots. A workgroup of W waves takes ceil(W / 2) slots on each of the two SIMDs of
//      a compute unit: getEUsPerCU is 2 for GFX10 in CU mode and getWavesPerEUForWorkGroup is
//      ceil(W / EUs) (llvm/lib/Target/AMDGPU/Utils/AMDGPUBaseInfo.cpp of LLVM 23.1.2, lines
//      1196 and 1246). The slots of one SIMD are half of bc250hsa_props.waves_per_cu. LLVM's
//      getMaxWavesPerEU gives 20 for GFX10.1 (line 1237); layer 1 reports 32 a compute unit,
//      16 a SIMD (bc250hsa/kmt_device.c). The smaller number is the one used here, so the
//      answer can only be too small, never too large.
//   2. Vector registers. On GFX10.1 in wave32 a SIMD has 1024 VGPRs for its waves
//      (getTotalNumVGPRs, line 1429; unit A reads the same 1024 as num_shader_visible_vgprs,
//      driver/contract/test/bc250_caps_unitA.c, E01), allocated in granules of 8
//      (getVGPRAllocGranule, line 1390: no GFX10.3 instructions on gfx1013). The waves one
//      SIMD holds are 1024 / alignTo(vgprs, 8), at most the wave slots
//      (getNumWavesPerEUWithNumVGPRs, line 1469).
//   3. Local memory, the group segment plus the dynamic request, against the local memory of
//      one compute unit, as the earlier model of hip_device.cpp already did.
//
// A fourth limit applies to a workgroup of more than one wave: the barrier resources, 16
// workgroups a compute unit in CU mode (getMaxWorkGroupsPerCU, line 1216).
//
// SGPRs do not limit a GFX10 wave: every wave gets its full SGPR file (getSGPRAllocGranule
// returns the whole addressable count for major 10 and later, line 1259).
//
// WGP mode. The device compiler builds every kernel for WGP mode unless -mcumode is given, and
// COMPUTE_PGM_RSRC1.WGP_MODE (bit 29, AMDGPUUsage.rst, GFX10-GFX12) then lets the waves of one
// workgroup use both compute units of a workgroup processor: four SIMDs (getEUsPerCU, line
// 1213), twice the local memory for all workgroups together (getLocalMemorySize, line 1178; one
// workgroup still addresses 64 KiB at most, getAddressableLocalMemorySize) and 32 barriers
// (getMaxWorkGroupsPerCU, line 1230). The CU-mode count above stays the answer whenever it is
// not 0, so no answer of the model that the measurements used changes. When it is 0, a WGP-mode
// kernel can still run if its workgroup fits the WGP: a 512-thread flash attention workgroup
// with 184 VGPRs needs 8 waves a SIMD on two SIMDs (1472 registers, too many) but 4 a SIMD on
// four (736). The model said 0 for such a kernel, and llama.cpp stopped at
// GGML_ASSERT(max_blocks_per_sm > 0) (ggml/src/ggml-cuda/fattn-common.cuh:1138, the 35B
// perplexity run of 2026-10-31). Such a workgroup needs more than half of the four SIMDs, so a
// WGP holds one of it at most, and the answer for that case is 1: multiProcessorCount counts
// compute units, and 0 means "the kernel cannot run". This is the one answer that can be too
// large (by 2); the caller uses it only to choose how many blocks share the work of one head.

#include "runtime_internal.h"

namespace bc250hip {

namespace {

constexpr uint32_t kSimdsPerCu = 2;          // getEUsPerCU, GFX10 CU mode
constexpr uint32_t kCusPerWgp = 2;           // getEUsPerCU, GFX10 WGP mode: four SIMDs
constexpr uint32_t kVgprsPerSimdWave32 = 1024;
constexpr uint32_t kVgprGranuleWave32 = 8;
constexpr uint32_t kBarriersPerCu = 16;      // getMaxWorkGroupsPerCU, CU mode
constexpr uint32_t kBarriersPerWgp = 32;     // getMaxWorkGroupsPerCU, WGP mode
constexpr uint32_t kRsrc1WgpMode = 1u << 29; // COMPUTE_PGM_RSRC1.WGP_MODE

uint32_t ceil_div(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

// The workgroups that one block of `cus` compute units holds (1: a CU, 2: a WGP).
uint32_t blocks_per_unit(const OccupancyInput& in, uint32_t cus) {
    const uint32_t simds = kSimdsPerCu * cus;
    const uint32_t waves_per_block = ceil_div(in.block_threads, in.wave_size);
    const uint32_t slots_per_simd = in.waves_per_cu / kSimdsPerCu;
    const uint32_t per_simd_per_block = ceil_div(waves_per_block, simds);
    if (per_simd_per_block > slots_per_simd) {
        return 0;
    }

    // The waves one SIMD holds with this register count. Only wave32 is modelled: layer 1
    // refuses a wave64 kernel by name (pm4_dispatch.c, bc250hsa_pm4_check_dispatch).
    uint32_t waves_per_simd = slots_per_simd;
    if (in.wave_size == 32u && in.vgprs != 0u) {
        const uint32_t rounded = ceil_div(in.vgprs, kVgprGranuleWave32) * kVgprGranuleWave32;
        const uint32_t by_vgpr = rounded > kVgprsPerSimdWave32 ? 0u : kVgprsPerSimdWave32 / rounded;
        if (by_vgpr < waves_per_simd) {
            waves_per_simd = by_vgpr;
        }
    }
    uint32_t blocks = waves_per_simd / per_simd_per_block;

    if (in.group_bytes > 0u) {
        const uint64_t by_lds = in.lds_bytes_per_cu * cus / in.group_bytes;
        if (by_lds < blocks) {
            blocks = static_cast<uint32_t>(by_lds);
        }
    }
    const uint32_t barriers = cus == 1u ? kBarriersPerCu : kBarriersPerWgp;
    if (waves_per_block > 1u && blocks > barriers) {
        blocks = barriers;
    }
    return blocks;
}

}  // namespace

// The VGPRs one wave of the kernel allocates. The metadata .vgpr_count is what the compiler
// states; a kernel whose metadata does not state it falls back to the granulated count of
// COMPUTE_PGM_RSRC1 bits 5:0, which is (granules - 1) in units of 8 for wave32
// (AMDGPUUsage.rst, compute_pgm_rsrc1 GRANULATED_WORKITEM_VGPR_COUNT, and
// getVGPREncodingGranule of the same AMDGPUBaseInfo.cpp, 8 in wave32).
uint32_t occupancy_kernel_vgprs(const bc250hsa_kernel& kernel) {
    if (kernel.vgpr_count != 0u) {
        return kernel.vgpr_count;
    }
    return ((kernel.compute_pgm_rsrc1 & 0x3Fu) + 1u) * kVgprGranuleWave32;
}

// Whether the kernel runs its workgroups in WGP mode: the bit that the dispatch passes to the CP
// unchanged (pm4_dispatch.c copies COMPUTE_PGM_RSRC1 from the descriptor).
bool occupancy_kernel_wgp_mode(const bc250hsa_kernel& kernel) {
    return (kernel.compute_pgm_rsrc1 & kRsrc1WgpMode) != 0u;
}

int occupancy_blocks_per_cu(const OccupancyInput& in) {
    if (in.block_threads == 0u || in.wave_size == 0u || in.waves_per_cu < kSimdsPerCu) {
        return 0;
    }
    // One workgroup addresses the local memory of one compute unit at most, in either mode.
    if (in.group_bytes > in.lds_bytes_per_cu) {
        return 0;
    }
    const uint32_t per_cu = blocks_per_unit(in, 1u);
    if (per_cu != 0u || !in.wgp_mode) {
        return static_cast<int>(per_cu);
    }
    return blocks_per_unit(in, kCusPerWgp) != 0u ? 1 : 0;
}

}  // namespace bc250hip
