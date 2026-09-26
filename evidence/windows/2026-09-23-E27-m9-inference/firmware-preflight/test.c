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
enum bc250_fw_file {
	BC250_FILE_SDMA = 0,    /* cyan_skillfish2_sdma.bin  */
	BC250_FILE_SDMA1,       /* cyan_skillfish2_sdma1.bin */
	BC250_FILE_CE,          /* cyan_skillfish2_ce.bin    */
	BC250_FILE_PFP,         /* cyan_skillfish2_pfp.bin   */
	BC250_FILE_ME,          /* cyan_skillfish2_me.bin    */
	BC250_FILE_MEC,         /* cyan_skillfish2_mec.bin   */
	BC250_FILE_MEC2,        /* cyan_skillfish2_mec2.bin  */
	BC250_FILE_RLC,         /* cyan_skillfish2_rlc.bin   */
	BC250_FILE_COUNT
};
typedef struct _BC250_PSP_FILES {
    PUCHAR Data[BC250_FILE_COUNT];
    ULONG Size[BC250_FILE_COUNT];
} BC250_PSP_FILES;

typedef struct _BC250_PSP_FIRMWARE {
    BC250_PSP_FILES Files;
    BC250_DEVICE* Owner;
    ULONGLONG VramMcBase,VramLength;
} BC250_PSP_FIRMWARE;


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
static void FreeFiles(_Inout_ BC250_PSP_FILES* Files)
{
    ULONG i;

    for (i = 0; i < BC250_FILE_COUNT; i++)
    {
        if (Files->Data[i] != NULL) ExFreePoolWithTag(Files->Data[i], BC250_PSP_TAG);
        Files->Data[i] = NULL;
    }
}
static NTSTATUS ReadFiles(_Out_ BC250_PSP_FILES* Files, _Out_ ULONG* FailedFile)
{
    ULONG i;
    NTSTATUS status;

    RtlZeroMemory(Files, sizeof(*Files));
    *FailedFile = 0;
    for (i = 0; i < BC250_FILE_COUNT; i++)
    {
        status = ReadOneFile(bc250_fw_file_name((enum bc250_fw_file)i), &Files->Data[i], &Files->Size[i]);
        if (!NT_SUCCESS(status))
        {
            GuardLog("psp: firmware file %s: 0x%08X", bc250_fw_file_name((enum bc250_fw_file)i), status);
            *FailedFile = i;
            FreeFiles(Files);
            return status;
        }
    }
    return STATUS_SUCCESS;
}
static void PspExecutePrepared(BC250_DEVICE* Device,BC250_ESCAPE_PSP* Data,const BC250_PSP_FIRMWARE* Prepared) {
 BC250_PSP_FILES files={0}; NTSTATUS status=0; ULONG failedFile=0;
    if (Data->Op > BC250_PSP_OP_UNLOAD) status = STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(status) && !Device->MmioPspEnabled) status = STATUS_DEVICE_NOT_READY;
    // Files first: file I/O needs PASSIVE_LEVEL, and the lock below raises to APC_LEVEL.
    if (NT_SUCCESS(status) && Prepared) {
        if (Data->Op!=BC250_PSP_OP_LOAD || Prepared->Owner!=Device ||
            Prepared->VramMcBase!=Device->VramMcBase || Prepared->VramLength!=Device->VramLength)
            status=STATUS_INVALID_DEVICE_STATE;
        else files=Prepared->Files; // borrowed; caller releases after execution/unwind
    } else if (NT_SUCCESS(status) && Data->Op != BC250_PSP_OP_UNLOAD) status = ReadFiles(&files, &failedFile);
    if (!NT_SUCCESS(status))
    {
        Data->Result = (long)failedFile;
        Data->NtStatus = (unsigned long)status;
        Data->Status = BC250_ESCAPE_STATUS_REFUSED;
        return;
    }

 hardwareCalls++; usedByte=files.Data[0][0]; Data->State=7;Data->CommandCount=2;Data->CommandsDone=loadFail?1:2;Data->Result=loadFail?-62:0;Data->NtStatus=0;Data->Status=loadFail?BC250_ESCAPE_STATUS_REFUSED:BC250_ESCAPE_STATUS_DONE;
 if (!Prepared) FreeFiles(&files);
}
void PspReleaseFirmware(BC250_PSP_FIRMWARE* Firmware)
{
    if (!Firmware) return;
    FreeFiles(&Firmware->Files);
    ExFreePoolWithTag(Firmware,BC250_PSP_TAG);
}
NTSTATUS PspPrepareFirmware(BC250_DEVICE* Device, BC250_ESCAPE_PSP* Report,
                           BC250_PSP_FIRMWARE** Firmware)
{
    BC250_PSP_FIRMWARE* prepared=NULL;
    NTSTATUS status;
    ULONG failed=0;
    if (!Firmware) return STATUS_INVALID_PARAMETER;
    *Firmware=NULL;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_PSP;
    Report->Op=BC250_PSP_OP_LOAD;
    if (!Device) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else if (!Device->Psp || !Device->MmioPspEnabled) status=STATUS_DEVICE_NOT_READY;
    else status=CheckWindow(Device);
    if (NT_SUCCESS(status)) {
        prepared=(BC250_PSP_FIRMWARE*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*prepared),BC250_PSP_TAG);
        if (!prepared) status=STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status)) {
        status=ReadFiles(&prepared->Files,&failed);
        Report->Result=(long)failed;
    }
    if (NT_SUCCESS(status)) {
        FillAddresses(Device,Report);
        status=LayOut(&prepared->Files,Report,NULL);
    }
    if (NT_SUCCESS(status)) {
        prepared->Owner=Device;prepared->VramMcBase=Device->VramMcBase;prepared->VramLength=Device->VramLength;
        *Firmware=prepared;
    } else PspReleaseFirmware(prepared);
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}
NTSTATUS PspInitializePrepared(BC250_DEVICE* Device, const BC250_PSP_FIRMWARE* Prepared,
                              BC250_ESCAPE_PSP* Report)
{
    NTSTATUS status;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_PSP;Report->Op=BC250_PSP_OP_LOAD;
    if (!Device || !Prepared) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else {
        PspExecutePrepared(Device,Report,Prepared);
        status=(NTSTATUS)Report->NtStatus;
        if (NT_SUCCESS(status) && (Report->Status!=BC250_ESCAPE_STATUS_DONE || Report->Result!=0 ||
            (Report->State&7u)!=7u || !Report->CommandCount ||
            Report->CommandsDone!=Report->CommandCount)) status=STATUS_IO_DEVICE_ERROR;
    }
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}

int main(void)
{
 BC250_DEVICE d={0};BC250_PSP_FIRMWARE* prepared=NULL;BC250_ESCAPE_PSP report;
 unsigned i;int priorReads;NTSTATUS status;
 d.Psp=&d;d.MmioPspEnabled=1;d.VramMcBase=0x100000000ull;d.VramLength=1ull<<30;
 for(i=1;i<=BC250_FILE_COUNT+1;i++) {
  allocations=0;failAllocation=(int)i;reads=hardwareCalls=0;
  check(PspPrepareFirmware(&d,&report,&prepared)==STATUS_INSUFFICIENT_RESOURCES &&
        !prepared&&!live&&!hardwareCalls,"each allocation failure releases all prepared file buffers without hardware");
 }
 failAllocation=0;
 for(i=0;i<BC250_FILE_COUNT;i++){
  failFile=(int)i;
  check(PspPrepareFirmware(&d,&report,&prepared)==STATUS_IO_DEVICE_ERROR &&
        !prepared&&!live&&report.Result==(long)i&&!hardwareCalls,"each missing file preserves index and unwinds prior files");
 }
 failFile=-1;layoutFail=1;
 check(PspPrepareFirmware(&d,&report,&prepared)==STATUS_IO_DEVICE_ERROR&&!prepared&&!live&&!hardwareCalls,
       "invalid firmware layout unwinds complete file set before hardware");
 layoutFail=0;
 check(PspPrepareFirmware(&d,&report,&prepared)==0&&prepared&&live==BC250_FILE_COUNT+1&&
       report.StagingUsed==17&&!hardwareCalls,"successful preparation owns one stable validated file snapshot");
 priorReads=reads;diskVersion=99;
 status=PspInitializePrepared(&d,prepared,&report);
 check(status==0&&usedByte==17&&reads==priorReads&&live==BC250_FILE_COUNT+1,
       "load borrows original bytes despite changed disk and never reopens or frees files");
 loadFail=1;
 check(PspInitializePrepared(&d,prepared,&report)==STATUS_IO_DEVICE_ERROR&&report.CommandsDone==1&&
       reads==priorReads&&live==BC250_FILE_COUNT+1,"partial hardware load retains firmware for caller-owned unwind");
 loadFail=0;hardwareCalls=0;d.VramMcBase++;
 check(PspInitializePrepared(&d,prepared,&report)==STATUS_INVALID_DEVICE_STATE&&!hardwareCalls&&reads==priorReads,
       "prepared firmware cannot load against changed VRAM geometry");
 d.VramMcBase--;prepared->Owner=NULL;
 check(PspInitializePrepared(&d,prepared,&report)==STATUS_INVALID_DEVICE_STATE&&!hardwareCalls,
       "prepared firmware belongs to one device");
 prepared->Owner=&d;d.GpuStopUnconfirmed=1;
 check(PspInitializePrepared(&d,prepared,&report)==STATUS_DEVICE_HARDWARE_ERROR&&!hardwareCalls,
       "quarantine blocks prepared load");
 d.GpuStopUnconfirmed=0;
 PspReleaseFirmware(prepared);prepared=NULL;
 check(!live,"explicit release frees prepared owner and every file once");
 PspReleaseFirmware(NULL);check(!live,"empty release is harmless");
 irql=1;priorReads=reads;
 check(PspPrepareFirmware(&d,&report,&prepared)==STATUS_INVALID_DEVICE_STATE&&!prepared&&reads==priorReads&&!live,
       "preflight refuses file IO above PASSIVE_LEVEL");
 irql=0;
 check(PspPrepareFirmware(NULL,&report,&prepared)==STATUS_INVALID_PARAMETER&&!prepared&&!live,"missing device refuses preparation");
 check(PspPrepareFirmware(&d,NULL,&prepared)==STATUS_INVALID_PARAMETER&&!prepared&&!live,"missing report refuses preparation");
 check(PspPrepareFirmware(&d,&report,NULL)==STATUS_INVALID_PARAMETER&&!live,"missing ownership output refuses preparation");
 check(PspInitializePrepared(&d,NULL,&report)==STATUS_INVALID_PARAMETER&&!hardwareCalls,"missing prepared data cannot fall back to rereading files");
 // Diagnostic path still owns and frees its own file reads.
 PspExecutePrepared(&d,&report,NULL);
 check(!live && usedByte==99 && reads==priorReads+BC250_FILE_COUNT,"legacy execution reads current files and releases its private copy");
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
