// Settings and records use the common state ABI. No service paths are rebuilt.
#pragma once
#include "../../../driver/state/client.h"

static DWORD ReadCuStateDword(const char* name, bool effective, DWORD* data, bool* present)
{
    AMDGPU_WINDOWS_STATE* state = nullptr;
    AMDGPU_STATE_VALUE value = {};
    *present = false;
    *data = 0;
    DWORD error = AmdgpuClientOpen(&state, 0);
    if (error) return error;
    error = AmdgpuClientRead(state, "", name, effective ? 1 : 0, &value, data, sizeof(*data));
    AmdgpuClientClose(state);
    if (value.status == AMDGPU_STATE_ABSENT && !effective) return ERROR_SUCCESS;
    if (error) return error;
    if (value.status != AMDGPU_STATE_OK || value.type != AMDGPU_STATE_DWORD || value.bytes != sizeof(*data))
        return ERROR_INVALID_DATA;
    *present = true;
    return ERROR_SUCCESS;
}

static DWORD WriteCuState(DWORD mode, bool haveDisable, DWORD disable, DWORD* rollbackError)
{
    static const char* const names[] = { "CuMode", "CuDisableWgp", "CuModeConfirmed" };
    DWORD before[3] = {}, wanted[3] = { mode, disable, 0 };
    bool present[3] = {}, wantPresent[3] = { true, haveDisable, false };
    AMDGPU_CLIENT_BATCH* batch = nullptr;
    *rollbackError = 0;
    DWORD error = AmdgpuClientBeginBatch(&batch);
    if (error) return error;
    for (unsigned i = 0; i < 3; ++i) {
        AMDGPU_STATE_VALUE value = {};
        error = AmdgpuClientBatchRead(batch, "", names[i], 0, &value, &before[i], sizeof(before[i]));
        if (value.status == AMDGPU_STATE_ABSENT) { error = 0; continue; }
        if (error) break;
        if (value.status != AMDGPU_STATE_OK || value.type != AMDGPU_STATE_DWORD || value.bytes != sizeof(DWORD)) {
            error = ERROR_INVALID_DATA; break;
        }
        present[i] = true;
    }
    bool changed = false;
    if (!error) for (unsigned i = 0; i < 3; ++i) {
        // A failed persistence flush can follow a successful value write.
        changed = true;
        error = wantPresent[i] ? AmdgpuClientBatchWrite(batch, "", names[i], AMDGPU_STATE_DWORD, &wanted[i], sizeof(DWORD))
                               : AmdgpuClientBatchClear(batch, "", names[i]);
        if (error) break;
    }
    if (!error) for (unsigned i = 0; i < 3; ++i) {
        AMDGPU_STATE_VALUE value = {}; DWORD readback = 0;
        error = AmdgpuClientBatchRead(batch, "", names[i], 0, &value, &readback, sizeof(readback));
        if (!wantPresent[i] && value.status == AMDGPU_STATE_ABSENT) { error = 0; continue; }
        if (error) break;
        if (!wantPresent[i] || value.status != AMDGPU_STATE_OK || value.type != AMDGPU_STATE_DWORD ||
            value.bytes != sizeof(DWORD) || readback != wanted[i]) { error = ERROR_INVALID_DATA; break; }
    }
    if (error && changed) for (unsigned i = 0; i < 3; ++i) {
        DWORD restored = present[i] ? AmdgpuClientBatchWrite(batch, "", names[i], AMDGPU_STATE_DWORD, &before[i], sizeof(DWORD))
                                    : AmdgpuClientBatchClear(batch, "", names[i]);
        if (restored && !*rollbackError) *rollbackError = restored;
    }
    DWORD end = AmdgpuClientEndBatch(batch);
    return error ? error : end;
}
