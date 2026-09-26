"""Extract the real startup coordinator; exercise CP checkpoints without hardware."""
from pathlib import Path
import sys
repo = Path(__file__).resolve().parents[3]
source = (repo / "driver/kmd/gfx.c").read_text()
a = source.index("static NTSTATUS SdmaIbStartupControl(")
bcontrol = source.index("\n}\n", a) + 3
control = source[a:bcontrol]
a = source.index("NTSTATUS GfxInitializeHardware(")
b = source.index("\n}\n", a) + 3
header = (repo / "driver/shim/include/bc250_gfx.h").read_text()
e = header.index("enum bc250_cp_step {")
f = header.index("};", e) + 2
prefix = r'''#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
typedef int32_t NTSTATUS;
typedef unsigned long ULONG;
typedef int BOOLEAN;
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_INVALID_DEVICE_STATE (-2)
#define STATUS_DEVICE_HARDWARE_ERROR (-3)
#define STATUS_IO_DEVICE_ERROR (-4)
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define BC250_ESCAPE_MAGIC 1
#define BC250_ESCAPE_RUN_GFX 2
#define BC250_GFX_OP_RUN 1
#define BC250_ESCAPE_STATUS_DONE 0
#define BC250_ESCAPE_STATUS_REFUSED 1
#define BC250_GFX_STAGE_CP 6
#define BC250_GFX_STAGE_INTERRUPTS 8
typedef struct {int GpuStopUnconfirmed,FullWddm,Started;void *Wddm;} BC250_DEVICE;
#define BC250_ESCAPE_RUN_SDMACOPY 16
#define BC250_ESCAPE_RUN_SDMAIB 18
#define BC250_SDMACOPY_DEFAULT_BYTES 4096
#define BC250_SDMACOPY_MAX_BYTES 65536
typedef struct {ULONG Magic,Command,Bytes,Status,NtStatus,LastSeq,LastValue,BytesCompared,Matched;long Result;} BC250_ESCAPE_SDMACOPY;
typedef struct {ULONG Magic,Command,Op,LastStage,NtStatus,Status,FailedStage,StagesDone;long Result;} BC250_ESCAPE_GFX;
static int capture,failed,mode,bad,calls,keeps,ibControl,ibCalls,vaControl,vaCalls;
static ULONG seen[32],last_cp;
static int KeGetCurrentIrql(void) {return 0;}
static ULONG GuardReadSetting(const wchar_t *name,ULONG value) {(void)value;return !wcscmp(name,L"EnableSdmaIbControl") ? (ULONG)ibControl : !wcscmp(name,L"EnableSdmaVaControl") ? (ULONG)vaControl : (ULONG)capture;}
static NTSTATUS SdmaVaStartupControl(BC250_DEVICE *d) {if(!d->FullWddm || d->Started || d->Wddm || ibCalls!=4)bad++;vaCalls++;return STATUS_SUCCESS;}
static void SdmaCopyEscape(BC250_DEVICE *d,BC250_ESCAPE_SDMACOPY *c) {
 ULONG command=(ibCalls==0 || ibCalls==3)?16:18;
 ULONG bytes=ibCalls<2?4096:65536;
 if(!d->FullWddm || d->Started || d->Wddm || calls!=1 || seen[0]!=800 || c->Command!=command || c->Bytes!=bytes)bad++;
 c->Status=0;c->NtStatus=0;c->Result=0;c->LastSeq=c->LastValue=(ULONG)++ibCalls;c->BytesCompared=c->Bytes;c->Matched=1;
}
#define GuardLog(...) ((void)0)
static void GuardLogKeep(void) {keeps++;}
static void GfxExecute(BC250_DEVICE *d,BC250_ESCAPE_GFX *r,ULONG cp) {
 (void)d;
 seen[calls++]=r->LastStage*100+cp;
 r->NtStatus=0;r->Result=0;r->FailedStage=0;r->Status=0;r->StagesDone=r->LastStage;
 if(cp) {
  if(cp!=last_cp+1)bad++;
  last_cp=cp;
  if((int)cp==failed) {
   if(mode==0)r->NtStatus=(ULONG)STATUS_IO_DEVICE_ERROR;
   else {r->Result=-62;r->Status=BC250_ESCAPE_STATUS_REFUSED;r->FailedStage=6;}
  }
 }
}
'''
suffix = r'''
int main(void) {
 BC250_DEVICE d={0};BC250_ESCAPE_GFX r;int m,k;NTSTATUS status;
 for(m=0;m<2;m++)for(k=0;k<=8;k++) {
  capture=1;failed=k;mode=m;calls=keeps=0;last_cp=0;
  status=GfxInitializeHardware(&d,&r);
  if(k==0) {
   if(status!=0||calls!=15||last_cp!=8||seen[13]!=700||seen[14]!=800||keeps!=32)bad++;
  } else if(status>=0||calls!=5+k||last_cp!=(ULONG)k||seen[calls-1]!=600+(ULONG)k||r.LastStage!=6)bad++;
 }
 capture=0;failed=0;calls=keeps=0;last_cp=0;
 status=GfxInitializeHardware(&d,&r);
 if(status!=0||calls!=1||seen[0]!=800||keeps!=0)bad++;
 ibControl=1;d.FullWddm=1;calls=keeps=0;
 status=GfxInitializeHardware(&d,&r);
 if(status!=0 || ibCalls!=4 || keeps!=8)bad++;
 ibControl=0;vaControl=1;ibCalls=0;calls=keeps=0;
 status=GfxInitializeHardware(&d,&r);
 if(status!=0 || ibCalls!=4 || vaCalls!=1 || keeps!=10)bad++;
 printf("CP startup coordinator: 21 scenarios, %d failures\n",bad);
 return bad?1:0;
}
'''
Path(sys.argv[1]).write_text(prefix + header[e:f] + "\n" + control + "\n" + source[a:b] + suffix)
