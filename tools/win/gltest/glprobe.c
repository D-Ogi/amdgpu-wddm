/*
 * glprobe - which OpenGL ICD the system offers this process, and what opengl32 does with it.
 *
 * Why it exists (BD-076): the first lab arm registered the Mesa WGL ICD in the display adapter's software key and
 * gltest still got Windows' own software OpenGL 1.1 in both a 64-bit and a 32-bit process. gltest reports what the
 * process got, never why. This tool reports the steps in between, each from its own source, so that a lab arm can
 * tell the possible causes apart instead of trying again:
 *
 *   1. what the kernel answers. D3DKMTQueryAdapterInfo(KMTQAITYPE_UMOPENGLINFO) returns
 *      D3DKMT_OPENGLINFO { WCHAR UmdOpenGlIcdFileName[MAX_PATH]; ULONG Version; ULONG Flags; } (WDK/SDK 10.0.26100,
 *      shared\d3dkmthk.h lines 1851-1856, KMTQAITYPE_UMOPENGLINFO = 2 at line 2365). The OpenGL runtime takes the
 *      ICD's name, version and flags from this query (https://learn.microsoft.com/windows-hardware/drivers/display/
 *      loading-an-opengl-installable-client-driver). Asked for every adapter of D3DKMTEnumAdapters2, for the
 *      adapter of the primary display (D3DKMTOpenAdapterFromGdiDisplayName) and for the adapter of a window's DC
 *      (D3DKMTOpenAdapterFromHdc), with each adapter's LUID, type and PCI identity, so the adapter that answers can
 *      be named. Each adapter also gets KMTQAITYPE_CHECKDRIVERUPDATESTATUS (= 11), which opengl32 asks before it
 *      accepts an ICD;
 *   2. what the registry holds, as the kernel reads it now: KMTQAITYPE_QUERYREGISTRY with
 *      D3DDDI_QUERYREGISTRY_ADAPTERKEY (d3dukmdt.h, D3DDDI_QUERYREGISTRY_INFO) for the six OpenGL values. When this
 *      shows the registration and UMOPENGLINFO does not, the kernel answers UMOPENGLINFO from something other than
 *      the key as it is now. The D3D user-mode driver names (KMTQAITYPE_UMDRIVERNAME) are printed beside it as the
 *      control: that registration is known to work on the lab;
 *   3. what opengl32 does: every DLL the loader maps or unmaps while the pixel format is chosen and set and while a
 *      context is created (LdrRegisterDllNotification, documented in the Windows SDK under "LdrDllNotification"),
 *      the pixel formats split into ICD, generic and generic-accelerated ones, the GL strings of the context, and
 *      every loaded module that exports DrvSetContext afterwards. An ICD that opengl32 loads and drops again shows
 *      up here as a load and an unload, which no module list taken afterwards can show;
 *   4. whether the named file can be loaded at all by this process: CreateFile for read, LoadLibrary, the nineteen
 *      Drv* exports the release installer requires (tools\release\installer\common.ps1, $script:OpenGlDrvExports)
 *      and DrvValidateVersion, called with the version the kernel returned (or with --validate-version for a
 *      --load-file, the file the lab kit pushes). This step runs last, after the module list of step 3 is printed,
 *      because it loads the DLL itself. --no-load-icd leaves it out.
 *
 * The window is created the way gltest creates its own (CS_OWNDC, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT) and is never
 * shown unless --show is given: the development PC must not get a window (the owner's rule). The whole run is
 * bounded by a watchdog thread, 30 s by default. Every line is flushed as it is written.
 *
 * Exit code 0 when the probe ran to the end (whatever it found: a probe reports, the lab kit decides), 1 when the
 * watchdog fired or a step could not run at all, 2 on a usage error.
 *
 *   glprobe.exe
 *   glprobe.exe --expect-icd amdgpu_wddm_gl.dll --log probe.txt
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <d3dkmthk.h>
#include <GL/gl.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ logging */

static CRITICAL_SECTION g_log_lock;
static FILE *g_log_file;
static int g_log_ready;

static void say(const char *fmt, ...)
{
    char line[4096];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = (int)sizeof(line) - 1;
    }
    line[n] = '\0';

    if (g_log_ready) {
        EnterCriticalSection(&g_log_lock);
    }
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (g_log_file) {
        fputs(line, g_log_file);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }
    if (g_log_ready) {
        LeaveCriticalSection(&g_log_lock);
    }
}

/* UTF-16 to UTF-8 for printing. Paths of the lab are ASCII, but a driver description need not be. */
static void narrow(const WCHAR *w, int wlen, char *out, size_t n)
{
    int got;

    out[0] = '\0';
    if (!w || !n) {
        return;
    }
    /* With wlen -1 the terminator is converted too; a name too long for out gives 0 and an empty text. */
    got = WideCharToMultiByte(CP_UTF8, 0, w, wlen, out, (int)n - 1, NULL, NULL);
    if (got < 0) {
        got = 0;
    }
    out[got] = '\0';
}

/* ------------------------------------------------------------------ watchdog */

static HANDLE g_done_event;
static unsigned g_seconds = 30;

static DWORD WINAPI watchdog_thread(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(g_done_event, g_seconds * 1000) == WAIT_TIMEOUT) {
        say("glprobe: FAIL watchdog bound=%us (the probe did not finish in time)", g_seconds);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
}

/* ------------------------------------------------------------------ options */

static int g_show;
static int g_load_icd = 1;
static char g_expect[MAX_PATH];
/* --load-file PATH: the direct load takes this file instead of the one the kernel named. The development PC has no
 * registration of ours, so this is how the direct-load step is checked there against the real ICD file. */
static char g_load_file[MAX_PATH];
/* The version DrvValidateVersion gets in the direct load of a --load-file (--validate-version, default 1, which is
 * what the release registers). A direct load of the file the kernel named gets the version the kernel returned. */
static ULONG g_validate_version = 1;

static int has_text(const char *s, const char *text)
{
    size_t n = strlen(text);
    size_t i;

    if (!n || !s) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        if (_strnicmp(s + i, text, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ DLL load notifications */

/* LdrRegisterDllNotification and its data, as the Windows SDK documentation of LdrDllNotification declares them. The
 * function lives in ntdll and is resolved at run time, so the build needs no ntdll import library. */
typedef struct {
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} probe_ldr_notification_data;

#define PROBE_LDR_LOADED   1
#define PROBE_LDR_UNLOADED 2

typedef VOID (CALLBACK *probe_ldr_callback)(ULONG, const probe_ldr_notification_data *, PVOID);
typedef NTSTATUS (NTAPI *probe_ldr_register)(ULONG, probe_ldr_callback, PVOID, PVOID *);

#define DLL_EVENTS 512

struct dll_event {
    int phase;
    int reason;
    WCHAR path[MAX_PATH];
};

static struct dll_event g_dll_events[DLL_EVENTS];
static volatile LONG g_dll_count;
static volatile LONG g_phase;
static const char *k_phase_names[] = { "start", "adapters", "pixelformat", "context", "after", "direct" };
enum { PHASE_START, PHASE_ADAPTERS, PHASE_PIXELFORMAT, PHASE_CONTEXT, PHASE_AFTER, PHASE_DIRECT };

/* Runs under the loader lock: it copies the name and returns, nothing else. */
static VOID CALLBACK dll_callback(ULONG reason, const probe_ldr_notification_data *data, PVOID context)
{
    LONG i;
    size_t n = 0;

    (void)context;
    i = InterlockedIncrement(&g_dll_count) - 1;
    if (i < 0 || i >= DLL_EVENTS) {
        return;
    }
    g_dll_events[i].phase = (int)g_phase;
    g_dll_events[i].reason = (int)reason;
    g_dll_events[i].path[0] = 0;
    if (data && data->FullDllName && data->FullDllName->Buffer) {
        n = data->FullDllName->Length / sizeof(WCHAR);
        if (n > MAX_PATH - 1) {
            n = MAX_PATH - 1;
        }
        memcpy(g_dll_events[i].path, data->FullDllName->Buffer, n * sizeof(WCHAR));
    }
    g_dll_events[i].path[n] = 0;
}

static void set_phase(int phase)
{
    InterlockedExchange(&g_phase, phase);
}

/* ------------------------------------------------------------------ debug strings */

/* Mesa logs to the debugger on Windows (src\util\log.c: MESA_LOG_CONTROL_WINDBG, logger_windbg calls
 * OutputDebugStringA), so an ICD that fails to start says why there and nowhere else. OutputDebugString raises
 * DBG_PRINTEXCEPTION_C (and DBG_PRINTEXCEPTION_WIDE_C first for the W form) and handles it itself when no debugger
 * takes it, so a vectored handler of this process sees every line first. The handler copies the text and lets the
 * search go on, so nothing changes for the caller. */
#define PROBE_DBG_PRINT      0x40010006L
#define PROBE_DBG_PRINT_WIDE 0x4001000AL
#define DEBUG_LINES 256

struct debug_line {
    int phase;
    char text[512];
};

static struct debug_line g_debug_lines[DEBUG_LINES];
static volatile LONG g_debug_count;

static LONG CALLBACK debug_handler(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e ? e->ExceptionRecord : NULL;
    LONG i;
    size_t n;
    const char *s;

    if (!r || r->ExceptionCode != PROBE_DBG_PRINT || r->NumberParameters < 2) {
        /* The wide form carries the ANSI copy as well and is followed by the ANSI exception when nobody handles it,
         * so only the ANSI form is copied: one line per message, not two. */
        return EXCEPTION_CONTINUE_SEARCH;
    }
    s = (const char *)r->ExceptionInformation[1];
    if (!s) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    i = InterlockedIncrement(&g_debug_count) - 1;
    if (i < 0 || i >= DEBUG_LINES) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_debug_lines[i].phase = (int)g_phase;
    n = (size_t)r->ExceptionInformation[0];
    if (n > sizeof(g_debug_lines[i].text) - 1) {
        n = sizeof(g_debug_lines[i].text) - 1;
    }
    memcpy(g_debug_lines[i].text, s, n);
    g_debug_lines[i].text[n] = '\0';
    return EXCEPTION_CONTINUE_SEARCH;
}

static void print_debug_lines(int phase)
{
    LONG count = g_debug_count;
    LONG i;
    int shown = 0;
    char clean[512];

    if (count > DEBUG_LINES) {
        count = DEBUG_LINES;
    }
    for (i = 0; i < count; i++) {
        size_t k;
        if (g_debug_lines[i].phase != phase) {
            continue;
        }
        for (k = 0; g_debug_lines[i].text[k] && k < sizeof(clean) - 1; k++) {
            char c = g_debug_lines[i].text[k];
            clean[k] = (c == '\r' || c == '\n' || c == '"') ? ' ' : c;
        }
        clean[k] = '\0';
        say("glprobe: debug-string phase=%s text=\"%s\"", k_phase_names[phase], clean);
        shown++;
    }
    say("glprobe: debug-strings phase=%s count=%d", k_phase_names[phase], shown);
}

/* Prints the loads and unloads of one phase, and the debug strings written in it. Counts the loads and unloads that
 * name the expected ICD. */
static int print_dll_events(int phase, int *loaded_expect, int *unloaded_expect)
{
    LONG count = g_dll_count;
    LONG i;
    int shown = 0;
    char path[MAX_PATH * 3];

    print_debug_lines(phase);
    if (count > DLL_EVENTS) {
        count = DLL_EVENTS;
    }
    for (i = 0; i < count; i++) {
        if (g_dll_events[i].phase != phase) {
            continue;
        }
        narrow(g_dll_events[i].path, -1, path, sizeof(path));
        say("glprobe: dll-%s phase=%s seq=%ld path=\"%s\"",
            g_dll_events[i].reason == PROBE_LDR_LOADED ? "load" :
            g_dll_events[i].reason == PROBE_LDR_UNLOADED ? "unload" : "other",
            k_phase_names[phase], (long)i, path);
        shown++;
        if (g_expect[0] && has_text(path, g_expect)) {
            if (g_dll_events[i].reason == PROBE_LDR_LOADED && loaded_expect) {
                (*loaded_expect)++;
            }
            if (g_dll_events[i].reason == PROBE_LDR_UNLOADED && unloaded_expect) {
                (*unloaded_expect)++;
            }
        }
    }
    say("glprobe: dll-events phase=%s count=%d", k_phase_names[phase], shown);
    return shown;
}

/* ------------------------------------------------------------------ adapter queries */

struct opengl_answer {
    NTSTATUS status;
    char name[MAX_PATH * 3];
    ULONG version;
    ULONG flags;
};

static NTSTATUS query(D3DKMT_HANDLE h, KMTQUERYADAPTERINFOTYPE type, void *data, UINT size)
{
    D3DKMT_QUERYADAPTERINFO q;

    memset(&q, 0, sizeof(q));
    q.hAdapter = h;
    q.Type = type;
    q.pPrivateDriverData = data;
    q.PrivateDriverDataSize = size;
    return D3DKMTQueryAdapterInfo(&q);
}

static void query_opengl(D3DKMT_HANDLE h, struct opengl_answer *a)
{
    D3DKMT_OPENGLINFO info;

    memset(&info, 0, sizeof(info));
    memset(a, 0, sizeof(*a));
    a->status = query(h, KMTQAITYPE_UMOPENGLINFO, &info, sizeof(info));
    info.UmdOpenGlIcdFileName[MAX_PATH - 1] = 0;
    narrow(info.UmdOpenGlIcdFileName, -1, a->name, sizeof(a->name));
    a->version = info.Version;
    a->flags = info.Flags;
}

/* One value of the adapter's software key, as the kernel reads it now. A REG_MULTI_SZ is printed with '|' between
 * its strings. */
static void query_registry(D3DKMT_HANDLE h, const char *tag, const WCHAR *value, ULONG type)
{
    static BYTE buf[sizeof(D3DDDI_QUERYREGISTRY_INFO) + 4096];
    D3DDDI_QUERYREGISTRY_INFO *q = (D3DDDI_QUERYREGISTRY_INFO *)buf;
    NTSTATUS st;
    char name[MAX_PATH];
    char text[4096];

    memset(buf, 0, sizeof(buf));
    q->QueryType = D3DDDI_QUERYREGISTRY_ADAPTERKEY;
    q->QueryFlags.Value = 0;
    wcsncpy(q->ValueName, value, MAX_PATH - 1);
    q->ValueType = type;
    q->PhysicalAdapterIndex = 0;
    st = query(h, KMTQAITYPE_QUERYREGISTRY, q, (UINT)sizeof(buf));
    narrow(value, -1, name, sizeof(name));
    text[0] = '\0';
    if (st == 0 && q->Status == D3DDDI_QUERYREGISTRY_STATUS_SUCCESS) {
        if (type == REG_DWORD) {
            _snprintf(text, sizeof(text) - 1, "%lu", (unsigned long)q->OutputDword);
            text[sizeof(text) - 1] = '\0';
        } else {
            /* REG_SZ or REG_MULTI_SZ: the strings, separated by '|', ending at the double terminator */
            size_t chars = q->OutputValueSize / sizeof(WCHAR);
            size_t max = (sizeof(buf) - offsetof(D3DDDI_QUERYREGISTRY_INFO, OutputString)) / sizeof(WCHAR);
            size_t i;
            WCHAR joined[2048];
            size_t j = 0;
            if (chars > max) {
                chars = max;
            }
            for (i = 0; i < chars && j < 2047; i++) {
                WCHAR c = q->OutputString[i];
                if (c == 0) {
                    if (i + 1 < chars && q->OutputString[i + 1] != 0) {
                        joined[j++] = L'|';
                    }
                    continue;
                }
                joined[j++] = c;
            }
            joined[j] = 0;
            narrow(joined, -1, text, sizeof(text));
        }
    }
    say("glprobe: %s regkey %s ntstatus=0x%08lX qstatus=%d size=%lu value=\"%s\"", tag, name, (unsigned long)st,
        st == 0 ? (int)q->Status : -1, st == 0 ? (unsigned long)q->OutputValueSize : 0ul, text);
}

/* Prints everything this probe asks of one adapter. Returns the UMOPENGLINFO answer in *gl. */
static void describe_adapter(D3DKMT_HANDLE h, const char *tag, LUID luid, struct opengl_answer *gl)
{
    D3DKMT_ADAPTERTYPE type;
    D3DKMT_QUERY_DEVICE_IDS ids;
    D3DKMT_ADAPTERREGISTRYINFO reg;
    D3DKMT_UMDFILENAMEINFO umd;
    static D3DKMT_DRIVER_DESCRIPTION desc;
    NTSTATUS st;
    char a[MAX_PATH * 3];
    char b[MAX_PATH * 3];
    int v;

    say("glprobe: %s luid=%08lX:%08lX", tag, (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);

    memset(&type, 0, sizeof(type));
    st = query(h, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type));
    say("glprobe: %s type ntstatus=0x%08lX value=0x%08X render=%u display=%u software=%u indirect=%u compute-only=%u",
        tag, (unsigned long)st, type.Value, type.RenderSupported, type.DisplaySupported, type.SoftwareDevice,
        type.IndirectDisplayDevice, type.ComputeOnly);

    memset(&ids, 0, sizeof(ids));
    st = query(h, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids));
    say("glprobe: %s pci ntstatus=0x%08lX vendor=0x%04X device=0x%04X subsys=0x%04X:0x%04X rev=0x%02X", tag,
        (unsigned long)st, ids.DeviceIds.VendorID, ids.DeviceIds.DeviceID, ids.DeviceIds.SubVendorID,
        ids.DeviceIds.SubSystemID, ids.DeviceIds.RevisionID);

    memset(&desc, 0, sizeof(desc));
    st = query(h, KMTQAITYPE_DRIVER_DESCRIPTION, &desc, sizeof(desc));
    desc.DriverDescription[4095] = 0;
    narrow(desc.DriverDescription, -1, a, sizeof(a));
    memset(&reg, 0, sizeof(reg));
    {
        NTSTATUS st2 = query(h, KMTQAITYPE_ADAPTERREGISTRYINFO, &reg, sizeof(reg));
        reg.AdapterString[MAX_PATH - 1] = 0;
        narrow(reg.AdapterString, -1, b, sizeof(b));
        say("glprobe: %s description ntstatus=0x%08lX text=\"%s\" registry-ntstatus=0x%08lX adapter-string=\"%s\"",
            tag, (unsigned long)st, a, (unsigned long)st2, b);
    }

    /* The control: the D3D user-mode driver names of the same adapter, through the same kind of query. */
    for (v = KMTUMDVERSION_DX11; v <= KMTUMDVERSION_DX12; v++) {
        memset(&umd, 0, sizeof(umd));
        umd.Version = (KMTUMDVERSION)v;
        st = query(h, KMTQAITYPE_UMDRIVERNAME, &umd, sizeof(umd));
        umd.UmdFileName[MAX_PATH - 1] = 0;
        narrow(umd.UmdFileName, -1, a, sizeof(a));
        say("glprobe: %s umdname dx%d ntstatus=0x%08lX name=\"%s\"", tag, v == KMTUMDVERSION_DX11 ? 11 : 12,
            (unsigned long)st, a);
    }

    query_opengl(h, gl);
    say("glprobe: %s umopenglinfo ntstatus=0x%08lX name=\"%s\" version=%lu flags=0x%lX", tag,
        (unsigned long)gl->status, gl->name, (unsigned long)gl->version, (unsigned long)gl->flags);

    /* opengl32 asks this of the ICD's adapter right after LoadLibrary and drops the ICD when the answer is not zero
       (CheckDriverUpdateStatus in opengl32 10.0.26100.9278): a driver updated under a running adapter. */
    {
        BOOL updated = FALSE;
        st = query(h, KMTQAITYPE_CHECKDRIVERUPDATESTATUS, &updated, sizeof(updated));
        say("glprobe: %s driver-update-status ntstatus=0x%08lX value=%d", tag, (unsigned long)st, (int)updated);
    }

    query_registry(h, tag, L"OpenGLDriverName", REG_MULTI_SZ);
    query_registry(h, tag, L"OpenGLVersion", REG_DWORD);
    query_registry(h, tag, L"OpenGLFlags", REG_DWORD);
    query_registry(h, tag, L"OpenGLDriverNameWow", REG_MULTI_SZ);
    query_registry(h, tag, L"OpenGLVersionWow", REG_DWORD);
    query_registry(h, tag, L"OpenGLFlagsWow", REG_DWORD);
}

static void close_adapter(D3DKMT_HANDLE h)
{
    D3DKMT_CLOSEADAPTER c;

    if (!h) {
        return;
    }
    c.hAdapter = h;
    (void)D3DKMTCloseAdapter(&c);
}

/* One way of naming the adapter a DC belongs to, and what the kernel answers for that adapter. */
struct source {
    int have;
    LUID luid;
    struct opengl_answer gl;
};

static void open_from_dc(HDC dc, const char *tag, struct source *s)
{
    D3DKMT_OPENADAPTERFROMHDC oh;
    NTSTATUS nst;

    memset(s, 0, sizeof(*s));
    if (!dc) {
        say("glprobe: %s open skipped, no DC", tag);
        return;
    }
    memset(&oh, 0, sizeof(oh));
    oh.hDc = dc;
    nst = D3DKMTOpenAdapterFromHdc(&oh);
    say("glprobe: %s open ntstatus=0x%08lX vidpn-source=%u", tag, (unsigned long)nst, oh.VidPnSourceId);
    if (nst == 0) {
        s->have = 1;
        s->luid = oh.AdapterLuid;
        describe_adapter(oh.hAdapter, tag, oh.AdapterLuid, &s->gl);
        close_adapter(oh.hAdapter);
    }
}

static int same_luid(LUID a, LUID b)
{
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

/* ------------------------------------------------------------------ the window, as gltest makes it */

static const char *g_class_name = "glprobe_window_class";

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE) {
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ------------------------------------------------------------------ module of an address */

static void module_of(const void *addr, char *out, size_t n)
{
    HMODULE mod = NULL;

    out[0] = '\0';
    if (!addr) {
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        if (!GetModuleFileNameA(mod, out, (DWORD)n)) {
            out[0] = '\0';
        }
    }
}

/* Every loaded module that exports DrvSetContext. Returns how many there are, and in *matched how many of them carry
 * the expected text in their path. */
static int list_drv_modules(const char *phase, int *matched)
{
    HMODULE mods[1024];
    DWORD needed = 0;
    size_t i;
    size_t count;
    char path[MAX_PATH];
    int found = 0;

    *matched = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, (DWORD)sizeof(mods), &needed)) {
        say("glprobe: modules phase=%s EnumProcessModules failed error=%lu", phase, GetLastError());
        return -1;
    }
    if (needed > (DWORD)sizeof(mods)) {
        needed = (DWORD)sizeof(mods);
    }
    count = (size_t)needed / sizeof(HMODULE);
    for (i = 0; i < count; i++) {
        if (GetProcAddress(mods[i], "DrvSetContext")) {
            path[0] = '\0';
            GetModuleFileNameA(mods[i], path, (DWORD)sizeof(path));
            say("glprobe: drv-module phase=%s path=\"%s\" DrvValidateVersion=%s", phase, path,
                GetProcAddress(mods[i], "DrvValidateVersion") ? "yes" : "no");
            found++;
            if (g_expect[0] && has_text(path, g_expect)) {
                (*matched)++;
            }
        }
    }
    say("glprobe: drv-modules phase=%s loaded-modules=%u exporting-DrvSetContext=%d matching-expect=%d", phase,
        (unsigned)count, found, *matched);
    return found;
}

/* ------------------------------------------------------------------ the direct load */

/* The nineteen entry points the release installer requires of an ICD (common.ps1, $script:OpenGlDrvExports). */
static const char *k_drv_exports[] = {
    "DrvCopyContext", "DrvCreateContext", "DrvCreateLayerContext", "DrvDeleteContext", "DrvDescribeLayerPlane",
    "DrvDescribePixelFormat", "DrvGetLayerPaletteEntries", "DrvGetProcAddress", "DrvPresentBuffers",
    "DrvRealizeLayerPalette", "DrvReleaseContext", "DrvSetCallbackProcs", "DrvSetContext",
    "DrvSetLayerPaletteEntries", "DrvSetPixelFormat", "DrvShareLists", "DrvSwapBuffers", "DrvSwapLayerBuffers",
    "DrvValidateVersion"
};

typedef BOOL (APIENTRY *probe_validate_version)(ULONG);

static void direct_load(const struct opengl_answer *gl)
{
    WCHAR wname[MAX_PATH];
    HANDLE f;
    HMODULE mod;
    DWORD attrs;
    DWORD err;
    size_t i;
    int missing = 0;
    char missing_list[1024];
    char path[MAX_PATH];

    const char *name = g_load_file[0] ? g_load_file : gl->name;

    if (!name[0]) {
        say("glprobe: direct-load skipped, the kernel named no ICD for this process");
        return;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MAX_PATH)) {
        say("glprobe: direct-load skipped, the name does not convert");
        return;
    }
    attrs = GetFileAttributesW(wname);
    err = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    f = CreateFileW(wname, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    say("glprobe: direct-load file name=\"%s\" source=%s exists=%s attrs-error=%lu read=%s read-error=%lu", name,
        g_load_file[0] ? "--load-file" : "kernel",
        attrs == INVALID_FILE_ATTRIBUTES ? "no" : "yes", err, f == INVALID_HANDLE_VALUE ? "no" : "yes",
        f == INVALID_HANDLE_VALUE ? GetLastError() : 0ul);
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
    }

    set_phase(PHASE_DIRECT);
    SetLastError(0);
    mod = LoadLibraryW(wname);
    err = mod ? 0 : GetLastError();
    if (!mod) {
        say("glprobe: direct-load LoadLibrary=fail error=%lu", err);
        print_dll_events(PHASE_DIRECT, NULL, NULL);
        return;
    }
    path[0] = '\0';
    GetModuleFileNameA(mod, path, (DWORD)sizeof(path));
    missing_list[0] = '\0';
    for (i = 0; i < sizeof(k_drv_exports) / sizeof(k_drv_exports[0]); i++) {
        if (!GetProcAddress(mod, k_drv_exports[i])) {
            missing++;
            strncat(missing_list, k_drv_exports[i], sizeof(missing_list) - strlen(missing_list) - 2);
            strncat(missing_list, " ", sizeof(missing_list) - strlen(missing_list) - 1);
        }
    }
    say("glprobe: direct-load LoadLibrary=ok module=\"%s\" exports-missing=%d %s", path, missing, missing_list);
    {
        probe_validate_version vv = (probe_validate_version)(void *)GetProcAddress(mod, "DrvValidateVersion");
        ULONG ver = g_load_file[0] ? g_validate_version : gl->version;
        if (vv) {
            BOOL ok = vv(ver);
            say("glprobe: direct-load DrvValidateVersion(%lu)=%s version-source=%s", (unsigned long)ver,
                ok ? "TRUE" : "FALSE", g_load_file[0] ? "--validate-version" : "kernel");
        } else {
            say("glprobe: direct-load DrvValidateVersion missing");
        }
    }
    print_dll_events(PHASE_DIRECT, NULL, NULL);
    /* The module stays loaded: unloading an ICD that opengl32 may also hold is not this probe's business. */
}

/* ------------------------------------------------------------------ main */

static void usage(FILE *out)
{
    fputs("glprobe: which OpenGL ICD the system offers this process, and what opengl32 does with it\n"
          "  --expect-icd TEXT   name the ICD this run looks for (verdict lines say whether each answer carries it)\n"
          "  --show              show the probe's window (hidden by default)\n"
          "  --seconds N         watchdog bound, 1..600 (default 30)\n"
          "  --log PATH          also write every line to PATH\n"
          "  --no-load-icd       leave out the direct load of the named ICD\n"
          "  --load-file PATH    the direct load takes PATH instead of the file the kernel named\n"
          "  --validate-version N  DrvValidateVersion of a --load-file gets N (default 1); the kernel's file gets\n"
          "                      the version the kernel returned\n"
          "Exit code 0 when the probe ran to the end, 1 when the watchdog fired or no step could run, 2 on a usage\n"
          "error.\n", out);
}

int main(int argc, char **argv)
{
    const char *log_path = NULL;
    HANDLE wd;
    int i;
    SYSTEMTIME st;
    ULONGLONG t0 = GetTickCount64();
    BOOL wow = FALSE;
    DWORD session = 0;
    char path[MAX_PATH];
    D3DKMT_ENUMADAPTERS2 ea;
    D3DKMT_ADAPTERINFO *infos = NULL;
    struct source src_hdc;
    struct source src_monitor;
    struct source src_primary;
    const struct source *win = NULL;
    const char *win_how = "none";
    struct opengl_answer gl_any;
    LUID expect_luid_value;
    char expect_luid[32] = "none";
    int expect_adapters = 0;
    int loaded_expect = 0;
    int unloaded_expect = 0;
    int drv_matched = 0;
    int drv_found = 0;
    HWND wnd = NULL;
    HDC dc = NULL;
    HGLRC rc = NULL;
    int chosen = 0;
    int pf_count = 0;
    int pf_icd = 0;
    int pf_generic = 0;
    int pf_mcd = 0;
    const char *ctx_kind = "none";
    const char *chosen_kind = "none";
    PVOID veh = NULL;
    char renderer[256] = "";
    char vendor[256] = "";
    char version[256] = "";
    int arb = 0;
    HMODULE ntdll;
    probe_ldr_register reg_fn = NULL;
    PVOID cookie = NULL;
    NTSTATUS nst;

    memset(&src_hdc, 0, sizeof(src_hdc));
    memset(&src_monitor, 0, sizeof(src_monitor));
    memset(&src_primary, 0, sizeof(src_primary));
    memset(&expect_luid_value, 0, sizeof(expect_luid_value));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--show") == 0) {
            g_show = 1;
        } else if (strcmp(argv[i], "--no-load-icd") == 0) {
            g_load_icd = 0;
        } else if (strcmp(argv[i], "--load-file") == 0) {
            if (++i >= argc) {
                fputs("glprobe: --load-file needs a path\n", stderr);
                return 2;
            }
            if (!argv[i][0] || strlen(argv[i]) >= sizeof(g_load_file)) {
                fprintf(stderr, "glprobe: --load-file value out of range: %s\n", argv[i]);
                return 2;
            }
            strcpy(g_load_file, argv[i]);
        } else if (strcmp(argv[i], "--validate-version") == 0) {
            unsigned long v;
            char *end = NULL;
            if (++i >= argc) {
                fputs("glprobe: --validate-version needs a number\n", stderr);
                return 2;
            }
            v = strtoul(argv[i], &end, 0);
            if (!end || *end || v > 0xFFFFFFFFul) {
                fprintf(stderr, "glprobe: --validate-version out of range: %s\n", argv[i]);
                return 2;
            }
            g_validate_version = (ULONG)v;
        } else if (strcmp(argv[i], "--seconds") == 0) {
            long v;
            if (++i >= argc) {
                fputs("glprobe: --seconds needs a value\n", stderr);
                return 2;
            }
            v = strtol(argv[i], NULL, 10);
            if (v < 1 || v > 600) {
                fprintf(stderr, "glprobe: --seconds out of range: %s\n", argv[i]);
                return 2;
            }
            g_seconds = (unsigned)v;
        } else if (strcmp(argv[i], "--log") == 0) {
            if (++i >= argc) {
                fputs("glprobe: --log needs a path\n", stderr);
                return 2;
            }
            log_path = argv[i];
        } else if (strcmp(argv[i], "--expect-icd") == 0) {
            if (++i >= argc) {
                fputs("glprobe: --expect-icd needs a text\n", stderr);
                return 2;
            }
            if (!argv[i][0] || strlen(argv[i]) >= sizeof(g_expect)) {
                fprintf(stderr, "glprobe: --expect-icd value out of range: %s\n", argv[i]);
                return 2;
            }
            strcpy(g_expect, argv[i]);
        } else {
            fprintf(stderr, "glprobe: unknown option \"%s\"\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    InitializeCriticalSection(&g_log_lock);
    g_log_ready = 1;
    if (log_path) {
        g_log_file = fopen(log_path, "w");
        if (!g_log_file) {
            fprintf(stderr, "glprobe: cannot write %s\n", log_path);
            return 2;
        }
    }
    g_done_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    wd = g_done_event ? CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL) : NULL;
    if (!wd) {
        fputs("glprobe: the watchdog thread did not start\n", stderr);
        return 1;
    }

    /* ---------------------------------------------------------------- the process */
    set_phase(PHASE_START);
    ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        reg_fn = (probe_ldr_register)(void *)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    }
    nst = reg_fn ? reg_fn(0, dll_callback, NULL, &cookie) : (NTSTATUS)0xC0000002L;
    veh = AddVectoredExceptionHandler(1, debug_handler);
    IsWow64Process(GetCurrentProcess(), &wow);
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    GetSystemTime(&st);
    {
        typedef LONG (WINAPI *rtl_get_version)(RTL_OSVERSIONINFOW *);
        RTL_OSVERSIONINFOW ver;
        rtl_get_version rgv = ntdll ? (rtl_get_version)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
        memset(&ver, 0, sizeof(ver));
        ver.dwOSVersionInfoSize = sizeof(ver);
        if (rgv) {
            rgv(&ver);
        }
        say("glprobe: start arch=%s wow64=%d pid=%lu session=%lu build=%lu utc=%04u-%02u-%02uT%02u:%02u:%02uZ "
            "bound=%us show=%d load-icd=%d expect=\"%s\"",
            sizeof(void *) == 8 ? "x64" : "x86", wow ? 1 : 0, GetCurrentProcessId(), session,
            (unsigned long)ver.dwBuildNumber, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            g_seconds, g_show, g_load_icd, g_expect);
    }
    say("glprobe: dll-watch ntstatus=0x%08lX %s debug-string-watch %s", (unsigned long)nst, nst == 0 ? "on" : "OFF",
        veh ? "on" : "OFF");
    path[0] = '\0';
    GetModuleFileNameA(GetModuleHandleA("opengl32.dll"), path, (DWORD)sizeof(path));
    say("glprobe: opengl32 path=\"%s\"", path);
    /* The positive control of the debug-string watch: this line must come back as a debug-string of the start
     * phase, or a missing ICD message later means nothing. */
    OutputDebugStringA("glprobe debug-string control");
    print_debug_lines(PHASE_START);

    /* ---------------------------------------------------------------- the adapters */
    set_phase(PHASE_ADAPTERS);
    memset(&ea, 0, sizeof(ea));
    nst = D3DKMTEnumAdapters2(&ea);
    say("glprobe: enum ntstatus=0x%08lX count=%lu", (unsigned long)nst, (unsigned long)ea.NumAdapters);
    if (nst == 0 && ea.NumAdapters > 0 && ea.NumAdapters < 64) {
        infos = (D3DKMT_ADAPTERINFO *)calloc(ea.NumAdapters, sizeof(*infos));
        if (infos) {
            ULONG k;
            ea.pAdapters = infos;
            nst = D3DKMTEnumAdapters2(&ea);
            say("glprobe: enum filled ntstatus=0x%08lX count=%lu", (unsigned long)nst, (unsigned long)ea.NumAdapters);
            if (nst != 0) {
                ea.NumAdapters = 0;
            }
            for (k = 0; k < ea.NumAdapters; k++) {
                char tag[32];
                _snprintf(tag, sizeof(tag) - 1, "enum[%lu]", (unsigned long)k);
                tag[sizeof(tag) - 1] = '\0';
                say("glprobe: %s sources=%lu", tag, (unsigned long)infos[k].NumOfSources);
                describe_adapter(infos[k].hAdapter, tag, infos[k].AdapterLuid, &gl_any);
                if (g_expect[0] && has_text(gl_any.name, g_expect)) {
                    expect_adapters++;
                    expect_luid_value = infos[k].AdapterLuid;
                    _snprintf(expect_luid, sizeof(expect_luid) - 1, "%08lX:%08lX",
                              (unsigned long)infos[k].AdapterLuid.HighPart,
                              (unsigned long)infos[k].AdapterLuid.LowPart);
                    expect_luid[sizeof(expect_luid) - 1] = '\0';
                }
                close_adapter(infos[k].hAdapter);
            }
        }
    }

    /* The primary display, by its GDI name. */
    {
        DISPLAY_DEVICEW dd;
        DWORD n;
        WCHAR primary[32] = L"";
        for (n = 0; n < 32; n++) {
            char dev[64];
            char str[MAX_PATH * 3];
            memset(&dd, 0, sizeof(dd));
            dd.cb = sizeof(dd);
            if (!EnumDisplayDevicesW(NULL, n, &dd, 0)) {
                break;
            }
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
                continue;
            }
            narrow(dd.DeviceName, -1, dev, sizeof(dev));
            narrow(dd.DeviceString, -1, str, sizeof(str));
            say("glprobe: display name=%s adapter=\"%s\" primary=%d", dev, str,
                (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? 1 : 0);
            if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
                wcsncpy(primary, dd.DeviceName, 31);
                primary[31] = 0;
            }
        }
        if (primary[0]) {
            D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME og;
            memset(&og, 0, sizeof(og));
            wcsncpy(og.DeviceName, primary, 31);
            nst = D3DKMTOpenAdapterFromGdiDisplayName(&og);
            say("glprobe: primary open ntstatus=0x%08lX vidpn-source=%u", (unsigned long)nst, og.VidPnSourceId);
            if (nst == 0) {
                src_primary.have = 1;
                src_primary.luid = og.AdapterLuid;
                describe_adapter(og.hAdapter, "primary", og.AdapterLuid, &src_primary.gl);
                close_adapter(og.hAdapter);
            }
        } else {
            say("glprobe: primary none (no display device is attached to the desktop as primary)");
        }
    }

    /* The window and its DC, as gltest makes them. */
    {
        WNDCLASSEXA wc;
        RECT r = { 0, 0, 64, 64 };
        DWORD style = WS_OVERLAPPEDWINDOW;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = g_class_name;
        if (!RegisterClassExA(&wc)) {
            say("glprobe: window RegisterClassExA failed error=%lu", GetLastError());
        } else {
            AdjustWindowRect(&r, style, FALSE);
            wnd = CreateWindowExA(0, g_class_name, "glprobe", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  r.right - r.left, r.bottom - r.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
        }
        if (!wnd) {
            say("glprobe: window CreateWindowExA failed error=%lu", GetLastError());
        } else {
            HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi;
            char dev[64] = "";
            memset(&mi, 0, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
                narrow(mi.szDevice, -1, dev, sizeof(dev));
            }
            if (g_show) {
                ShowWindow(wnd, SW_SHOW);
            }
            pump();
            dc = GetDC(wnd);
            say("glprobe: window created shown=%d monitor=%s dc=%s", g_show, dev, dc ? "yes" : "no");
            /* The window's own DC, which is the DC an application hands to opengl32. */
            open_from_dc(dc, "hdc", &src_hdc);
            /* The DC of the display the window is on. On a desktop of several displays a window DC maps to no
             * single display and D3DKMTOpenAdapterFromHdc refuses it (STATUS_INVALID_PARAMETER on the development
             * PC, which has two), so the window's adapter is also named through its display. */
            if (mi.szDevice[0]) {
                HDC mdc = CreateDCW(mi.szDevice, mi.szDevice, NULL, NULL);
                open_from_dc(mdc, "monitor-dc", &src_monitor);
                if (mdc) {
                    DeleteDC(mdc);
                }
            }
        }
    }
    /* The window's adapter: from its own DC when the kernel names one, else from its display, else the primary. */
    if (src_hdc.have) {
        win = &src_hdc;
        win_how = "hdc";
    } else if (src_monitor.have) {
        win = &src_monitor;
        win_how = "monitor-dc";
    } else if (src_primary.have) {
        win = &src_primary;
        win_how = "primary";
    }
    print_dll_events(PHASE_ADAPTERS, &loaded_expect, &unloaded_expect);

    /* ---------------------------------------------------------------- pixel formats */
    if (dc) {
        PIXELFORMATDESCRIPTOR pfd;
        PIXELFORMATDESCRIPTOR want;
        int n;
        set_phase(PHASE_PIXELFORMAT);
        memset(&pfd, 0, sizeof(pfd));
        pf_count = DescribePixelFormat(dc, 1, sizeof(pfd), &pfd);
        for (n = 1; n <= pf_count && n <= 1024; n++) {
            memset(&pfd, 0, sizeof(pfd));
            if (!DescribePixelFormat(dc, n, sizeof(pfd), &pfd)) {
                continue;
            }
            if (!(pfd.dwFlags & PFD_SUPPORT_OPENGL)) {
                continue;
            }
            if (!(pfd.dwFlags & PFD_GENERIC_FORMAT)) {
                pf_icd++;
            } else if (pfd.dwFlags & PFD_GENERIC_ACCELERATED) {
                pf_mcd++;
            } else {
                pf_generic++;
            }
        }
        /* gltest's own request (gltest.c, target_create) */
        memset(&want, 0, sizeof(want));
        want.nSize = sizeof(want);
        want.nVersion = 1;
        want.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        want.iPixelType = PFD_TYPE_RGBA;
        want.cColorBits = 32;
        want.cAlphaBits = 8;
        want.cDepthBits = 24;
        want.cStencilBits = 8;
        want.iLayerType = PFD_MAIN_PLANE;
        chosen = ChoosePixelFormat(dc, &want);
        memset(&pfd, 0, sizeof(pfd));
        if (chosen) {
            DescribePixelFormat(dc, chosen, sizeof(pfd), &pfd);
        }
        chosen_kind = !chosen ? "none" : !(pfd.dwFlags & PFD_GENERIC_FORMAT) ? "icd" :
                      (pfd.dwFlags & PFD_GENERIC_ACCELERATED) ? "generic-accelerated" : "generic";
        {
            BOOL set_ok = chosen ? SetPixelFormat(dc, chosen, &pfd) : FALSE;
            DWORD set_err = set_ok ? 0 : GetLastError();
            say("glprobe: pfd count=%d opengl-icd=%d opengl-generic=%d opengl-generic-accelerated=%d chosen=%d "
                "chosen-flags=0x%08lX chosen-kind=%s set=%s set-error=%lu get=%d",
                pf_count, pf_icd, pf_generic, pf_mcd, chosen, (unsigned long)pfd.dwFlags, chosen_kind,
                set_ok ? "ok" : "fail", set_err, GetPixelFormat(dc));
        }
        print_dll_events(PHASE_PIXELFORMAT, &loaded_expect, &unloaded_expect);
    }

    /* ---------------------------------------------------------------- a context */
    if (dc) {
        DWORD err;
        set_phase(PHASE_CONTEXT);
        SetLastError(0);
        rc = wglCreateContext(dc);
        err = rc ? 0 : GetLastError();
        if (rc && wglMakeCurrent(dc, rc)) {
            const char *s;
            PROC p;
            char mod[MAX_PATH];
            s = (const char *)glGetString(GL_VENDOR);
            strncpy(vendor, s ? s : "(null)", sizeof(vendor) - 1);
            s = (const char *)glGetString(GL_RENDERER);
            strncpy(renderer, s ? s : "(null)", sizeof(renderer) - 1);
            s = (const char *)glGetString(GL_VERSION);
            strncpy(version, s ? s : "(null)", sizeof(version) - 1);
            ctx_kind = (strcmp(renderer, "GDI Generic") == 0) ? "generic" : "icd";
            say("glprobe: wgl create=ok current=ok vendor=\"%s\" renderer=\"%s\" version=\"%s\" kind=%s", vendor,
                renderer, version, ctx_kind);
            p = wglGetProcAddress("wglCreateContextAttribsARB");
            arb = p != NULL;
            module_of((const void *)p, mod, sizeof(mod));
            say("glprobe: wgl wglCreateContextAttribsARB=%s module=\"%s\"", arb ? "yes" : "no", mod);
            p = wglGetProcAddress("glCreateShader");
            module_of((const void *)p, mod, sizeof(mod));
            say("glprobe: wgl glCreateShader=%s module=\"%s\"", p ? "yes" : "no", mod);
            wglMakeCurrent(NULL, NULL);
        } else {
            say("glprobe: wgl create=%s error=%lu current=fail", rc ? "ok" : "fail", rc ? GetLastError() : err);
        }
        if (rc) {
            wglDeleteContext(rc);
        }
        print_dll_events(PHASE_CONTEXT, &loaded_expect, &unloaded_expect);
    }

    /* ---------------------------------------------------------------- what is in the process now */
    set_phase(PHASE_AFTER);
    drv_found = list_drv_modules("after-context", &drv_matched);

    /* The summary of the steps above, before the direct load can change the module list. */
    say("glprobe: summary window-adapter=%s window-luid=%08lX:%08lX hdc-open=%s monitor-dc-open=%s "
        "primary-luid=%08lX:%08lX window-is-primary=%s",
        win_how, win ? (unsigned long)win->luid.HighPart : 0ul, win ? (unsigned long)win->luid.LowPart : 0ul,
        src_hdc.have ? "yes" : "no", src_monitor.have ? "yes" : "no",
        (unsigned long)src_primary.luid.HighPart, (unsigned long)src_primary.luid.LowPart,
        (win && src_primary.have && same_luid(win->luid, src_primary.luid)) ? "yes" : "no");
    say("glprobe: summary window-icd=\"%s\" window-version=%lu window-flags=0x%lX window-ntstatus=0x%08lX",
        win ? win->gl.name : "", win ? (unsigned long)win->gl.version : 0ul, win ? (unsigned long)win->gl.flags : 0ul,
        win ? (unsigned long)win->gl.status : 0ul);
    if (g_expect[0]) {
        say("glprobe: summary expect=\"%s\" kernel-names-it-for-window=%s kernel-names-it-on-adapters=%d "
            "adapter-luid=%s adapter-is-window-adapter=%s opengl32-loaded-it=%d opengl32-unloaded-it=%d "
            "drv-module-carries-it=%d",
            g_expect, (win && has_text(win->gl.name, g_expect)) ? "yes" : "no", expect_adapters, expect_luid,
            (win && expect_adapters && same_luid(win->luid, expect_luid_value)) ? "yes" : "no",
            loaded_expect, unloaded_expect, drv_matched);
    }
    say("glprobe: summary pfd-icd=%d chosen-kind=%s context=%s renderer=\"%s\" version=\"%s\" arb=%s drv-modules=%d",
        pf_icd, chosen_kind, ctx_kind, renderer, version, arb ? "yes" : "no", drv_found);

    /* ---------------------------------------------------------------- the direct load, last */
    if (g_load_icd) {
        if (win) {
            direct_load(&win->gl);
        } else {
            say("glprobe: direct-load skipped, no adapter was named for the window");
        }
    } else {
        say("glprobe: direct-load skipped on request (--no-load-icd)");
    }

    if (dc) {
        ReleaseDC(wnd, dc);
    }
    if (wnd) {
        DestroyWindow(wnd);
    }
    free(infos);
    GetSystemTime(&st);
    say("glprobe: end utc=%04u-%02u-%02uT%02u:%02u:%02uZ elapsed-ms=%llu", st.wYear, st.wMonth, st.wDay, st.wHour,
        st.wMinute, st.wSecond, (unsigned long long)(GetTickCount64() - t0));
    SetEvent(g_done_event);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    /* A loaded ICD's own clean-up runs on process exit, and the watchdog no longer guards it. Exit at once, the way
     * a crashed client would leave, so that a slow detach in a driver cannot hold the lab's bound. */
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
