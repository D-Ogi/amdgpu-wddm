// Linked only into bc250d3d_router-test.dll. The harness redirects HKLM to a
// private application hive before loading this DLL. No SCM or GPU calls.
#include "../../../state/client.h"
#include <cstring>
#ifndef BC250_ROUTER_STATE_FIXTURE
#error This state double must never be linked into a production router.
#endif
extern "C" __declspec(dllexport) DWORD WINAPI Bc250RouterStateFixture(void) { return 1; }
DWORD WINAPI AmdgpuClientOpen(AMDGPU_WINDOWS_STATE **state, DWORD writable)
{
    HKEY key = nullptr;
    *state = nullptr;
    if (writable) return ERROR_ACCESS_DENIED;
    LSTATUS error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\amdgpu-wddm\\RouterTestState",0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY,&key);
    if (!error) *state = reinterpret_cast<AMDGPU_WINDOWS_STATE *>(key);
    return error;
}
void WINAPI AmdgpuClientClose(AMDGPU_WINDOWS_STATE *state)
{ if (state) RegCloseKey(reinterpret_cast<HKEY>(state)); }
DWORD WINAPI AmdgpuClientRead(AMDGPU_WINDOWS_STATE *state,const char *path,const char *name,DWORD effective,
    AMDGPU_STATE_VALUE *v,void *data,DWORD bytes)
{
    ZeroMemory(v,sizeof(*v));v->status=AMDGPU_STATE_UNKNOWN;
    const WCHAR *wide = !strcmp(name,"InteropLastState") ? L"InteropLastState" :
        !strcmp(name,"EnableGpuPresentBlit") ? L"EnableGpuPresentBlit" :
        !strcmp(name,"EnableCddDwmInterop") ? L"EnableCddDwmInterop" : nullptr;
    if (path[0] || !wide || effective>1) return ERROR_INVALID_PARAMETER;
    DWORD type=0, size=bytes;
    LSTATUS error=RegQueryValueExW(reinterpret_cast<HKEY>(state),wide,nullptr,&type,(BYTE *)data,&size);
    v->type=type;v->bytes=size;v->os_error=error;v->source=AMDGPU_STATE_OVERRIDE;
    if(error==ERROR_FILE_NOT_FOUND){v->status=AMDGPU_STATE_ABSENT;return error;}
    if(error){v->status=AMDGPU_STATE_IO;return error;}
    if(type!=REG_DWORD || size!=sizeof(DWORD)){v->status=AMDGPU_STATE_TYPE;return ERROR_INVALID_DATA;}
    v->status=AMDGPU_STATE_OK;return ERROR_SUCCESS;
}
