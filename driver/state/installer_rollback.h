// Fixed protected rollback envelope. Only the installer links this contract.
#pragma once
#include <windows.h>
#include "transfer.h"
#define AMDGPU_ROLLBACK_MAGIC 0x31425241u
typedef struct amdgpu_rollback {
    DWORD magic,version,bytes,snapshot_bytes;
    BYTE inf_hash[32],sys_hash[32],manifest_hash[32];
    WCHAR source_inf[32];BYTE source_inf_hash[32];
    AMDGPU_TRANSFER_HEADER snapshot; // followed by the transfer entries
} AMDGPU_ROLLBACK;
#define AMDGPU_ROLLBACK_PREFIX FIELD_OFFSET(AMDGPU_ROLLBACK,snapshot)
#define AMDGPU_ROLLBACK_MAX (AMDGPU_ROLLBACK_PREFIX+AMDGPU_TRANSFER_MAX_BYTES)
DWORD AmdgpuRollbackCheck(const void *data,DWORD bytes);
// plan visits every persistent value, including explicit absence. It never
// copies volatile boot confirmations or diagnostic records to the new boot.
typedef DWORD (*AMDGPU_ROLLBACK_WRITE)(void*,const AMDGPU_STATE_SPEC*,BOOL,const void*,DWORD);
DWORD AmdgpuRollbackConvert(const void *data,DWORD bytes,AMDGPU_ROLLBACK_WRITE write,void *context);
DWORD AmdgpuWindowsRollbackRead(const BYTE tx[16],BOOL complete,void **data,DWORD *bytes);
DWORD AmdgpuWindowsRollbackSave(const void *data,DWORD bytes,BOOL complete);
DWORD AmdgpuWindowsRollbackClear(const void *data,DWORD bytes);
DWORD AmdgpuWindowsRollbackBoard(void);
DWORD AmdgpuInstallerRollbackRestore(const void *data,DWORD bytes);
