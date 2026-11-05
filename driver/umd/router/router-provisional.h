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
//     OffCount      REG_DWORD  1 = one failed provisional boot of that path, 2 = its retry failed too (permanent)
//     OffAt         REG_DWORD  Boots when Off was written
//     Boots         REG_DWORD  boots whose first decision read a provisional start (the start before was confirmed)
//   HKLM\SOFTWARE\amdgpu-wddm\ProtectedLatch    volatile (router-latch.h)
//     Provisional   REG_DWORD  1 = a decision of this boot wrote Pending
//     BootCounted   REG_DWORD  1 = Boots counts this boot already
//
// The router writes and flushes Pending before the GPU open of a provisional route. Each later decision that reads
// a start state carries out what it finds:
//   - a confirmed start: Pending goes. When Off names the same path, that start was the retry and it is confirmed:
//     Off, OffCount and OffAt go too (retry-ok);
//   - Pending of an earlier boot (no Provisional mark) and UnconfirmedStarts 2 or more: the start before this one was
//     never confirmed (a hang, a crash or a power loss before the confirmation). Off takes the path with OffCount 1
//     and OffAt = Boots, and Pending goes; when Off named that path already (a failed retry), OffCount 2;
//   - Pending of an earlier boot and UnconfirmedStarts 0 or 1: the start before this one was confirmed. Pending goes
//     (and Off of the same path, as for a confirmed start).
// Off with OffCount 1 closes the provisional route of that path until Boots - OffAt reaches ProvisionalRetryAfter:
// then one provisional start takes the GPU UMD again (state "retry", Pending written as for any provisional route).
// OffCount 2 closes it until a new driver package (a new path) or until a tool removes Off. A new driver package has
// a new path and tries again at once.
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
static const wchar_t ProvisionalOffCountValue[] = L"OffCount";
static const wchar_t ProvisionalOffAtValue[] = L"OffAt";
static const wchar_t ProvisionalBootsValue[] = L"Boots";
static const wchar_t ProvisionalMarkValue[] = L"Provisional";
static const wchar_t ProvisionalCountedValue[] = L"BootCounted";
static const size_t ProvisionalPathChars = 1024;
static const DWORD ProvisionalRetryAfter = 5;    // boots after a confirmed start, from a first Off to its one retry
static const DWORD ProvisionalOffPermanent = 2;  // OffCount of a path whose retry failed as well

struct ProvisionalView {
    bool store;              // the record is open for writing
    bool off;                // Off names the GPU UMD of this decision and closes the provisional route
    bool retry;              // Off names it, but this decision is its one retry (the provisional route is open)
    bool pending_this_boot;  // Pending names that GPU UMD and this boot wrote it
    unsigned long off_count; // OffCount of that Off (1, or 2 = permanent), 0 when Off names another path or none
    unsigned long since;     // Boots - OffAt of that Off: boots after a confirmed start since Off was written
    const char *state;       // "on", "off", "retry", "nostore" ("unread" when no start state was read)
    const char *action;      // "none", "cleared", "retry-ok", "set-off", "written", "retry-ok+written", "kept",
                             // "write-failed"
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

// Once per boot, at the first decision of the boot that reads a provisional start: the start before this one was
// confirmed. The mark of this boot goes into the volatile latch key first, then the count, so that a write that fails
// half way counts one boot too few (a later retry), never one too many.
static inline bool CountBootOnce(REGSAM view)
{
    HKEY latch = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, LatchKeyPath, 0, nullptr, LatchKeyOptions,
                        KEY_QUERY_VALUE | KEY_SET_VALUE | view, nullptr, &latch, nullptr) != ERROR_SUCCESS)
        return false;
    DWORD mark = 0;
    bool count = !(ReadLatchDword(latch, ProvisionalCountedValue, &mark) && mark == 1);
    if (count) {
        const DWORD one = 1;
        count = RegSetValueExW(latch, ProvisionalCountedValue, 0, REG_DWORD, (const BYTE *)&one, sizeof(one)) ==
                ERROR_SUCCESS;
    }
    RegCloseKey(latch);
    return count;
}

static inline bool WriteProvisionalDword(HKEY key, const wchar_t *name, DWORD value)
{
    return RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value)) == ERROR_SUCCESS;
}

// Reads the record for a decision that read a start state, and carries out what it finds (see above).
inline ProvisionalView ReadProvisional(REGSAM view, StartHealth start, unsigned long unconfirmed, const wchar_t *gpu)
{
    ProvisionalView v = {false, false, false, false, 0, 0, "nostore", "none"};
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ProvisionalKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_QUERY_VALUE | KEY_SET_VALUE | view, nullptr, &key, nullptr) == ERROR_SUCCESS)
        v.store = true;
    else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ProvisionalKeyPath, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS)
        key = nullptr;
    wchar_t pending[ProvisionalPathChars], off[ProvisionalPathChars];
    const bool hasPending = key && ReadProvisionalText(key, ProvisionalPendingValue, pending, ProvisionalPathChars);
    bool hasOff = key && ReadProvisionalText(key, ProvisionalOffValue, off, ProvisionalPathChars);
    DWORD offCount = 0, offAt = 0, boots = 0;
    const bool hasOffCount = key && ReadLatchDword(key, ProvisionalOffCountValue, &offCount);
    const bool hasOffAt = key && ReadLatchDword(key, ProvisionalOffAtValue, &offAt);
    if (!key || !ReadLatchDword(key, ProvisionalBootsValue, &boots)) boots = 0;  // absent or of another kind
    // Off without its count (a record of an earlier router, or a count of another kind) is a first failure.
    if (!hasOffCount || offCount == 0) offCount = 1;
    if (offCount > ProvisionalOffPermanent) offCount = ProvisionalOffPermanent;
    bool flush = false;
    if (v.store && start == StartHealth::Provisional && CountBootOnce(view)) {
        ++boots;
        flush |= WriteProvisionalDword(key, ProvisionalBootsValue, boots);
    }
    // Off without the count at its time: the wait for the retry starts now.
    if (hasOff && !hasOffAt && v.store) {
        offAt = boots;
        flush |= WriteProvisionalDword(key, ProvisionalOffAtValue, offAt);
    }
    if (hasPending && v.store) {
        const bool thisBoot = ProvisionalMarkedThisBoot(view);
        // Pending of the path that Off names: that start was the one retry of an Off path.
        const bool retried = hasOff && !_wcsicmp(pending, off);
        if (start == StartHealth::Confirmed || (!thisBoot && unconfirmed < 2)) {
            if (RegDeleteValueW(key, ProvisionalPendingValue) == ERROR_SUCCESS) v.action = "cleared";
            if (retried) {
                // The retry reached its confirmation: the provisional route is on again for that path.
                RegDeleteValueW(key, ProvisionalOffValue);
                RegDeleteValueW(key, ProvisionalOffCountValue);
                RegDeleteValueW(key, ProvisionalOffAtValue);
                hasOff = false;
                v.action = "retry-ok";
            }
            flush = true;
        } else if (!thisBoot) {
            // The start before this one took the provisional route and never got its confirmation. A second failure
            // of the same path makes Off permanent; a failure of another path starts again at one.
            const DWORD count = retried ? ProvisionalOffPermanent : 1;
            const DWORD bytes = (DWORD)((wcslen(pending) + 1) * sizeof(wchar_t));
            if (RegSetValueExW(key, ProvisionalOffValue, 0, REG_SZ, (const BYTE *)pending, bytes) == ERROR_SUCCESS &&
                WriteProvisionalDword(key, ProvisionalOffCountValue, count) &&
                WriteProvisionalDword(key, ProvisionalOffAtValue, boots)) {
                RegDeleteValueW(key, ProvisionalPendingValue);
                wcscpy_s(off, pending);
                hasOff = true;
                offCount = count;
                offAt = boots;
                v.action = "set-off";
            }
            flush = true;
        } else {
            v.pending_this_boot = gpu && *gpu && !_wcsicmp(pending, gpu);
        }
    }
    if (flush) RegFlushKey(key);
    if (hasOff && gpu && *gpu && !_wcsicmp(off, gpu)) {
        v.off_count = offCount;
        v.since = boots >= offAt ? boots - offAt : 0;  // a count that went back (a reset record) waits again
        // One retry after ProvisionalRetryAfter boots that followed a confirmed start, on a provisional start that
        // can write its own Pending; never after a second failure.
        if (offCount < ProvisionalOffPermanent && start == StartHealth::Provisional && v.store &&
            v.since >= ProvisionalRetryAfter)
            v.retry = true;
        else
            v.off = true;
    }
    v.state = v.retry ? "retry" : v.off ? "off" : v.store ? "on" : "nostore";
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
