#include "client.h"
#include "windows_lock.h"
#include <string.h>
#ifdef AMDGPU_CLIENT_EXPORTS
#define API __declspec(dllexport)
#else
#define API
#endif

static DWORD Error(unsigned int result,DWORD error)
{
    if(result==AMDGPU_STATE_OK)return ERROR_SUCCESS;
    if(error && result!=AMDGPU_STATE_SCHEMA)return error;
    switch(result) {
    case AMDGPU_STATE_ABSENT:return ERROR_FILE_NOT_FOUND;
    case AMDGPU_STATE_ACCESS:return ERROR_ACCESS_DENIED;
    case AMDGPU_STATE_SCHEMA:return ERROR_REVISION_MISMATCH;
    case AMDGPU_STATE_INCOMPLETE:return ERROR_NOT_READY;
    case AMDGPU_STATE_BUFFER:return ERROR_MORE_DATA;
    case AMDGPU_STATE_TYPE:case AMDGPU_STATE_RANGE:return ERROR_INVALID_DATA;
    default:return ERROR_INVALID_PARAMETER;
    }
}
API DWORD WINAPI AmdgpuClientCount(void){return AmdgpuStateCount();}
API DWORD WINAPI AmdgpuClientSpec(DWORD index,AMDGPU_CLIENT_SPEC *out,DWORD bytes)
{
    const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(index);
    if(!out || bytes!=sizeof(*out) || !spec)return ERROR_INVALID_PARAMETER;
    ZeroMemory(out,sizeof(*out));out->abi=AMDGPU_CLIENT_ABI;out->bytes=sizeof(*out);
    out->kind=spec->kind;out->type=spec->type;out->max_bytes=spec->max_bytes;
    out->access=spec->access;out->private_read=spec->private_read;out->group=spec->group;
    if(strlen(spec->path)>=sizeof(out->path) || strlen(spec->name)>=sizeof(out->name))return ERROR_INVALID_DATA;
    memcpy(out->path,spec->path,strlen(spec->path)+1);memcpy(out->name,spec->name,strlen(spec->name)+1);
    return ERROR_SUCCESS;
}
API DWORD WINAPI AmdgpuClientOpen(AMDGPU_WINDOWS_STATE **out,DWORD writable)
{
    DWORD error=0;unsigned int result;
    if(writable>1)return ERROR_INVALID_PARAMETER;
    result=AmdgpuWindowsStateOpen(out,writable,&error);return Error(result,error);
}
API void WINAPI AmdgpuClientClose(AMDGPU_WINDOWS_STATE *state){AmdgpuWindowsStateClose(state);}
API DWORD WINAPI AmdgpuClientRead(AMDGPU_WINDOWS_STATE *state,const char *path,const char *name,
    DWORD effective,AMDGPU_STATE_VALUE *value,void *data,DWORD bytes)
{
    const AMDGPU_STATE_SPEC *spec;unsigned int mode,result;
    if(!value)return ERROR_INVALID_PARAMETER;
    ZeroMemory(value,sizeof(*value));value->status=AMDGPU_STATE_INVALID_ARGUMENT;
    if(effective>1)return ERROR_INVALID_PARAMETER;
    spec=AmdgpuStateFind(path,name);
    if(!spec)return ERROR_INVALID_PARAMETER;
    mode=spec->kind==AMDGPU_STATE_SETTING?(effective?AMDGPU_STATE_QUERY_EFFECTIVE:AMDGPU_STATE_QUERY_OVERRIDE):AMDGPU_STATE_READ_RECORD;
    result=AmdgpuWindowsStateQuery(state,path,name,mode,value,data,bytes);return Error(result,value->os_error);
}
API DWORD WINAPI AmdgpuClientWrite(AMDGPU_WINDOWS_STATE *state,const char *path,const char *name,
    DWORD type,const void *data,DWORD bytes)
{
    DWORD error=0;unsigned int result=AmdgpuWindowsStateSet(state,path,name,type,data,bytes,&error);
    return Error(result,error);
}
API DWORD WINAPI AmdgpuClientClear(AMDGPU_WINDOWS_STATE *state,const char *path,const char *name)
{
    DWORD error=0;unsigned int result=AmdgpuWindowsStateClear(state,path,name,&error);return Error(result,error);
}
struct amdgpu_client_batch {
    AMDGPU_WINDOWS_STATE *state;
    HANDLE writer;
    DWORD thread;
};
static DWORD BatchCheck(AMDGPU_CLIENT_BATCH *batch)
{
    DWORD error=0;unsigned int result;
    if(!batch)return ERROR_INVALID_PARAMETER;
    if(batch->thread!=GetCurrentThreadId())return ERROR_INVALID_THREAD_ID;
    result=AmdgpuWindowsStateCheckWrite(batch->state,&error);return Error(result,error);
}
API DWORD WINAPI AmdgpuClientBeginBatch(AMDGPU_CLIENT_BATCH **out)
{
    AMDGPU_CLIENT_BATCH *batch;DWORD error;
    if(!out)return ERROR_INVALID_PARAMETER;*out=NULL;
    batch=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*batch));
    if(!batch)return ERROR_NOT_ENOUGH_MEMORY;
    batch->thread=GetCurrentThreadId();
    error=AmdgpuStateWriterLock(&batch->writer);
    if(!error)error=AmdgpuClientOpen(&batch->state,1);
    if(!error)error=BatchCheck(batch);
    if(error){AmdgpuClientEndBatch(batch);return error;}
    *out=batch;return ERROR_SUCCESS;
}
API DWORD WINAPI AmdgpuClientEndBatch(AMDGPU_CLIENT_BATCH *batch)
{
    if(!batch)return ERROR_SUCCESS;
    if(batch->thread!=GetCurrentThreadId())return ERROR_INVALID_THREAD_ID;
    AmdgpuClientClose(batch->state);AmdgpuStateWriterUnlock(batch->writer);
    HeapFree(GetProcessHeap(),0,batch);return ERROR_SUCCESS;
}
API DWORD WINAPI AmdgpuClientBatchRead(AMDGPU_CLIENT_BATCH *batch,const char *path,const char *name,
    DWORD effective,AMDGPU_STATE_VALUE *value,void *data,DWORD bytes)
{
    DWORD error=BatchCheck(batch);
    if(error) {
        if(value){ZeroMemory(value,sizeof(*value));value->status=AMDGPU_STATE_IO;value->os_error=error;}
        if(data && bytes)SecureZeroMemory(data,bytes);
        return error;
    }
    return AmdgpuClientRead(batch->state,path,name,effective,value,data,bytes);
}
API DWORD WINAPI AmdgpuClientBatchWrite(AMDGPU_CLIENT_BATCH *batch,const char *path,const char *name,
    DWORD type,const void *data,DWORD bytes)
{
    DWORD error=BatchCheck(batch);
    return error?error:AmdgpuClientWrite(batch->state,path,name,type,data,bytes);
}
API DWORD WINAPI AmdgpuClientBatchClear(AMDGPU_CLIENT_BATCH *batch,const char *path,const char *name)
{
    DWORD error=BatchCheck(batch);
    return error?error:AmdgpuClientClear(batch->state,path,name);
}
API DWORD WINAPI AmdgpuClientLogDirectory(WCHAR *out,DWORD cch)
{
    SC_HANDLE manager,service;DWORD error,needed=0;size_t length;
    if(!out || cch<7)return ERROR_INVALID_PARAMETER;
    out[0]=0;
    manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT);
    if(!manager)return GetLastError();
    service=OpenServiceW(manager,L"bc250kmd",SERVICE_QUERY_STATUS);
    if(!service){error=GetLastError();CloseServiceHandle(manager);return error;}
    error=GetSharedServiceDirectory(service,ServiceSharedDirectoryPersistentState,out,cch-5,&needed);
    CloseServiceHandle(service);CloseServiceHandle(manager);
    if(error){out[0]=0;return error;}
    for(length=0;length<cch-5 && out[length];++length) {}
    if(!length || length==cch-5){out[0]=0;return ERROR_INVALID_DATA;}
    if(out[length-1]!=L'\\')out[length++]=L'\\';
    memcpy(out+length,L"Logs",5*sizeof(WCHAR));
    return ERROR_SUCCESS;
}
