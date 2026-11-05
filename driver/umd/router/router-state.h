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

// What the logon images' gates read from the kernel driver's state (router-policy.h StartHealthOf, LatchView). Only
// a value that reads OK counts. An absent UnconfirmedStarts is unreadable (every start of a kernel driver of this
// package writes it), an absent GuardBoot\Confirmed is "not confirmed in this boot", and an absent AdapterRestarts is
// 0 (no adapter restart was ever recorded). Any other status is an unknown value, which closes its gate.
struct RouterStartState {
    bool open;                 // the state client opened
    bool unconfirmed_known;
    unsigned long unconfirmed; // Parameters UnconfirmedStarts
    bool boot_known;
    unsigned long boot;        // GuardBoot\Confirmed
    bool restarts_known;
    unsigned long restarts;    // HangRecovery\AdapterRestarts
};

static inline RouterStartState ReadRouterStartState()
{
    RouterStartState r = {};
    AMDGPU_WINDOWS_STATE *state = nullptr;
    if (AmdgpuClientOpen(&state, 0) != ERROR_SUCCESS) return r;
    r.open = true;
    AMDGPU_STATE_VALUE value = {};
    DWORD v = 0;
    (void)AmdgpuClientRead(state, "", "UnconfirmedStarts", 0, &value, &v, sizeof(v));
    if (value.status == AMDGPU_STATE_OK) { r.unconfirmed_known = true; r.unconfirmed = v; }
    v = 0; value = {};
    (void)AmdgpuClientRead(state, "GuardBoot", "Confirmed", 0, &value, &v, sizeof(v));
    if (value.status == AMDGPU_STATE_OK) { r.boot_known = true; r.boot = v; }
    else if (value.status == AMDGPU_STATE_ABSENT) { r.boot_known = true; r.boot = 0; }
    v = 0; value = {};
    (void)AmdgpuClientRead(state, "HangRecovery", "AdapterRestarts", 0, &value, &v, sizeof(v));
    if (value.status == AMDGPU_STATE_OK) { r.restarts_known = true; r.restarts = v; }
    else if (value.status == AMDGPU_STATE_ABSENT) { r.restarts_known = true; r.restarts = 0; }
    AmdgpuClientClose(state);
    return r;
}
