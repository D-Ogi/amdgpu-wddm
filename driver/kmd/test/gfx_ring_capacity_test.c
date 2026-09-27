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
    CHECK(amdgpu_ring_alloc(&ring,~0u)!=0);
    CHECK(!bc250_ring_has_space(&ring,2048)); // guard dword, even when empty
    CHECK(bc250_fence_reached(2,1)); CHECK(bc250_fence_reached(2,2));
    CHECK(!bc250_fence_reached(1,2)); CHECK(!bc250_fence_reached(0,1));
    CHECK(bc250_fence_reached(1,0xffffffffu));
    CHECK(!bc250_fence_reached(0xffffffffu,1));
    printf("GFX ring capacity/fence order: %u checks, %u failures\n", checks, bad);
    return bad ? 1:0;
}
