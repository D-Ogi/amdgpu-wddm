// redirblt-probe - does dxgkrnl on this build admit a redirected windowed Blt from a non-runtime client?
//
// Path B of ADR 0018 (the OpenGL-ICD window present) was refused at admission in E45, E46 and DWM035 with
// STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE: a token-less D3DKMTPresent(Blt) of a linear VRAM allocation into a
// window under the DWM. The documented producer of a redirected windowed Blt is a handshake with the DWM that
// the OpenGL runtime performs for an ICD: DwmDxGetWindowSharedSurface (dwmapi ordinal 100) with the
// GDI-surface flag, then D3DKMTPresent with Flags.RedirectedBlt and a D3DKMT_PM_REDIRECTED_BLT token that
// carries the update id. This tool performs that handshake itself and presents once per variant, so that one
// bounded lab run says which of these holds on this build: ordinal 100 is gone, the DWM declines the window,
// the shared handle does not open on our adapter, or dxgkrnl admits (or refuses) each variant.
//
// It runs ON THE TARGET, in the interactive user session, under the hosted GPU DWM with interop enabled.
// On the development PC it only answers --help. No lab claim is made by the build; the transcript is.
//
// Every D3DKMT call prints its name and NTSTATUS, every dwmapi call its HRESULT. No wait is unbounded: the
// handshake runs on a worker thread with its own deadline and the whole process has a watchdog. Failures
// unwind what was built, in reverse. Exit codes: 0 the sequence ran (a refusal is a result, not a failure),
// 1 a step failed before the variants, 2 bad arguments, 3 dwmapi ordinal 100 is absent, 4 the watchdog fired.
//
// Sources for the constants: SDK 10.0.26100 d3dkmthk.h (present flags, token layout, client hints),
// winerror.h (DWM_S_/DWM_E_ codes), dwmapi ordinals from the Windows 7 documentation
// (ref\win32-docs ... dwmdxgetwindowsharedsurface.md), the KMD's LB7A block from driver\kmd\gdi_private.h.
// Research: scratch\pdev-research\redirected-blt\REPORT.md (local workspace, outside the repository).

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>       // NTSTATUS; d3dkmthk.h needs it and does not include it
#include <d3dkmthk.h>
#include <dxgiformat.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "redirblt_policy.h"    // the decisions, as pure functions pinned by redirblt-policy-test.c

// ---- what the KMD accepts -----------------------------------------------------------------------------
//
// driver\kmd\gdi_private.h: a 32-byte LB7A v1 block with Type 0 (no GDI trailer). WddmSurfaceGeometry for
// Type 0 wants a 4-byte-per-pixel format, Pitch a multiple of 4 and at least Width * 4, Size >= Pitch * Height.
// build.ps1 compares the magic against the driver source.
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul    // "LB7A"

typedef struct _BC250_WDDM_ALLOCATION_PRIVATE {
    ULONG Magic;
    ULONG Version;
    ULONG Width;
    ULONG Height;
    ULONG Pitch;
    ULONG Format;                       // D3DDDIFORMAT
    ULONGLONG Size;
} BC250_WDDM_ALLOCATION_PRIVATE;

C_ASSERT(sizeof(BC250_WDDM_ALLOCATION_PRIVATE) == 32);

#define BC250_ALLOCATION_PRIVATE_VERSION 1u
#define GPU_PAGE_SIZE 4096ull
// The fork's winsys maps its allocations in this range and lets VidMm choose the address
// (radv_wddm2_bo.h: RADV_WDDM2_HEAP_START, RADV_WDDM2_REPLAY_HEAP_START).
#define VA_MIN 0x0000000200000000ull
#define VA_MAX (1ull << 45)
// RADV's linear GFX10 surfaces come with a 256-byte pitch alignment; the probe uses the same so that the
// source looks like a swapchain image to the KMD's engine copy.
#define PITCH_ALIGN 256ul

// ---- the DWM handshake, Windows 7 documentation ---------------------------------------------------------
//
// Neither function is in a header or import library; the documentation gives the ordinals and prototypes.
// LUID travels by value, as the page declares it.
#define DWMDX_ORDINAL_GET_WINDOW_SHARED_SURFACE 100
#define DWMDX_ORDINAL_UPDATE_WINDOW_SHARED_SURFACE 101
#define DWM_REDIRECTION_FLAG_SUPPORT_PRESENT_TO_GDI_SURFACE 0x10ul

typedef HRESULT (WINAPI *PFN_DWMDXGETWINDOWSHAREDSURFACE)(HWND hwnd, LUID luidAdapter, HMONITOR hmonitorAssociation,
                                                          DWORD dwFlags, DXGI_FORMAT* pfmtWindow, HANDLE* phDxSurface,
                                                          UINT64* puiUpdateId);
typedef HRESULT (WINAPI *PFN_DWMDXUPDATEWINDOWSHAREDSURFACE)(HWND hwnd, UINT64 uiUpdateId, DWORD dwFlags,
                                                             HMONITOR hmonitorAssociation, RECT* prc);

// The SDK carries the codes; the values are pinned here so that a header change is a build failure.
C_ASSERT(DWM_S_GDI_REDIRECTION_SURFACE == 0x00263005L);
C_ASSERT(DWM_S_GDI_REDIRECTION_SURFACE_BLT_VIA_GDI == 0x00263008L);
C_ASSERT((unsigned long)DWM_E_ADAPTER_NOT_FOUND == 0x80263005ul);
C_ASSERT((unsigned long)DWM_E_NO_REDIRECTION_SURFACE_AVAILABLE == 0x80263003ul);
C_ASSERT((unsigned long)DWM_E_COMPOSITIONDISABLED == 0x80263001ul);
// The token is part of D3DKMT_PRESENT at a fixed size; the flag bit and the model are what the trace decodes.
C_ASSERT(sizeof(D3DKMT_PRESENTHISTORYTOKEN) == 1080);
C_ASSERT(D3DKMT_PM_REDIRECTED_BLT == 3);
C_ASSERT(D3DKMT_CLIENTHINT_OPENGL == 1);
C_ASSERT(D3DKMT_CLIENTHINT_VULKAN == 4);
// WS_EX_NOREDIRECTIONBITMAP (winuser.h) opts a window out of its GDI redirection bitmap; the probe asserts it
// is clear, and WS_EX_LAYERED too.
C_ASSERT(WS_EX_NOREDIRECTIONBITMAP == 0x00200000L);

// ---- options ------------------------------------------------------------------------------------------

typedef REDIRBLT_VARIANT VARIANT_ID;

static const char* const g_VariantNames[VariantCount] = { "V0", "A", "B", "C", "D" };

typedef struct _OPTIONS {
    BOOL HaveLuid;
    LUID Luid;
    const char* Match;
    LONG Width;                         // client area
    LONG Height;
    UINT32 Color;                       // ARGB as stored in an A8R8G8B8 pixel
    BOOL Variants[VariantCount];        // which to run, in V0..D order
    BOOL All;                           // do not stop at the first success
    BOOL Update;                        // after an S_OK handshake, call ordinal 101 once (never after 0x263005)
    BOOL HandshakeOnly;                 // window, handshake and (unless --no-open) the open; no source, context, present
    BOOL GdiPaint;                      // paint the client area once with GDI before the handshake
    BOOL NoOpen;                        // diagnostic: do not open the shared handle; B, C and D are then skipped
    DWORD PumpMs;                       // message pump after ShowWindow
    DWORD HoldSeconds;                  // keep the window and the allocation after the variants
    DWORD HandshakeTimeoutMs;
    DWORD WatchdogMs;
} OPTIONS;

// ---- probe state --------------------------------------------------------------------------------------

typedef struct _HANDSHAKE {
    HRESULT hr;
    DXGI_FORMAT Format;
    HANDLE Surface;
    UINT64 UpdateId;
    DWORD Ms;
    BOOL Completed;                     // the worker thread returned
    REDIRBLT_HANDLE_KIND Kind;          // how Surface was classified (unknown until the LUID query says)
    LUID Luid;                          // adapter of the surface, from D3DKMTGetSharedResourceAdapterLuid
    BOOL LuidKnown;
    BOOL LuidOurs;
} HANDSHAKE;

// The opened shared surface. Owned (a resource to release) and Ready (usable as a present destination) are
// tracked apart: a later failure still leaves something to release, and nothing half-prepared is presented to.
typedef struct _OPENED {
    BOOL Owned;                         // hResource must be destroyed
    BOOL Ready;                         // LUID ours, LB7A blob, mapped, resident
    HANDLE OpenedFrom;                  // the raw handshake handle this was opened from
    UINT64 UpdateId;                    // the update id bound to it: that handshake's, or a later one's on the same handle
    REDIRBLT_HANDLE_KIND Kind;
    D3DKMT_HANDLE hResource;
    D3DKMT_HANDLE hAllocation;
    UINT64 GpuVa;                       // reported by the open, then by our own map
    UINT64 MappedVa;
    UINT64 MappedSize;
    UINT NumAllocations;
    UINT TotalPrivate, ResourcePrivate, RuntimePrivate;
    BYTE Private32[32];                 // the first 32 bytes of the allocation's private data (an LB7A block, if ours)
    UINT PrivateBytes;
} OPENED;

typedef struct _PROBE {
    OPTIONS Opt;
    HMODULE Dwmapi;
    PFN_DWMDXGETWINDOWSHAREDSURFACE GetSurface;
    PFN_DWMDXUPDATEWINDOWSHAREDSURFACE UpdateSurface;
    HWND Window;
    RECT Client;                        // {0,0,w,h}
    LUID Luid;
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hPagingQueue;
    D3DKMT_HANDLE hPagingFenceObject;
    volatile UINT64* PagingFence;
    D3DKMT_HANDLE hSource;
    UINT64 SourceSize;
    ULONG SourcePitch;
    UINT64 SourceVa;
    D3DKMT_HANDLE hContext;             // ClientHint VULKAN, the fork's present context
    D3DKMT_HANDLE hContextGl;           // ClientHint OPENGL, variant D
    HANDSHAKE Handshake;
    UINT HandshakeCount;
    OPENED Opened;
    REDIRBLT_HANDLE_SET OwnedNtHandles; // every NT handle a handshake gave us, closed once at teardown
    NTSTATUS VariantStatus[VariantCount];
    BOOL VariantRan[VariantCount];
    int FirstSuccess;                   // -1 = none
} PROBE;

// ---- transcript ---------------------------------------------------------------------------------------

#define STATUS_ENTRY(x) { (x), #x }

static const struct { NTSTATUS Status; const char* Name; } g_StatusNames[] = {
    STATUS_ENTRY(STATUS_SUCCESS),
    STATUS_ENTRY(STATUS_PENDING),
    STATUS_ENTRY(STATUS_INVALID_PARAMETER),
    STATUS_ENTRY(STATUS_INVALID_HANDLE),
    STATUS_ENTRY(STATUS_NO_MEMORY),
    STATUS_ENTRY(STATUS_BUFFER_TOO_SMALL),
    STATUS_ENTRY(STATUS_INSUFFICIENT_RESOURCES),
    STATUS_ENTRY(STATUS_NOT_SUPPORTED),
    STATUS_ENTRY(STATUS_NOT_IMPLEMENTED),
    STATUS_ENTRY(STATUS_ACCESS_DENIED),
    STATUS_ENTRY(STATUS_OBJECT_NAME_NOT_FOUND),
    STATUS_ENTRY(STATUS_DEVICE_REMOVED),
    STATUS_ENTRY(STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE),
    STATUS_ENTRY(STATUS_GRAPHICS_ALLOCATION_INVALID),
    STATUS_ENTRY(STATUS_GRAPHICS_INVALID_ALLOCATION_USAGE),
    STATUS_ENTRY(STATUS_GRAPHICS_ALLOCATION_BUSY),
    STATUS_ENTRY(STATUS_GRAPHICS_CANT_LOCK_MEMORY),
    STATUS_ENTRY(STATUS_GRAPHICS_NO_VIDEO_MEMORY),
    STATUS_ENTRY(STATUS_GRAPHICS_INVALID_DRIVER_MODEL),
    STATUS_ENTRY(STATUS_GRAPHICS_PRESENT_DENIED),
    STATUS_ENTRY(STATUS_GRAPHICS_PRESENT_OCCLUDED),
    STATUS_ENTRY(STATUS_GRAPHICS_PRESENT_MODE_CHANGED),
    STATUS_ENTRY(STATUS_GRAPHICS_PRESENT_REDIRECTION_DISABLED),
    STATUS_ENTRY(STATUS_GRAPHICS_PRESENT_UNOCCLUDED),
    STATUS_ENTRY(STATUS_GRAPHICS_PRESENT_INVALID_WINDOW),
    STATUS_ENTRY(STATUS_GRAPHICS_PRESENT_BUFFER_NOT_BOUND),
    STATUS_ENTRY(STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE),
};

static const struct { HRESULT hr; const char* Name; } g_HresultNames[] = {
    { S_OK, "S_OK (dedicated DX surface: readback route)" },
    { DWM_S_GDI_REDIRECTION_SURFACE, "DWM_S_GDI_REDIRECTION_SURFACE" },
    { DWM_S_GDI_REDIRECTION_SURFACE_BLT_VIA_GDI, "DWM_S_GDI_REDIRECTION_SURFACE_BLT_VIA_GDI" },
    { DWM_E_COMPOSITIONDISABLED, "DWM_E_COMPOSITIONDISABLED" },
    { DWM_E_REMOTING_NOT_SUPPORTED, "DWM_E_REMOTING_NOT_SUPPORTED" },
    { DWM_E_NO_REDIRECTION_SURFACE_AVAILABLE, "DWM_E_NO_REDIRECTION_SURFACE_AVAILABLE" },
    { DWM_E_NOT_QUEUING_PRESENTS, "DWM_E_NOT_QUEUING_PRESENTS" },
    { DWM_E_ADAPTER_NOT_FOUND, "DWM_E_ADAPTER_NOT_FOUND" },
    { DWM_E_TEXTURE_TOO_LARGE, "DWM_E_TEXTURE_TOO_LARGE" },
    { E_INVALIDARG, "E_INVALIDARG" },
    { E_FAIL, "E_FAIL" },
    { E_NOTIMPL, "E_NOTIMPL" },
};

static const char* StatusName(NTSTATUS Status)
{
    size_t i;

    for (i = 0; i < ARRAYSIZE(g_StatusNames); i++)
        if (g_StatusNames[i].Status == Status) return g_StatusNames[i].Name;
    return "";
}

static const char* HresultName(HRESULT hr)
{
    size_t i;

    for (i = 0; i < ARRAYSIZE(g_HresultNames); i++)
        if (g_HresultNames[i].hr == hr) return g_HresultNames[i].Name;
    return "";
}

static NTSTATUS Report(const char* Call, NTSTATUS Status)
{
    const char* name = StatusName(Status);

    printf("%-40s 0x%08lX%s%s\n", Call, (unsigned long)Status, name[0] ? "  " : "", name);
    fflush(stdout);
    return Status;
}

static HRESULT ReportHr(const char* Call, HRESULT hr)
{
    const char* name = HresultName(hr);

    printf("%-40s hr=0x%08lX%s%s\n", Call, (unsigned long)hr, name[0] ? "  " : "", name);
    fflush(stdout);
    return hr;
}

static void Note(const char* Format, ...)
{
    va_list args;

    va_start(args, Format);
    printf("    ");
    vprintf(Format, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

// UTC with 100 ns resolution, the same clock the ETW rows carry, so the transcript and the trace line up.
static void Stamp(const char* What)
{
    FILETIME ft;
    SYSTEMTIME st;
    LARGE_INTEGER qpc;

    GetSystemTimePreciseAsFileTime(&ft);
    QueryPerformanceCounter(&qpc);
    FileTimeToSystemTime(&ft, &st);
    printf("STAMP %s utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ filetime=%llu qpc=%llu\n", What, st.wYear, st.wMonth,
           st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
           ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime, (unsigned long long)qpc.QuadPart);
    fflush(stdout);
}

// ---- watchdog -------------------------------------------------------------------------------------------

static HANDLE g_Done;

static DWORD WINAPI WatchdogThread(LPVOID Parameter)
{
    DWORD ms = (DWORD)(ULONG_PTR)Parameter;

    if (WaitForSingleObject(g_Done, ms) == WAIT_TIMEOUT)
    {
        printf("WATCHDOG fired after %lu ms, terminating\n", ms);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 4);
    }
    return 0;
}

static BOOL StartWatchdog(DWORD Milliseconds)
{
    HANDLE thread;

    g_Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_Done == NULL) return FALSE;
    thread = CreateThread(NULL, 0, WatchdogThread, (LPVOID)(ULONG_PTR)Milliseconds, 0, NULL);
    if (thread == NULL) return FALSE;
    CloseHandle(thread);
    return TRUE;
}

// ---- the window -----------------------------------------------------------------------------------------
//
// A plain top-level window that never draws with GDI: WM_PAINT only validates, WM_ERASEBKGND claims to have
// erased, the class has no background brush. Whether such a window owns a GDI redirection surface is one of
// the things the run measures (--gdi-paint paints it once, as the control).

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
        ValidateRect(hwnd, NULL);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        return 0;                       // the runner ends the process; a stray close must not tear the window down
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void Pump(DWORD Milliseconds)
{
    ULONGLONG end = GetTickCount64() + Milliseconds;
    MSG msg;

    do
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    } while (GetTickCount64() < end);
}

static BOOL StepWindow(PROBE* Probe)
{
    WNDCLASSW wc;
    RECT rect;
    const DWORD style = WS_OVERLAPPEDWINDOW;
    DWORD exstyle;
    RECT outer, client;
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"BC250RedirBltProbe";
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    if (!RegisterClassW(&wc)) { Note("RegisterClassW failed %lu", GetLastError()); return FALSE; }

    rect.left = 0; rect.top = 0; rect.right = Probe->Opt.Width; rect.bottom = Probe->Opt.Height;
    if (!AdjustWindowRect(&rect, style, FALSE)) { Note("AdjustWindowRect failed %lu", GetLastError()); return FALSE; }

    Stamp("create-window");
    Probe->Window = CreateWindowExW(0, wc.lpszClassName, L"BC250 redirected-blt probe", style, 320, 240,
                                    rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);
    if (Probe->Window == NULL) { Note("CreateWindowExW failed %lu", GetLastError()); return FALSE; }
    // Square corners, as the colour control asks for, so that a later pixel oracle sees the whole client area.
    ReportHr("DwmSetWindowAttribute (corners)",
             DwmSetWindowAttribute(Probe->Window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner)));
    Stamp("show-window");
    ShowWindow(Probe->Window, SW_SHOWNORMAL);
    SetForegroundWindow(Probe->Window);
    UpdateWindow(Probe->Window);

    exstyle = (DWORD)GetWindowLongPtrW(Probe->Window, GWL_EXSTYLE);
    GetWindowRect(Probe->Window, &outer);
    GetClientRect(Probe->Window, &client);
    Probe->Client = client;
    printf("WINDOW hwnd=0x%p style=0x%08lX exstyle=0x%08lX client=%ldx%ld outer=%ldx%ld at=%ld,%ld\n",
           (void*)Probe->Window, (unsigned long)GetWindowLongPtrW(Probe->Window, GWL_STYLE), (unsigned long)exstyle,
           client.right - client.left, client.bottom - client.top, outer.right - outer.left, outer.bottom - outer.top,
           outer.left, outer.top);
    fflush(stdout);
    if (exstyle & WS_EX_NOREDIRECTIONBITMAP) { Note("WS_EX_NOREDIRECTIONBITMAP is set: no redirection surface by design"); return FALSE; }
    if (exstyle & WS_EX_LAYERED) { Note("WS_EX_LAYERED is set: not the window this probe is about"); return FALSE; }
    if (client.right - client.left != Probe->Opt.Width || client.bottom - client.top != Probe->Opt.Height)
    {
        Note("client area is %ldx%ld, wanted %ldx%ld", client.right - client.left, client.bottom - client.top,
             Probe->Opt.Width, Probe->Opt.Height);
        return FALSE;
    }

    Pump(Probe->Opt.PumpMs);
    if (Probe->Opt.GdiPaint)
    {
        // The control: one GDI fill of the client area, so that win32k has a reason to give the window a
        // redirection bitmap if it creates them lazily. Grey, distinct from the presented colour.
        HDC dc = GetDC(Probe->Window);
        HBRUSH brush = CreateSolidBrush(RGB(0x60, 0x60, 0x60));

        if (dc != NULL && brush != NULL)
        {
            FillRect(dc, &client, brush);
            GdiFlush();
            Note("GDI paint: client area filled with grey");
        }
        if (brush != NULL) DeleteObject(brush);
        if (dc != NULL) ReleaseDC(Probe->Window, dc);
        Stamp("gdi-paint");
        Pump(200);
    }
    return TRUE;
}

// ---- step 0: dwmapi ordinals, session, composition ---------------------------------------------------------

static void DwmapiVersion(HMODULE Module, char* Out, size_t OutBytes)
{
    WCHAR path[MAX_PATH];
    DWORD handle = 0, size;
    void* block;
    VS_FIXEDFILEINFO* info = NULL;
    UINT len = 0;

    Out[0] = '\0';
    if (!GetModuleFileNameW(Module, path, ARRAYSIZE(path))) return;
    size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0) return;
    block = malloc(size);
    if (block == NULL) return;
    if (GetFileVersionInfoW(path, 0, size, block) && VerQueryValueW(block, L"\\", (void**)&info, &len) && info != NULL)
        sprintf_s(Out, OutBytes, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    free(block);
}

static BOOL StepStatic(PROBE* Probe)
{
    DWORD session = 0;
    DWORD console = WTSGetActiveConsoleSessionId();
    BOOL composition = FALSE;
    char version[64];

    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Probe->Dwmapi = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (Probe->Dwmapi == NULL) { Note("dwmapi.dll did not load: %lu", GetLastError()); return FALSE; }
    DwmapiVersion(Probe->Dwmapi, version, sizeof(version));
    Probe->GetSurface = (PFN_DWMDXGETWINDOWSHAREDSURFACE)GetProcAddress(
        Probe->Dwmapi, MAKEINTRESOURCEA(DWMDX_ORDINAL_GET_WINDOW_SHARED_SURFACE));
    Probe->UpdateSurface = (PFN_DWMDXUPDATEWINDOWSHAREDSURFACE)GetProcAddress(
        Probe->Dwmapi, MAKEINTRESOURCEA(DWMDX_ORDINAL_UPDATE_WINDOW_SHARED_SURFACE));
    ReportHr("DwmIsCompositionEnabled", DwmIsCompositionEnabled(&composition));
    printf("PROBE pid=%lu session=%lu console=%lu composition=%d dwmapi=%s ordinal100=%d ordinal101=%d\n",
           GetCurrentProcessId(), session, console, composition ? 1 : 0, version[0] ? version : "?",
           Probe->GetSurface != NULL, Probe->UpdateSurface != NULL);
    fflush(stdout);
    if (session != console) Note("not the console session: the DWM of this session is not the one on the screen");
    if (!composition) Note("composition is off: the handshake answers DWM_E_COMPOSITIONDISABLED by design");
    return TRUE;
}

// ---- adapter, device, source allocation, context --------------------------------------------------------------

static BOOL WideContainsAscii(const WCHAR* Haystack, const char* Needle)
{
    size_t n = strlen(Needle);
    size_t i, j;

    if (n == 0) return FALSE;
    for (i = 0; Haystack[i] != L'\0'; i++)
    {
        for (j = 0; j < n; j++)
        {
            WCHAR a = Haystack[i + j];
            WCHAR b = (WCHAR)Needle[j];

            if (a >= L'A' && a <= L'Z') a = (WCHAR)(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = (WCHAR)(b - L'A' + L'a');
            if (a != b) break;
        }
        if (j == n) return TRUE;
    }
    return FALSE;
}

static NTSTATUS QueryAdapter(D3DKMT_HANDLE hAdapter, KMTQUERYADAPTERINFOTYPE Type, void* Buffer, UINT Size)
{
    D3DKMT_QUERYADAPTERINFO query;

    ZeroMemory(&query, sizeof(query));
    query.hAdapter = hAdapter;
    query.Type = Type;
    query.pPrivateDriverData = Buffer;
    query.PrivateDriverDataSize = Size;
    return D3DKMTQueryAdapterInfo(&query);
}

static BOOL StepFindAdapter(PROBE* Probe)
{
    D3DKMT_ENUMADAPTERS2 enumerate;
    D3DKMT_ADAPTERINFO* adapters;
    ULONG count, i;
    BOOL found = FALSE;

    ZeroMemory(&enumerate, sizeof(enumerate));
    if (!NT_SUCCESS(Report("D3DKMTEnumAdapters2 (count)", D3DKMTEnumAdapters2(&enumerate)))) return FALSE;
    count = enumerate.NumAdapters;
    if (count == 0) return FALSE;
    adapters = (D3DKMT_ADAPTERINFO*)calloc(count, sizeof(*adapters));
    if (adapters == NULL) return FALSE;
    enumerate.NumAdapters = count;
    enumerate.pAdapters = adapters;
    if (!NT_SUCCESS(Report("D3DKMTEnumAdapters2", D3DKMTEnumAdapters2(&enumerate)))) { free(adapters); return FALSE; }
    count = enumerate.NumAdapters;
    for (i = 0; i < count; i++)
    {
        D3DKMT_ADAPTERREGISTRYINFO registry;
        D3DKMT_UMDFILENAMEINFO umd;
        D3DKMT_CLOSEADAPTER close;
        BOOL match;

        ZeroMemory(&registry, sizeof(registry));
        ZeroMemory(&umd, sizeof(umd));
        umd.Version = KMTUMDVERSION_DX11;
        printf("  adapter %lu: LUID %08lX:%08lX  NumOfSources %lu\n", i, (unsigned long)adapters[i].AdapterLuid.HighPart,
               (unsigned long)adapters[i].AdapterLuid.LowPart, adapters[i].NumOfSources);
        if (NT_SUCCESS(QueryAdapter(adapters[i].hAdapter, KMTQAITYPE_ADAPTERREGISTRYINFO, &registry, sizeof(registry))))
            printf("    AdapterString \"%ls\"  ChipType \"%ls\"\n", registry.AdapterString, registry.ChipType);
        if (NT_SUCCESS(QueryAdapter(adapters[i].hAdapter, KMTQAITYPE_UMDRIVERNAME, &umd, sizeof(umd))))
            printf("    UmdFileName   \"%ls\"\n", umd.UmdFileName);
        if (Probe->Opt.HaveLuid)
            match = adapters[i].AdapterLuid.LowPart == Probe->Opt.Luid.LowPart &&
                    adapters[i].AdapterLuid.HighPart == Probe->Opt.Luid.HighPart;
        else
            match = WideContainsAscii(registry.AdapterString, Probe->Opt.Match) ||
                    WideContainsAscii(registry.ChipType, Probe->Opt.Match) ||
                    WideContainsAscii(umd.UmdFileName, Probe->Opt.Match);
        if (match && !found) { Probe->Luid = adapters[i].AdapterLuid; found = TRUE; printf("    ^ selected\n"); }
        ZeroMemory(&close, sizeof(close));
        close.hAdapter = adapters[i].hAdapter;
        (void)D3DKMTCloseAdapter(&close);
    }
    free(adapters);
    fflush(stdout);
    if (!found) { printf("no adapter matched\n"); return FALSE; }
    return TRUE;
}

static BOOL StepOpenAdapter(PROBE* Probe)
{
    D3DKMT_OPENADAPTERFROMLUID open;
    D3DKMT_CREATEDEVICE device;
    D3DKMT_CREATEPAGINGQUEUE queue;
    D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP owner;

    ZeroMemory(&open, sizeof(open));
    open.AdapterLuid = Probe->Luid;
    if (!NT_SUCCESS(Report("D3DKMTOpenAdapterFromLuid", D3DKMTOpenAdapterFromLuid(&open)))) return FALSE;
    Probe->hAdapter = open.hAdapter;
    printf("ADAPTER luid=%08lX:%08lX hAdapter=0x%08lX\n", (unsigned long)Probe->Luid.HighPart,
           (unsigned long)Probe->Luid.LowPart, (unsigned long)Probe->hAdapter);

    ZeroMemory(&device, sizeof(device));
    device.hAdapter = Probe->hAdapter;
    if (!NT_SUCCESS(Report("D3DKMTCreateDevice", D3DKMTCreateDevice(&device)))) return FALSE;
    Probe->hDevice = device.hDevice;
    Note("hDevice 0x%08lX", (unsigned long)Probe->hDevice);

    ZeroMemory(&queue, sizeof(queue));
    queue.hDevice = Probe->hDevice;
    queue.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    queue.PhysicalAdapterIndex = 0;
    if (!NT_SUCCESS(Report("D3DKMTCreatePagingQueue", D3DKMTCreatePagingQueue(&queue)))) return FALSE;
    Probe->hPagingQueue = queue.hPagingQueue;
    Probe->hPagingFenceObject = queue.hSyncObject;
    Probe->PagingFence = (volatile UINT64*)queue.FenceValueCPUVirtualAddress;

    // The E46 witness: whether anyone holds VidPn source 0 exclusively. The DWM does not; the refusal came anyway.
    ZeroMemory(&owner, sizeof(owner));
    owner.hAdapter = Probe->hAdapter;
    owner.VidPnSourceId = 0;
    Report("D3DKMTCheckVidPnExclusiveOwnership (0)", D3DKMTCheckVidPnExclusiveOwnership(&owner));
    return TRUE;
}

static BOOL WaitPagingFence(PROBE* Probe, UINT64 Value, const char* What)
{
    ULONGLONG deadline = GetTickCount64() + 5000;

    if (Value == 0) return TRUE;
    if (Probe->PagingFence == NULL) { Note("paging fence for %s is %llu with no CPU mapping", What, Value); return FALSE; }
    for (;;)
    {
        UINT64 seen = *Probe->PagingFence;

        if (seen >= Value) return TRUE;
        if (GetTickCount64() >= deadline) { Note("TIMEOUT on the %s paging fence: %llu, wanted %llu", What, seen, Value); return FALSE; }
        Sleep(1);
    }
}

static BOOL MapAndMakeResident(PROBE* Probe, D3DKMT_HANDLE hAllocation, UINT64 Size, const char* Name, UINT64* Va)
{
    D3DDDI_MAPGPUVIRTUALADDRESS map;
    D3DDDI_MAKERESIDENT resident;
    D3DKMT_HANDLE list[1];
    UINT priorities[1];
    NTSTATUS status;
    char label[64];

    ZeroMemory(&map, sizeof(map));
    map.hPagingQueue = Probe->hPagingQueue;
    map.hAllocation = hAllocation;
    map.MinimumAddress = VA_MIN;
    map.MaximumAddress = VA_MAX;
    map.SizeInPages = (Size + GPU_PAGE_SIZE - 1) / GPU_PAGE_SIZE;
    map.Protection.Write = 1;
    sprintf_s(label, sizeof(label), "D3DKMTMapGpuVirtualAddress [%s]", Name);
    if (!NT_SUCCESS(Report(label, D3DKMTMapGpuVirtualAddress(&map)))) return FALSE;
    *Va = map.VirtualAddress;
    Note("%s mapped at 0x%016llX, %llu page(s), paging fence %llu", Name, map.VirtualAddress, map.SizeInPages,
         map.PagingFenceValue);
    if (!WaitPagingFence(Probe, map.PagingFenceValue, label)) return FALSE;

    list[0] = hAllocation;
    priorities[0] = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    ZeroMemory(&resident, sizeof(resident));
    resident.hPagingQueue = Probe->hPagingQueue;
    resident.NumAllocations = 1;
    resident.AllocationList = list;
    resident.PriorityList = priorities;
    sprintf_s(label, sizeof(label), "D3DKMTMakeResident [%s]", Name);
    status = Report(label, D3DKMTMakeResident(&resident));
    if (status != STATUS_PENDING && !NT_SUCCESS(status)) return FALSE;
    if (resident.NumAllocations != 1) { Note("%u of 1 made resident", resident.NumAllocations); return FALSE; }
    return WaitPagingFence(Probe, resident.PagingFenceValue, label);
}

static BOOL StepSource(PROBE* Probe)
{
    BC250_WDDM_ALLOCATION_PRIVATE private;
    D3DDDI_ALLOCATIONINFO2 info;
    D3DKMT_CREATEALLOCATION create;
    D3DKMT_LOCK2 lock;
    D3DKMT_UNLOCK2 unlock;
    ULONG width = (ULONG)Probe->Opt.Width, height = (ULONG)Probe->Opt.Height;
    ULONG pitch = ((width * 4ul) + PITCH_ALIGN - 1ul) & ~(PITCH_ALIGN - 1ul);
    UINT64 size = ((UINT64)pitch * height + GPU_PAGE_SIZE - 1) & ~(GPU_PAGE_SIZE - 1);
    UINT32* pixels;
    ULONG x, y;

    ZeroMemory(&private, sizeof(private));
    private.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    private.Version = BC250_ALLOCATION_PRIVATE_VERSION;
    private.Width = width;
    private.Height = height;
    private.Pitch = pitch;
    private.Format = (ULONG)D3DDDIFMT_A8R8G8B8;
    private.Size = size;
    ZeroMemory(&info, sizeof(info));
    info.pPrivateDriverData = &private;
    info.PrivateDriverDataSize = (UINT)sizeof(private);
    ZeroMemory(&create, sizeof(create));
    create.hDevice = Probe->hDevice;
    create.NumAllocations = 1;
    create.pAllocationInfo2 = &info;
    Note("source LB7A: %lux%lu pitch %lu format A8R8G8B8 size %llu", width, height, pitch, size);
    if (!NT_SUCCESS(Report("D3DKMTCreateAllocation2 [source]", D3DKMTCreateAllocation2(&create)))) return FALSE;
    Probe->hSource = info.hAllocation;
    Probe->SourceSize = size;
    Probe->SourcePitch = pitch;
    printf("SOURCE halloc=0x%08lX size=%llu pitch=%lu\n", (unsigned long)Probe->hSource, size, pitch);
    if (!MapAndMakeResident(Probe, Probe->hSource, size, "source", &Probe->SourceVa)) return FALSE;

    // The known colour, written by the CPU through Lock2 (the KMD marks its LB7A allocations CPU-visible).
    ZeroMemory(&lock, sizeof(lock));
    lock.hDevice = Probe->hDevice;
    lock.hAllocation = Probe->hSource;
    if (!NT_SUCCESS(Report("D3DKMTLock2 [source]", D3DKMTLock2(&lock))) || lock.pData == NULL) return FALSE;
    for (y = 0; y < height; y++)
    {
        pixels = (UINT32*)((BYTE*)lock.pData + (UINT64)y * pitch);
        for (x = 0; x < width; x++) pixels[x] = Probe->Opt.Color;
    }
    ZeroMemory(&unlock, sizeof(unlock));
    unlock.hDevice = Probe->hDevice;
    unlock.hAllocation = Probe->hSource;
    if (!NT_SUCCESS(Report("D3DKMTUnlock2 [source]", D3DKMTUnlock2(&unlock)))) return FALSE;
    Note("filled with 0x%08X", Probe->Opt.Color);
    return TRUE;
}

// The fork's present context, field for field (radv_wddm2_wsi.c, radv_wddm2_wsi_present_context): node 0,
// EngineAffinity 1, ClientHint VULKAN, no flags, no private data. Variant D differs only in the hint.
static BOOL CreateContext(PROBE* Probe, D3DKMT_CLIENTHINT Hint, D3DKMT_HANDLE* Out, const char* Name)
{
    D3DKMT_CREATECONTEXTVIRTUAL create;
    char label[64];

    ZeroMemory(&create, sizeof(create));
    create.hDevice = Probe->hDevice;
    create.NodeOrdinal = 0;
    create.EngineAffinity = 1;
    create.ClientHint = Hint;
    sprintf_s(label, sizeof(label), "D3DKMTCreateContextVirtual [%s]", Name);
    if (!NT_SUCCESS(Report(label, D3DKMTCreateContextVirtual(&create)))) return FALSE;
    *Out = create.hContext;
    Note("%s hContext 0x%08lX (node 0, affinity 1, hint %u)", Name, (unsigned long)*Out, (unsigned)Hint);
    return TRUE;
}

// ---- step 1: the handshake ------------------------------------------------------------------------------------

typedef struct _HANDSHAKE_CALL {
    PROBE* Probe;
    HANDSHAKE Result;
} HANDSHAKE_CALL;

static DWORD WINAPI HandshakeThread(LPVOID Parameter)
{
    HANDSHAKE_CALL* call = (HANDSHAKE_CALL*)Parameter;
    ULONGLONG t0 = GetTickCount64();

    call->Result.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    call->Result.Surface = NULL;
    call->Result.UpdateId = 0;
    call->Result.hr = call->Probe->GetSurface(call->Probe->Window, call->Probe->Luid, NULL,
                                              DWM_REDIRECTION_FLAG_SUPPORT_PRESENT_TO_GDI_SURFACE,
                                              &call->Result.Format, &call->Result.Surface, &call->Result.UpdateId);
    call->Result.Ms = (DWORD)(GetTickCount64() - t0);
    call->Result.Completed = TRUE;
    return 0;
}

// Which adapter the shared handle belongs to, and thereby what kind of handle it is: a global share handle
// (the Windows 7 form) answers the first query, an NT handle the second. A query only: nothing is opened.
static void ClassifyHandle(PROBE* Probe)
{
    HANDSHAKE* h = &Probe->Handshake;
    D3DKMT_GETSHAREDRESOURCEADAPTERLUID luid;

    h->Kind = HandleUnknown;
    h->LuidKnown = FALSE;
    h->LuidOurs = FALSE;
    if (h->Surface == NULL) { printf("SHARED kind=none\n"); fflush(stdout); return; }
    ZeroMemory(&luid, sizeof(luid));
    luid.hGlobalShare = (D3DKMT_HANDLE)(ULONG_PTR)h->Surface;
    if (NT_SUCCESS(Report("D3DKMTGetSharedResourceAdapterLuid (global)", D3DKMTGetSharedResourceAdapterLuid(&luid))))
        h->Kind = HandleGlobal;
    else
    {
        ZeroMemory(&luid, sizeof(luid));
        luid.hNtHandle = h->Surface;
        if (NT_SUCCESS(Report("D3DKMTGetSharedResourceAdapterLuid (nt)", D3DKMTGetSharedResourceAdapterLuid(&luid))))
            h->Kind = HandleNt;
    }
    if (h->Kind == HandleUnknown) { printf("SHARED kind=unknown handle=0x%p (not closed at exit)\n", h->Surface); fflush(stdout); return; }
    h->Luid = luid.AdapterLuid;
    h->LuidKnown = TRUE;
    h->LuidOurs = h->Luid.LowPart == Probe->Luid.LowPart && h->Luid.HighPart == Probe->Luid.HighPart;
    if (RedirbltHandleSetAdd(&Probe->OwnedNtHandles, h->Surface, h->Kind))
        Note("NT handle 0x%p is ours: closed once at teardown", h->Surface);
    printf("SHARED kind=%s handle=0x%p luid=%08lX:%08lX ours=%d\n", h->Kind == HandleGlobal ? "global" : "nt", h->Surface,
           (unsigned long)h->Luid.HighPart, (unsigned long)h->Luid.LowPart, h->LuidOurs);
    fflush(stdout);
}

// DWM_REDIRECTION_FLAG_WAIT is documented with value 0, so every call may block until a VSync has passed. The
// call therefore runs on its own thread with a deadline. A worker that has not returned still holds the
// window, the dwmapi module and the probe block, and the watchdog is cancelled on a normal exit: so an
// unresolved wait ends the process here, without the ordinary teardown (exit 5), never with TerminateThread.
static BOOL Handshake(PROBE* Probe)
{
    HANDSHAKE_CALL* call;
    HANDLE thread;
    DWORD wait, error;
    const char* why = "";

    if (Probe->GetSurface == NULL) return FALSE;
    call = (HANDSHAKE_CALL*)calloc(1, sizeof(*call));
    if (call == NULL) return FALSE;
    call->Probe = Probe;
    Probe->HandshakeCount++;
    Stamp("handshake");
    thread = CreateThread(NULL, 0, HandshakeThread, call, 0, NULL);
    if (thread == NULL) { free(call); return FALSE; }
    wait = WaitForSingleObject(thread, Probe->Opt.HandshakeTimeoutMs);
    error = wait == WAIT_FAILED ? GetLastError() : 0;
    if (RedirbltAfterWait(wait, error, &why) != WaitProceed)
    {
        printf("HANDSHAKE n=%u unresolved wait=0x%08lX error=%lu deadline_ms=%lu: %s\n", Probe->HandshakeCount,
               (unsigned long)wait, error, Probe->Opt.HandshakeTimeoutMs, why);
        printf("SUMMARY ordinal100=1 ordinal101=%d handshakes=%u handshake_hr=unresolved first_success=none exit=5\n",
               Probe->UpdateSurface != NULL, Probe->HandshakeCount);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 5);
        return FALSE;
    }
    CloseHandle(thread);
    Probe->Handshake = call->Result;
    free(call);
    ReportHr("DwmDxGetWindowSharedSurface", Probe->Handshake.hr);
    printf("HANDSHAKE n=%u hr=0x%08lX fmt=%u handle=0x%p update=%llu ms=%lu\n", Probe->HandshakeCount,
           (unsigned long)Probe->Handshake.hr, (unsigned)Probe->Handshake.Format, Probe->Handshake.Surface,
           (unsigned long long)Probe->Handshake.UpdateId, Probe->Handshake.Ms);
    fflush(stdout);
    if (SUCCEEDED(Probe->Handshake.hr)) ClassifyHandle(Probe);
    return SUCCEEDED(Probe->Handshake.hr);
}

// ---- step 1b: open the shared surface on our device -----------------------------------------------------------------

static void CloseOpened(PROBE* Probe)
{
    OPENED* o = &Probe->Opened;

    if (!o->Owned) { ZeroMemory(o, sizeof(*o)); return; }
    if (o->MappedVa != 0)
    {
        D3DKMT_FREEGPUVIRTUALADDRESS free_va;

        ZeroMemory(&free_va, sizeof(free_va));
        free_va.hAdapter = Probe->hAdapter;
        free_va.BaseAddress = o->MappedVa;
        free_va.Size = o->MappedSize;
        (void)Report("D3DKMTFreeGpuVirtualAddress [opened]", D3DKMTFreeGpuVirtualAddress(&free_va));
    }
    {
        // An opened resource is released through its resource handle; the allocations go with it.
        D3DKMT_DESTROYALLOCATION2 destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hDevice = Probe->hDevice;
        destroy.hResource = o->hResource;
        (void)Report("D3DKMTDestroyAllocation2 [opened]", D3DKMTDestroyAllocation2(&destroy));
    }
    ZeroMemory(o, sizeof(*o));
}

// Opens the surface of the latest handshake. Ready only when the surface is on our adapter, the blob is the
// KMD's LB7A block, and the map and the residency both succeeded; Owned as soon as the open succeeded, so that
// a later failure still releases the resource. Anything unknown is a rejection (review 236 item 5).
static BOOL OpenShared(PROBE* Probe)
{
    OPENED* o = &Probe->Opened;
    HANDSHAKE* h = &Probe->Handshake;
    HANDLE hs = h->Surface;
    NTSTATUS status;
    UINT total = 0, resource = 0, runtime = 0, num = 0;
    void* buffer = NULL;
    D3DDDI_OPENALLOCATIONINFO2* infos = NULL;
    UINT i;
    BOOL blobOurs = FALSE, mapped = FALSE, resident = FALSE;
    const char* why = "";
    UINT64 size = 0;

    CloseOpened(Probe);
    if (hs == NULL || !h->Completed) { Note("no surface handle to open"); return FALSE; }
    if (h->Kind == HandleUnknown) { printf("OPEN rejected: handle kind unknown\n"); fflush(stdout); return FALSE; }
    if (!h->LuidOurs)
    {
        printf("OPEN rejected: the surface lives on adapter %08lX:%08lX, not ours\n", (unsigned long)h->Luid.HighPart,
               (unsigned long)h->Luid.LowPart);
        fflush(stdout);
        return FALSE;
    }
    if (Probe->hDevice == 0) { printf("OPEN rejected: no device\n"); fflush(stdout); return FALSE; }
    o->OpenedFrom = hs;
    o->UpdateId = h->UpdateId;
    o->Kind = h->Kind;

    if (h->Kind == HandleGlobal)
    {
        D3DKMT_QUERYRESOURCEINFO query;

        ZeroMemory(&query, sizeof(query));
        query.hDevice = Probe->hDevice;
        query.hGlobalShare = (D3DKMT_HANDLE)(ULONG_PTR)hs;
        if (!NT_SUCCESS(Report("D3DKMTQueryResourceInfo", D3DKMTQueryResourceInfo(&query)))) return FALSE;
        total = query.TotalPrivateDriverDataSize; resource = query.ResourcePrivateDriverDataSize;
        runtime = query.PrivateRuntimeDataSize; num = query.NumAllocations;
    }
    else
    {
        D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query;

        ZeroMemory(&query, sizeof(query));
        query.hDevice = Probe->hDevice;
        query.hNtHandle = hs;
        if (!NT_SUCCESS(Report("D3DKMTQueryResourceInfoFromNtHandle", D3DKMTQueryResourceInfoFromNtHandle(&query)))) return FALSE;
        total = query.TotalPrivateDriverDataSize; resource = query.ResourcePrivateDriverDataSize;
        runtime = query.PrivateRuntimeDataSize; num = query.NumAllocations;
    }
    Note("resource: %u allocation(s), private %u total / %u resource / %u runtime bytes", num, total, resource, runtime);
    if (num == 0 || num > 16) { printf("OPEN rejected: %u allocations\n", num); fflush(stdout); return FALSE; }
    buffer = calloc(1, (size_t)total + resource + runtime + 16);
    infos = (D3DDDI_OPENALLOCATIONINFO2*)calloc(num, sizeof(*infos));
    if (buffer == NULL || infos == NULL) { free(buffer); free(infos); return FALSE; }

    if (h->Kind == HandleGlobal)
    {
        D3DKMT_OPENRESOURCE open;

        ZeroMemory(&open, sizeof(open));
        open.hDevice = Probe->hDevice;
        open.hGlobalShare = (D3DKMT_HANDLE)(ULONG_PTR)hs;
        open.NumAllocations = num;
        open.pOpenAllocationInfo2 = infos;
        open.pTotalPrivateDriverDataBuffer = buffer;
        open.TotalPrivateDriverDataBufferSize = total;
        open.pResourcePrivateDriverData = (BYTE*)buffer + total;
        open.ResourcePrivateDriverDataSize = resource;
        open.pPrivateRuntimeData = (BYTE*)buffer + total + resource;
        open.PrivateRuntimeDataSize = runtime;
        status = Report("D3DKMTOpenResource", D3DKMTOpenResource(&open));
        o->hResource = open.hResource;
    }
    else
    {
        D3DKMT_OPENRESOURCEFROMNTHANDLE open;

        ZeroMemory(&open, sizeof(open));
        open.hDevice = Probe->hDevice;
        open.hNtHandle = hs;
        open.NumAllocations = num;
        open.pOpenAllocationInfo2 = infos;
        open.pTotalPrivateDriverDataBuffer = buffer;
        open.TotalPrivateDriverDataBufferSize = total;
        open.pResourcePrivateDriverData = (BYTE*)buffer + total;
        open.ResourcePrivateDriverDataSize = resource;
        open.pPrivateRuntimeData = (BYTE*)buffer + total + resource;
        open.PrivateRuntimeDataSize = runtime;
        status = Report("D3DKMTOpenResourceFromNtHandle", D3DKMTOpenResourceFromNtHandle(&open));
        o->hResource = open.hResource;
    }
    if (!NT_SUCCESS(status)) { free(buffer); free(infos); return FALSE; }
    o->Owned = TRUE;                    // from here on, CloseOpened releases it whatever happens below
    o->NumAllocations = num; o->TotalPrivate = total; o->ResourcePrivate = resource; o->RuntimePrivate = runtime;
    o->hAllocation = infos[0].hAllocation;
    o->GpuVa = infos[0].GpuVirtualAddress;
    o->PrivateBytes = infos[0].PrivateDriverDataSize;
    if (infos[0].pPrivateDriverData != NULL && infos[0].PrivateDriverDataSize != 0)
        memcpy(o->Private32, infos[0].pPrivateDriverData,
               infos[0].PrivateDriverDataSize < 32 ? infos[0].PrivateDriverDataSize : 32);
    printf("OPENED numalloc=%u hres=0x%08lX halloc=0x%08lX va=0x%016llX private=%u bytes first16=", num,
           (unsigned long)o->hResource, (unsigned long)o->hAllocation, (unsigned long long)o->GpuVa, o->PrivateBytes);
    for (i = 0; i < 16; i++) printf("%02X", o->Private32[i]);
    printf("\n");
    fflush(stdout);
    free(buffer);
    free(infos);

    // Geometry: only the KMD's own LB7A block is trusted for the size of the map.
    if (o->PrivateBytes >= 32)
    {
        BC250_WDDM_ALLOCATION_PRIVATE p;

        memcpy(&p, o->Private32, sizeof(p));
        Note("blob: magic 0x%08lX version %lu %lux%lu pitch %lu format %lu size %llu%s", p.Magic, p.Version, p.Width,
             p.Height, p.Pitch, p.Format, (unsigned long long)p.Size,
             p.Magic == BC250_WDDM_ALLOCATION_PRIVATE_MAGIC ? " (LB7A)" : " (not LB7A)");
        if (p.Magic == BC250_WDDM_ALLOCATION_PRIVATE_MAGIC && p.Version == BC250_ALLOCATION_PRIVATE_VERSION && p.Size != 0 &&
            p.Size < (1ull << 32) && p.Width != 0 && p.Height != 0 && p.Pitch >= p.Width * 4ul)
        {
            blobOurs = TRUE;
            size = p.Size;
        }
    }
    if (blobOurs && o->hAllocation != 0)
    {
        // A destination the KMD can address: mapped in our process, as the fork maps every opened allocation.
        if (o->GpuVa != 0) { mapped = TRUE; resident = TRUE; Note("open reported a GPU VA already; no map of our own"); }
        else if (MapAndMakeResident(Probe, o->hAllocation, size, "opened", &o->MappedVa)) { o->MappedSize = size; mapped = TRUE; resident = TRUE; }
        else if (o->MappedVa != 0) { o->MappedSize = size; mapped = TRUE; }   // the map went through, residency did not
    }
    o->Ready = RedirbltDestinationReady(h->LuidKnown, h->LuidOurs, blobOurs && o->hAllocation != 0, mapped, resident, &why);
    printf("DESTINATION ready=%d from=0x%p update=%llu: %s\n", o->Ready, o->OpenedFrom, (unsigned long long)o->UpdateId, why);
    fflush(stdout);
    return o->Ready;
}

// ---- step 2: the present variants ---------------------------------------------------------------------------------

static NTSTATUS PresentVariant(PROBE* Probe, VARIANT_ID Variant)
{
    D3DKMT_PRESENT present;
    RECT full = Probe->Client;
    NTSTATUS status;
    D3DKMT_HANDLE context = Variant == VariantD ? Probe->hContextGl : Probe->hContext;
    BOOL token = Variant != VariantV0;
    BOOL destination = Variant == VariantB || Variant == VariantC || Variant == VariantD;

    ZeroMemory(&present, sizeof(present));
    present.hContext = context;
    present.hWindow = Probe->Window;
    present.hSource = Probe->hSource;
    present.DstRect = full;
    present.SrcRect = full;
    present.SubRectCnt = 1;
    present.pSrcSubRects = &full;
    present.Flags.Blt = 1;
    present.Flags.DstRectValid = 1;
    present.Flags.SrcRectValid = 1;
    if (token)
    {
        present.Flags.RedirectedBlt = 1;
        present.PresentHistoryToken.Model = D3DKMT_PM_REDIRECTED_BLT;
        present.PresentHistoryToken.TokenSize = 0;
        present.PresentHistoryToken.Token.Blt.EventId = Probe->Handshake.UpdateId;
        present.PresentHistoryToken.Token.Blt.DirtyRegions.NumRects = 1;
        present.PresentHistoryToken.Token.Blt.DirtyRegions.Rects[0] = full;
    }
    if (destination)
    {
        // RunVariants admitted this variant only with a ready destination opened from this very handshake.
        present.hDestination = Probe->Opened.hAllocation;
        if (Variant == VariantC)
            present.PresentHistoryToken.Token.Blt.hPhysicalSurface = (ULONG64)(ULONG_PTR)Probe->Opened.OpenedFrom;
    }

    Stamp("present");
    printf("PRESENT variant=%s context=0x%08lX flags=0x%08X hsource=0x%08lX hdest=0x%08lX token_model=%u eventid=%llu"
           " physical=0x%llX rect=%ld,%ld,%ld,%ld\n", g_VariantNames[Variant], (unsigned long)context,
           present.Flags.Value, (unsigned long)present.hSource, (unsigned long)present.hDestination,
           (unsigned)present.PresentHistoryToken.Model, (unsigned long long)present.PresentHistoryToken.Token.Blt.EventId,
           (unsigned long long)present.PresentHistoryToken.Token.Blt.hPhysicalSurface, full.left, full.top, full.right,
           full.bottom);
    fflush(stdout);
    status = Report("D3DKMTPresent", D3DKMTPresent(&present));
    printf("RESULT variant=%s status=0x%08lX name=%s composition=%u\n", g_VariantNames[Variant], (unsigned long)status,
           StatusName(status)[0] ? StatusName(status) : "?", (unsigned)present.bOptimizeForComposition);
    fflush(stdout);
    return status;
}

static void RunVariants(PROBE* Probe)
{
    int v;

    for (v = 0; v < VariantCount; v++)
    {
        BOOL needsHandshake = v != VariantV0;
        BOOL needsDestination = v == VariantB || v == VariantC || v == VariantD;
        BOOL fromThisHandle = FALSE;
        const char* why = "";

        if (!Probe->Opt.Variants[v]) continue;
        if (needsHandshake)
        {
            // A fresh update id per attempt; the DWM may reserve one per call. A destination opened from an
            // earlier handshake handle is stale for this one: reopen, never mix ids and surfaces. The same
            // handle with a new id is the same validated surface: bind the new id to it (review 238).
            (void)Handshake(Probe);
            if (needsDestination && !Probe->Opt.NoOpen && Probe->Handshake.Completed &&
                Probe->Handshake.hr == DWM_S_GDI_REDIRECTION_SURFACE)
            {
                const char* action = "";

                switch (RedirbltDestinationAction(Probe->Opened.Owned, Probe->Opened.OpenedFrom, Probe->Opened.UpdateId,
                                                  Probe->Handshake.Surface, Probe->Handshake.UpdateId, &action))
                {
                case DestinationReopen:
                    (void)OpenShared(Probe);
                    break;
                case DestinationRebind:
                    printf("REBIND from=0x%p update=%llu->%llu: %s\n", Probe->Opened.OpenedFrom,
                           (unsigned long long)Probe->Opened.UpdateId, (unsigned long long)Probe->Handshake.UpdateId, action);
                    fflush(stdout);
                    Probe->Opened.UpdateId = Probe->Handshake.UpdateId;
                    break;
                case DestinationKeep:
                    break;
                }
            }
            fromThisHandle = RedirbltDestinationBound(Probe->Opened.Owned, Probe->Opened.OpenedFrom, Probe->Opened.UpdateId,
                                                      Probe->Handshake.Surface, Probe->Handshake.UpdateId);
        }
        if (!RedirbltVariantAllowed((VARIANT_ID)v, Probe->Handshake.Completed, Probe->Handshake.hr, Probe->Opt.NoOpen,
                                    Probe->Opened.Ready, fromThisHandle, &why))
        {
            printf("SKIP variant=%s: %s\n", g_VariantNames[v], why);
            fflush(stdout);
            continue;
        }
        if (v == VariantD && Probe->hContextGl == 0 && !CreateContext(Probe, D3DKMT_CLIENTHINT_OPENGL, &Probe->hContextGl, "opengl"))
        {
            printf("SKIP variant=D: no OPENGL context\n");
            continue;
        }
        Probe->VariantStatus[v] = PresentVariant(Probe, (VARIANT_ID)v);
        Probe->VariantRan[v] = TRUE;
        Pump(100);
        if (NT_SUCCESS(Probe->VariantStatus[v]))
        {
            if (Probe->FirstSuccess < 0) Probe->FirstSuccess = v;
            if (!Probe->Opt.All) break;
        }
    }
}

// ---- step 3: the update call, only on request and only after an S_OK handshake ------------------------------------
//
// Ordinal 101 is documented for the S_OK branch alone (dedicated DX surface, D3DKMTRender route). The
// GDI-surface branch carries its update id in the present token and documents no update call, so a successful
// redirected variant never leads here (review 236 item 2).

static void UpdateAfterHandshake(PROBE* Probe)
{
    RECT rc = Probe->Client;
    HRESULT hr;
    const char* why = "";

    if (Probe->UpdateSurface == NULL) { Note("ordinal 101 absent, no update call"); return; }
    if (!RedirbltUpdateAllowed(Probe->Handshake.Completed, Probe->Handshake.hr, &why)) { Note("no update call: %s", why); return; }
    hr = Probe->UpdateSurface(Probe->Window, Probe->Handshake.UpdateId, 0, NULL, &rc);
    ReportHr("DwmDxUpdateWindowSharedSurface", hr);
    printf("UPDATE hr=0x%08lX update=%llu\n", (unsigned long)hr, (unsigned long long)Probe->Handshake.UpdateId);
    fflush(stdout);
}

// ---- teardown ----------------------------------------------------------------------------------------------------

static void Teardown(PROBE* Probe)
{
    UINT i;

    printf("-- teardown\n");
    fflush(stdout);
    if (Probe->hContextGl != 0)
    {
        D3DKMT_DESTROYCONTEXT destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hContext = Probe->hContextGl;
        (void)Report("D3DKMTDestroyContext [opengl]", D3DKMTDestroyContext(&destroy));
        Probe->hContextGl = 0;
    }
    if (Probe->hContext != 0)
    {
        D3DKMT_DESTROYCONTEXT destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hContext = Probe->hContext;
        (void)Report("D3DKMTDestroyContext [present]", D3DKMTDestroyContext(&destroy));
        Probe->hContext = 0;
    }
    CloseOpened(Probe);
    // Only NT handles the LUID query classified as such, each once; global share handles and unclassified
    // handles are not handles of this process.
    for (i = 0; i < Probe->OwnedNtHandles.Count; i++)
    {
        Note("CloseHandle 0x%p -> %d", Probe->OwnedNtHandles.Handles[i], CloseHandle(Probe->OwnedNtHandles.Handles[i]));
        Probe->OwnedNtHandles.Handles[i] = NULL;
    }
    Probe->OwnedNtHandles.Count = 0;
    if (Probe->hSource != 0)
    {
        D3DKMT_DESTROYALLOCATION2 destroy;
        D3DKMT_HANDLE list[1];

        if (Probe->SourceVa != 0)
        {
            D3DKMT_FREEGPUVIRTUALADDRESS free_va;

            ZeroMemory(&free_va, sizeof(free_va));
            free_va.hAdapter = Probe->hAdapter;
            free_va.BaseAddress = Probe->SourceVa;
            free_va.Size = Probe->SourceSize;
            (void)Report("D3DKMTFreeGpuVirtualAddress [source]", D3DKMTFreeGpuVirtualAddress(&free_va));
        }
        list[0] = Probe->hSource;
        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hDevice = Probe->hDevice;
        destroy.phAllocationList = list;
        destroy.AllocationCount = 1;
        (void)Report("D3DKMTDestroyAllocation2 [source]", D3DKMTDestroyAllocation2(&destroy));
        Probe->hSource = 0;
    }
    if (Probe->hPagingQueue != 0)
    {
        D3DDDI_DESTROYPAGINGQUEUE destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hPagingQueue = Probe->hPagingQueue;
        (void)Report("D3DKMTDestroyPagingQueue", D3DKMTDestroyPagingQueue(&destroy));
        Probe->hPagingQueue = 0;
    }
    if (Probe->hDevice != 0)
    {
        D3DKMT_DESTROYDEVICE destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hDevice = Probe->hDevice;
        (void)Report("D3DKMTDestroyDevice", D3DKMTDestroyDevice(&destroy));
        Probe->hDevice = 0;
    }
    if (Probe->hAdapter != 0)
    {
        D3DKMT_CLOSEADAPTER close;

        ZeroMemory(&close, sizeof(close));
        close.hAdapter = Probe->hAdapter;
        (void)Report("D3DKMTCloseAdapter", D3DKMTCloseAdapter(&close));
        Probe->hAdapter = 0;
    }
    if (Probe->Window != NULL) { DestroyWindow(Probe->Window); Probe->Window = NULL; }
    if (Probe->Dwmapi != NULL) { FreeLibrary(Probe->Dwmapi); Probe->Dwmapi = NULL; }
}

// ---- options -------------------------------------------------------------------------------------------------------

static void Usage(void)
{
    printf(
        "redirblt-probe - the DWM redirected-blt handshake and D3DKMTPresent variants from a non-runtime client.\n"
        "Runs on the target, in the interactive session, under the hosted GPU DWM. Build-only on the dev PC.\n"
        "\n"
        "  --match <text>     substring of the adapter string / chip type / UMD name (default \"bc250\")\n"
        "  --luid H:L         pick the adapter by LUID in hex instead\n"
        "  --size WxH         client area (default 641x479: an odd size that stands out in the trace)\n"
        "  --color <hex32>    ARGB pixel value of the source (default FF0000FF, blue)\n"
        "  --variants <list>  comma list from V0,A,B,C,D (default all, in that order)\n"
        "  --all              run every listed variant instead of stopping at the first STATUS_SUCCESS\n"
        "  --handshake-only   window, adapter identity, handshake and (unless --no-open) the open; no source,\n"
        "                     no context, no present. The first discriminating run.\n"
        "  --gdi-paint        fill the client area once with GDI before the handshake (control)\n"
        "  --no-open          diagnostic: do not open the shared handle; B, C and D are skipped, never run\n"
        "                     with hDestination 0. With --handshake-only, no device is created either.\n"
        "  --update           after an S_OK handshake (dedicated DX surface) call ordinal 101 once; never after\n"
        "                     DWM_S_GDI_REDIRECTION_SURFACE, whatever a present variant returned\n"
        "  --pump <ms>        message pump after ShowWindow (default 500)\n"
        "  --hold <s>         keep the window and the source alive after the variants (default 5)\n"
        "  --handshake-timeout <ms>  deadline for one DwmDxGetWindowSharedSurface call (default 2000)\n"
        "  --timeout <s>      watchdog on the whole process (default 90, raised to cover --hold)\n"
        "  --help\n"
        "\n"
        "Variants: V0 = E45 form (Blt, rects, no token); A = V0 + RedirectedBlt + REDIRECTED_BLT token with the\n"
        "update id; B = A + hDestination = the opened shared surface; C = B + hPhysicalSurface = the raw handle;\n"
        "D = B on a context with ClientHint OPENGL. B, C, D need a destination opened from the same handshake.\n"
        "Exit: 0 sequence ran, 1 a step failed, 2 bad arguments, 3 dwmapi ordinal 100 absent, 4 watchdog,\n"
        "5 a handshake call did not return (terminated without teardown).\n");
}

static BOOL ParseLuid(const char* Text, LUID* Luid)
{
    char* end;
    unsigned long high, low;

    high = strtoul(Text, &end, 16);
    if (*end != ':') return FALSE;
    low = strtoul(end + 1, &end, 16);
    if (*end != '\0') return FALSE;
    Luid->HighPart = (LONG)high;
    Luid->LowPart = low;
    return TRUE;
}

static BOOL ParseSize(const char* Text, LONG* Width, LONG* Height)
{
    char* end;
    unsigned long w, h;

    w = strtoul(Text, &end, 10);
    if (*end != 'x' && *end != 'X') return FALSE;
    h = strtoul(end + 1, &end, 10);
    if (*end != '\0' || w == 0 || h == 0 || w > 8192 || h > 8192) return FALSE;
    *Width = (LONG)w;
    *Height = (LONG)h;
    return TRUE;
}

static BOOL ParseVariants(const char* Text, BOOL* Variants)
{
    const char* p = Text;
    int v;

    for (v = 0; v < VariantCount; v++) Variants[v] = FALSE;
    while (*p != '\0')
    {
        const char* q = p;
        size_t n;
        BOOL known = FALSE;

        while (*q != '\0' && *q != ',') q++;
        n = (size_t)(q - p);
        for (v = 0; v < VariantCount; v++)
            if (strlen(g_VariantNames[v]) == n && _strnicmp(p, g_VariantNames[v], n) == 0) { Variants[v] = TRUE; known = TRUE; }
        if (!known) return FALSE;
        p = *q == ',' ? q + 1 : q;
    }
    return TRUE;
}

#define NEED_VALUE(i, argc) do { if ((i) + 1 >= (argc)) { printf("%s needs a value\n", argv[i]); return 2; } } while (0)

int main(int argc, char** argv)
{
    PROBE probe;
    int i, v;
    int code = 0;
    BOOL needDevice, needSource;

    ZeroMemory(&probe, sizeof(probe));
    probe.Opt.Match = "bc250";
    probe.Opt.Width = 641;
    probe.Opt.Height = 479;
    probe.Opt.Color = 0xFF0000FFu;
    for (v = 0; v < VariantCount; v++) probe.Opt.Variants[v] = TRUE;
    probe.Opt.PumpMs = 500;
    probe.Opt.HoldSeconds = 5;
    probe.Opt.HandshakeTimeoutMs = 2000;
    probe.Opt.WatchdogMs = 90000;
    probe.FirstSuccess = -1;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { Usage(); return 0; }
        else if (strcmp(argv[i], "--match") == 0) { NEED_VALUE(i, argc); probe.Opt.Match = argv[++i]; }
        else if (strcmp(argv[i], "--luid") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseLuid(argv[++i], &probe.Opt.Luid)) { printf("bad --luid, want HIGH:LOW in hex\n"); return 2; }
            probe.Opt.HaveLuid = TRUE;
        }
        else if (strcmp(argv[i], "--size") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseSize(argv[++i], &probe.Opt.Width, &probe.Opt.Height)) { printf("bad --size, want WxH\n"); return 2; }
        }
        else if (strcmp(argv[i], "--color") == 0)
        {
            char* end;

            NEED_VALUE(i, argc);
            probe.Opt.Color = (UINT32)strtoul(argv[++i], &end, 16);
            if (*end != '\0') { printf("bad --color, want 8 hex digits ARGB\n"); return 2; }
        }
        else if (strcmp(argv[i], "--variants") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseVariants(argv[++i], probe.Opt.Variants)) { printf("bad --variants, want a comma list of V0,A,B,C,D\n"); return 2; }
        }
        else if (strcmp(argv[i], "--all") == 0) probe.Opt.All = TRUE;
        else if (strcmp(argv[i], "--handshake-only") == 0) probe.Opt.HandshakeOnly = TRUE;
        else if (strcmp(argv[i], "--gdi-paint") == 0) probe.Opt.GdiPaint = TRUE;
        else if (strcmp(argv[i], "--no-open") == 0) probe.Opt.NoOpen = TRUE;
        else if (strcmp(argv[i], "--update") == 0) probe.Opt.Update = TRUE;
        else if (strcmp(argv[i], "--pump") == 0)
        {
            NEED_VALUE(i, argc);
            probe.Opt.PumpMs = (DWORD)strtoul(argv[++i], NULL, 10);
            if (probe.Opt.PumpMs > 60000) { printf("bad --pump\n"); return 2; }
        }
        else if (strcmp(argv[i], "--hold") == 0)
        {
            NEED_VALUE(i, argc);
            probe.Opt.HoldSeconds = (DWORD)strtoul(argv[++i], NULL, 10);
            if (probe.Opt.HoldSeconds > 600) { printf("bad --hold\n"); return 2; }
        }
        else if (strcmp(argv[i], "--handshake-timeout") == 0)
        {
            NEED_VALUE(i, argc);
            probe.Opt.HandshakeTimeoutMs = (DWORD)strtoul(argv[++i], NULL, 10);
            if (probe.Opt.HandshakeTimeoutMs == 0 || probe.Opt.HandshakeTimeoutMs > 60000) { printf("bad --handshake-timeout\n"); return 2; }
        }
        else if (strcmp(argv[i], "--timeout") == 0)
        {
            unsigned long seconds;

            NEED_VALUE(i, argc);
            seconds = strtoul(argv[++i], NULL, 10);
            if (seconds == 0 || seconds > 3600) { printf("bad --timeout, seconds\n"); return 2; }
            probe.Opt.WatchdogMs = (DWORD)(seconds * 1000);
        }
        else { printf("unknown option %s\n", argv[i]); Usage(); return 2; }
    }
    {
        DWORD needed = probe.Opt.HoldSeconds * 1000 + probe.Opt.PumpMs + 6 * probe.Opt.HandshakeTimeoutMs + 20000;

        if (probe.Opt.WatchdogMs < needed) probe.Opt.WatchdogMs = needed;
    }
    needDevice = RedirbltNeedDevice(probe.Opt.HandshakeOnly, probe.Opt.NoOpen);
    needSource = RedirbltNeedSourceAndContext(probe.Opt.HandshakeOnly);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!StartWatchdog(probe.Opt.WatchdogMs)) { printf("no watchdog\n"); return 1; }
    printf("redirblt-probe: %ldx%ld colour 0x%08X, watchdog %lu ms, handshake-only %d, no-open %d, device %d, source %d\n",
           probe.Opt.Width, probe.Opt.Height, probe.Opt.Color, probe.Opt.WatchdogMs, probe.Opt.HandshakeOnly, probe.Opt.NoOpen,
           needDevice, needSource);
    SetProcessDPIAware();

    if (!StepStatic(&probe)) { code = 1; goto done; }
    if (probe.GetSurface == NULL)
    {
        printf("SUMMARY ordinal100=0: the handshake does not exist on this build; nothing to present\n");
        code = 3;
        goto done;
    }
    if (!StepWindow(&probe)) { code = 1; goto done; }
    if (!StepFindAdapter(&probe)) { code = 1; goto done; }
    if (needDevice && !StepOpenAdapter(&probe)) { code = 1; goto done; }
    if (needSource)
    {
        if (!StepSource(&probe)) { code = 1; goto done; }
        if (!CreateContext(&probe, D3DKMT_CLIENTHINT_VULKAN, &probe.hContext, "present")) { code = 1; goto done; }
    }

    // The first handshake; with a GDI surface offered, the open, then a second handshake to see whether the
    // update id moves per call (which also tells whether the handle stays the same).
    if (Handshake(&probe) && probe.Handshake.hr == DWM_S_GDI_REDIRECTION_SURFACE)
    {
        UINT64 first = probe.Handshake.UpdateId;
        HANDLE firstHandle = probe.Handshake.Surface;

        if (!probe.Opt.NoOpen) (void)OpenShared(&probe);
        if (Handshake(&probe))
            Note("update id %llu then %llu (%s), handle %s", (unsigned long long)first, (unsigned long long)probe.Handshake.UpdateId,
                 probe.Handshake.UpdateId != first ? "one per call" : "unchanged",
                 probe.Handshake.Surface == firstHandle ? "same" : "DIFFERENT");
    }
    else if (probe.Handshake.Completed)
        Note("the DWM did not offer a GDI redirection surface for this window on this adapter (see HANDSHAKE)");

    if (!probe.Opt.HandshakeOnly) RunVariants(&probe);
    if (probe.Opt.Update) UpdateAfterHandshake(&probe);

    {
        POINT origin = { 0, 0 };
        UINT32 c = probe.Opt.Color;

        ClientToScreen(probe.Window, &origin);
        printf("capture_ready rgb=%u,%u,%u x=%ld y=%ld width=%ld height=%ld hold_ms=%lu source=%d\n", (c >> 16) & 0xFF,
               (c >> 8) & 0xFF, c & 0xFF, origin.x, origin.y, probe.Client.right, probe.Client.bottom,
               probe.Opt.HoldSeconds * 1000, probe.hSource != 0);
        Stamp("hold");
        Pump(probe.Opt.HoldSeconds * 1000);
    }

done:
    printf("SUMMARY ordinal100=%d ordinal101=%d handshakes=%u handshake_hr=0x%08lX handle_kind=%s destination_ready=%d first_success=%s",
           probe.GetSurface != NULL, probe.UpdateSurface != NULL, probe.HandshakeCount, (unsigned long)probe.Handshake.hr,
           probe.Handshake.Kind == HandleNt ? "nt" : probe.Handshake.Kind == HandleGlobal ? "global" : "unknown",
           probe.Opened.Ready, probe.FirstSuccess >= 0 ? g_VariantNames[probe.FirstSuccess] : "none");
    for (v = 0; v < VariantCount; v++)
        if (probe.VariantRan[v]) printf(" %s=0x%08lX", g_VariantNames[v], (unsigned long)probe.VariantStatus[v]);
    printf(" exit=%d\n", code);
    Teardown(&probe);
    SetEvent(g_Done);
    return code;
}
