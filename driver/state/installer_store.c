// Legacy writes exist only in the explicit rollback converter, with the device
// stopped and persistently disabled. Runtime clients never use this adapter.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "installer_rollback.h"
static void Wide(const char *s,WCHAR *d){while(*s)*d++=(BYTE)*s++;*d=0;}
static DWORD WriteValue(void *context,const AMDGPU_STATE_SPEC *spec,BOOL present,const void *data,DWORD bytes)
{
    HKEY root=context,key=NULL;WCHAR path[128],name[128];DWORD error,type=0,got;void *read=NULL;
    if(strlen(spec->path)>=128 || strlen(spec->name)>=128)return ERROR_INVALID_DATA;
    Wide(spec->path,path);Wide(spec->name,name);
    if(present)error=RegCreateKeyExW(root,path,0,NULL,REG_OPTION_NON_VOLATILE,KEY_QUERY_VALUE|KEY_SET_VALUE,NULL,&key,NULL);
    else error=RegOpenKeyExW(root,path,0,KEY_QUERY_VALUE|KEY_SET_VALUE,&key);
    if(error==ERROR_FILE_NOT_FOUND && !present)return 0;if(error)return error;
    error=present?RegSetValueExW(key,name,0,spec->type,data,bytes):RegDeleteValueW(key,name);
    if(error==ERROR_FILE_NOT_FOUND && !present)error=0;if(error)goto done;
    error=RegFlushKey(key);if(error)goto done;
    if(present){read=HeapAlloc(GetProcessHeap(),0,bytes);if(!read){error=ERROR_NOT_ENOUGH_MEMORY;goto done;}}
    got=bytes;error=RegQueryValueExW(key,name,NULL,&type,read,&got);
    if(!present){error=error==ERROR_FILE_NOT_FOUND?0:error?error:ERROR_INVALID_DATA;goto done;}
    if(!error && (type!=spec->type || got!=bytes || memcmp(read,data,bytes)))error=ERROR_INVALID_DATA;
done:
    if(read)HeapFree(GetProcessHeap(),0,read);if(key)RegCloseKey(key);return error;
}
DWORD AmdgpuInstallerRollbackRestore(const void *data,DWORD bytes)
{
    HKEY root=NULL;DWORD error=AmdgpuRollbackCheck(data,bytes);if(error)return error;
    error=AmdgpuWindowsRollbackBoard();if(error)return error;
    // INF must already have created the old service and its Parameters root.
    error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters",0,
        KEY_QUERY_VALUE|KEY_SET_VALUE|KEY_CREATE_SUB_KEY|KEY_ENUMERATE_SUB_KEYS|KEY_WOW64_64KEY,&root);
    if(error)return error;
    error=AmdgpuRollbackConvert(data,bytes,WriteValue,root);
    if(!error)error=RegFlushKey(root);RegCloseKey(root);return error;
}
