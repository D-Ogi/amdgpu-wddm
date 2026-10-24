/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <string.h>
#include "amdgpu.h"
#include "bc250_fence_order.h"
static unsigned int bad, checks, bells;
#define CHECK(x) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)
void bc250_shim_log(int level, void *dev, const char *fmt, ...) { (void)level; (void)dev; (void)fmt; }
void bc250_shim_wdoorbell64(struct amdgpu_device *a, unsigned int i, unsigned long long v)
{ (void)a; (void)i; (void)v; ++bells; }
int main(void)
{
    struct amdgpu_device adev = {0};
    struct amdgpu_ring_funcs funcs = {AMDGPU_RING_TYPE_GFX,255,0x80000000};
    struct amdgpu_ring ring = {0};
    u32 memory[2048], before[2048];
    volatile u32 read = 0;
    u64 writeback = 0;
    unsigned int i, lap;
    ring.adev=&adev; ring.funcs=&funcs; ring.ring=memory; ring.ring_size=sizeof(memory);
    ring.max_dw=2048; ring.buf_mask=2047; ring.ptr_mask=~(u64)0;
    ring.rptr_cpu_addr=&read; ring.wptr_cpu_addr=&writeback;
    ring.track_rptr=true; ring.use_doorbell=true;
    memset(memory,0,sizeof(memory));
    for (i=0; i<7; ++i) {
        CHECK(amdgpu_ring_alloc(&ring,25)==0);
        amdgpu_ring_write(&ring,0x100+i); amdgpu_ring_commit(&ring);
    }
    CHECK(ring.wptr==1792 && writeback==1792 && bells==7);
    memcpy(before,memory,sizeof(memory));
    CHECK(amdgpu_ring_alloc(&ring,25)!=0);
    CHECK(ring.wptr==1792 && !memcmp(before,memory,sizeof(memory)) && bells==7);
    read=256;
    CHECK(amdgpu_ring_alloc(&ring,25)==0);
    amdgpu_ring_write(&ring,0x200); amdgpu_ring_commit(&ring);
    CHECK(ring.wptr==2048 && memory[0]==0x100 && memory[1792]==0x200);
    CHECK(amdgpu_ring_alloc(&ring,25)!=0);
    read=512;
    CHECK(amdgpu_ring_alloc(&ring,25)==0);
    amdgpu_ring_write(&ring,0x300); amdgpu_ring_commit(&ring);
    CHECK(ring.wptr==2304 && memory[0]==0x300 && memory[256]==0x101);
    // Padding, a two-slot request, and both 32-bit and ring-index wrap.
    read=(u32)ring.wptr;
    CHECK(amdgpu_ring_alloc(&ring,257)==0 && ring.count_dw==512);
    amdgpu_ring_undo(&ring);
    ring.wptr=0xffffff00ull; read=0xffffff00u;
    for (lap=0; lap<10000; ++lap) {
        CHECK(amdgpu_ring_alloc(&ring,25)==0);
        amdgpu_ring_write(&ring,lap); amdgpu_ring_commit(&ring);
        read=(u32)ring.wptr;
    }
    CHECK(ring.wptr>0x100000000ull);
    // A consumer that lags while the producer crosses 2^32 (the lap loop above lets the reader catch up
    // after every submit). The reader stays at 0xfffffe00: seven slots and no more, the eighth is refused
    // with the ring and the memory as they were, and one retired slot admits the next job. The writes
    // start at ring index 1536 and wrap the index after the second job.
    ring.wptr=0xfffffe00ull; read=0xfffffe00u;
    for (i=0; i<7; ++i) {
        CHECK(amdgpu_ring_alloc(&ring,25)==0);
        amdgpu_ring_write(&ring,0x400+i); amdgpu_ring_commit(&ring);
    }
    CHECK(ring.wptr==0x100000500ull && writeback==0x100000500ull);
    CHECK(memory[1536]==0x400 && memory[1792]==0x401 && memory[0]==0x402 && memory[1024]==0x406);
    memcpy(before,memory,sizeof(memory));
    CHECK(amdgpu_ring_alloc(&ring,25)!=0);
    CHECK(ring.wptr==0x100000500ull && !memcmp(before,memory,sizeof(memory)));
    read+=256u;
    CHECK(amdgpu_ring_alloc(&ring,25)==0);
    amdgpu_ring_undo(&ring);
    CHECK(ring.wptr==0x100000500ull);
    // The reader crosses 2^32 as well and catches up: seven slots are free again, and an eighth never fits
    // beside the guard dword.
    read=0x00000500u;
    CHECK(bc250_ring_has_space(&ring,1792) && !bc250_ring_has_space(&ring,1793));
    CHECK(amdgpu_ring_alloc(&ring,~0u)!=0);
    CHECK(!bc250_ring_has_space(&ring,2048)); // guard dword, even when empty
    CHECK(bc250_fence_reached(2,1)); CHECK(bc250_fence_reached(2,2));
    CHECK(!bc250_fence_reached(1,2)); CHECK(!bc250_fence_reached(0,1));
    CHECK(bc250_fence_reached(1,0xffffffffu));
    CHECK(!bc250_fence_reached(0xffffffffu,1));
    {
        // BD-235: no reservation straddles a multiple of 2^30 dwords (ring byte 2^32). The paging submission of
        // arm-ringstart2 boot a: 963 dwords from dword 0x3FFFFED0 get 304 NOPs (ring index 0x6D0-0x7FF) first and
        // start at 0x40000000; the reservation itself is unchanged.
        struct amdgpu_ring_funcs sdma = {AMDGPU_RING_TYPE_SDMA,15,0};
        ring.funcs=&sdma; ring.track_rptr=false; ring.max_dw=1024;
        memset(memory,0x5a,sizeof(memory));
        ring.wptr=0x3ffffed0ull;
        CHECK(bc250_ring_boundary_pad_size(&ring,963)==304);
        CHECK(amdgpu_ring_alloc(&ring,963)==0 && ring.wptr==0x40000000ull && ring.wptr_old==0x40000000ull &&
              ring.count_dw==976 && memory[0x6d0]==0 && memory[0x7ff]==0 && memory[0x6cf]==0x5a5a5a5au &&
              memory[0]==0x5a5a5a5au);
        // A reservation that ends exactly at the boundary, and the lab control with the rule off: no pad.
        ring.wptr=0x40000000ull-16;
        CHECK(bc250_ring_boundary_pad_size(&ring,16)==0 && amdgpu_ring_alloc(&ring,16)==0 && ring.wptr==0x40000000ull-16);
        ring.wptr=0x3ffffed0ull; bc250_ring_boundary_pad=0;
        CHECK(amdgpu_ring_alloc(&ring,963)==0 && ring.wptr==0x3ffffed0ull);
        bc250_ring_boundary_pad=1;
        // A CP ring (256-dword slots) with a two-slot request across 2^32 dwords: one slot of NOPs, then the request.
        // With the read pointer tracked, the pad and the request need room together; the refusal changes nothing.
        ring.funcs=&funcs; ring.track_rptr=true; ring.max_dw=2048;
        ring.wptr=0xffffff00ull; read=0xffffff00u;
        CHECK(bc250_ring_boundary_pad_size(&ring,257)==256);
        CHECK(amdgpu_ring_alloc(&ring,257)==0 && ring.wptr==0x100000000ull && ring.count_dw==512);
        ring.wptr=0xffffff00ull; read=0xfffffb00u;   // 1024 dwords in flight: 1023 free, the pad and 512 fit
        CHECK(amdgpu_ring_alloc(&ring,257)==0 && ring.wptr==0x100000000ull);
        ring.wptr=0xffffff00ull; read=0xfffff900u;   // 1536 in flight: 511 free, the pad and 512 do not fit
        memcpy(before,memory,sizeof(memory));
        CHECK(amdgpu_ring_alloc(&ring,257)!=0 && ring.wptr==0xffffff00ull && !memcmp(before,memory,sizeof(memory)));
    }
    printf("GFX ring capacity/fence order: %u checks, %u failures\n", checks, bad);
    return bad ? 1:0;
}
