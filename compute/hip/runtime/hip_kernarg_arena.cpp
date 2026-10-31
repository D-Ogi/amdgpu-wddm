// hip_kernarg_arena.cpp - the dense kernel argument arena, as offsets only.
//
// The pool of hip_launch.cpp gives each launch a buffer of its own, one page at the least, so
// the 32 dispatches of a batch read their arguments from 32 different pages. The ROCm runtime
// does not: its kernel argument ring (ManagedBuffer, ROCm/clr rocvirtual.cpp at fc23bef4) packs
// the arguments of consecutive dispatches side by side. This arena does the same, so that the
// arguments of a whole batch lie in a few pages. That this is faster on unit A is a
// HYPOTHESIS until a lab arm measures it (BC250_HIP_KERNARG_ARENA=0 is that arm's control).
//
// What keeps the arguments correct is unchanged from the pool, and this file does not depend
// on more:
//   1. The host processor writes a slice before the submission that reads it: hipLaunchKernel
//      packs, calls bc250hsa_write_barrier, and only then submits.
//   2. Every indirect buffer starts with the full cache acquire of bc250hsa_pm4_ib_head
//      (bc250hsa/pm4_dispatch.c), which invalidates GL2, GL1, the vector caches and the scalar
//      cache, so a submission reads what the host processor wrote before it.
//   3. A block is written again only when no launch holds a slice of it and the highest fence
//      value of its slices has retired, which is the rule the pool applies to one buffer.
// Slices are aligned to a GL2 line (128 bytes), so no two launches share a line.
//
// The functions here are pure and run under the process lock; tests/host/test_hip_kernarg_arena
// checks them with no device.

#include "runtime_internal.h"

namespace bc250hip {

namespace {

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool power_of_two(uint32_t value) { return value != 0u && (value & (value - 1u)) == 0u; }

}  // namespace

ArenaTake arena_take(KernargArena& arena, uint32_t bytes, uint32_t alignment, uint64_t retired,
                     uint32_t* offset_out, uint64_t* wait_for) {
    using A = KernargArena;
    if (alignment == 0u) {
        alignment = 16u;
    }
    if (!power_of_two(alignment) || alignment > A::kBlockBytes || bytes > A::kMaxSlice) {
        return ArenaTake::kFallback;
    }
    if (alignment < A::kGranule) {
        alignment = A::kGranule;
    }
    // A kernel with no arguments still gets a slice, as it gets a buffer of the pool: the
    // dispatch packet and the hidden arguments of the next launch must not land on its line.
    const uint32_t size = align_up(bytes == 0u ? 1u : bytes, A::kGranule);

    const uint32_t start = align_up(arena.offset, alignment);
    if (start <= A::kBlockBytes && size <= A::kBlockBytes - start) {
        arena.blocks[arena.current].takers++;
        arena.offset = start + size;
        *offset_out = arena.current * A::kBlockBytes + start;
        return ArenaTake::kTaken;
    }

    // The open block is full. The next block may still be read by the GPU, held by a launch
    // that has not settled, or claimed by a thread that waits for it.
    const uint32_t next = (arena.current + 1u) % A::kBlocks;
    A::Block& block = arena.blocks[next];
    if (block.takers != 0u || block.claimed) {
        return ArenaTake::kFallback;
    }
    if (block.fence > retired) {
        block.claimed = true;
        *wait_for = block.fence;
        *offset_out = next * A::kBlockBytes;
        return ArenaTake::kWait;
    }
    block.fence = 0;
    arena.current = next;
    arena.offset = size;
    block.takers = 1u;
    *offset_out = next * A::kBlockBytes;
    return ArenaTake::kTaken;
}

void arena_unclaim(KernargArena& arena, uint32_t block_offset) {
    const uint32_t index = block_offset / KernargArena::kBlockBytes;
    if (index < KernargArena::kBlocks) {
        arena.blocks[index].claimed = false;
    }
}

void arena_settle(KernargArena& arena, uint32_t offset, uint64_t fence) {
    const uint32_t index = offset / KernargArena::kBlockBytes;
    if (index >= KernargArena::kBlocks) {
        return;
    }
    KernargArena::Block& block = arena.blocks[index];
    if (fence > block.fence) {
        block.fence = fence;
    }
    if (block.takers != 0u) {
        block.takers--;
    }
}

}  // namespace bc250hip
