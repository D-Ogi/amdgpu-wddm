#pragma once
#include "../../state/client.h"

struct RouterKmdState {
    bool blit_on, interop_on;
    const char *source;
};

static inline RouterKmdState ReadRouterKmdState()
{
    RouterKmdState result = { false, false, "unreadable" };
    AMDGPU_WINDOWS_STATE *state = nullptr;
    if (AmdgpuClientOpen(&state, 0) != ERROR_SUCCESS) return result;
    AMDGPU_STATE_VALUE value = {};
    DWORD bits = 0;
    DWORD error = AmdgpuClientRead(state, "", "InteropLastState", 0, &value, &bits, sizeof(bits));
    if (!error && value.status == AMDGPU_STATE_OK) {
        result.source = "latched";
        result.blit_on = (bits & 1) != 0;
        result.interop_on = (bits & 2) != 0;
    } else if (value.status == AMDGPU_STATE_ABSENT) {
        // Only a missing record permits reading the effective settings. Errors
        // never authorize defaults or the older driver's settings route.
        result.source = "settings";
        error = AmdgpuClientRead(state, "", "EnableGpuPresentBlit", 1, &value, &bits, sizeof(bits));
        result.blit_on = !error && value.status == AMDGPU_STATE_OK && bits == 1;
        const bool blit_error = error && value.status != AMDGPU_STATE_ABSENT;
        error = AmdgpuClientRead(state, "", "EnableCddDwmInterop", 1, &value, &bits, sizeof(bits));
        result.interop_on = !error && value.status == AMDGPU_STATE_OK && bits == 1;
        if (blit_error || (error && value.status != AMDGPU_STATE_ABSENT)) {
            result.source = "settings-invalid";
            result.blit_on = result.interop_on = false;
        }
    } else result.source = "latched-invalid";
    AmdgpuClientClose(state);
    return result;
}
