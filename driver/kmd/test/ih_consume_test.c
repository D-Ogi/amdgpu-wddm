/* Host control of the actual IH Consume body. Hardware delivery is modeled. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
typedef unsigned long ULONG;
typedef unsigned int u32;
#ifndef _Inout_
#define _Inout_
#endif
#define NT_SUCCESS(x) ((x)>=0)
#define BC250_IH_DPC_ROUNDS 4
struct amdgpu_device { struct { struct { unsigned ring_size,rptr; } ih; } irq; };
struct bc250_iv_entry { int unused; };
typedef struct { unsigned OverflowCount, DecodeErrors, Rptr, count; } Stats;
typedef struct { struct amdgpu_device *DpcAdev; struct { long Fault; } DpcSequence; long Active; unsigned Rptr; int StatsLock; Stats Stats; } BC250_IH;
static unsigned wp, gets, pubs, decodes;
static int armed, pending, inject, bad_decode, streaming, overflow_mode;
static long InterlockedExchange(long *p,long v) { long old=*p;*p=v;return old; }
static void KeAcquireSpinLockAtDpcLevel(int *p) { if(*p) abort();*p=1; }
static void KeReleaseSpinLockFromDpcLevel(int *p) { if(!*p) abort();*p=0; }
static u32 bc250_ih_get_wptr(struct amdgpu_device *a,bool *overflow) { (void)a;gets++;*overflow=overflow_mode!=0;if(overflow_mode)a->irq.ih.rptr=96;return wp; }
static int bc250_ih_decode(struct amdgpu_device *a,u32 *r,struct bc250_iv_entry *e) { (void)e;decodes++;if(bad_decode)return -1;*r=(*r+32)%a->irq.ih.ring_size;return 0; }
static void Note(Stats *s,const struct bc250_iv_entry *e) { (void)e;s->count++; }
static void bc250_ih_set_rptr(struct amdgpu_device *a,u32 r) {
    a->irq.ih.rptr=r;pubs++;armed=1;if(streaming)wp=(r+32)%a->irq.ih.ring_size;
    if(inject) { inject=0;wp=32;pending=armed;armed=0; }
}
#include "ih_consume_actual.inc"
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n",__LINE__,#c); return 1; } } while(0)
static struct amdgpu_device dev;
static BC250_IH ih;
static void reset(void) { memset(&dev,0,sizeof(dev));memset(&ih,0,sizeof(ih));dev.irq.ih.ring_size=128;ih.DpcAdev=&dev;ih.Active=1;wp=gets=pubs=decodes=0;armed=pending=inject=bad_decode=streaming=overflow_mode=0; }
int main(void) {
    reset();Consume(&ih);CHECK(pubs==1 && armed && decodes==0 && gets==1);
    reset();inject=1;Consume(&ih);CHECK(pending && pubs==1);pending=0;Consume(&ih);CHECK(ih.Stats.count==1 && ih.Rptr==32 && armed);
    reset();wp=64;Consume(&ih);CHECK(decodes==2 && ih.Rptr==64 && armed && gets<=4);
    reset();ih.Rptr=96;wp=32;Consume(&ih);CHECK(decodes==2 && ih.Rptr==32 && armed);
    reset();ih.DpcSequence.Fault=-1;Consume(&ih);CHECK(!ih.Active && pubs==0 && decodes==0);
    reset();wp=1;Consume(&ih);CHECK(!ih.Active && pubs==0 && decodes==0);
    reset();ih.Active=0;Consume(&ih);CHECK(pubs==0);
    reset();wp=32;bad_decode=1;Consume(&ih);CHECK(ih.Stats.DecodeErrors==1 && gets<=4);
    reset();streaming=1;wp=32;Consume(&ih);CHECK(decodes==4 && gets==4 && pubs==4);
    reset();overflow_mode=1;wp=32;Consume(&ih);CHECK(ih.Stats.OverflowCount>0 && decodes<=4 && gets<=4);
    puts("PASS: empty rearm, late arrival, nonempty, wrap, fault, alignment, inactive, decode error, budget, overflow");return 0;
}
