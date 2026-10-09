// M16 step 3: does this target have the 8-bit dot product instructions?
//
// One kernel per builtin, so that each refusal names its own builtin and its own target feature.
// Build it for gfx1013 (the BC-250's part) and for gfx1030 (the positive control, an RDNA2 part
// that ggml-cuda's own CDNA/RDNA2 branch uses with sdot4).
//
//   clang++ -x hip --offload-arch=<arch> --offload-device-only -nogpuinc -nogpulib \
//           -O2 -std=c++17 -c -o <out> probe-dot.hip -DBC250_PROBE=<1|2>

#define __device__ __attribute__((device))
#define __global__ __attribute__((global))

#if BC250_PROBE == 1
// v_dot4_i32_i8, which needs the dot1-insts target feature.
__global__ void probe_sdot4(const int *a, const int *b, int *c) {
  *c = __builtin_amdgcn_sdot4(*a, *b, *c, false);
}
#elif BC250_PROBE == 2
// v_dot4_i32_iu8, which needs the dot8-insts target feature.
__global__ void probe_sudot4(const int *a, const int *b, int *c) {
  *c = __builtin_amdgcn_sudot4(true, *a, true, *b, *c, false);
}
#else
#error "define BC250_PROBE as 1 (sdot4) or 2 (sudot4)"
#endif
