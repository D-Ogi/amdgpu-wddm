// m16_kernels.hip - the three kernels of the M16 step-1 trial, and the only input of
// the committed code object and fat binary beside it (see PROVENANCE.txt).
//
// No ROCm headers and no ROCm device library are used, so the kernels only use clang
// builtins. The HIP attribute spellings that clang understands without the HIP
// headers stand in for the __global__ and __shared__ macros, because layer 1 must
// build its own test fixtures before layer 2 ships hip_runtime.h.
//
// Each kernel answers one question of the step-1 trial:
//   vadd           does a clang-built code object run at all, with explicit arguments
//   reduce256      is the computed COMPUTE_PGM_RSRC2.LDS_SIZE right (decision 3)
//   writeGridSize  is the hidden argument block filled from the metadata list
//
// Build: compute/hip/build.ps1 -Rebuild (clang -x hip --offload-arch=gfx1013
//        --offload-device-only -nogpuinc -nogpulib -O2 -std=c++17).

#define KERNEL extern "C" __attribute__((global))
#define GROUP __attribute__((shared))

// 1. Plain kernel: 4 pointer/scalar arguments, no LDS, no scratch, no device library.
KERNEL void vadd(const float *a, const float *b, float *c, int n) {
  int i = __builtin_amdgcn_workgroup_id_x() * 256 + __builtin_amdgcn_workitem_id_x();
  if (i < n) {
    c[i] = a[i] + b[i];
  }
}

// 2. Kernel with LDS and a barrier: shows group_segment_fixed_size and the barrier
//    instruction that our dispatch must not have to emulate.
KERNEL void reduce256(const float *in, float *out) {
  GROUP float tile[256];
  unsigned t = __builtin_amdgcn_workitem_id_x();
  tile[t] = in[__builtin_amdgcn_workgroup_id_x() * 256 + t];
  __builtin_amdgcn_s_barrier();
  for (unsigned s = 128; s > 0; s >>= 1) {
    if (t < s) {
      tile[t] += tile[t + s];
    }
    __builtin_amdgcn_s_barrier();
  }
  if (t == 0) {
    out[__builtin_amdgcn_workgroup_id_x()] = tile[0];
  }
}

// 3. Kernel that reads the hidden (implicit) kernel arguments through the builtin
//    that HIP uses for blockDim. It shows which implicit argument block the runtime
//    must fill in, because the generated code loads from the implicit kernarg pointer.
KERNEL void writeGridSize(unsigned *out) {
  __attribute__((address_space(4))) const unsigned *implicit =
      (__attribute__((address_space(4))) const unsigned *)
          __builtin_amdgcn_implicitarg_ptr();
  unsigned t = __builtin_amdgcn_workitem_id_x();
  if (t == 0) {
    // The code object v5 implicit argument block starts with block_count_x, _y and _z
    // as 32-bit words at offsets 0, 4 and 8. Those three hold the work-group count of
    // the dispatch, which AMDGPUUsage.rst says is "not the same as the value in the AQL
    // dispatch packet, which has the grid size in work-items". The host writes the same
    // three numbers from the launch and compares them.
    out[0] = implicit[0];
    out[1] = implicit[1];
    out[2] = implicit[2];
  }
}
