// SPDX-License-Identifier: MIT
// The provisional start of the logon images (router-policy.h StartHealth::Provisional, docs/design/logon-gpu-route.md).
//
// The first start of a boot counts UnconfirmedStarts from 0 to 1 (driver/kmd/guard.c GuardCheckAndCountStart), and
// the confirmation that writes 0 comes at least 60 s later (docs/design/wddm-start-confirmation.md). A count of 1 with
// no confirmation in this boot is therefore the normal first start after a boot whose start was confirmed, and the
// logon images may take the GPU UMD on it: the sign-in screen of a cold boot draws on the GPU from its first frame.
// A start after an unconfirmed one reads 2 and stays on the CPU UMD. This record stops a loop in which the provisional
// route hangs every second boot (hang, CPU boot that confirms, provisional again, hang):
//
//   HKLM\SOFTWARE\amdgpu-wddm\ProtectedStart    non-volatile; Local System (LogonUI.exe, consent.exe) writes it
//     Pending       REG_SZ     the GPU UMD path of a provisional GPU route whose start is not known to be confirmed
//     Off           REG_SZ     the GPU UMD path for which the provisional route is off
//   HKLM\SOFTWARE\amdgpu-wddm\ProtectedLatch    volatile (router-latch.h)
//     Provisional   REG_DWORD  1 = a decision of this boot wrote Pending
//
// The router writes and flushes Pending before the GPU open of a provisional route. Each later decision that reads
// a start state carries out what it finds:
//   - a confirmed start: Pending goes;
//   - Pending of an earlier boot (no Provisional mark) and UnconfirmedStarts 2 or more: the start before this one was
//     never confirmed (a hang, a crash or a power loss before the confirmation). Off takes the path and Pending goes.
//     The provisional route stays off for that GPU UMD path. A new driver package has a new path and tries again;
//   - Pending of an earlier boot and UnconfirmedStarts 0 or 1: the start before this one was confirmed. Pending goes.
// The record follows the guard's own count: a tool that writes UnconfirmedStarts 0 before a restart (the lab kit's
// restart.ps1) also clears this evidence. A process that cannot write the record (LockApp.exe,
// CredentialUIBroker.exe) does not take the provisional route; it waits for the confirmation.
#pragma once
#include <windows.h>
#include <cstring>
#include <cwchar>
#include "router-policy.h"
#include "router-latch.h"

namespace bc250router {

static const wchar_t ProvisionalKeyPath[] = L"SOFTWARE\\amdgpu-wddm\\ProtectedStart";
static const wchar_t ProvisionalPendingValue[] = L"Pending";
static const wchar_t ProvisionalOffValue[] = L"Off";
static const wchar_t ProvisionalMarkValue[] = L"Provisional";
static const size_t ProvisionalPathChars = 1024;

struct ProvisionalView {
    bool store;              // the record is open for writing
    bool off;                // Off names the GPU UMD of this decision
    bool pending_this_boot;  // Pending names that GPU UMD and this boot wrote it
    const char *state;       // "on", "off", "nostore"
    const char *action;      // "none", "cleared", "set-off", "written", "write-failed"
};

static inline bool ReadProvisionalText(HKEY key, const wchar_t *name, wchar_t *out, DWORD chars)
{
    DWORD type = 0, bytes = (chars - 1) * sizeof(wchar_t);
    ZeroMemory(out, chars * sizeof(wchar_t));
    return RegQueryValueExW(key, name, nullptr, &type, (BYTE *)out, &bytes) == ERROR_SUCCESS && type == REG_SZ &&
           out[0];
}

static inline bool ProvisionalMarkedThisBoot(REGSAM view)
{
    HKEY latch = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LatchKeyPath, 0, KEY_QUERY_VALUE | view, &latch) != ERROR_SUCCESS)
        return false;
    DWORD mark = 0;
    const bool marked = ReadLatchDword(latch, ProvisionalMarkValue, &mark) && mark == 1;
    RegCloseKey(latch);
    return marked;
}

// Reads the record for a decision that read a start state, and carries out what it finds (see above).
inline ProvisionalView ReadProvisional(REGSAM view, StartHealth start, unsigned long unconfirmed, const wchar_t *gpu)
{
    ProvisionalView v = {false, false, false, "nostore", "none"};
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ProvisionalKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_QUERY_VALUE | KEY_SET_VALUE | view, nullptr, &key, nullptr) == ERROR_SUCCESS)
        v.store = true;
    else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ProvisionalKeyPath, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS)
        key = nullptr;
    wchar_t pending[ProvisionalPathChars], off[ProvisionalPathChars];
    const bool hasPending = key && ReadProvisionalText(key, ProvisionalPendingValue, pending, ProvisionalPathChars);
    bool hasOff = key && ReadProvisionalText(key, ProvisionalOffValue, off, ProvisionalPathChars);
    if (hasPending && v.store) {
        const bool thisBoot = ProvisionalMarkedThisBoot(view);
        if (start == StartHealth::Confirmed || (!thisBoot && unconfirmed < 2)) {
            if (RegDeleteValueW(key, ProvisionalPendingValue) == ERROR_SUCCESS) v.action = "cleared";
        } else if (!thisBoot) {
            const DWORD bytes = (DWORD)((wcslen(pending) + 1) * sizeof(wchar_t));
            if (RegSetValueExW(key, ProvisionalOffValue, 0, REG_SZ, (const BYTE *)pending, bytes) == ERROR_SUCCESS) {
                RegDeleteValueW(key, ProvisionalPendingValue);
                RegFlushKey(key);
                wcscpy_s(off, pending);
                hasOff = true;
                v.action = "set-off";
            }
        } else {
            v.pending_this_boot = gpu && *gpu && !_wcsicmp(pending, gpu);
        }
    }
    v.off = hasOff && gpu && *gpu && !_wcsicmp(off, gpu);
    v.state = v.off ? "off" : v.store ? "on" : "nostore";
    if (key) RegCloseKey(key);
    return v;
}

// Before the GPU open of a provisional route: Pending on the disk, then the mark of this boot in the volatile latch
// store (HKLM only). False when either write fails; the caller then keeps the image on the CPU UMD.
inline bool WriteProvisional(REGSAM view, const LatchStore &latch, const wchar_t *gpu, bool pendingThisBoot)
{
    if (pendingThisBoot) return true;
    if (!latch.key || strcmp(latch.where, "hklm") || !gpu || !*gpu) return false;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ProvisionalKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_QUERY_VALUE | KEY_SET_VALUE | view, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const DWORD bytes = (DWORD)((wcslen(gpu) + 1) * sizeof(wchar_t));
    bool ok = RegSetValueExW(key, ProvisionalPendingValue, 0, REG_SZ, (const BYTE *)gpu, bytes) == ERROR_SUCCESS &&
              RegFlushKey(key) == ERROR_SUCCESS;
    if (ok) {
        const DWORD one = 1;
        ok = RegSetValueExW(latch.key, ProvisionalMarkValue, 0, REG_DWORD, (const BYTE *)&one, sizeof(one)) ==
             ERROR_SUCCESS;
        // Without the mark, the next decision of this boot would read Pending as a record of an earlier boot.
        if (!ok) RegDeleteValueW(key, ProvisionalPendingValue);
    }
    RegCloseKey(key);
    return ok;
}

} // namespace bc250router
