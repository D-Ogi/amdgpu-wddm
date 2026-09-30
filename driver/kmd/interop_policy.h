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
