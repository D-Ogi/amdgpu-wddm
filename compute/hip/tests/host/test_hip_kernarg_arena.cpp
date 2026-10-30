// test_hip_kernarg_arena.cpp - the kernel argument arena of hip_kernarg_arena.cpp, with no device.
//
// The three rules of that file are checked one by one: slices of consecutive launches lie side
// by side on GL2 lines of their own, a block is not written again while a launch holds a slice
// of it, and a block whose fence value has not retired is waited for by one thread only.
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

using bc250hip::ArenaTake;
using bc250hip::KernargArena;

static int take(KernargArena& arena, uint32_t bytes, uint32_t alignment, uint64_t retired,
                uint32_t* offset, uint64_t* wait_for = nullptr) {
    uint64_t unused = 0;
    return static_cast<int>(bc250hip::arena_take(arena, bytes, alignment, retired, offset,
                                                 wait_for != nullptr ? wait_for : &unused));
}

static const int kTaken = static_cast<int>(ArenaTake::kTaken);
static const int kWait = static_cast<int>(ArenaTake::kWait);
static const int kFallback = static_cast<int>(ArenaTake::kFallback);

// Takes 4096-byte slices until the open block is the one after `block`, and settles every
// slice of `block` with `fence`. Returns the offset of the first slice in the next block.
static uint32_t fill_and_settle(KernargArena& arena, uint32_t block, uint64_t fence,
                                uint64_t retired) {
    uint32_t offset = 0;
    for (;;) {
        CHECK_INT(take(arena, 4096u, 16u, retired, &offset), kTaken);
        if (offset / KernargArena::kBlockBytes != block) {
            break;
        }
        bc250hip::arena_settle(arena, offset, fence);
    }
    return offset;
}

int main() {
    using A = KernargArena;
    uint32_t offset = 0;
    uint64_t wait_for = 0;

    // 1. Side by side. The vadd fixture of test_hip_mock packs 288 bytes at 64-byte alignment:
    //    the slice takes three lines, and the next launch starts on the fourth.
    {
        KernargArena arena;
        CHECK_INT(take(arena, 288u, 64u, 0u, &offset), kTaken);
        CHECK_INT(offset, 0);
        CHECK_INT(take(arena, 288u, 64u, 0u, &offset), kTaken);
        CHECK_INT(offset, 384);
        CHECK_INT(take(arena, 100u, 256u, 0u, &offset), kTaken);   // 768 is 256-aligned
        CHECK_INT(offset, 768);
        CHECK_INT(take(arena, 0u, 0u, 0u, &offset), kTaken);       // no arguments: one line
        CHECK_INT(offset, 896);
        CHECK_INT(take(arena, 16u, 0u, 0u, &offset), kTaken);
        CHECK_INT(offset, 1024);
        CHECK_INT(arena.blocks[0].takers, 5);
    }

    // 2. What the arena leaves to the pool.
    {
        KernargArena arena;
        CHECK_INT(take(arena, A::kMaxSlice + 1u, 16u, 0u, &offset), kFallback);
        CHECK_INT(take(arena, 64u, 48u, 0u, &offset), kFallback);   // not a power of two
        CHECK_INT(take(arena, A::kMaxSlice, 16u, 0u, &offset), kTaken);
    }

    // 3. A full block opens the next one, and its slices count there.
    {
        KernargArena arena;
        const uint32_t first_of_next = fill_and_settle(arena, 0u, 7u, 0u);
        CHECK_INT(first_of_next, A::kBlockBytes);
        CHECK_INT(arena.current, 1);
        CHECK_INT(arena.blocks[0].takers, 0);
        CHECK_INT(arena.blocks[0].fence, 7);
        CHECK_INT(arena.blocks[1].takers, 1);
    }

    // 4. The wrap. Every block holds slices of fence 10 + block. Coming back to block 0 with
    //    only 5 retired waits for 10, and the block is claimed: a second thread gets the pool.
    //    After the wait the take opens block 0 and clears its fence value.
    {
        KernargArena arena;
        for (uint32_t block = 0; block < A::kBlocks; ++block) {
            const uint64_t fence = 10u + block;
            if (block + 1u < A::kBlocks) {
                offset = fill_and_settle(arena, block, fence, 0u);
                // The first slice of the next block is open; settle it with the next fence.
                bc250hip::arena_settle(arena, offset, fence + 1u);
            }
        }
        // Block 15 is open with one settled slice. Fill the rest of it.
        for (;;) {
            const int answer = take(arena, 4096u, 16u, 5u, &offset, &wait_for);
            if (answer != kTaken) {
                CHECK_INT(answer, kWait);
                break;
            }
            bc250hip::arena_settle(arena, offset, 25u);
        }
        CHECK_INT(wait_for, 10);
        CHECK_INT(offset, 0);
        CHECK_INT(arena.blocks[0].claimed, 1);
        uint32_t other = 0;
        CHECK_INT(take(arena, 4096u, 16u, 5u, &other), kFallback);   // claimed by the waiter
        bc250hip::arena_unclaim(arena, offset);
        CHECK_INT(arena.blocks[0].claimed, 0);
        CHECK_INT(take(arena, 4096u, 16u, 10u, &offset), kTaken);    // 10 has retired now
        CHECK_INT(offset, 0);
        CHECK_INT(arena.current, 0);
        CHECK_INT(arena.blocks[0].fence, 0);
        CHECK_INT(arena.blocks[0].takers, 1);
    }

    // 5. A block that a launch still holds is never opened again, whatever has retired: the
    //    launch may be writing its arguments with the lock released.
    {
        KernargArena arena;
        CHECK_INT(take(arena, 4096u, 16u, 0u, &offset), kTaken);
        const uint32_t held = offset;   // never settled until the end
        uint32_t last = 0;
        for (uint32_t block = 0; block < A::kBlocks; ++block) {
            for (;;) {
                const int answer = take(arena, 4096u, 16u, 1000u, &last);
                if (answer != kTaken) {
                    CHECK_INT(answer, kFallback);
                    break;
                }
                if (last != held) {
                    bc250hip::arena_settle(arena, last, 1u);
                }
            }
            if (arena.current == A::kBlocks - 1u) {
                break;
            }
        }
        CHECK_INT(arena.current, A::kBlocks - 1u);
        CHECK_INT(take(arena, 4096u, 16u, 1000u, &offset), kFallback);
        bc250hip::arena_settle(arena, held, 1u);
        CHECK_INT(take(arena, 4096u, 16u, 1000u, &offset), kTaken);
        CHECK_INT(offset, 0);
    }

    // 6. A failed launch settles with 0: the slice is free, and the fence value stays.
    {
        KernargArena arena;
        CHECK_INT(take(arena, 64u, 16u, 0u, &offset), kTaken);
        bc250hip::arena_settle(arena, offset, 3u);
        CHECK_INT(take(arena, 64u, 16u, 0u, &offset), kTaken);
        bc250hip::arena_settle(arena, offset, 0u);
        CHECK_INT(arena.blocks[0].takers, 0);
        CHECK_INT(arena.blocks[0].fence, 3);
    }

    std::printf("test_hip_kernarg_arena: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
