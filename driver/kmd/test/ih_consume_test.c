/* Host control of the actual IH Consume body. Hardware delivery is modeled. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef long NTSTATUS;
typedef unsigned int u32;
typedef bool BOOLEAN;
#define TRUE true
#define FALSE false
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _In_
#define _In_
#endif
#define NT_SUCCESS(x) ((x)>=0)
#define STATUS_SUCCESS 0
#define BC250_IH_DPC_ROUNDS 4
struct amdgpu_device { struct { struct { unsigned ring_size,rptr; } ih; } irq; };
/* KMD193: the fault fields the consumer now reads out of a vector. */
struct bc250_iv_entry { unsigned client_id,src_id,ring_id,vmid; unsigned src_data[4]; };
#define BC250_IH_DCE_CLIENT 4u
#define BC250_IH_OTG0_VUPDATE_SOURCE 0x57u
#include "ih_fault.h"
typedef struct { unsigned OverflowCount, DecodeErrors, Rptr, count; } Stats;
typedef struct { struct amdgpu_device *DpcAdev; struct { long Fault; } DpcSequence; long Active, InDpc, DpcAgain, DpcCount, VsyncPending; unsigned Rptr; int StatsLock; Stats Stats; BC250_IH_FAULT_STATE Faults; } BC250_IH;
typedef struct { void *Ih; struct { void (*DxgkCbQueueDpc)(void *); void *DeviceHandle; } Dxgk; } BC250_DEVICE;
static BC250_IH fixture;
static unsigned wp, gets, pubs, decodes, queued, fault_read, overflow_read, misalign_read, vector_client, vector_source;
/* KMD193 fixture: the vector's fault payload, the modeled clock, and what the reporter saw. */
static unsigned vector_vmid, vector_ring, vector_src0, vector_src1;
static ULONGLONG mock_interrupt_time;
static unsigned fault_reports, fault_register_reads;
static BC250_IH_FAULT_STATE last_report;
static void queue_dpc(void *p) { (void)p;queued++; }
static long InterlockedIncrement(long *p) { return ++*p; }
static long InterlockedCompareExchange(long *p,long v,long expected) { long old=*p;if(old==expected)*p=v;return old; }
static int armed, irq_pending, inject, bad_decode, streaming, overflow_mode, preinject, level;
static long InterlockedExchange(long *p,long v) { long old=*p;*p=v;return old; }
static void KeAcquireSpinLockAtDpcLevel(int *p) { if(*p) abort();*p=1; }
static void KeReleaseSpinLockFromDpcLevel(int *p) { if(!*p) abort();*p=0; }
static ULONGLONG KeQueryInterruptTime(void) { return mock_interrupt_time; }
/* The real IhFaultReport sits above Consume in ih.c and is not part of the extracted body: modeled here, and
 * the model asserts the one property that matters - the stats lock is NOT held when it runs. */
static void IhFaultReport(const BC250_DEVICE *device,const BC250_IH_FAULT_STATE *fault)
{
    (void)device;
    if(fixture.StatsLock) abort();
    fault_reports++;
    fault_register_reads+=3;
    last_report=*fault;
}
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
static int bc250_ih_decode(struct amdgpu_device *a,u32 *r,struct bc250_iv_entry *e) { e->client_id=vector_client;e->src_id=vector_source;e->ring_id=vector_ring;e->vmid=vector_vmid;e->src_data[0]=vector_src0;e->src_data[1]=vector_src1;e->src_data[2]=0;e->src_data[3]=0;decodes++;if(bad_decode)return -1;*r=(*r+32)%a->irq.ih.ring_size;return 0; }
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
static void reset(void) { memset(&dev,0,sizeof(dev));memset(&fixture,0,sizeof(fixture));memset(&last_report,0,sizeof(last_report));dev.irq.ih.ring_size=128;fixture.DpcAdev=&dev;fixture.Active=1;wp=gets=pubs=decodes=queued=fault_read=overflow_read=misalign_read=vector_client=vector_source=0;armed=irq_pending=inject=bad_decode=streaming=overflow_mode=preinject=level=0;vector_vmid=vector_ring=vector_src0=vector_src1=0;fault_reports=fault_register_reads=0;mock_interrupt_time=0; }
int main(void) {
    BC250_DEVICE device;
    device.Ih=&fixture;device.Dxgk.DxgkCbQueueDpc=queue_dpc;device.Dxgk.DeviceHandle=NULL;
    reset();Consume(&device,&fixture);CHECK(pubs==1 && armed && decodes==0 && gets<=2);
    reset();inject=1;Consume(&device,&fixture);CHECK((irq_pending || fixture.Stats.count==1) && pubs>=1);irq_pending=0;Consume(&device,&fixture);CHECK(fixture.Stats.count==1 && fixture.Rptr==32 && armed);
    reset();wp=64;Consume(&device,&fixture);CHECK(decodes==2 && fixture.Rptr==64 && armed && gets<=8);
    reset();fixture.Rptr=96;wp=32;Consume(&device,&fixture);CHECK(decodes==2 && fixture.Rptr==32 && armed);
    reset();fixture.DpcSequence.Fault=-1;Consume(&device,&fixture);CHECK(!fixture.Active && pubs==0 && decodes==0);
    reset();wp=1;Consume(&device,&fixture);CHECK(!fixture.Active && pubs==0 && decodes==0);
    reset();fixture.Active=0;Consume(&device,&fixture);CHECK(pubs==0);
    reset();wp=32;bad_decode=1;Consume(&device,&fixture);CHECK(fixture.Stats.DecodeErrors==1 && gets<=8);
    reset();streaming=1;wp=32;CHECK(Consume(&device,&fixture));CHECK(decodes==4 && gets<=8 && pubs==4);
    reset();overflow_mode=1;wp=32;Consume(&device,&fixture);CHECK(fixture.Stats.OverflowCount>0 && decodes<=4 && gets<=8);
    reset();preinject=1;Consume(&device,&fixture);CHECK(fixture.Stats.count==1 || irq_pending);
    reset();preinject=1;level=1;Consume(&device,&fixture);CHECK(fixture.Stats.count==1 || irq_pending);
    reset();streaming=1;wp=32;IhDpc(&device);CHECK(queued==1 && decodes==4 && fixture.InDpc==0);
    CHECK(progress_passes==1 && progress_requeues==1);   /* the progress exit sees the budget yield */
    reset();IhDpc(&device);CHECK(queued==0 && pubs==1 && fixture.InDpc==0);
    CHECK(progress_passes==2 && progress_requeues==1);   /* and the ordinary return, without a yield */
    reset();fault_read=2;IhDpc(&device);CHECK(!fixture.Active && pubs==1 && queued==0 && fixture.Stats.DecodeErrors==1);
    reset();misalign_read=2;IhDpc(&device);CHECK(!fixture.Active && pubs==1 && queued==0 && fixture.Stats.DecodeErrors==1);
    reset();overflow_read=2;wp=32;Consume(&device,&fixture);CHECK(fixture.Stats.OverflowCount==1 && fixture.Rptr==32 && fixture.Stats.count==3);
    reset();wp=32;vector_client=4;vector_source=0x57;Consume(&device,&fixture);CHECK(IhTakeVsync(&device));CHECK(!IhTakeVsync(&device));
    reset();wp=32;vector_client=8;vector_source=0x57;Consume(&device,&fixture);CHECK(!IhTakeVsync(&device));
    reset();wp=32;vector_client=4;vector_source=0x56;Consume(&device,&fixture);CHECK(!IhTakeVsync(&device));
    reset();fixture.VsyncPending=1;fixture.Active=0;CHECK(!IhTakeVsync(&device));CHECK(fixture.VsyncPending==0);
    /* KMD193, trial 245's shape: a UTCL2 burst on one page. One report and one latch read for the whole
     * second, whatever the vector count, and the report carries the decoded page, VMID and direction. */
    reset();streaming=1;wp=32;vector_client=BC250_IH_UTCL2_CLIENT;vector_source=0x0;vector_vmid=1;vector_ring=0xDE;
    vector_src0=0x11BC40;vector_src1=0x50;mock_interrupt_time=20841649000ull;
    Consume(&device,&fixture);
    CHECK(fault_reports==1 && fault_register_reads==3);
    CHECK(fixture.Faults.Total==4 && fixture.Faults.InSecond==4 && fixture.Faults.Bursts==1);
    CHECK(last_report.FirstVa==0x11BC40000ull && last_report.VmId==1 && last_report.RingId==0xDE);
    CHECK(!last_report.Write && !last_report.Retry && last_report.PreviousInSecond==0);
    /* The next second opens a new bucket: one more report, carrying the previous second's count. */
    mock_interrupt_time+=10000000ull;
    Consume(&device,&fixture);
    CHECK(fault_reports==2 && fixture.Faults.Bursts==2 && last_report.PreviousInSecond==4);
    CHECK(fixture.Faults.Total==8 && fixture.Faults.InSecond==4);
    /* A non-UTCL2 vector is not a fault, however it is decorated. */
    reset();wp=32;vector_client=4;vector_source=0x57;vector_src0=0x11BC40;vector_src1=0x50;
    Consume(&device,&fixture);CHECK(fault_reports==0 && fixture.Faults.Total==0);
    puts("PASS: empty rearm, late arrival, nonempty, wrap, fault, alignment, inactive, decode error, budget, overflow, prepublish edge, prepublish level, utcl2 burst");return 0;
}
