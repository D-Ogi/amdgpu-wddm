// Executes the production legacy registry writer against a memory-only Win32
// adapter. No HKLM/HKCU, service or filesystem operation can reach the host.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "installer_rollback.h"
typedef struct value {WCHAR path[128],name[128];DWORD type,bytes;BYTE data[65536];BOOL present;} VALUE;
static VALUE Values[256];static DWORD Count,Calls,FailAt,Checks,Failures,BoardError;
static WCHAR CurrentPath[128];
#define CHECK(x) do{Checks++;if(!(x)){printf("FAIL %u line %u: %s\n",Checks,__LINE__,#x);Failures++;}}while(0)
static BOOL Fault(void){return ++Calls==FailAt;}
static VALUE *Value(PCWSTR name)
{DWORD i;for(i=0;i<Count;i++)if(!wcscmp(Values[i].path,CurrentPath) && !wcscmp(Values[i].name,name))return &Values[i];
 if(Count==256)return NULL;wcscpy_s(Values[Count].path,128,CurrentPath);wcscpy_s(Values[Count].name,128,name);return &Values[Count++];}
static LSTATUS WINAPI OpenKey(HKEY root,LPCWSTR path,DWORD options,REGSAM access,PHKEY out)
{(void)options;(void)access;if(Fault())return ERROR_ACCESS_DENIED;
 if(root==HKEY_LOCAL_MACHINE){CHECK(!wcscmp(path,L"SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters"));CurrentPath[0]=0;*out=(HKEY)1;}
 else {wcscpy_s(CurrentPath,128,path);*out=(HKEY)2;}return 0;}
static LSTATUS WINAPI CreateKey(HKEY root,LPCWSTR path,DWORD reserved,LPWSTR cls,DWORD options,REGSAM access,
 const LPSECURITY_ATTRIBUTES sa,PHKEY out,LPDWORD disposition)
{(void)reserved;(void)cls;(void)sa;(void)disposition;return OpenKey(root,path,options,access,out);}
static LSTATUS WINAPI SetValue(HKEY key,LPCWSTR name,DWORD reserved,DWORD type,const BYTE *data,DWORD bytes)
{VALUE *v;(void)key;(void)reserved;if(Fault())return ERROR_WRITE_FAULT;v=Value(name);if(!v || bytes>sizeof(v->data))return ERROR_MORE_DATA;
 v->type=type;v->bytes=bytes;v->present=TRUE;memcpy(v->data,data,bytes);return 0;}
static LSTATUS WINAPI DeleteValue(HKEY key,LPCWSTR name)
{VALUE *v;(void)key;if(Fault())return ERROR_WRITE_FAULT;v=Value(name);v->present=FALSE;return 0;}
static LSTATUS WINAPI QueryValue(HKEY key,LPCWSTR name,LPDWORD reserved,LPDWORD type,LPBYTE data,LPDWORD bytes)
{VALUE *v;(void)key;(void)reserved;if(Fault())return ERROR_READ_FAULT;v=Value(name);if(!v->present)return ERROR_FILE_NOT_FOUND;
 *type=v->type;if(*bytes<v->bytes){*bytes=v->bytes;return ERROR_MORE_DATA;}*bytes=v->bytes;if(data)memcpy(data,v->data,v->bytes);return 0;}
static LSTATUS WINAPI Flush(HKEY key){(void)key;return Fault()?ERROR_WRITE_FAULT:0;}
static LSTATUS WINAPI Close(HKEY key){(void)key;return 0;}
#define RegOpenKeyExW OpenKey
#define RegCreateKeyExW CreateKey
#define RegSetValueExW SetValue
#define RegDeleteValueW DeleteValue
#define RegQueryValueExW QueryValue
#define RegFlushKey Flush
#define RegCloseKey Close
#include "installer_store.c"
DWORD AmdgpuWindowsRollbackBoard(void){return BoardError;}
static UINT Fetch(void *context,UINT source,const AMDGPU_STATE_SPEC *spec,AMDGPU_STATE_VALUE *v,void *data,UINT capacity)
{DWORD value=0;(void)context;(void)source;(void)capacity;
 if(!spec->path[0] && (!strcmp(spec->name,"CpuTune") || !strcmp(spec->name,"UnconfirmedStarts") || !strcmp(spec->name,"CpuPending"))) {
     if(strcmp(spec->name,"CpuTune"))value=2;memcpy(data,&value,4);v->type=4;v->bytes=4;return 0;}
 return AMDGPU_STATE_ABSENT;}
int main(void)
{
    AMDGPU_ROLLBACK *r=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,AMDGPU_ROLLBACK_MAX);AMDGPU_TRANSFER_HEADER h={0};UINT n;DWORD total,i;
    h.origin=AMDGPU_TRANSFER_CURRENT;h.source_schema=AmdgpuStateSchema();memset(h.transaction,1,16);memset(h.source_generation,2,16);
    memset(h.device_hash,7,32);AmdgpuStateBootToken(91,h.source_boot);memcpy(h.catalog,AmdgpuStateCatalogId(),64);
    strcpy_s(h.source_version,32,"0.7.216.103");strcpy_s(h.destination_version,32,"0.7.216.102");
    CHECK(!AmdgpuTransferCapture(&h,Fetch,NULL,&r->snapshot,AMDGPU_TRANSFER_MAX_BYTES,&n));
    r->magic=AMDGPU_ROLLBACK_MAGIC;r->version=1;r->snapshot_bytes=n;r->bytes=AMDGPU_ROLLBACK_PREFIX+n;
    memset(r->inf_hash,1,96);wcscpy_s(r->source_inf,32,L"oem17.inf");memset(r->source_inf_hash,4,32);
    CHECK(!AmdgpuInstallerRollbackRestore(r,r->bytes));total=Calls;CurrentPath[0]=0;
    CHECK(!Value(L"CpuTune")->present);CHECK(Value(L"UnconfirmedStarts")->present && *(DWORD*)Value(L"UnconfirmedStarts")->data==2);
    CHECK(Value(L"CpuPending")->present && *(DWORD*)Value(L"CpuPending")->data==2);
    CHECK(!Value(L"LastStage")->present);CHECK(Value(L"DpmMaxMHz")->present && *(DWORD*)Value(L"DpmMaxMHz")->data==2000);
    BoardError=ERROR_INVALID_DATA;Calls=0;CHECK(AmdgpuInstallerRollbackRestore(r,r->bytes)==ERROR_INVALID_DATA);CHECK(!Calls);BoardError=0;
    for(i=1;i<=total;i++){Calls=0;FailAt=i;CHECK(AmdgpuInstallerRollbackRestore(r,r->bytes)!=0);FailAt=0;Calls=0;CHECK(!AmdgpuInstallerRollbackRestore(r,r->bytes));}
    HeapFree(GetProcessHeap(),0,r);printf("installer legacy adapter: %lu checks, %lu failures\n",Checks,Failures);return Failures?1:0;
}
