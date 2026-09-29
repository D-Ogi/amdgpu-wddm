/* Host control of the actual IH Consume body. Hardware delivery is modeled. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
typedef unsigned long ULONG;
typedef unsigned int u32;
typedef bool BOOLEAN;
#define TRUE true
#define FALSE false
#ifndef _Inout_
#define _Inout_
#endif
#define NT_SUCCESS(x) ((x)>=0)
#define BC250_IH_DPC_ROUNDS 4
struct amdgpu_device { struct { struct { unsigned ring_size,rptr; } ih; } irq; };
struct bc250_iv_entry { unsigned client_id,src_id; };
#define BC250_IH_DCE_CLIENT 4u
#define BC250_IH_OTG0_VUPDATE_SOURCE 0x57u
typedef struct { unsigned OverflowCount, DecodeErrors, Rptr, count; } Stats;
typedef struct { struct amdgpu_device *DpcAdev; struct { long Fault; } DpcSequence; long Active, InDpc, DpcAgain, DpcCount, VsyncPending; unsigned Rptr; int StatsLock; Stats Stats; } BC250_IH;
typedef struct { void *Ih; struct { void (*DxgkCbQueueDpc)(void *); void *DeviceHandle; } Dxgk; } BC250_DEVICE;
static BC250_IH fixture;
static unsigned wp, gets, pubs, decodes, queued, fault_read, overflow_read, misalign_read, vector_client, vector_source;
static void queue_dpc(void *p) { (void)p;queued++; }
static long InterlockedIncrement(long *p) { return ++*p; }
static long InterlockedCompareExchange(long *p,long v,long expected) { long old=*p;if(old==expected)*p=v;return old; }
static int armed, irq_pending, inject, bad_decode, streaming, overflow_mode, preinject, level;
static long InterlockedExchange(long *p,long v) { long old=*p;*p=v;return old; }
static void KeAcquireSpinLockAtDpcLevel(int *p) { if(*p) abort();*p=1; }
static void KeReleaseSpinLockFromDpcLevel(int *p) { if(!*p) abort();*p=0; }
static u32 bc250_ih_get_wptr(struct amdgpu_device *a,bool *overflow) {
    u32 result;
    gets++;
    if(gets==fault_read) fixture.DpcSequence.Fault=-1;
    *overflow=overflow_mode!=0 || gets==overflow_read;
    if(*overflow) a->irq.ih.rptr=96;
    result=(gets==misalign_read)?1:wp;
    if(preinject){preinject=0;wp=32;}
    return result;
}
static int bc250_ih_decode(struct amdgpu_device *a,u32 *r,struct bc250_iv_entry *e) { e->client_id=vector_client;e->src_id=vector_source;decodes++;if(bad_decode)return -1;*r=(*r+32)%a->irq.ih.ring_size;return 0; }
static void Note(Stats *s,const struct bc250_iv_entry *e) { (void)e;s->count++; }
/* hang.c's progress recorders: interlocked stores with no effect on the consumer's control flow. */
typedef long LONG;
static unsigned progress_passes,progress_requeues;
#define ProgressEnter(site) ((void)0)
#define ProgressExit(site,value) ((void)(value))
#define ProgressIhDone(passes,requeued) (progress_passes+=(passes),progress_requeues+=(requeued)!=0)
static void bc250_ih_set_rptr(struct amdgpu_device *a,u32 r) {
    a->irq.ih.rptr=r;pubs++;armed=1;if(level && wp!=r){irq_pending=1;armed=0;}if(streaming)wp=(r+32)%a->irq.ih.ring_size;
    if(inject) { inject=0;wp=32;irq_pending=armed;armed=0; }
}
#include "ih_consume_actual.inc"
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n",__LINE__,#c); return 1; } } while(0)
static struct amdgpu_device dev;
static void reset(void) { memset(&dev,0,sizeof(dev));memset(&fixture,0,sizeof(fixture));dev.irq.ih.ring_size=128;fixture.DpcAdev=&dev;fixture.Active=1;wp=gets=pubs=decodes=queued=fault_read=overflow_read=misalign_read=vector_client=vector_source=0;armed=irq_pending=inject=bad_decode=streaming=overflow_mode=preinject=level=0; }
int main(void) {
    BC250_DEVICE device;
    device.Ih=&fixture;device.Dxgk.DxgkCbQueueDpc=queue_dpc;device.Dxgk.DeviceHandle=NULL;
    reset();Consume(&fixture);CHECK(pubs==1 && armed && decodes==0 && gets<=2);
    reset();inject=1;Consume(&fixture);CHECK((irq_pending || fixture.Stats.count==1) && pubs>=1);irq_pending=0;Consume(&fixture);CHECK(fixture.Stats.count==1 && fixture.Rptr==32 && armed);
    reset();wp=64;Consume(&fixture);CHECK(decodes==2 && fixture.Rptr==64 && armed && gets<=8);
    reset();fixture.Rptr=96;wp=32;Consume(&fixture);CHECK(decodes==2 && fixture.Rptr==32 && armed);
    reset();fixture.DpcSequence.Fault=-1;Consume(&fixture);CHECK(!fixture.Active && pubs==0 && decodes==0);
    reset();wp=1;Consume(&fixture);CHECK(!fixture.Active && pubs==0 && decodes==0);
    reset();fixture.Active=0;Consume(&fixture);CHECK(pubs==0);
    reset();wp=32;bad_decode=1;Consume(&fixture);CHECK(fixture.Stats.DecodeErrors==1 && gets<=8);
    reset();streaming=1;wp=32;CHECK(Consume(&fixture));CHECK(decodes==4 && gets<=8 && pubs==4);
    reset();overflow_mode=1;wp=32;Consume(&fixture);CHECK(fixture.Stats.OverflowCount>0 && decodes<=4 && gets<=8);
    reset();preinject=1;Consume(&fixture);CHECK(fixture.Stats.count==1 || irq_pending);
    reset();preinject=1;level=1;Consume(&fixture);CHECK(fixture.Stats.count==1 || irq_pending);
    reset();streaming=1;wp=32;IhDpc(&device);CHECK(queued==1 && decodes==4 && fixture.InDpc==0);
    CHECK(progress_passes==1 && progress_requeues==1);   /* the progress exit sees the budget yield */
    reset();IhDpc(&device);CHECK(queued==0 && pubs==1 && fixture.InDpc==0);
    CHECK(progress_passes==2 && progress_requeues==1);   /* and the ordinary return, without a yield */
    reset();fault_read=2;IhDpc(&device);CHECK(!fixture.Active && pubs==1 && queued==0 && fixture.Stats.DecodeErrors==1);
    reset();misalign_read=2;IhDpc(&device);CHECK(!fixture.Active && pubs==1 && queued==0 && fixture.Stats.DecodeErrors==1);
    reset();overflow_read=2;wp=32;Consume(&fixture);CHECK(fixture.Stats.OverflowCount==1 && fixture.Rptr==32 && fixture.Stats.count==3);
    reset();wp=32;vector_client=4;vector_source=0x57;Consume(&fixture);CHECK(IhTakeVsync(&device));CHECK(!IhTakeVsync(&device));
    reset();wp=32;vector_client=8;vector_source=0x57;Consume(&fixture);CHECK(!IhTakeVsync(&device));
    reset();wp=32;vector_client=4;vector_source=0x56;Consume(&fixture);CHECK(!IhTakeVsync(&device));
    reset();fixture.VsyncPending=1;fixture.Active=0;CHECK(!IhTakeVsync(&device));CHECK(fixture.VsyncPending==0);
    puts("PASS: empty rearm, late arrival, nonempty, wrap, fault, alignment, inactive, decode error, budget, overflow, prepublish edge, prepublish level");return 0;
}
