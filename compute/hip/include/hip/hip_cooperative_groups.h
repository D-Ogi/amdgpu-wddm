// hip_cooperative_groups.h - the cooperative groups ggml-hip names (M16, route B, step 3).
//
// Why it exists: llama.cpp's ggml-hip backend includes this header unconditionally
// (ggml/src/ggml-cuda/softmax.cu:6) and uses exactly three names of it: `cooperative_groups`,
// `cg::grid_group` with `cg::this_grid()`, and `g.sync()` (softmax.cu:155, :186, :224).
//
// What this part can promise, and what it cannot. A workgroup synchronises inside itself with
// one barrier instruction, and `thread_block::sync()` below is that barrier. A WHOLE GRID
// cannot: this route has one hardware queue, no cooperative dispatch packet and no reserved
// occupancy, so two workgroups of one grid are not guaranteed to be resident at the same time
// and a barrier between them would wait for a wave that has not started. hipLaunchCooperativeKernel
// therefore says no, and hipDeviceGetAttribute answers 0 for hipDeviceAttributeCooperativeLaunch.
// ggml-cuda reads that attribute at start-up and keeps its plain path, so the kernel that calls
// `grid_group::sync()` is compiled and never launched.
//
// `grid_group::sync()` is therefore written as the honest thing: it traps. A kernel that reaches
// it has been launched through a path that promised a guarantee this part does not give, and a
// trap at the barrier is a fault at the defect, not a hang or a wrong answer two kernels later.
// Nie obiecuj, czego nie mozesz dowiezc - do not promise what you cannot deliver.

#ifndef BC250_HIP_COOPERATIVE_GROUPS_H
#define BC250_HIP_COOPERATIVE_GROUPS_H

#include <hip/hip_runtime.h>

#if !defined(__cplusplus)
#error "hip_cooperative_groups.h is a C++ header"
#endif

namespace cooperative_groups {

class thread_group {
 public:
  __device__ unsigned int size() const { return __size; }
  __device__ unsigned int num_threads() const { return __size; }
  __device__ unsigned int thread_rank() const { return __rank; }
  __device__ bool is_valid() const { return __size != 0; }

 protected:
  __device__ thread_group(unsigned int size, unsigned int rank) : __size(size), __rank(rank) {}
  unsigned int __size;
  unsigned int __rank;
};

class thread_block : public thread_group {
 public:
  __device__ thread_block()
      : thread_group((unsigned int)blockDim.x * (unsigned int)blockDim.y *
                         (unsigned int)blockDim.z,
                     (unsigned int)threadIdx.x +
                         (unsigned int)blockDim.x *
                             ((unsigned int)threadIdx.y +
                              (unsigned int)blockDim.y * (unsigned int)threadIdx.z)) {}

  __device__ void sync() const { __syncthreads(); }
  __device__ dim3 group_index() const {
    dim3 r;
    r.x = (unsigned int)blockIdx.x;
    r.y = (unsigned int)blockIdx.y;
    r.z = (unsigned int)blockIdx.z;
    return r;
  }
  __device__ dim3 thread_index() const {
    dim3 r;
    r.x = (unsigned int)threadIdx.x;
    r.y = (unsigned int)threadIdx.y;
    r.z = (unsigned int)threadIdx.z;
    return r;
  }
};

class grid_group : public thread_group {
 public:
  __device__ grid_group()
      : thread_group((unsigned int)gridDim.x * (unsigned int)blockDim.x, 0) {}

  // See the header comment: this part gives no grid-wide residency guarantee, so there is no
  // barrier to stand here. A kernel that reaches it was launched through a path that must not
  // have been taken.
  __device__ void sync() const { __builtin_trap(); }
};

__device__ inline thread_block this_thread_block() { return thread_block(); }
__device__ inline grid_group this_grid() { return grid_group(); }

__device__ inline void sync(const thread_block &g) { g.sync(); }
__device__ inline void sync(const grid_group &g) { g.sync(); }

}  // namespace cooperative_groups

#endif  // BC250_HIP_COOPERATIVE_GROUPS_H
