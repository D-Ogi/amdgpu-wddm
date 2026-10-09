// M16 step 3: the two dp4a forms ggml-cuda can take on gfx1013, isolated.
//
// BC250_DP4A=1  the generic C byte loop, which ggml-cuda falls to when no architecture macro
//               matches (ggml/src/ggml-cuda/common.cuh, the #else of ggml_cuda_dp4a)
// BC250_DP4A=2  the RDNA1 inline assembly, which ggml-cuda uses when RDNA1 is defined
//
// PROVENANCE: both bodies are the two branches of ggml_cuda_dp4a in llama.cpp b86d2f075
// (ggml/src/ggml-cuda/common.cuh:711-734), MIT licence. They are reproduced here unchanged so
// that the comparison is of the two code paths and of nothing else: one kernel, one call, no
// surrounding matrix multiply to optimise across.
//
//   clang++ -x hip --offload-arch=gfx1013 --offload-device-only --no-gpu-bundle-output \
//           -nogpuinc -nogpulib -O2 -std=c++17 -DBC250_DP4A=<1|2> -c -o <out> probe-dp4a.hip

#define __device__ __attribute__((device))
#define __global__ __attribute__((global))
#define __forceinline__ inline __attribute__((always_inline))

typedef signed char int8_t;
typedef int8_t int8x4_t __attribute__((ext_vector_type(4)));

static __device__ __forceinline__ int bc250_dp4a(const int a, const int b, int c) {
#if BC250_DP4A == 2
    int tmp1;
    int tmp2;
    asm("\n \
        v_mul_i32_i24 %1, sext(%3), sext(%4) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0 \n \
        v_mul_i32_i24 %2, sext(%3), sext(%4) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:BYTE_1 \n \
        v_add3_u32 %0, %1, %2, %0 \n \
        v_mul_i32_i24 %1, sext(%3), sext(%4) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:BYTE_2 \n \
        v_mul_i32_i24 %2, sext(%3), sext(%4) dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_3 src1_sel:BYTE_3 \n \
        v_add3_u32 %0, %1, %2, %0 \n \
        "
        : "+v"(c), "=&v"(tmp1), "=&v"(tmp2)
        : "v"(a), "v"(b)
    );
#else
    const int8x4_t va = reinterpret_cast<const int8x4_t&>(a);
    const int8x4_t vb = reinterpret_cast<const int8x4_t&>(b);
    c += va[0] * vb[0] + va[1] * vb[1] + va[2] * vb[2] + va[3] * vb[3];
#endif
    return c;
}

// Every operand is indexed by the lane, so that it lives in a vector register. MEASURED: with
// operands that are the same in every lane, clang puts the whole generic form on the scalar unit
// (s_mul_i32 and s_bfe_i32), which is not the code MMQ gets and would make the comparison
// meaningless.
#define BC250_LANE() ((int)__builtin_amdgcn_workitem_id_x())

// One call, so that the kernel's instructions are the dp4a's instructions plus the two loads
// and the one store around it.
__global__ void probe_dp4a_one(const int *a, const int *b, int *c) {
  const int t = BC250_LANE();
  c[t] = bc250_dp4a(a[t], b[t], c[t]);
}

// Eight independent calls over eight pairs, which is the shape MMQ has: a row of accumulators,
// each fed by its own pair. It shows what the two forms cost when the scheduler has work to
// interleave.
__global__ void probe_dp4a_eight(const int *a, const int *b, int *c) {
  const int t = BC250_LANE() * 8;
  int acc[8];
#pragma unroll
  for (int i = 0; i < 8; ++i) {
    acc[i] = c[t + i];
  }
#pragma unroll
  for (int i = 0; i < 8; ++i) {
    acc[i] = bc250_dp4a(a[t + i], b[t + i], acc[i]);
  }
#pragma unroll
  for (int i = 0; i < 8; ++i) {
    c[t + i] = acc[i];
  }
}
