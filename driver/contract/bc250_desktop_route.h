// SPDX-License-Identifier: MIT
#pragma once
// M15.14: the desktop route, as the router in the compositor decided it, for the application shells.
// User mode only: the router (driver/umd/router/router.cpp) writes it, the D3D12 shell
// (driver/umd/d3d12/scanout-mode.h) and the D3D11 shell (driver/umd/dxvk/scanout-primary.h) read it. The
// kernel driver does not include this header.
//
// Why it exists. A shell may ask for a scan-out primary only when the compositor can still read that buffer
// whenever it composes it (a window over the game, a flip the operating system does not take). The GPU
// compositor (the hosted UMD) reads it with the GPU. The CPU compositor (the CPU UMD) reads every surface
// with the CPU, and a scan-out primary is VRAM with no CPU mapping (bc250_scanout_primary.h, FORCE_CPU).
// The router sends dwm.exe to the CPU UMD for three reasons: the kill switch DwmForceCpu, the kernel
// driver's desktop switches, and a hosted open that failed ("fallback=1" in the route line). A shell can
// read the first two in the registry. The third happens inside dwm.exe, and no other process can see it.
// So the router in dwm.exe writes each decision here, and the shells read the last one.
//
// Where. A named section in the session's namespace, BC250_DESKTOP_ROUTE_NAME, 32 bytes. dwm.exe already
// shares named objects with the applications of its session this way: DXGI opens the compositor's
// Local\DWM_DX_FULLSCREEN_TRANSITION_EVENT, whose owner is the compositor's account (observed on Windows 11
// 26200, 2026-10-08). The router keeps its handle until dwm.exe ends, so the record ends with the
// compositor that wrote it: a "gpu" record of an earlier compositor or an earlier boot cannot be read. A
// registry value would outlive both, and dwm.exe cannot write HKLM\SOFTWARE.
//
// Trust. Any process of the session can create a section with this name before dwm.exe does. A reader
// therefore accepts a record only when the owner of the section is the compositor's account, Window
// Manager\DWM-<session> (S-1-5-90-0-<session>). No other process can make that account the owner. The DACL
// gives SYSTEM and the owner all access and everyone read access, so no other process can write a record
// that the compositor created.
//
// Fail safe. No section, a section the reader cannot open, a foreign owner, a header of another version and
// every route other than GPU read as "not the GPU route", and the shells keep the composed primary. A
// router older than this header writes no record, so a shell on such a desktop stays composed. The router
// and the shells ship in one package.
//
// The route word is one aligned 32-bit store, so a reader sees one decision or the next one, never a mix.
// dwm.exe can open the adapter more than once (after a lost device, or through the D3D10.0 entry), and the
// record holds the last decision. The other fields are for the trace only.
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>

#pragma comment(lib, "advapi32.lib")

#define BC250_DESKTOP_ROUTE_NAME L"Local\\bc250-desktop-route"
#define BC250_DESKTOP_ROUTE_MAGIC 0x52443242u   /* B2DR */
#define BC250_DESKTOP_ROUTE_VERSION 1u
#define BC250_DESKTOP_ROUTE_BYTES 32u

/* The route word: the compositor's last decision. */
#define BC250_DESKTOP_ROUTE_NONE         0u  /* the section exists and holds no decision yet */
#define BC250_DESKTOP_ROUTE_GPU          1u  /* the hosted GPU UMD opened the adapter */
#define BC250_DESKTOP_ROUTE_KILL_SWITCH  2u  /* the CPU UMD: DwmForceCpu */
#define BC250_DESKTOP_ROUTE_SWITCHES_OFF 3u  /* the CPU UMD: the kernel driver's desktop switches are not both on */
#define BC250_DESKTOP_ROUTE_FALLBACK     4u  /* the CPU UMD: the hosted open failed (fallback=1) */

/* What a reader found. */
#define BC250_DESKTOP_ROUTE_READ_OK     0u
#define BC250_DESKTOP_ROUTE_READ_ABSENT 1u   /* no section with this name in the session */
#define BC250_DESKTOP_ROUTE_READ_DENIED 2u   /* a section exists, and this process cannot open, query or map it */
#define BC250_DESKTOP_ROUTE_READ_OWNER  3u   /* the owner of the section is not the expected account */
#define BC250_DESKTOP_ROUTE_READ_SHAPE  4u   /* the header is not the one of this version */

struct bc250_desktop_route {
    unsigned int magic;
    unsigned int version;
    unsigned int size;
    unsigned int route;      /* BC250_DESKTOP_ROUTE_*, written last with one aligned store */
    unsigned int pid;        /* the compositor process that wrote the last decision */
    unsigned int hosted_hr;  /* the HRESULT of the hosted open; S_FALSE when the decision did not try it */
    unsigned int decisions;  /* how many decisions this compositor wrote */
    unsigned int reserved;
};
typedef char bc250_desktop_route_size_check[
    sizeof(struct bc250_desktop_route)==BC250_DESKTOP_ROUTE_BYTES ? 1 : -1];

/* The one answer the shells use: the compositor published the GPU route. */
static __inline int bc250_desktop_route_gpu(unsigned int status, const struct bc250_desktop_route* record)
{
    return status == BC250_DESKTOP_ROUTE_READ_OK && record && record->route == BC250_DESKTOP_ROUTE_GPU;
}

/* One word for the trace: the read status when the read failed, otherwise the route. */
static __inline const char* bc250_desktop_route_text(unsigned int status, unsigned int route)
{
    switch (status) {
    case BC250_DESKTOP_ROUTE_READ_OK: break;
    case BC250_DESKTOP_ROUTE_READ_ABSENT: return "absent";
    case BC250_DESKTOP_ROUTE_READ_DENIED: return "denied";
    case BC250_DESKTOP_ROUTE_READ_OWNER: return "foreign-owner";
    case BC250_DESKTOP_ROUTE_READ_SHAPE: return "bad-record";
    default: return "unknown";
    }
    switch (route) {
    case BC250_DESKTOP_ROUTE_NONE: return "none";
    case BC250_DESKTOP_ROUTE_GPU: return "gpu";
    case BC250_DESKTOP_ROUTE_KILL_SWITCH: return "cpu-kill-switch";
    case BC250_DESKTOP_ROUTE_SWITCHES_OFF: return "cpu-switches-off";
    case BC250_DESKTOP_ROUTE_FALLBACK: return "cpu-fallback";
    default: return "unknown";
    }
}

/* Window Manager\DWM-<session>, S-1-5-90-0-<session>: the account that dwm.exe of that session runs as. */
static __inline PSID bc250_desktop_route_compositor_sid(DWORD session, SE_SID* sid)
{
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (!InitializeSid(&sid->Sid, &nt, 3)) return NULL;
    *GetSidSubAuthority(&sid->Sid, 0) = SECURITY_WINDOW_MANAGER_BASE_RID;
    *GetSidSubAuthority(&sid->Sid, 1) = 0;
    *GetSidSubAuthority(&sid->Sid, 2) = session;
    return &sid->Sid;
}

/* The reader. name and owner are parameters so that a host test can read a record it wrote itself; the
   shells call bc250_desktop_route_read_session. *record is all zero unless the status is READ_OK. */
static __inline unsigned int bc250_desktop_route_read(const wchar_t* name, PSID owner,
                                                      struct bc250_desktop_route* record)
{
    HANDLE section;
    PSID found = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    const volatile struct bc250_desktop_route* view;
    unsigned int status;
    ZeroMemory(record, sizeof(*record));
    section = OpenFileMappingW(FILE_MAP_READ | READ_CONTROL, FALSE, name);
    if (!section)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? BC250_DESKTOP_ROUTE_READ_ABSENT : BC250_DESKTOP_ROUTE_READ_DENIED;
    if (GetSecurityInfo(section, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &found, NULL, NULL, NULL,
                        &descriptor) != ERROR_SUCCESS) {
        CloseHandle(section);
        return BC250_DESKTOP_ROUTE_READ_DENIED;
    }
    status = owner && found && EqualSid(found, owner) ? BC250_DESKTOP_ROUTE_READ_OK : BC250_DESKTOP_ROUTE_READ_OWNER;
    LocalFree(descriptor);
    if (status == BC250_DESKTOP_ROUTE_READ_OK) {
        view = (const volatile struct bc250_desktop_route*)MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(*record));
        if (!view) status = BC250_DESKTOP_ROUTE_READ_DENIED;
        else {
            record->magic = view->magic;
            record->version = view->version;
            record->size = view->size;
            record->route = view->route;
            record->pid = view->pid;
            record->hosted_hr = view->hosted_hr;
            record->decisions = view->decisions;
            UnmapViewOfFile((LPCVOID)view);
            if (record->magic != BC250_DESKTOP_ROUTE_MAGIC || record->version != BC250_DESKTOP_ROUTE_VERSION ||
                record->size != BC250_DESKTOP_ROUTE_BYTES)
                status = BC250_DESKTOP_ROUTE_READ_SHAPE;
        }
        if (status != BC250_DESKTOP_ROUTE_READ_OK) ZeroMemory(record, sizeof(*record));
    }
    CloseHandle(section);
    return status;
}

/* The record of this process's session, accepted only from that session's compositor account. */
static __inline unsigned int bc250_desktop_route_read_session(struct bc250_desktop_route* record)
{
    DWORD session = 0;
    SE_SID sid;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        ZeroMemory(record, sizeof(*record));
        return BC250_DESKTOP_ROUTE_READ_DENIED;
    }
    return bc250_desktop_route_read(BC250_DESKTOP_ROUTE_NAME, bc250_desktop_route_compositor_sid(session, &sid),
                                    record);
}

/* The writer, for the router in dwm.exe. It creates the section with the DACL above, or opens it when a
   reader still held the record of an earlier compositor of this session. It returns the view, which the
   caller keeps until the process ends. The section handle is not closed, so the record lives exactly as long
   as the compositor. NULL, with the Win32 error in *error, when the section cannot be made. */
static __inline struct bc250_desktop_route* bc250_desktop_route_create(const wchar_t* name, DWORD* error)
{
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, FALSE};
    HANDLE section;
    struct bc250_desktop_route* view;
    *error = 0;
    /* GR on a section is SECTION_QUERY, SECTION_MAP_READ and READ_CONTROL: what the reader opens with. OW
       (owner rights) gives the owner all access and replaces its implicit rights. */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;OW)(A;;GR;;;WD)",
                                                              SDDL_REVISION_1, &attributes.lpSecurityDescriptor, NULL)) {
        *error = GetLastError();
        return NULL;
    }
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(*view), name);
    if (!section) *error = GetLastError();
    LocalFree(attributes.lpSecurityDescriptor);
    if (!section) return NULL;
    view = (struct bc250_desktop_route*)MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(*view));
    if (!view) {
        *error = GetLastError();
        CloseHandle(section);
        return NULL;
    }
    view->magic = BC250_DESKTOP_ROUTE_MAGIC;
    view->version = BC250_DESKTOP_ROUTE_VERSION;
    view->size = BC250_DESKTOP_ROUTE_BYTES;
    return view;
}

/* One decision: the fields for the trace first, the route word last. */
static __inline void bc250_desktop_route_store(struct bc250_desktop_route* view, unsigned int route,
                                               unsigned int pid, unsigned int hosted_hr)
{
    view->pid = pid;
    view->hosted_hr = hosted_hr;
    view->decisions = view->decisions + 1u;
    InterlockedExchange((volatile LONG*)&view->route, (LONG)route);
}
