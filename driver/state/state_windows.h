// User-mode adapter. Works without an active adapter, including Code 43.
// Handles and paths remain private; callers cannot redirect the service store.
#pragma once
#include <windows.h>
#include "state.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct amdgpu_windows_state AMDGPU_WINDOWS_STATE;
unsigned int AmdgpuWindowsStateOpen(AMDGPU_WINDOWS_STATE **state, unsigned int writable, DWORD *error);
// The active generation as a migration source: the same schema under any well-formed catalog identity. The
// installer worker reads the installed settings by name before it stages a package with another catalog and
// captures them for the KMD's transaction. Value writes, CheckWrite and Retire are refused on this handle;
// freeze, identity and capture work. Every other reader uses AmdgpuWindowsStateOpen.
unsigned int AmdgpuWindowsStateOpenSource(AMDGPU_WINDOWS_STATE **state, unsigned int writable, DWORD *error);
void AmdgpuWindowsStateClose(AMDGPU_WINDOWS_STATE *state);
// Validate the retained generation and unfrozen writer boundary without writing.
// A multi-call client must also retain the common writer lock across its calls.
unsigned int AmdgpuWindowsStateCheckWrite(AMDGPU_WINDOWS_STATE *state,DWORD *error);
unsigned int AmdgpuWindowsStateQuery(AMDGPU_WINDOWS_STATE *state, const char *path, const char *name,
    unsigned int mode, AMDGPU_STATE_VALUE *value, void *data, unsigned int capacity);
unsigned int AmdgpuWindowsStateSet(AMDGPU_WINDOWS_STATE *state, const char *path, const char *name,
    unsigned int type, const void *data, unsigned int bytes, DWORD *error);
unsigned int AmdgpuWindowsStateClear(AMDGPU_WINDOWS_STATE *state, const char *path, const char *name,
    DWORD *error);
#ifdef __cplusplus
}
#endif
