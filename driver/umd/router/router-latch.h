// SPDX-License-Identifier: MIT
// The fault latch of the logon images (router-policy.h ProtectedKindOf, docs/design/logon-gpu-route.md).
//
// A logon image that takes the application GPU UMD leaves a marker before the GPU open and removes it when its
// process ends through ExitProcess (DLL_PROCESS_DETACH with a non-null reserved argument). A process that ends any
// other way - an unhandled exception, whose default handler ends the process with TerminateProcess, a kill, a fault
// inside the GPU UMD - leaves its marker. The next decision of the same image finds a marker whose process is gone,
// sets the image's latch, and that image takes the CPU UMD for the rest of the boot. The key is volatile, so the
// configuration manager drops the latch at the next boot, and the next boot tries the GPU UMD again on a confirmed
// start. A boot that dies (a 0x116 after repeated resets, a power loss) loses the latch, and the kernel driver's
// own closes take over: the interop session marker closes the desktop route (InteropClosedReason 4), the desktop
// gate then closes, and the boot-loop guard counts the start.
//
//   HKLM\SOFTWARE\amdgpu-wddm\ProtectedLatch    volatile; Local System (LogonUI.exe, consent.exe) writes here
//   HKCU\Software\amdgpu-wddm\ProtectedLatch    volatile; the fallback for a user process (CredentialUIBroker.exe)
//     Base                REG_DWORD  HangRecovery\AdapterRestarts at the first logon-image decision of this boot
//     <image>             REG_DWORD  1 = latched: a GPU process of the image ended without a clean exit
//     <image>|<pid>       REG_QWORD  the marker: the creation time (FILETIME) of a live GPU process of the image
// <image> is the lower-case base name. A standard user can write only the HKCU store, and only for that user's own
// processes; a value there can move a process of that user to the CPU UMD and nothing else.
#pragma once
#include <windows.h>
#include <cwchar>
#include <cwctype>
#include "router-policy.h"

namespace bc250router {

static const wchar_t LatchKeyPath[] = L"SOFTWARE\\amdgpu-wddm\\ProtectedLatch";
static const wchar_t LatchBaseValue[] = L"Base";

#ifdef BC250_ROUTER_STATE_FIXTURE
// The private-hive gate reads the store back after the child process ended, so the fixture keeps it on disk.
static const DWORD LatchKeyOptions = REG_OPTION_NON_VOLATILE;
#else
static const DWORD LatchKeyOptions = REG_OPTION_VOLATILE;
#endif

struct LatchStore {
    HKEY key;
    const char *where;  // "hklm", "hkcu" or "none"
};

inline LatchStore OpenLatchStore(REGSAM view)
{
    LatchStore s = {nullptr, "none"};
    const REGSAM access = KEY_QUERY_VALUE | KEY_SET_VALUE | view;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, LatchKeyPath, 0, nullptr, LatchKeyOptions, access, nullptr, &s.key,
                        nullptr) == ERROR_SUCCESS) {
        s.where = "hklm";
        return s;
    }
    s.key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, LatchKeyPath, 0, nullptr, LatchKeyOptions, access, nullptr, &s.key,
                        nullptr) == ERROR_SUCCESS) {
        s.where = "hkcu";
        return s;
    }
    s.key = nullptr;
    return s;
}

inline void LatchImageName(const wchar_t *exe, wchar_t *out, size_t chars)
{
    size_t i = 0;
    for (; exe && exe[i] && i + 1 < chars; ++i) out[i] = (wchar_t)towlower(exe[i]);
    out[i] = 0;
}

inline bool ProcessCreation(HANDLE process, ULONGLONG *created)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(process, &c, &e, &k, &u)) return false;
    *created = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    return true;
}

// Whether the process of a marker is gone: no process with that id, a process with that id and another creation
// time (the id was used again), or an exited process that a handle keeps. A process that this one may not open is
// counted as alive: the latch then waits for a decision that can tell.
inline bool MarkerProcessGone(DWORD pid, ULONGLONG created)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return GetLastError() == ERROR_INVALID_PARAMETER;
    ULONGLONG now = 0;
    DWORD code = 0;
    bool gone = false;
    if (ProcessCreation(p, &now) && now != created) gone = true;
    else if (GetExitCodeProcess(p, &code) && code != STILL_ACTIVE) gone = true;
    CloseHandle(p);
    return gone;
}

static inline bool ReadLatchDword(HKEY key, const wchar_t *name, DWORD *value)
{
    DWORD type = 0, bytes = sizeof(*value);
    return RegQueryValueExW(key, name, nullptr, &type, (BYTE *)value, &bytes) == ERROR_SUCCESS && type == REG_DWORD &&
           bytes == sizeof(*value);
}

// Reads the store for one image and carries out what it finds: a marker whose process is gone sets the image's
// value to 1 and goes; an absent base takes the current AdapterRestarts. The answer is LatchOf of what was found.
inline LatchView ReadLatch(HKEY key, const wchar_t *exe, bool restarts_known, unsigned long restarts)
{
    LatchView v = {};
    v.restarts_known = restarts_known;
    v.restarts = restarts;
    if (!key) return v;
    v.store = true;
    wchar_t image[MAX_PATH];
    LatchImageName(exe, image, MAX_PATH);
    DWORD flag = 0;
    if (ReadLatchDword(key, image, &flag) && flag == 1) v.faulted = true;
    // The markers of this image, collected first: a value is not deleted while the key is enumerated.
    const size_t prefix = wcslen(image);
    wchar_t gone[16][MAX_PATH + 16];
    unsigned goneCount = 0;
    for (DWORD i = 0; goneCount < 16; ++i) {
        wchar_t name[MAX_PATH + 16];
        DWORD chars = (DWORD)(sizeof(name) / sizeof(name[0])), type = 0, bytes = 0;
        ULONGLONG created = 0;
        bytes = sizeof(created);
        const LSTATUS s = RegEnumValueW(key, i, name, &chars, nullptr, &type, (BYTE *)&created, &bytes);
        if (s == ERROR_NO_MORE_ITEMS) break;
        if (s != ERROR_SUCCESS && s != ERROR_MORE_DATA) break;
        if (wcsncmp(name, image, prefix) || name[prefix] != L'|') continue;
        wchar_t *end = nullptr;
        const unsigned long pid = wcstoul(name + prefix + 1, &end, 10);
        // A marker of another shape is gone as well: only this router writes the store.
        const bool shaped = s == ERROR_SUCCESS && type == REG_QWORD && bytes == sizeof(created) && end && !*end && pid;
        if (!shaped || MarkerProcessGone((DWORD)pid, created)) wcscpy_s(gone[goneCount++], name);
    }
    for (unsigned i = 0; i < goneCount; ++i) {
        const DWORD one = 1;
        if (RegSetValueExW(key, image, 0, REG_DWORD, (const BYTE *)&one, sizeof(one)) == ERROR_SUCCESS)
            RegDeleteValueW(key, gone[i]);
        v.faulted = true;
    }
    v.dead_markers = goneCount;
    DWORD base = 0;
    if (ReadLatchDword(key, LatchBaseValue, &base)) {
        v.base_known = true;
        v.base = base;
    } else if (restarts_known) {
        const DWORD now = (DWORD)restarts;
        if (RegSetValueExW(key, LatchBaseValue, 0, REG_DWORD, (const BYTE *)&now, sizeof(now)) == ERROR_SUCCESS) {
            v.base_known = true;
            v.base = now;
        }
    }
    return v;
}

// The marker of this process. Armed before the GPU open, removed at a clean exit (DisarmLatchAtExit, from DllMain) or
// when the GPU open failed and the process went to the CPU UMD (DisarmLatch).
static SRWLOCK LatchLock = SRWLOCK_INIT;
static HKEY LatchMarkerKey;
static wchar_t LatchMarkerName[MAX_PATH + 16];

inline bool ArmLatch(HKEY key, const wchar_t *exe)
{
    if (!key) return false;
    ULONGLONG created = 0;
    if (!ProcessCreation(GetCurrentProcess(), &created)) return false;
    wchar_t image[MAX_PATH], name[MAX_PATH + 16];
    LatchImageName(exe, image, MAX_PATH);
    if (_snwprintf_s(name, _TRUNCATE, L"%ls|%lu", image, GetCurrentProcessId()) < 0) return false;
    if (RegSetValueExW(key, name, 0, REG_QWORD, (const BYTE *)&created, sizeof(created)) != ERROR_SUCCESS) return false;
    // Keep this module until the process ends, so that DLL_PROCESS_DETACH comes only from ExitProcess and not from
    // a FreeLibrary of the runtime, which would remove the marker while the GPU UMD stays loaded.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)&LatchMarkerKey, &self);
    AcquireSRWLockExclusive(&LatchLock);
    if (!LatchMarkerKey) {
        HANDLE process = GetCurrentProcess();
        HANDLE copy = nullptr;
        if (DuplicateHandle(process, (HANDLE)key, process, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
            LatchMarkerKey = (HKEY)copy;
    }
    wcscpy_s(LatchMarkerName, name);
    const bool kept = LatchMarkerKey != nullptr;
    ReleaseSRWLockExclusive(&LatchLock);
    if (!kept) RegDeleteValueW(key, name); // nothing could remove it at the exit: a clean exit would read as a fault
    return kept;
}

inline void DisarmLatch()
{
    AcquireSRWLockExclusive(&LatchLock);
    if (LatchMarkerKey && LatchMarkerName[0]) RegDeleteValueW(LatchMarkerKey, LatchMarkerName);
    LatchMarkerName[0] = 0;
    ReleaseSRWLockExclusive(&LatchLock);
}

// DLL_PROCESS_DETACH at ExitProcess: the other threads are gone, so no lock is taken. The registry handle stays open
// until the process ends.
inline void DisarmLatchAtExit()
{
    if (LatchMarkerKey && LatchMarkerName[0]) RegDeleteValueW(LatchMarkerKey, LatchMarkerName);
}

} // namespace bc250router
