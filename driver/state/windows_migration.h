// Installer-only export. Legacy paths occur here solely to read the old schema;
// they are never runtime fallbacks and this file is not linked into the KMD.
#pragma once
#include "state_windows.h"
#include "transfer.h"
unsigned int AmdgpuWindowsCaptureLegacy(const AMDGPU_TRANSFER_HEADER *metadata,
    void *buffer,unsigned int capacity,unsigned int *bytes,DWORD *error,DWORD *board_error);
// Installer comparison only, never a runtime fallback. Public persistent root DWORDs only.
unsigned int AmdgpuWindowsLegacyComparisonValue(const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,DWORD *data);
// Identity of the validated active generation. A stale handle never returns an
// identifier for the replacement generation; the caller must reopen and retry.
unsigned int AmdgpuWindowsStateIdentity(AMDGPU_WINDOWS_STATE *state,
    unsigned char generation[16],DWORD *error);
// Freeze serializes all new-schema user-mode writers. The installer stops the
// old driver before its final capture; the freeze does not freeze kernel state.
unsigned int AmdgpuWindowsStateFreeze(AMDGPU_WINDOWS_STATE *state,
    const unsigned char transaction[16],DWORD *error);
unsigned int AmdgpuWindowsStateCancelFreeze(AMDGPU_WINDOWS_STATE *state,
    const unsigned char transaction[16],DWORD *error);
// Explicit legacy rollback only: caller has stopped SCM, disabled the PDO and
// restored/read-verified the protected receipt's legacy settings and board state.
// Deletes only the retained, frozen generation's StateV2 tree, then flushes its
// OS-returned shared parent. A failure is not proof that deletion did not happen.
unsigned int AmdgpuWindowsStateRetire(AMDGPU_WINDOWS_STATE *state,
    const unsigned char transaction[16],DWORD *error);
unsigned int AmdgpuWindowsCaptureCurrent(AMDGPU_WINDOWS_STATE *state,const AMDGPU_TRANSFER_HEADER *metadata,
    void *buffer,unsigned int capacity,unsigned int *bytes,DWORD *error);
