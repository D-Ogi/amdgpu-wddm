/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#define BC250_GATHER_SLOTS 7u
#define VK_ERROR_DEVICE_LOST (-4)
#define NT_SUCCESS(x) ((x)>=0)
#define WDDM2_DISPATCH(x) x
#define p_atomic_read(p) (*(p))
struct vk_wddm2_fence { uint32_t handle; uint64_t wait_value; uint64_t* value_map; };
struct bc250_gather_slot { uint64_t retire_value; unsigned payload; };
struct radv_wddm2_queue {
    struct bc250_gather_slot bc250_gather[BC250_GATHER_SLOTS];
    unsigned bc250_gather_index,context_h;
    struct vk_wddm2_fence bc250_progress;
    bool bc250_submit_failed;
};
struct ws { bool bc250_trace_submits; unsigned device_h; };
struct radv_wddm2_ctx { struct ws* ws; };
struct submit { unsigned cs_count; };
typedef struct { unsigned ObjectCount; uint32_t* ObjectHandleArray; unsigned BroadcastContextCount;
    unsigned* BroadcastContextArray; uint64_t* MonitoredFenceValueArray; } D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2;
static unsigned checks,bad,waits,submits,payload=1;
static uint64_t observed_value,last_wait,last_signal;
static bool fail_signal;
#define CHECK(x) do { ++checks; if (!(x)) {++bad;printf("FAIL line %u: %s\n",__LINE__,#x);} }while(0)
static bool vk_wddm2_fence_wait(unsigned device,struct vk_wddm2_fence* f)
{
    (void)device;
    if(observed_value>=f->wait_value)return true;
    ++waits; last_wait=f->wait_value; observed_value=f->wait_value;
    return true;
}
static int radv_wddm2_bc250_submit(struct radv_wddm2_ctx* c,struct radv_wddm2_queue* q,const struct submit* s)
{
    struct bc250_gather_slot* slot=&q->bc250_gather[q->bc250_gather_index];
    (void)c;(void)s;
    CHECK(observed_value>=slot->retire_value);
    slot->payload=payload++; ++submits;
    return 0;
}
static int SignalSynchronizationObjectFromGpu2(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2* s)
{ last_signal=*s->MonitoredFenceValueArray; return fail_signal ? -1:0; }
static int actual_submit(struct radv_wddm2_ctx* ctx,struct radv_wddm2_queue* queue,const struct submit* submit)
{
    unsigned wait_count=0,signal_count=0;
    int status;
#include "gather_actual.inc"
    return 0;
}
int main(void)
{
    struct ws ws={0}; struct radv_wddm2_ctx ctx={&ws}; struct radv_wddm2_queue q={0};
    struct submit s={2}; unsigned i,j,prior;
    q.bc250_progress.value_map=&observed_value;
    for(i=0;i<7;i++) CHECK(actual_submit(&ctx,&q,&s)==0);
    CHECK(waits==0 && submits==7 && observed_value==0 && last_signal==7);
    for(j=0;j<7;j++)CHECK(q.bc250_gather[j].payload==j+1);
    CHECK(actual_submit(&ctx,&q,&s)==0);
    CHECK(waits==1 && last_wait==1 && observed_value==1 && q.bc250_gather[0].payload==8);
    for(j=1;j<7;j++)CHECK(q.bc250_gather[j].payload==j+1);
    for(i=0;i<1000;i++) CHECK(actual_submit(&ctx,&q,&s)==0);
    CHECK(last_wait==last_signal-7);
    // Failure after an accepted IB must poison the queue before any later slot reuse.
    fail_signal=true; CHECK(actual_submit(&ctx,&q,&s)==VK_ERROR_DEVICE_LOST);
    prior=submits; CHECK(q.bc250_submit_failed);
    CHECK(actual_submit(&ctx,&q,&s)==VK_ERROR_DEVICE_LOST && submits==prior);
    printf("RADV actual gather selection/retirement: %u checks, %u failures\n",checks,bad);
    return bad ? 1:0;
}
