#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bc250kmd_escape.h"
#define NT_SUCCESS(s) ((s)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define GuardLog(...) ((void)0)
#define STATUS_SUCCESS 0L
#define STATUS_INVALID_PARAMETER (-1L)
#define STATUS_INVALID_DEVICE_STATE (-2L)
#define STATUS_DEVICE_HARDWARE_ERROR (-3L)
#define STATUS_DEVICE_NOT_READY (-4L)
#define STATUS_INSUFFICIENT_RESOURCES (-5L)
#define STATUS_IO_DEVICE_ERROR (-6L)
#define PASSIVE_LEVEL 0
#define POOL_FLAG_NON_PAGED 0
#define BC250_PSP_TAG 0
typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned char* PUCHAR;
typedef unsigned long long ULONGLONG;
typedef struct {void*Psp;int MmioPspEnabled,GpuStopUnconfirmed;ULONGLONG VramMcBase,VramLength;} BC250_DEVICE;
/* TYPES */
static int allocations,live,failAllocation,reads,failFile=-1,layoutFail,loadFail,irql,hardwareCalls,usedByte;
static unsigned char diskVersion=17;
static int checks,failures;
static void check(int ok,const char*name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static int KeGetCurrentIrql(void){return irql;}
static void* ExAllocatePool2(unsigned flag,size_t size,unsigned tag)
{(void)flag;(void)tag;allocations++;if(allocations==failAllocation)return NULL;live++;return calloc(1,size);}
static void ExFreePoolWithTag(void*p,unsigned tag){(void)tag;if(p){live--;free(p);}}
static const char* bc250_fw_file_name(enum bc250_fw_file file)
{static char name[2];name[0]=(char)('a'+file);name[1]=0;return name;}
static NTSTATUS ReadOneFile(const char*name,PUCHAR*data,ULONG*size)
{
 int index=name[0]-'a';reads++;*data=NULL;*size=0;
 if(index==failFile)return STATUS_IO_DEVICE_ERROR;
 *data=ExAllocatePool2(0,8,0);if(!*data)return STATUS_INSUFFICIENT_RESOURCES;
 memset(*data,diskVersion+index,8);*size=8;return 0;
}
static NTSTATUS CheckWindow(const BC250_DEVICE*d){return d->VramLength?0:STATUS_DEVICE_NOT_READY;}
static void FillAddresses(const BC250_DEVICE*d,BC250_ESCAPE_PSP*r){r->StagingMc=d->VramMcBase;}
static NTSTATUS LayOut(const BC250_PSP_FILES*f,BC250_ESCAPE_PSP*r,PUCHAR staging)
{
 unsigned i;(void)staging;
 for(i=0;i<BC250_FILE_COUNT;i++)if(!f->Data[i]||f->Size[i]!=8)return STATUS_IO_DEVICE_ERROR;
 r->CommandCount=BC250_FILE_COUNT+1;r->StagingUsed=f->Data[0][0];
 return layoutFail?STATUS_IO_DEVICE_ERROR:0;
}
