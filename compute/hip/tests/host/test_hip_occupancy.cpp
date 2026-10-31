// test_hip_occupancy.cpp - the occupancy model of hip_occupancy.cpp, with no device.
//
// Every expected number is worked out by hand from the LLVM functions that hip_occupancy.cpp
// names: 16 wave slots a SIMD (half of the 32 that layer 1 reports a compute unit), two SIMDs a
// compute unit, 1024 VGPRs a SIMD in wave32 in granules of 8, and 16 barriers a compute unit.
// The control is the model without registers (vgprs 0), which must give what the earlier model
// of hip_device.cpp gave. In WGP mode the model gives the CU-mode answer unless that is 0; then a
// workgroup that fits the four SIMDs of a WGP gives 1.
#include "runtime_internal.h"

#include <cstdio>

static unsigned checks, failures;
#define CHECK_INT(actual, expected)                                                         \
    do {                                                                                    \
        ++checks;                                                                           \
        const long long a_ = static_cast<long long>(actual);                                \
        const long long e_ = static_cast<long long>(expected);                              \
        if (a_ != e_) {                                                                     \
            ++failures;                                                                     \
            std::printf("FAIL %s:%d %s is %lld, expected %lld\n", __FILE__, __LINE__,      \
                        #actual, a_, e_);                                                   \
        }                                                                                   \
    } while (0)

static int blocks(uint32_t threads, uint32_t vgprs, uint64_t group_bytes = 0u, bool wgp = false) {
    bc250hip::OccupancyInput in;
    in.block_threads = threads;
    in.wave_size = 32u;
    in.waves_per_cu = 32u;
    in.vgprs = vgprs;
    in.group_bytes = group_bytes;
    in.lds_bytes_per_cu = 65536u;
    in.wgp_mode = wgp;
    return bc250hip::occupancy_blocks_per_cu(in);
}

int main() {
    // The wave slots alone: 8 waves take 4 slots on each SIMD, 16 / 4 = 4. This is the answer
    // of the model before registers, for every case below that registers do not limit.
    CHECK_INT(blocks(256u, 0u), 4);
    CHECK_INT(blocks(256u, 32u), 4);      // 1024 / 32 = 32 waves a SIMD, the slots limit to 16
    CHECK_INT(blocks(256u, 128u), 2);     // 1024 / 128 = 8, 8 / 4 = 2
    CHECK_INT(blocks(256u, 130u), 1);     // 130 rounds to 136, 1024 / 136 = 7, 7 / 4 = 1
    CHECK_INT(blocks(128u, 256u), 2);     // 4 waves, 2 a SIMD; 1024 / 256 = 4, 4 / 2 = 2
    CHECK_INT(blocks(1024u, 128u), 0);    // 32 waves need 16 a SIMD, 8 fit: no workgroup fits
    CHECK_INT(blocks(32u, 8u), 16);       // one wave: no barrier, 16 slots a SIMD
    CHECK_INT(blocks(64u, 24u), 16);      // 2 waves, 1 a SIMD; 1024 / 24 = 42, slots 16, barriers 16
    CHECK_INT(blocks(32u, 1025u), 0);     // 1032 registers do not fit one SIMD
    CHECK_INT(blocks(64u, 24u, 20000u), 3);    // 65536 / 20000 = 3
    CHECK_INT(blocks(64u, 24u, 65537u), 0);    // more local memory than a compute unit has

    // WGP mode: a CU-mode answer that is not 0 stays as it is.
    CHECK_INT(blocks(256u, 128u, 0u, true), 2);
    CHECK_INT(blocks(256u, 130u, 0u, true), 1);
    CHECK_INT(blocks(32u, 8u, 0u, true), 16);
    CHECK_INT(blocks(64u, 24u, 20000u, true), 3);
    // The defect of 2026-10-31: flash_attn_tile<256,256,...>, 512 threads, 184 VGPRs, 17920 bytes.
    // CU mode: 16 waves need 8 a SIMD, 1024 / 184 = 5 hold: 0. WGP mode: 4 a SIMD, 5 / 4 = 1.
    CHECK_INT(blocks(512u, 184u, 17920u), 0);
    CHECK_INT(blocks(512u, 184u, 17920u, true), 1);
    CHECK_INT(blocks(512u, 256u, 17920u, true), 1);   // 4 waves a SIMD of 256 VGPRs: exactly 1024
    CHECK_INT(blocks(1024u, 128u, 0u, true), 1);      // 8 a SIMD of 128 VGPRs (CU mode: 0, above)
    CHECK_INT(blocks(1024u, 136u, 0u, true), 0);      // 8 a SIMD, 1024 / 136 = 7: not even a WGP
    CHECK_INT(blocks(64u, 24u, 65537u, true), 0);     // one workgroup still addresses 64 KiB at most

    // The register count of a kernel: the metadata first, the descriptor when the metadata is 0.
    bc250hsa_kernel kernel{};
    kernel.vgpr_count = 40u;
    kernel.compute_pgm_rsrc1 = 5u;
    CHECK_INT(bc250hip::occupancy_kernel_vgprs(kernel), 40);
    kernel.vgpr_count = 0u;
    CHECK_INT(bc250hip::occupancy_kernel_vgprs(kernel), 48);   // (5 + 1) * 8
    // WGP_MODE is bit 29 of COMPUTE_PGM_RSRC1: 0xe0af0016 is the descriptor of the kernel above.
    CHECK_INT(bc250hip::occupancy_kernel_wgp_mode(kernel), 0);
    kernel.compute_pgm_rsrc1 = 0xe0af0016u;
    CHECK_INT(bc250hip::occupancy_kernel_wgp_mode(kernel), 1);
    kernel.compute_pgm_rsrc1 = 0xc0af0016u;
    CHECK_INT(bc250hip::occupancy_kernel_wgp_mode(kernel), 0);

    std::printf("test_hip_occupancy: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
