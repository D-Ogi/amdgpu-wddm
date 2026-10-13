// Versioned C ABI shared by the CLI, control app and installer. No registry
// handles or reconstructed paths cross this boundary.
#pragma once
#include "state_windows.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AMDGPU_CLIENT_ABI 1u
#ifdef AMDGPU_CLIENT_EXPORTS
#define AMDGPU_CLIENT_API __declspec(dllexport)
#else
#define AMDGPU_CLIENT_API
#endif
typedef struct amdgpu_client_spec {
    unsigned int abi,bytes,kind,type,max_bytes,access,private_read,group;
    char path[128],name[128];
} AMDGPU_CLIENT_SPEC;
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientCount(void);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientSpec(DWORD index,AMDGPU_CLIENT_SPEC *out,DWORD bytes);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientOpen(AMDGPU_WINDOWS_STATE **out,DWORD writable);
AMDGPU_CLIENT_API void WINAPI AmdgpuClientClose(AMDGPU_WINDOWS_STATE *state);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientRead(AMDGPU_WINDOWS_STATE *state,const char *path,const char *name,
    DWORD effective,AMDGPU_STATE_VALUE *value,void *data,DWORD bytes);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientWrite(AMDGPU_WINDOWS_STATE *state,const char *path,const char *name,
    DWORD type,const void *data,DWORD bytes);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientClear(AMDGPU_WINDOWS_STATE *state,const char *path,const char *name);
// A batch retains one generation and the common installer/writer mutex from
// backup through mutation, verification and any rollback. Every call including
// End must run on Begin's thread. No crash atomicity or automatic rollback.
typedef struct amdgpu_client_batch AMDGPU_CLIENT_BATCH;
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientBeginBatch(AMDGPU_CLIENT_BATCH **out);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientEndBatch(AMDGPU_CLIENT_BATCH *batch);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientBatchRead(AMDGPU_CLIENT_BATCH *batch,const char *path,const char *name,
    DWORD effective,AMDGPU_STATE_VALUE *value,void *data,DWORD bytes);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientBatchWrite(AMDGPU_CLIENT_BATCH *batch,const char *path,const char *name,
    DWORD type,const void *data,DWORD bytes);
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientBatchClear(AMDGPU_CLIENT_BATCH *batch,const char *path,const char *name);
// OS-returned shared data directory plus the fixed Logs child. No directory is
// created and no caller-supplied path is opened.
AMDGPU_CLIENT_API DWORD WINAPI AmdgpuClientLogDirectory(WCHAR *out,DWORD cch);
#ifdef __cplusplus
}
#endif
