/* CPU-only: actual wrapper + escape + service + policy, OS locks/I/O mocked. */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include "bc250kmd_escape.h"
#include "board_memory_service.h"
#include "uma_transport.h"
#define main portable_service_controls
#include "board_memory_service_test.c"
#undef main
#define STATUS_SUCCESS ((NTSTATUS)0)
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#endif
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022L)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bbL)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
#define STATUS_DEVICE_HARDWARE_ERROR ((NTSTATUS)0xc0000185L)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184L)
#define STATUS_RETRY ((NTSTATUS)0xc000022dL)
#define PASSIVE_LEVEL 0
typedef unsigned KIRQL;
typedef unsigned EX_PUSH_LOCK;
typedef struct {
 volatile LONG64 UmaActiveBytes;
 volatile LONG RetainedPowerPhase,BoardMemoryProviderId;
 ULONG BoardMemoryReason,BoardMemoryBiosId;
 unsigned char BoardMemoryMachineId[16];
 unsigned BoardMemorySnapshotLock,BoardMemoryMapping;
 BOOLEAN BoardMemoryStarted;
 struct board_memory_state BoardMemoryState;
} BC250_DEVICE;
typedef struct BC250_BOARD_MEMORY_PROVIDER {
 void (*Query)(BC250_DEVICE*,BC250_ESCAPE_BOARD_MEMORY*);
 NTSTATUS (*Set)(BC250_DEVICE*,const BC250_ESCAPE_BOARD_MEMORY*);
 NTSTATUS (*Restore)(BC250_DEVICE*,const BC250_ESCAPE_BOARD_MEMORY*);
 void (*Probe)(BC250_DEVICE*,BC250_ESCAPE_BOARD_MEMORY_PROBE*,BOOLEAN,ULONG);
} BC250_BOARD_MEMORY_PROVIDER;
static unsigned irql, held, detections, detect_ok, detected_mapping;
static struct fixture machine;
static unsigned KeGetCurrentIrql(void){return irql;}
static void KeEnterCriticalRegion(void){}
static void KeLeaveCriticalRegion(void){}
static void ExAcquirePushLockExclusive(EX_PUSH_LOCK* l){(void)l;CHECK(!held);held=1;}
static void ExReleasePushLockExclusive(EX_PUSH_LOCK* l){(void)l;CHECK(held);held=0;}
static void KeInitializeSpinLock(unsigned* l){*l=0;}
static void KeAcquireSpinLock(unsigned* l,KIRQL* old){CHECK(!*l);*l=1;*old=irql;}
static void KeReleaseSpinLock(unsigned* l,KIRQL old){(void)old;CHECK(*l);*l=0;}
int BoardMemoryDetect(struct bc250_uma_transport* t,unsigned char b[28]) {
 CHECK(held && irql==0);++detections;
 if(!detect_ok){memset(t,0,sizeof(*t));memset(b,0,28);return 0;}
 t->mapping=detected_mapping;memcpy(b,machine.hw,28);return 1;
}
void bc250_uma_transport_io(struct bc250_uma_transport* t,struct bc250_uma_io* io) {
 CHECK(held && t->mapping==detected_mapping);io->context=&machine;io->read=rd;io->write=wr;
}
void BoardMemoryStoreIo(BC250_DEVICE* d,struct board_memory_store* st) {
 CHECK(held && d->BoardMemoryBiosId);st->context=&machine;st->load=load;st->save=save;
}
void AblBoardMemoryProbeRequest(BC250_DEVICE* d,BC250_ESCAPE_BOARD_MEMORY_PROBE* p,BOOLEAN a,ULONG f)
{(void)d;(void)p;(void)a;(void)f;}
/* ACTUAL_WRITE */
/* ACTUAL_UMA */
static BC250_ESCAPE_BOARD_MEMORY request(unsigned op) {
 BC250_ESCAPE_BOARD_MEMORY q;memset(&q,0,sizeof(q));q.Magic=BC250_ESCAPE_MAGIC;
 q.Command=BC250_ESCAPE_RUN_BOARD_MEMORY;q.Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
 q.AbiVersion=BC250_BOARD_MEMORY_ABI;q.Op=op;return q;
}
static void fresh(BC250_DEVICE* d) {
 memset(d,0,sizeof(*d));memset(&machine,0,sizeof(machine));block(machine.hw,8192);
 d->BoardMemoryProviderId=1;d->BoardMemoryBiosId=3;d->UmaActiveBytes=8ll<<30;
 irql=held=detections=0;detect_ok=1;detected_mapping=BC250_UMA_MAPPING_SLOT;
 BoardMemoryInitialize(d);
}
int main(void) {
 BC250_DEVICE d;BC250_ESCAPE_BOARD_MEMORY q;D3DDDI_ESCAPEFLAGS sw={0},hw={0};
 unsigned reads,loads,writes,detects,i;
 sw.NoAdapterSynchronization=1;hw.HardwareAccess=1;
 CHECK(portable_service_controls()==0);
 fresh(&d);d.BoardMemoryProviderId=0;BoardMemoryStart(&d);CHECK(!detections && !machine.reads && !machine.writes && !d.BoardMemoryStarted);
 fresh(&d);d.BoardMemoryBiosId=0;BoardMemoryStart(&d);CHECK(!detections && !machine.writes);
 fresh(&d);d.UmaActiveBytes=0;BoardMemoryStart(&d);CHECK(!detections && !machine.writes);
 fresh(&d);irql=1;BoardMemoryStart(&d);CHECK(!detections);irql=0;
 fresh(&d);detect_ok=0;BoardMemoryStart(&d);CHECK(detections==1 && !machine.writes && !d.BoardMemoryStarted);
 fresh(&d);BoardMemoryStart(&d);CHECK(d.BoardMemoryStarted && d.BoardMemoryState.ready);
 reads=machine.reads;loads=machine.loads;detects=detections;
 q=request(0);BoardMemoryRequest(&d,&q,FALSE,sw.Value);
 CHECK(q.Status==BC250_ESCAPE_STATUS_DONE && q.RequestedMiB==8192 && (q.Flags&BC250_BOARD_MEMORY_WRITE_ALLOWED));
 CHECK(machine.reads==reads && machine.loads==loads && detections==detects);
 for(i=0;i<4;++i) {
  q=request(1);q.RequestedMiB=12288;memcpy(q.ObservedBlock,machine.hw,28);
  if(i==0)BoardMemoryRequest(&d,&q,FALSE,hw.Value);
  else if(i==1)BoardMemoryRequest(&d,&q,TRUE,sw.Value);
  else if(i==2){irql=1;BoardMemoryRequest(&d,&q,TRUE,hw.Value);irql=0;}
  else {d.RetainedPowerPhase=1;BoardMemoryRequest(&d,&q,TRUE,hw.Value);d.RetainedPowerPhase=0;}
  CHECK(q.Status!=BC250_ESCAPE_STATUS_DONE && !machine.writes && detects==detections);
 }
 q=request(1);q.RequestedMiB=12288;memcpy(q.ObservedBlock,machine.hw,28);q.ObservedBlock[6]^=1;
 BoardMemoryRequest(&d,&q,TRUE,hw.Value);CHECK(q.NtStatus==(ULONG)STATUS_RETRY && !machine.writes);
 q=request(1);q.RequestedMiB=12288;memcpy(q.ObservedBlock,machine.hw,28);BoardMemoryRequest(&d,&q,TRUE,hw.Value);
 CHECK(q.Status==0 && q.NtStatus==0 && q.RequestedMiB==12288 && q.PreviousMiB==8192 && !machine.order_bad);
 CHECK((q.Flags&(BC250_BOARD_MEMORY_READ_VALID|BC250_BOARD_MEMORY_BACKUP_VALID))==(BC250_BOARD_MEMORY_READ_VALID|BC250_BOARD_MEMORY_BACKUP_VALID));
 q=request(2);memcpy(q.ObservedBlock,machine.hw,28);BoardMemoryRequest(&d,&q,TRUE,hw.Value);
 CHECK(q.Status==0 && q.RequestedMiB==8192 && machine.hw[0]==0x24);
 BoardMemoryStop(&d);reads=machine.reads;writes=machine.writes;detects=detections;
 q=request(1);q.RequestedMiB=12288;memcpy(q.ObservedBlock,machine.hw,28);BoardMemoryRequest(&d,&q,TRUE,hw.Value);
 CHECK(q.Status!=0 && machine.reads==reads && machine.writes==writes && detections==detects && !d.BoardMemoryStarted);
 fresh(&d);BoardMemoryStart(&d);detected_mapping=BC250_UMA_MAPPING_OFFSET;
 q=request(1);q.RequestedMiB=12288;memcpy(q.ObservedBlock,machine.hw,28);BoardMemoryRequest(&d,&q,TRUE,hw.Value);
 CHECK(q.Status!=0 && !machine.writes && !(q.Flags&BC250_BOARD_MEMORY_WRITE_ALLOWED));
 fresh(&d);BoardMemoryStart(&d);machine.fail_write=2;
 q=request(1);q.RequestedMiB=12288;memcpy(q.ObservedBlock,machine.hw,28);BoardMemoryRequest(&d,&q,TRUE,hw.Value);
 CHECK(q.Status!=0 && q.ResultCode==BC250_UMA_RESTORED && q.RequestedMiB==8192 && !machine.pending);
 fresh(&d);BoardMemoryStart(&d);machine.all_writes_fail=1;
 q=request(1);q.RequestedMiB=12288;memcpy(q.ObservedBlock,machine.hw,28);BoardMemoryRequest(&d,&q,TRUE,hw.Value);
 CHECK(q.Status!=0 && q.Reason==6 && !(q.Flags&BC250_BOARD_MEMORY_WRITE_ALLOWED) && machine.pending==1);
 BoardMemoryStop(&d);BoardMemoryStart(&d);CHECK(d.BoardMemoryState.blocked);
 printf("Board memory wrapper: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
