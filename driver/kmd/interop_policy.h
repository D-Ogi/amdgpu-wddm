/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// The GPU DWM interop switches' start policy (docs/design/gpu-dwm-interop-switches.md). Plain C, no kernel
// header: interop.c feeds it what the registry said and carries out what it answers; the host test
// (test/interop_policy_test.c) runs the same object code's source against every input combination.
//
// Two operator switches under Services\bc250kmd\Parameters, both REG_DWORD, both start-latched:
//   EnableGpuPresentBlit   bit 1: a Blt present becomes one GPU copy (wddm.c Present, SubmitCommandVirtual BGP1)
//   EnableCddDwmInterop    bit 2: DRIVERCAPS.PresentationCaps.DriverSupportsCddDwmInterop
// Since 0.7.181 an absent value means 1 (on). 0 turns a switch off, 1 on, any other value closes both.
//
// The session marker (InteropSession) is on disk while a device that used the path is alive; a start that
// finds it, and no volatile record of the same boot, knows the machine died with the path in use and closes
// both switches durably (both written 0, InteropClosedReason 4), the way DPM falls back to fixed-lab.
#pragma once

#define BC250_INTEROP_BLIT 1u               // EnableGpuPresentBlit
#define BC250_INTEROP_CDD 2u                // EnableCddDwmInterop
#define BC250_INTEROP_ALL 3u

// Numbered like enum bc250_dpm_reason where the meaning is the same; 3 (unconfirmed), 6 and 8 are unused.
enum bc250_interop_reason {
    BC250_INTEROP_REASON_NONE = 0,              // open as requested
    BC250_INTEROP_REASON_NOT_REQUESTED = 1,     // both switches explicitly 0
    BC250_INTEROP_REASON_INVALID_SETTING = 2,   // a switch holds neither 0 nor 1: both closed this start
    BC250_INTEROP_REASON_UNCLEAN = 4,           // the last boot died with the path in use: both closed durably
    BC250_INTEROP_REASON_REGISTRY = 5,          // a switch or the marker could not be read: both closed this start
    BC250_INTEROP_REASON_NOT_RUN = 7,           // no full WDDM start (display-only, or before the first start)
    BC250_INTEROP_REASON_COUNT = 8
};

// What a registry read gave.
enum bc250_interop_read {
    BC250_INTEROP_ABSENT = 0,                   // STATUS_OBJECT_NAME_NOT_FOUND
    BC250_INTEROP_PRESENT = 1,                  // a REG_DWORD, value valid
    BC250_INTEROP_UNREADABLE = 2                // any other failure, a wrong type included
};

struct bc250_interop_value {
    unsigned int state;                         // enum bc250_interop_read
    unsigned int value;                         // PRESENT only
};

struct bc250_interop_inputs {
    struct bc250_interop_value blit, cdd;       // the two operator switches
    struct bc250_interop_value session;         // InteropSession: present while a session is marked
    struct bc250_interop_value closed;          // InteropClosedReason: the driver closed the switches itself
    int boot_record;                            // the volatile record says this boot marked a session already
};

// Decision flags; the escape's BC250_INTEROP_FLAG_* carry the same bit values.
#define BC250_INTEROP_DECISION_UNCLEAN 4u           // a marker of an earlier boot: it died in a session
#define BC250_INTEROP_DECISION_STALE 8u             // a marker of this boot: an earlier start ended without unmark
#define BC250_INTEROP_DECISION_CLOSED_BY_DRIVER 16u // the switches are 0 because the driver wrote them so

struct bc250_interop_decision {
    unsigned int requested;                     // BC250_INTEROP_* bits the switches ask for
    unsigned int effective;                     // what this start runs with; always a subset of requested
    unsigned int reason;                        // enum bc250_interop_reason
    unsigned int closed_reason;                 // InteropClosedReason after the decision, 0 when none
    unsigned int flags;                         // BC250_INTEROP_DECISION_*
    int persist_close;                          // write both switches 0 and InteropClosedReason = reason
    int clear_closed;                           // delete InteropClosedReason: the operator opened them again
    int clear_session;                          // delete InteropSession (only after persist_close succeeded)
};

void bc250_interop_decide(const struct bc250_interop_inputs* in, struct bc250_interop_decision* out);
const char* bc250_interop_reason_name(unsigned int reason);

// The session marker's life within one start (BD-059). interop.c feeds every event in, under its lock, and
// carries out the answer: write the marker (mark), or delete it with InteropLastEnd = unmark. It sets marked
// itself, after the registry write or delete succeeded, so that a failed one is retried by the next event.
//
// A system power transition ends the session: at a clean restart DWM's devices are never destroyed and dxgkrnl
// does not stop the adapter, so the marker goes at the transition's start, while the registry is still up
// (\Callback\PowerState PO_CB_SYSTEM_STATE_LOCK, then the adapter's D3 for the system action), and no event marks
// again until the system is back in S0. A machine that dies with the path in use dies before any of these, so
// its marker stays.
enum bc250_interop_event {
    BC250_INTEROP_EVENT_BEGIN = 1,          // the first interop Blt present of a DDI device: one more user
    BC250_INTEROP_EVENT_END = 2,            // DestroyDevice of a counted device: one user less
    BC250_INTEROP_EVENT_STOP = 3,           // the device stops: no user is left
    BC250_INTEROP_EVENT_SYSTEM_DOWN = 4,    // \Callback\PowerState: a system sleep or shutdown is imminent
    BC250_INTEROP_EVENT_ADAPTER_DOWN = 5,   // DxgkDdiSetPowerState: the adapter goes to D1-D3 for a system action
    BC250_INTEROP_EVENT_UP = 6              // the system (callback) or the adapter (D0) is back
};

// InteropLastEnd: how a session ended. bc250kmd_escape.h's BC250_INTEROP_END_* carry the same values.
#define BC250_INTEROP_SESSION_END_NONE 0u
#define BC250_INTEROP_SESSION_END_STOP 1u           // the device stopped
#define BC250_INTEROP_SESSION_END_USERS 2u          // the last counted device was destroyed (DWM exit)
#define BC250_INTEROP_SESSION_END_SYSTEM_POWER 3u   // a system sleep or shutdown began (the power callback)
#define BC250_INTEROP_SESSION_END_ADAPTER_D3 4u     // the adapter went down for a system action

struct bc250_interop_session {
    unsigned int users;                     // counted devices alive
    int marked;                             // InteropSession is on disk for this start (set by the caller)
    int down;                               // a system power transition began and has not come back: no mark
};

struct bc250_interop_step {
    int mark;                               // write the volatile record, then InteropSession
    unsigned int unmark;                    // delete InteropSession, InteropLastEnd = this; 0 = nothing
};

void bc250_interop_session_step(struct bc250_interop_session* s, unsigned int event, struct bc250_interop_step* out);
// A system action of DxgkDdiSetPowerState (POWER_ACTION values): sleep, hibernate, shutdown, reset, off.
int bc250_interop_system_action(unsigned int action);
