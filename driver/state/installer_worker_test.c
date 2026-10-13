// Actual worker orchestration with OS adapters replaced in memory. No registry,
// service, device, file ACL, GPU or SYSTEM process is touched by this executable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "windows_migration.h"
#include "windows_handoff.h"
#include "installer_rollback.h"
extern DWORD AmdgpuInstallerTestExecute(DWORD,WCHAR**);
static BYTE saved[AMDGPU_TRANSFER_MAX_BYTES],removed[AMDGPU_TRANSFER_MAX_BYTES];
static UINT saved_bytes,removed_bytes,origin,failure,freeze,stop,publish,archives,captured,checks;
static DWORD board_error,pending_error,remove_error,origin_error,device_error;static BOOL current_package;
static int failed;
ULONG AmdgpuInstallerTestBoot=91;
static BYTE rollback[AMDGPU_ROLLBACK_MAX],completed[AMDGPU_ROLLBACK_MAX],active_rollback[AMDGPU_ROLLBACK_MAX];
static DWORD rb_bytes,complete_bytes,active_bytes,restores,retired,enabled,source_removed,target_error,restore_error,retire_error,complete_error;
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL %u line %u: %s\n",checks,__LINE__,#x);failed++;}}while(0)
static void Reset(UINT source)
{
    saved_bytes=removed_bytes=failure=freeze=stop=publish=archives=captured=0;
    board_error=pending_error=remove_error=origin_error=device_error=0;origin=source;current_package=FALSE;
    rb_bytes=complete_bytes=active_bytes=restores=retired=enabled=source_removed=target_error=restore_error=retire_error=complete_error=0;
    AmdgpuInstallerTestBoot=91;
}
static DWORD Roll(PCWSTR op)
{
    WCHAR *args[]={L"test",(WCHAR*)op,L"01010101010101010101010101010101",L"0.7.216.102",L"PCI\\VEN_1002&DEV_13FE\\FIXTURE",
        L"1111111111111111111111111111111111111111111111111111111111111111",
        L"2222222222222222222222222222222222222222222222222222222222222222",
        L"3333333333333333333333333333333333333333333333333333333333333333"};
    return AmdgpuInstallerTestExecute(8,args);
}
static DWORD Run(PCWSTR op,PCWSTR transaction)
{
    WCHAR *args[]={L"test",(WCHAR*)op,(WCHAR*)transaction,L"0.7.216.102",L"PCI\\VEN_1002&DEV_13FE\\FIXTURE"};
    return AmdgpuInstallerTestExecute(5,args);
}
DWORD AmdgpuInstallerTestDevice(PCWSTR id,AMDGPU_TRANSFER_HEADER *h,BOOL stopped)
{
    (void)id;memset(h->device_hash,7,32);strcpy_s(h->source_version,32,"0.7.216.100");
    if(current_package)h->source_schema=AmdgpuStateSchema();
    if(stopped)stop++;return device_error;
}
DWORD AmdgpuInstallerTestOrigin(UINT *out){*out=origin;return origin_error;}
DWORD AmdgpuInstallerTestQuiescent(PCWSTR id){(void)id;return stop && !enabled?0:ERROR_BUSY;}
DWORD AmdgpuInstallerTestSource(PCWSTR id,AMDGPU_ROLLBACK *r,BOOL remove)
{(void)id;if(remove){CHECK(stop && active_bytes && !enabled);source_removed++;return 0;}
 wcscpy_s(r->source_inf,32,L"oem17.inf");memset(r->source_inf_hash,4,32);return 0;}
DWORD AmdgpuInstallerTestTarget(PCWSTR id,const AMDGPU_ROLLBACK *r,BOOL mode)
{(void)id;(void)r;if(target_error)return target_error;if(!stop || (enabled && mode<2) || (mode==4 && !enabled))return ERROR_BUSY;
 if(mode==3){CHECK(complete_bytes && retired);enabled++;}return 0;}
UINT AmdgpuWindowsStateOpen(AMDGPU_WINDOWS_STATE **out,UINT writable,DWORD *error)
{(void)writable;*error=failure==1?ERROR_ACCESS_DENIED:0;*out=(AMDGPU_WINDOWS_STATE*)1;return *error?AMDGPU_STATE_ACCESS:0;}
void AmdgpuWindowsStateClose(AMDGPU_WINDOWS_STATE *s){(void)s;}
UINT AmdgpuWindowsStateIdentity(AMDGPU_WINDOWS_STATE *s,BYTE generation[16],DWORD *error)
{(void)s;memset(generation,2,16);*error=failure==2?ERROR_REVISION_MISMATCH:0;return *error?AMDGPU_STATE_SCHEMA:0;}
UINT AmdgpuWindowsStateFreeze(AMDGPU_WINDOWS_STATE *s,const BYTE tx[16],DWORD *error)
{(void)s;(void)tx;freeze++;*error=failure==3?ERROR_WRITE_FAULT:0;return *error?AMDGPU_STATE_IO:0;}
UINT AmdgpuWindowsStateRetire(AMDGPU_WINDOWS_STATE *s,const BYTE tx[16],DWORD *error)
{(void)s;(void)tx;CHECK(restores && !enabled);*error=retire_error;if(*error)return AMDGPU_STATE_IO;retired++;origin=AMDGPU_TRANSFER_LEGACY;return 0;}
static UINT Fetch(void *context,UINT source,const AMDGPU_STATE_SPEC *spec,AMDGPU_STATE_VALUE *v,void *data,UINT capacity)
{
    DWORD one=1;(void)context;(void)source;(void)capacity;
    if(origin!=AMDGPU_TRANSFER_FRESH && !strcmp(spec->name,"CpuTune") && !spec->path[0]) {
        memcpy(data,&one,4);v->type=4;v->bytes=4;return 0;
    }
    return AMDGPU_STATE_ABSENT;
}
UINT AmdgpuWindowsCaptureCurrent(AMDGPU_WINDOWS_STATE *s,const AMDGPU_TRANSFER_HEADER *h,
    void *buffer,UINT capacity,UINT *bytes,DWORD *error)
{
    (void)s;captured++;CHECK(freeze==1);*error=failure==4?ERROR_ACCESS_DENIED:0;
    return *error?AMDGPU_STATE_ACCESS:AmdgpuTransferCapture(h,Fetch,NULL,buffer,capacity,bytes);
}
UINT AmdgpuWindowsCaptureLegacy(const AMDGPU_TRANSFER_HEADER *h,void *buffer,UINT capacity,UINT *bytes,DWORD *error,DWORD *board)
{
    AMDGPU_TRANSFER_HEADER copy=*h;captured++;*error=failure==4?ERROR_ACCESS_DENIED:0;*board=board_error;
    copy.board_kind=board_error?AMDGPU_BOARD_UNPROVEN:AMDGPU_BOARD_FRESH;
    return *error?AMDGPU_STATE_ACCESS:AmdgpuTransferCapture(&copy,Fetch,NULL,buffer,capacity,bytes);
}
static DWORD Read(const BYTE *source,UINT size,DWORD error,void **out,DWORD *bytes)
{
    *out=NULL;*bytes=0;if(error)return error;if(!size)return ERROR_FILE_NOT_FOUND;
    *out=HeapAlloc(GetProcessHeap(),0,size);if(!*out)return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(*out,source,size);*bytes=size;return 0;
}
DWORD AmdgpuWindowsReadHandoff(void **out,DWORD *bytes){return Read(saved,saved_bytes,pending_error,out,bytes);}
DWORD AmdgpuWindowsReadRemoval(void **out,DWORD *bytes){return Read(removed,removed_bytes,remove_error,out,bytes);}
DWORD AmdgpuWindowsRollbackRead(const BYTE tx[16],BOOL complete,void **out,DWORD *bytes)
{
    DWORD error=Read(complete==2?active_rollback:complete?completed:rollback,complete==2?active_bytes:complete?complete_bytes:rb_bytes,0,out,bytes);
    if(!error && tx && memcmp(((AMDGPU_ROLLBACK*)*out)->snapshot.transaction,tx,16)) {
        HeapFree(GetProcessHeap(),0,*out);*out=NULL;*bytes=0;return ERROR_REVISION_MISMATCH;
    }return error;
}
DWORD AmdgpuWindowsRollbackSave(const void *data,DWORD bytes,BOOL complete)
{
    CHECK(!AmdgpuRollbackCheck(data,bytes));if(complete==1 && complete_error)return complete_error;
    if(failure==7)return ERROR_WRITE_FAULT;
    memcpy(complete==2?active_rollback:complete?completed:rollback,data,bytes);
    if(complete==2)active_bytes=bytes;else if(complete)complete_bytes=bytes;else rb_bytes=bytes;return 0;
}
DWORD AmdgpuWindowsRollbackClear(const void *data,DWORD bytes)
{CHECK(enabled && bytes==active_bytes && !memcmp(data,active_rollback,bytes));active_bytes=0;return 0;}
DWORD AmdgpuInstallerRollbackRestore(const void *data,DWORD bytes)
{CHECK(stop && !enabled && !AmdgpuRollbackCheck(data,bytes));restores++;return restore_error;}
DWORD AmdgpuWindowsRollbackBoard(void){CHECK(stop && !enabled);return board_error;}
static DWORD ConvertWrite(void *context,const AMDGPU_STATE_SPEC *spec,BOOL present,const void *data,DWORD bytes)
{
    DWORD *calls=context;(*calls)++;
    if(spec->kind==AMDGPU_STATE_VOLATILE || (spec->kind==AMDGPU_STATE_RECORD && !spec->group))CHECK(!present);
    if(!strcmp(spec->name,"CpuTune"))CHECK(present && bytes==4 && *(const DWORD*)data==1);
    return failure==*calls?ERROR_WRITE_FAULT:0;
}
DWORD AmdgpuWindowsPublishHandoff(const void *buffer,DWORD bytes)
{
    publish++;CHECK(AmdgpuTransferCheck(buffer,bytes,NULL)==0);
    if(failure==5)return ERROR_WRITE_FAULT;memcpy(saved,buffer,bytes);saved_bytes=bytes;return 0;
}
DWORD AmdgpuWindowsArchiveState(const void *buffer,DWORD bytes)
{
    archives++;CHECK(stop==1);CHECK(AmdgpuTransferCheck(buffer,bytes,NULL)==0);
    if(failure==6)return ERROR_WRITE_FAULT;memcpy(removed,buffer,bytes);removed_bytes=bytes;return 0;
}
DWORD AmdgpuWindowsAcknowledgeHandoff(const BYTE tx[16],const BYTE device[32],const char *version)
{
    AMDGPU_TRANSFER_HEADER h;if(AmdgpuTransferCheck(saved,saved_bytes,&h))return ERROR_INVALID_DATA;
    return memcmp(tx,h.transaction,16)||memcmp(device,h.device_hash,32)||strcmp(version,h.destination_version)?ERROR_REVISION_MISMATCH:0;
}
int main(void)
{
    static const WCHAR tx[]=L"01010101010101010101010101010101";
    AMDGPU_TRANSFER_HEADER h;UINT i;
    for(i=AMDGPU_TRANSFER_FRESH;i<=AMDGPU_TRANSFER_CURRENT;i++) {
        Reset(i);CHECK(!Run(L"prepare",tx));CHECK(publish==1);CHECK(captured==1);
        CHECK(!AmdgpuTransferCheck(saved,saved_bytes,&h));CHECK(h.origin==i);
        CHECK(freeze==(UINT)(i==AMDGPU_TRANSFER_CURRENT));CHECK(stop==0);
        CHECK(!Run(L"prepare",tx));CHECK(publish==1);CHECK(captured==1);
        CHECK(Run(L"prepare",L"02020202020202020202020202020202")==ERROR_REVISION_MISMATCH);
        CHECK(!Run(L"ack",tx));
    }
    for(i=1;i<=5;i++) {
        Reset(AMDGPU_TRANSFER_CURRENT);failure=i;CHECK(Run(L"prepare",tx)!=0);
        CHECK(!saved_bytes);CHECK(stop==0);
    }
    Reset(AMDGPU_TRANSFER_CURRENT);pending_error=ERROR_ACCESS_DENIED;
    CHECK(Run(L"prepare",tx)==ERROR_ACCESS_DENIED);CHECK(freeze==0 && publish==0);
    Reset(AMDGPU_TRANSFER_FRESH);remove_error=ERROR_INVALID_DATA;
    CHECK(Run(L"prepare",tx)==ERROR_INVALID_DATA);CHECK(publish==0);
    Reset(AMDGPU_TRANSFER_LEGACY);board_error=ERROR_INVALID_SECURITY_DESCR;
    CHECK(Run(L"prepare",tx)==ERROR_INVALID_SECURITY_DESCR);CHECK(publish==0);
    Reset(AMDGPU_TRANSFER_CURRENT);origin_error=ERROR_ACCESS_DENIED;
    CHECK(Run(L"prepare",tx)==ERROR_ACCESS_DENIED);CHECK(freeze==0);
    Reset(AMDGPU_TRANSFER_LEGACY);current_package=TRUE;
    CHECK(Run(L"prepare",tx)==ERROR_INVALID_DATA);CHECK(publish==0 && captured==0);
    Reset(AMDGPU_TRANSFER_CURRENT);CHECK(!Run(L"archive",tx));CHECK(archives==1 && stop==1 && freeze==1);
    CHECK(!AmdgpuTransferCheck(removed,removed_bytes,&h));CHECK(h.origin==AMDGPU_TRANSFER_CURRENT);
    // SCM and S are gone. The old final archive supplies settings to a new
    // transaction, and never supplies fresh-board authorization.
    origin=AMDGPU_TRANSFER_FRESH;
    CHECK(!Run(L"prepare",L"02020202020202020202020202020202"));
    CHECK(!AmdgpuTransferCheck(saved,saved_bytes,&h));CHECK(h.origin==AMDGPU_TRANSFER_ARCHIVE);
    CHECK(h.board_kind==AMDGPU_BOARD_UNPROVEN);CHECK(h.source_generation[0]==2);
    CHECK(h.transaction[0]==2);CHECK(h.source_version[0]!=0);
    Reset(AMDGPU_TRANSFER_CURRENT);failure=6;
    CHECK(Run(L"archive",tx)==ERROR_WRITE_FAULT);CHECK(!removed_bytes && freeze==1);
    Reset(AMDGPU_TRANSFER_CURRENT);device_error=ERROR_BUSY;
    CHECK(Run(L"archive",tx)==ERROR_BUSY);CHECK(archives==0);
    Reset(AMDGPU_TRANSFER_CURRENT);CHECK(Run(L"erase-everything",tx)==ERROR_INVALID_PARAMETER);
    CHECK(Run(L"prepare",L"00000000000000000000000000000000")==ERROR_INVALID_PARAMETER);
    CHECK(Run(L"prepare",L"../redirect")==ERROR_INVALID_PARAMETER);
    Reset(AMDGPU_TRANSFER_CURRENT);current_package=TRUE;
    CHECK(!Roll(L"rollback-capture"));CHECK(rb_bytes && active_bytes && freeze==1 && stop==1);
    CHECK(!Roll(L"rollback-check"));CHECK(!Roll(L"rollback-remove"));CHECK(source_removed==1);
    CHECK(Run(L"prepare",tx)==ERROR_BUSY);
    CHECK(Roll(L"rollback-restore")==ERROR_RETRY);CHECK(!restores && !enabled);
    AmdgpuInstallerTestBoot++;target_error=ERROR_REVISION_MISMATCH;
    CHECK(Roll(L"rollback-restore")==ERROR_REVISION_MISMATCH);CHECK(!restores);
    target_error=0;restore_error=ERROR_ACCESS_DENIED;
    CHECK(Roll(L"rollback-restore")==ERROR_ACCESS_DENIED);CHECK(!retired && !enabled);
    restore_error=0;retire_error=ERROR_WRITE_FAULT;
    CHECK(Roll(L"rollback-restore")==ERROR_WRITE_FAULT);CHECK(!complete_bytes && !enabled);
    retire_error=0;complete_error=ERROR_WRITE_FAULT;
    CHECK(Roll(L"rollback-restore")==ERROR_WRITE_FAULT);CHECK(retired && !complete_bytes && !enabled);
    complete_error=0;CHECK(!Roll(L"rollback-restore"));CHECK(complete_bytes && !enabled);
    CHECK(!Roll(L"rollback-enable"));CHECK(enabled && !active_bytes);
    CHECK(!Roll(L"rollback-enable"));CHECK(enabled==1);
    CHECK(Roll(L"rollback-remove")==ERROR_REVISION_MISMATCH);
    CHECK(!Roll(L"rollback-restore"));CHECK(enabled==1);
    {DWORD calls=0;failure=0;CHECK(!AmdgpuRollbackConvert(rollback,rb_bytes,ConvertWrite,&calls));CHECK(calls==AmdgpuStateCount());
     for(i=1;i<=AmdgpuStateCount();i++){calls=0;failure=i;CHECK(AmdgpuRollbackConvert(rollback,rb_bytes,ConvertWrite,&calls)==ERROR_WRITE_FAULT);CHECK(calls==i);}}
    Reset(AMDGPU_TRANSFER_CURRENT);current_package=TRUE;failure=7;
    CHECK(Roll(L"rollback-capture")==ERROR_WRITE_FAULT);CHECK(stop && !enabled && !rb_bytes);
    Reset(AMDGPU_TRANSFER_CURRENT);current_package=TRUE;board_error=ERROR_INVALID_DATA;
    CHECK(Roll(L"rollback-capture")==ERROR_INVALID_DATA);CHECK(!rb_bytes && !source_removed && !enabled);
    printf("installer worker: %u checks, %u failures\n",checks,failed);return failed?1:0;
}
