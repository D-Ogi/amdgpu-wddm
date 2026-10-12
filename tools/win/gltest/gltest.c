/*
 * gltest - the project's own OpenGL 4.x client.
 *
 * Why it exists: the owner's rule is that multithreading is tested with our own clients before a game is
 * allowed to be the witness (owner, 2026-09-29). BD-076 registers a system OpenGL ICD for our driver,
 * and the lane needs a client that says, in one line per case, whether a context came up, which DLL served
 * it, whether the pixels are the expected ones, whether two threads with two contexts in one share group
 * work, and whether a compute shader writes what it was told to write.
 *
 * Contracts used here, with the local source of each value:
 *   - The ICD is selected by opengl32.dll from the display adapter's driver key. The kernel side of that
 *     query is D3DKMTQueryAdapterInfo with KMTQAITYPE_UMOPENGLINFO, which returns
 *     D3DKMT_OPENGLINFO { WCHAR UmdOpenGlIcdFileName[MAX_PATH]; ULONG Version; ULONG Flags; }
 *     (WDK/SDK 10.0.26100, shared\d3dkmthk.h lines 1851-1856 and 2365). This client does not call that
 *     interface. It proves which DLL won by naming the module that owns the addresses opengl32 handed back,
 *     and by naming every loaded module that exports DrvSetContext, the ICD entry point every Windows
 *     OpenGL ICD must export (Mesa's own list: src\gallium\targets\wgl\gallium_wgl.def.in).
 *   - The WGL and GL enumerants below are copied from the Khronos headers in ref\mesa\include\GL
 *     (wglext.h lines 62-200, glcorearb.h lines 606-2442). They are not declared from memory, and they are
 *     declared here rather than included so that this tool depends on the Windows SDK alone.
 *
 * No third-party loader: every entry point past OpenGL 1.1 comes from wglGetProcAddress, and a missing one
 * is reported by name instead of crashing.
 *
 * Nothing is ever shown on screen unless --show is given: the development PC must not get a window
 * (owner's rule). The whole run is bounded by a watchdog thread, 60 s by default.
 *
 *   gltest.exe                                 all cases, hidden, 60 s bound
 *   gltest.exe --case threads --seconds 90
 *   gltest.exe --case present --show --log present.txt
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <GL/gl.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ types the 1.1 SDK header lacks */

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef unsigned __int64 GLuint64;
typedef struct __GLsync *GLsync;

/* ------------------------------------------------------------------ enumerants (ref\mesa\include\GL) */

#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C
#endif
#define GL_MAJOR_VERSION                  0x821B
#define GL_MINOR_VERSION                  0x821C
#define GL_CONTEXT_PROFILE_MASK           0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT       0x00000001
#define GL_DYNAMIC_COPY                   0x88EA
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_FRAMEBUFFER                    0x8D40
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_SHADER_STORAGE_BUFFER          0x90D2
#define GL_SHADER_STORAGE_BARRIER_BIT     0x00002000
#define GL_BUFFER_UPDATE_BARRIER_BIT      0x00000200
#define GL_COMPUTE_SHADER                 0x91B9
#define GL_SYNC_GPU_COMMANDS_COMPLETE     0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT        0x00000001
#define GL_ALREADY_SIGNALED               0x911A
#define GL_TIMEOUT_EXPIRED                0x911B
#define GL_CONDITION_SATISFIED            0x911C
#define GL_WAIT_FAILED                    0x911D

#ifndef GL_RGBA8
#define GL_RGBA8                          0x8058
#endif

#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_FLAGS_ARB             0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001
#define WGL_DRAW_TO_WINDOW_ARB            0x2001
#define WGL_ACCELERATION_ARB              0x2003
#define WGL_SUPPORT_OPENGL_ARB            0x2010
#define WGL_DOUBLE_BUFFER_ARB             0x2011
#define WGL_PIXEL_TYPE_ARB                0x2013
#define WGL_COLOR_BITS_ARB                0x2014
#define WGL_ALPHA_BITS_ARB                0x201B
#define WGL_DEPTH_BITS_ARB                0x2022
#define WGL_STENCIL_BITS_ARB              0x2023
#define WGL_FULL_ACCELERATION_ARB         0x2027
#define WGL_TYPE_RGBA_ARB                 0x202B

/* ------------------------------------------------------------------ logging */

static CRITICAL_SECTION g_log_lock;
static FILE *g_log_file;
static int g_log_ready;

static void say(const char *fmt, ...)
{
    char line[2048];
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

/* ------------------------------------------------------------------ watchdog */

static HANDLE g_done_event;
static unsigned g_seconds = 60;

static DWORD WINAPI watchdog_thread(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(g_done_event, g_seconds * 1000) == WAIT_TIMEOUT) {
        say("FAIL watchdog bound=%us (the run did not finish in time)", g_seconds);
        say("gltest: FAIL");
        /* A GL thread may be inside the driver holding a lock, so the orderly exit paths can deadlock.
         * The log is already flushed line by line, so nothing is lost by stopping here. */
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
}

/* ------------------------------------------------------------------ GL entry points */

typedef GLuint (APIENTRY *PFN_glCreateShader)(GLenum);
typedef void (APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (APIENTRY *PFN_glCompileShader)(GLuint);
typedef void (APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glDeleteShader)(GLuint);
typedef GLuint (APIENTRY *PFN_glCreateProgram)(void);
typedef void (APIENTRY *PFN_glAttachShader)(GLuint, GLuint);
typedef void (APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void (APIENTRY *PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (APIENTRY *PFN_glUseProgram)(GLuint);
typedef void (APIENTRY *PFN_glDeleteProgram)(GLuint);
typedef GLint (APIENTRY *PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void (APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef void (APIENTRY *PFN_glUniform1ui)(GLint, GLuint);
typedef void (APIENTRY *PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void (APIENTRY *PFN_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (APIENTRY *PFN_glGetBufferSubData)(GLenum, GLintptr, GLsizeiptr, void *);
typedef void (APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glBindBufferBase)(GLenum, GLuint, GLuint);
typedef void (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glDispatchCompute)(GLuint, GLuint, GLuint);
typedef void (APIENTRY *PFN_glMemoryBarrier)(GLbitfield);
typedef GLsync (APIENTRY *PFN_glFenceSync)(GLenum, GLbitfield);
typedef GLenum (APIENTRY *PFN_glClientWaitSync)(GLsync, GLbitfield, GLuint64);
typedef void (APIENTRY *PFN_glDeleteSync)(GLsync);

static PFN_glCreateShader pglCreateShader;
static PFN_glShaderSource pglShaderSource;
static PFN_glCompileShader pglCompileShader;
static PFN_glGetShaderiv pglGetShaderiv;
static PFN_glGetShaderInfoLog pglGetShaderInfoLog;
static PFN_glDeleteShader pglDeleteShader;
static PFN_glCreateProgram pglCreateProgram;
static PFN_glAttachShader pglAttachShader;
static PFN_glLinkProgram pglLinkProgram;
static PFN_glGetProgramiv pglGetProgramiv;
static PFN_glGetProgramInfoLog pglGetProgramInfoLog;
static PFN_glUseProgram pglUseProgram;
static PFN_glDeleteProgram pglDeleteProgram;
static PFN_glGetUniformLocation pglGetUniformLocation;
static PFN_glUniform1i pglUniform1i;
static PFN_glUniform1ui pglUniform1ui;
static PFN_glGenVertexArrays pglGenVertexArrays;
static PFN_glBindVertexArray pglBindVertexArray;
static PFN_glDeleteVertexArrays pglDeleteVertexArrays;
static PFN_glGenBuffers pglGenBuffers;
static PFN_glBindBuffer pglBindBuffer;
static PFN_glBufferData pglBufferData;
static PFN_glGetBufferSubData pglGetBufferSubData;
static PFN_glDeleteBuffers pglDeleteBuffers;
static PFN_glBindBufferBase pglBindBufferBase;
static PFN_glGenFramebuffers pglGenFramebuffers;
static PFN_glBindFramebuffer pglBindFramebuffer;
static PFN_glFramebufferTexture2D pglFramebufferTexture2D;
static PFN_glCheckFramebufferStatus pglCheckFramebufferStatus;
static PFN_glDeleteFramebuffers pglDeleteFramebuffers;
static PFN_glDispatchCompute pglDispatchCompute;
static PFN_glMemoryBarrier pglMemoryBarrier;
static PFN_glFenceSync pglFenceSync;
static PFN_glClientWaitSync pglClientWaitSync;
static PFN_glDeleteSync pglDeleteSync;

struct gl_slot {
    const char *name;
    FARPROC *slot;
    int core43;      /* 1: only needed for the compute work, missing is not fatal below GL 4.3 */
};

static const struct gl_slot g_gl_slots[] = {
    { "glCreateShader", (FARPROC *)&pglCreateShader, 0 },
    { "glShaderSource", (FARPROC *)&pglShaderSource, 0 },
    { "glCompileShader", (FARPROC *)&pglCompileShader, 0 },
    { "glGetShaderiv", (FARPROC *)&pglGetShaderiv, 0 },
    { "glGetShaderInfoLog", (FARPROC *)&pglGetShaderInfoLog, 0 },
    { "glDeleteShader", (FARPROC *)&pglDeleteShader, 0 },
    { "glCreateProgram", (FARPROC *)&pglCreateProgram, 0 },
    { "glAttachShader", (FARPROC *)&pglAttachShader, 0 },
    { "glLinkProgram", (FARPROC *)&pglLinkProgram, 0 },
    { "glGetProgramiv", (FARPROC *)&pglGetProgramiv, 0 },
    { "glGetProgramInfoLog", (FARPROC *)&pglGetProgramInfoLog, 0 },
    { "glUseProgram", (FARPROC *)&pglUseProgram, 0 },
    { "glDeleteProgram", (FARPROC *)&pglDeleteProgram, 0 },
    { "glGetUniformLocation", (FARPROC *)&pglGetUniformLocation, 0 },
    { "glUniform1i", (FARPROC *)&pglUniform1i, 0 },
    { "glUniform1ui", (FARPROC *)&pglUniform1ui, 0 },
    { "glGenVertexArrays", (FARPROC *)&pglGenVertexArrays, 0 },
    { "glBindVertexArray", (FARPROC *)&pglBindVertexArray, 0 },
    { "glDeleteVertexArrays", (FARPROC *)&pglDeleteVertexArrays, 0 },
    { "glGenBuffers", (FARPROC *)&pglGenBuffers, 0 },
    { "glBindBuffer", (FARPROC *)&pglBindBuffer, 0 },
    { "glBufferData", (FARPROC *)&pglBufferData, 0 },
    { "glGetBufferSubData", (FARPROC *)&pglGetBufferSubData, 0 },
    { "glDeleteBuffers", (FARPROC *)&pglDeleteBuffers, 0 },
    { "glBindBufferBase", (FARPROC *)&pglBindBufferBase, 1 },
    { "glGenFramebuffers", (FARPROC *)&pglGenFramebuffers, 0 },
    { "glBindFramebuffer", (FARPROC *)&pglBindFramebuffer, 0 },
    { "glFramebufferTexture2D", (FARPROC *)&pglFramebufferTexture2D, 0 },
    { "glCheckFramebufferStatus", (FARPROC *)&pglCheckFramebufferStatus, 0 },
    { "glDeleteFramebuffers", (FARPROC *)&pglDeleteFramebuffers, 0 },
    { "glDispatchCompute", (FARPROC *)&pglDispatchCompute, 1 },
    { "glMemoryBarrier", (FARPROC *)&pglMemoryBarrier, 1 },
    { "glFenceSync", (FARPROC *)&pglFenceSync, 0 },
    { "glClientWaitSync", (FARPROC *)&pglClientWaitSync, 0 },
    { "glDeleteSync", (FARPROC *)&pglDeleteSync, 0 },
};

/* ------------------------------------------------------------------ WGL extension entry points */

typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFN_wglChoosePixelFormatARB)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
typedef const char * (WINAPI *PFN_wglGetExtensionsStringARB)(HDC);
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);

static PFN_wglCreateContextAttribsARB pwglCreateContextAttribsARB;
static PFN_wglChoosePixelFormatARB pwglChoosePixelFormatARB;
static PFN_wglGetExtensionsStringARB pwglGetExtensionsStringARB;
static PFN_wglSwapIntervalEXT pwglSwapIntervalEXT;

/* ------------------------------------------------------------------ small helpers */

static const char *g_class_name = "gltest_window_class";
static int g_class_registered;
static int g_gl_loaded;
static int g_gl_major;
static int g_gl_minor;
static int g_glsl_version = 330;

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE) {
        return 0;   /* the client decides when it ends, not the window manager */
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static int register_class(void)
{
    WNDCLASSEXA wc;

    if (g_class_registered) {
        return 1;
    }
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;        /* WGL wants a DC of its own, not one from the cache */
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = g_class_name;
    if (!RegisterClassExA(&wc)) {
        say("  error: RegisterClassExA failed, GetLastError=%lu", GetLastError());
        return 0;
    }
    g_class_registered = 1;
    return 1;
}

struct target {
    HWND wnd;
    HDC dc;
    int shown;
};

static void target_destroy(struct target *t)
{
    if (t->dc) {
        ReleaseDC(t->wnd, t->dc);
        t->dc = NULL;
    }
    if (t->wnd) {
        DestroyWindow(t->wnd);
        t->wnd = NULL;
    }
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* Creates a window and sets its pixel format. The window is never shown unless show is 1: on the
 * development PC a visible window is forbidden, and the lab arm does not need one either. */
static int target_create(struct target *t, int width, int height, int show, const char *title)
{
    RECT r;
    DWORD style = WS_OVERLAPPEDWINDOW;
    int chosen = 0;
    PIXELFORMATDESCRIPTOR pfd;

    memset(t, 0, sizeof(*t));
    if (!register_class()) {
        return 0;
    }
    r.left = 0;
    r.top = 0;
    r.right = width;
    r.bottom = height;
    AdjustWindowRect(&r, style, FALSE);
    t->wnd = CreateWindowExA(0, g_class_name, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!t->wnd) {
        say("  error: CreateWindowExA failed, GetLastError=%lu", GetLastError());
        return 0;
    }
    t->dc = GetDC(t->wnd);
    if (!t->dc) {
        say("  error: GetDC failed");
        target_destroy(t);
        return 0;
    }

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    if (pwglChoosePixelFormatARB) {
        const int attribs[] = {
            WGL_DRAW_TO_WINDOW_ARB, 1,
            WGL_SUPPORT_OPENGL_ARB, 1,
            WGL_DOUBLE_BUFFER_ARB, 1,
            WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
            WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
            WGL_COLOR_BITS_ARB, 32,
            WGL_ALPHA_BITS_ARB, 8,
            WGL_DEPTH_BITS_ARB, 24,
            WGL_STENCIL_BITS_ARB, 8,
            0, 0
        };
        UINT found = 0;
        int formats[8];
        if (pwglChoosePixelFormatARB(t->dc, attribs, NULL, 8, formats, &found) && found > 0) {
            chosen = formats[0];
        }
    }
    if (!chosen) {
        chosen = ChoosePixelFormat(t->dc, &pfd);
    }
    if (!chosen) {
        say("  error: no pixel format, GetLastError=%lu", GetLastError());
        target_destroy(t);
        return 0;
    }
    if (!DescribePixelFormat(t->dc, chosen, sizeof(pfd), &pfd)) {
        say("  error: DescribePixelFormat failed for format %d", chosen);
        target_destroy(t);
        return 0;
    }
    if (!SetPixelFormat(t->dc, chosen, &pfd)) {
        say("  error: SetPixelFormat(%d) failed, GetLastError=%lu", chosen, GetLastError());
        target_destroy(t);
        return 0;
    }
    if (show) {
        ShowWindow(t->wnd, SW_SHOW);
        t->shown = 1;
    }
    pump();
    return 1;
}

/* The version ladder: the brief asks for core 4.6 and, when that is refused, the highest that works. */
static const int g_version_ladder[][2] = {
    { 4, 6 }, { 4, 5 }, { 4, 4 }, { 4, 3 }, { 4, 2 }, { 4, 1 }, { 4, 0 }, { 3, 3 }
};

static HGLRC rc_create(HDC dc, HGLRC share, int *out_major, int *out_minor, int quiet)
{
    size_t i;

    if (!pwglCreateContextAttribsARB) {
        return NULL;
    }
    for (i = 0; i < sizeof(g_version_ladder) / sizeof(g_version_ladder[0]); i++) {
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, g_version_ladder[i][0],
            WGL_CONTEXT_MINOR_VERSION_ARB, g_version_ladder[i][1],
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            WGL_CONTEXT_FLAGS_ARB, 0,
            0, 0
        };
        HGLRC rc = pwglCreateContextAttribsARB(dc, share, attribs);
        if (rc) {
            if (out_major) {
                *out_major = g_version_ladder[i][0];
            }
            if (out_minor) {
                *out_minor = g_version_ladder[i][1];
            }
            if (!quiet && i > 0) {
                say("  context: core %d.%d refused, %d.%d accepted",
                    g_version_ladder[0][0], g_version_ladder[0][1],
                    g_version_ladder[i][0], g_version_ladder[i][1]);
            }
            return rc;
        }
    }
    return NULL;
}

static int gl_quiet_errors(void)
{
    int n = 0;
    while (glGetError() != GL_NO_ERROR && n < 64) {
        n++;
    }
    return n;
}

static int gl_check(const char *where)
{
    GLenum e;
    int bad = 0;
    while ((e = glGetError()) != GL_NO_ERROR) {
        say("  error: GL error 0x%04X after %s", (unsigned)e, where);
        bad++;
        if (bad > 8) {
            break;
        }
    }
    return bad == 0;
}

static int load_gl_functions(void)
{
    size_t i;
    int missing = 0;

    if (g_gl_loaded) {
        return 1;
    }
    for (i = 0; i < sizeof(g_gl_slots) / sizeof(g_gl_slots[0]); i++) {
        FARPROC p = wglGetProcAddress(g_gl_slots[i].name);
        *g_gl_slots[i].slot = p;
        if (!p) {
            say("  warn: wglGetProcAddress(\"%s\") returned NULL", g_gl_slots[i].name);
            if (!g_gl_slots[i].core43) {
                missing++;
            }
        }
    }
    g_gl_loaded = 1;
    return missing == 0;
}

/* ------------------------------------------------------------------ which DLL served the context */

static void module_path_of(FARPROC addr, char *out, size_t n)
{
    union { FARPROC fn; LPCSTR p; } u;
    HMODULE mod = NULL;

    out[0] = '\0';
    if (!addr) {
        return;
    }
    u.fn = addr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           u.p, &mod) && mod) {
        if (!GetModuleFileNameA(mod, out, (DWORD)n)) {
            out[0] = '\0';
        }
    }
}

/* Every Windows OpenGL ICD exports the Drv* entry points opengl32 calls (Mesa's own export list is
 * src\gallium\targets\wgl\gallium_wgl.def.in). Naming each loaded module that exports DrvSetContext is
 * the second, independent way to say which DLL is the ICD of this process. */
static void report_icd_modules(void)
{
    HMODULE mods[512];
    DWORD needed = 0;
    size_t i;
    size_t count;
    char path[MAX_PATH];
    int found = 0;

    if (!EnumProcessModules(GetCurrentProcess(), mods, (DWORD)sizeof(mods), &needed)) {
        say("  info: icd-scan unavailable, EnumProcessModules failed (%lu)", GetLastError());
        return;
    }
    if (needed > (DWORD)sizeof(mods)) {
        needed = (DWORD)sizeof(mods);
    }
    count = (size_t)needed / sizeof(HMODULE);
    for (i = 0; i < count; i++) {
        if (GetProcAddress(mods[i], "DrvSetContext") || GetProcAddress(mods[i], "DrvSetContext@12")) {
            path[0] = '\0';
            if (GetModuleFileNameA(mods[i], path, (DWORD)sizeof(path))) {
                say("  info: icd-export DrvSetContext in %s", path);
                found++;
            }
        }
    }
    if (!found) {
        say("  info: icd-export no loaded module exports DrvSetContext"
            " (opengl32 may be serving this process itself)");
    }
}

/* ------------------------------------------------------------------ shader plumbing */

static GLuint compile_one(GLenum stage, const char *src, const char *tag)
{
    GLuint sh;
    GLint ok = 0;
    const GLchar *srcs[1];

    sh = pglCreateShader(stage);
    if (!sh) {
        say("  error: glCreateShader failed for %s", tag);
        return 0;
    }
    srcs[0] = src;
    pglShaderSource(sh, 1, (const GLchar *const *)srcs, NULL);
    pglCompileShader(sh);
    pglGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        char *log;
        pglGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        if (len > 0 && len < 8192) {
            log = (char *)malloc((size_t)len + 1);
            if (log) {
                GLsizei got = 0;
                pglGetShaderInfoLog(sh, len, &got, log);
                log[len] = '\0';
                say("  error: %s did not compile: %s", tag, log);
                free(log);
            }
        } else {
            say("  error: %s did not compile (no log)", tag);
        }
        pglDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint make_program(const char *vs_src, const char *fs_src, const char *cs_src, const char *tag)
{
    GLuint vs = 0;
    GLuint fs = 0;
    GLuint cs = 0;
    GLuint prog = 0;
    GLint ok = 0;

    if (cs_src) {
        cs = compile_one(GL_COMPUTE_SHADER, cs_src, tag);
        if (!cs) {
            return 0;
        }
    } else {
        vs = compile_one(GL_VERTEX_SHADER, vs_src, tag);
        if (!vs) {
            return 0;
        }
        fs = compile_one(GL_FRAGMENT_SHADER, fs_src, tag);
        if (!fs) {
            pglDeleteShader(vs);
            return 0;
        }
    }
    prog = pglCreateProgram();
    if (!prog) {
        say("  error: glCreateProgram failed for %s", tag);
    } else {
        if (cs) {
            pglAttachShader(prog, cs);
        } else {
            pglAttachShader(prog, vs);
            pglAttachShader(prog, fs);
        }
        pglLinkProgram(prog);
        pglGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            GLint len = 0;
            pglGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
            if (len > 0 && len < 8192) {
                char *log = (char *)malloc((size_t)len + 1);
                if (log) {
                    GLsizei got = 0;
                    pglGetProgramInfoLog(prog, len, &got, log);
                    log[len] = '\0';
                    say("  error: %s did not link: %s", tag, log);
                    free(log);
                }
            } else {
                say("  error: %s did not link (no log)", tag);
            }
            pglDeleteProgram(prog);
            prog = 0;
        }
    }
    if (vs) {
        pglDeleteShader(vs);
    }
    if (fs) {
        pglDeleteShader(fs);
    }
    if (cs) {
        pglDeleteShader(cs);
    }
    return prog;
}

static void source_of(char *out, size_t n, const char *body)
{
    _snprintf(out, n - 1, "#version %d core\n%s", g_glsl_version, body);
    out[n - 1] = '\0';
}

/* ------------------------------------------------------------------ shader bodies */

/* One triangle that covers the whole viewport, built from gl_VertexID: no vertex buffer, no attributes. */
static const char *k_vs_fullscreen =
    "void main()\n"
    "{\n"
    "    float x = (gl_VertexID == 1) ? 3.0 : -1.0;\n"
    "    float y = (gl_VertexID == 2) ? 3.0 : -1.0;\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    "}\n";

/* Every channel is k/255.0 for an integer k, so the UNORM8 encoding is exact and the readback can be
 * compared byte for byte instead of with a tolerance. */
static const char *k_fs_pattern =
    "out vec4 frag;\n"
    "void main()\n"
    "{\n"
    "    int tx = int(gl_FragCoord.x);\n"
    "    int ty = int(gl_FragCoord.y);\n"
    "    int r = tx & 255;\n"
    "    int g = ty & 255;\n"
    "    int b = 64 + ((tx + ty) & 127);\n"
    "    frag = vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, 1.0);\n"
    "}\n";

static const char *k_fs_thread =
    "uniform int uFrame;\n"
    "uniform int uLane;\n"
    "out vec4 frag;\n"
    "void main()\n"
    "{\n"
    "    int tx = int(gl_FragCoord.x);\n"
    "    int ty = int(gl_FragCoord.y);\n"
    "    int r = (tx * 3 + ty * 5 + uFrame * 7 + uLane * 11) & 255;\n"
    "    int g = uFrame & 255;\n"
    "    int b = (tx + uLane) & 255;\n"
    "    frag = vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, 1.0);\n"
    "}\n";

static const char *k_cs_fill =
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Out { uint v[]; };\n"
    "uniform uint uSeed;\n"
    "void main()\n"
    "{\n"
    "    uint i = gl_GlobalInvocationID.x;\n"
    "    uint x = i * 2654435761u + uSeed;\n"
    "    x ^= x >> 15;\n"
    "    x *= 2246822519u;\n"
    "    x ^= x >> 13;\n"
    "    v[i] = x;\n"
    "}\n";

/* The negative control displaces every expected value by one. A comparison that cannot fail proves
 * nothing, so draw, threads and compute must all FAIL under --negative-control; info and present compare
 * nothing that the flag touches. The three functions below are the only place an expected value is
 * computed, which is why the displacement lives here. */
static unsigned g_negative;

static unsigned mix32(unsigned i, unsigned seed)
{
    unsigned x = i * 2654435761u + seed;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return x + g_negative;
}

static unsigned char pattern_byte(int x, int y, int channel)
{
    unsigned d = g_negative;
    switch (channel) {
    case 0: return (unsigned char)((unsigned)(x & 255) + d);
    case 1: return (unsigned char)((unsigned)(y & 255) + d);
    case 2: return (unsigned char)((unsigned)(64 + ((x + y) & 127)) + d);
    default: return (unsigned char)(255u + d);
    }
}

static unsigned char thread_byte(int x, int y, int frame, int lane, int channel)
{
    unsigned d = g_negative;
    switch (channel) {
    case 0: return (unsigned char)((unsigned)((x * 3 + y * 5 + frame * 7 + lane * 11) & 255) + d);
    case 1: return (unsigned char)((unsigned)(frame & 255) + d);
    case 2: return (unsigned char)((unsigned)((x + lane) & 255) + d);
    default: return (unsigned char)(255u + d);
    }
}

/* ------------------------------------------------------------------ offscreen render target */

struct fbo {
    GLuint fb;
    GLuint tex;
    int w;
    int h;
};

static int fbo_create(struct fbo *f, int w, int h)
{
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    pglGenFramebuffers(1, &f->fb);
    glGenTextures(1, &f->tex);
    glBindTexture(GL_TEXTURE_2D, f->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pglBindFramebuffer(GL_FRAMEBUFFER, f->fb);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
    if (pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        say("  error: framebuffer %dx%d is not complete", w, h);
        return 0;
    }
    glViewport(0, 0, w, h);
    return gl_check("fbo setup");
}

static void fbo_destroy(struct fbo *f)
{
    if (f->fb) {
        pglBindFramebuffer(GL_FRAMEBUFFER, 0);
        pglDeleteFramebuffers(1, &f->fb);
        f->fb = 0;
    }
    if (f->tex) {
        glDeleteTextures(1, &f->tex);
        f->tex = 0;
    }
}

/* ------------------------------------------------------------------ case: info */

static int case_info(void)
{
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    char path[MAX_PATH];
    GLint profile = 0;

    say("  info: GL_VENDOR = %s", vendor ? vendor : "(null)");
    say("  info: GL_RENDERER = %s", renderer ? renderer : "(null)");
    say("  info: GL_VERSION = %s", version ? version : "(null)");
    say("  info: GL_SHADING_LANGUAGE_VERSION = %s", glsl ? glsl : "(null)");
    say("  info: context = core %d.%d (asked for %d.%d first)", g_gl_major, g_gl_minor,
        g_version_ladder[0][0], g_version_ladder[0][1]);
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    gl_quiet_errors();
    say("  info: GL_CONTEXT_PROFILE_MASK = 0x%X (core bit %s)", (unsigned)profile,
        (profile & GL_CONTEXT_CORE_PROFILE_BIT) ? "set" : "clear");

    module_path_of((FARPROC)pglCreateShader, path, sizeof(path));
    if (path[0]) {
        say("  info: icd module = %s (owns the address wglGetProcAddress returned for glCreateShader)", path);
    } else {
        say("  info: icd module = unknown (no module owns the glCreateShader address)");
    }
    path[0] = '\0';
    if (GetModuleFileNameA(GetModuleHandleA("opengl32.dll"), path, (DWORD)sizeof(path))) {
        say("  info: opengl32 = %s", path);
    }
    report_icd_modules();

    if (!vendor || !renderer || !version || !glsl) {
        say("FAIL info one of the GL strings is missing");
        return 1;
    }
    if (g_gl_major < 3) {
        say("FAIL info no core context above GL 3.0 (%d.%d)", g_gl_major, g_gl_minor);
        return 1;
    }
    say("PASS info core %d.%d, renderer %s", g_gl_major, g_gl_minor, renderer);
    return 0;
}

/* ------------------------------------------------------------------ case: draw */

#define DRAW_SIZE 256

static int case_draw(void)
{
    char vs[1024];
    char fs[1024];
    GLuint prog;
    GLuint vao = 0;
    struct fbo f;
    unsigned char *pix;
    int x;
    int y;
    int c;
    long mismatches = 0;
    int first_x = -1;
    int first_y = -1;
    int rc = 1;
    static const int sx[4] = { 0, 1, 128, 255 };
    static const int sy[4] = { 0, 255, 128, 7 };
    int i;

    source_of(vs, sizeof(vs), k_vs_fullscreen);
    source_of(fs, sizeof(fs), k_fs_pattern);
    prog = make_program(vs, fs, NULL, "draw program");
    if (!prog) {
        say("FAIL draw the GLSL %d program did not build", g_glsl_version);
        return 1;
    }
    if (!fbo_create(&f, DRAW_SIZE, DRAW_SIZE)) {
        pglDeleteProgram(prog);
        say("FAIL draw no %dx%d framebuffer", DRAW_SIZE, DRAW_SIZE);
        return 1;
    }
    pix = (unsigned char *)malloc((size_t)DRAW_SIZE * DRAW_SIZE * 4);
    if (!pix) {
        fbo_destroy(&f);
        pglDeleteProgram(prog);
        say("FAIL draw out of memory");
        return 1;
    }

    pglGenVertexArrays(1, &vao);
    pglBindVertexArray(vao);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    pglUseProgram(prog);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, DRAW_SIZE, DRAW_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pix);

    if (gl_check("draw and readback")) {
        for (y = 0; y < DRAW_SIZE; y++) {
            for (x = 0; x < DRAW_SIZE; x++) {
                const unsigned char *p = pix + ((size_t)y * DRAW_SIZE + (size_t)x) * 4;
                for (c = 0; c < 4; c++) {
                    if (p[c] != pattern_byte(x, y, c)) {
                        if (first_x < 0) {
                            first_x = x;
                            first_y = y;
                        }
                        mismatches++;
                        break;
                    }
                }
            }
        }
        /* The four named points are printed whatever the verdict: they are what a reader checks by hand. */
        for (i = 0; i < 4; i++) {
            const unsigned char *p = pix + ((size_t)sy[i] * DRAW_SIZE + (size_t)sx[i]) * 4;
            say("  draw: point (%3d,%3d) got %3u,%3u,%3u,%3u want %3u,%3u,%3u,%3u",
                sx[i], sy[i], p[0], p[1], p[2], p[3],
                pattern_byte(sx[i], sy[i], 0), pattern_byte(sx[i], sy[i], 1),
                pattern_byte(sx[i], sy[i], 2), pattern_byte(sx[i], sy[i], 3));
        }
        if (mismatches == 0) {
            say("PASS draw %dx%d exact, %d pixels compared", DRAW_SIZE, DRAW_SIZE, DRAW_SIZE * DRAW_SIZE);
            rc = 0;
        } else {
            say("FAIL draw %ld of %d pixels differ, first at (%d,%d)",
                mismatches, DRAW_SIZE * DRAW_SIZE, first_x, first_y);
        }
    } else {
        say("FAIL draw GL reported an error during the draw or the readback");
    }

    free(pix);
    pglBindVertexArray(0);
    pglDeleteVertexArrays(1, &vao);
    fbo_destroy(&f);
    pglUseProgram(0);
    pglDeleteProgram(prog);
    return rc;
}

/* ------------------------------------------------------------------ case: present */

#define PRESENT_W 640
#define PRESENT_H 360
#define PRESENT_FRAMES 120

static int g_show;

static int case_present(void)
{
    struct target t;
    HGLRC rc = NULL;
    int major = 0;
    int minor = 0;
    int frame;
    long mismatches = 0;
    int swap_fail = 0;
    LARGE_INTEGER freq;
    LARGE_INTEGER t0;
    LARGE_INTEGER t1;
    double ms;
    double fps;
    int verdict = 1;
    int interval_set = 0;
    static const unsigned char palette[4][3] = {
        { 32, 64, 96 }, { 200, 16, 8 }, { 8, 200, 16 }, { 16, 8, 200 }
    };

    if (!target_create(&t, PRESENT_W, PRESENT_H, g_show, "gltest present")) {
        say("FAIL present no %dx%d window", PRESENT_W, PRESENT_H);
        return 1;
    }
    rc = rc_create(t.dc, NULL, &major, &minor, 1);
    if (!rc || !wglMakeCurrent(t.dc, rc)) {
        if (rc) {
            wglDeleteContext(rc);
        }
        target_destroy(&t);
        say("FAIL present no core context on the presented window");
        return 1;
    }
    gl_quiet_errors();
    if (pwglSwapIntervalEXT) {
        interval_set = pwglSwapIntervalEXT(0) ? 1 : 0;
    }
    say("  present: window %dx%d %s, core %d.%d, swap interval %s",
        PRESENT_W, PRESENT_H, t.shown ? "shown" : "hidden (never ShowWindow)", major, minor,
        interval_set ? "0" : "driver default");

    glViewport(0, 0, PRESENT_W, PRESENT_H);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (frame = 0; frame < PRESENT_FRAMES; frame++) {
        const unsigned char *col = palette[frame & 3];
        unsigned char got[4];

        glDrawBuffer(GL_BACK);
        glClearColor((float)col[0] / 255.0f, (float)col[1] / 255.0f, (float)col[2] / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(PRESENT_W / 2, PRESENT_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
        if (got[0] != col[0] || got[1] != col[1] || got[2] != col[2]) {
            if (mismatches == 0) {
                say("  present: frame %d centre pixel got %u,%u,%u want %u,%u,%u",
                    frame, got[0], got[1], got[2], col[0], col[1], col[2]);
            }
            mismatches++;
        }
        if (!SwapBuffers(t.dc)) {
            say("  error: SwapBuffers failed at frame %d, GetLastError=%lu", frame, GetLastError());
            swap_fail++;
            break;
        }
        pump();
    }
    glFinish();
    QueryPerformanceCounter(&t1);
    ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    fps = ms > 0.0 ? (double)frame * 1000.0 / ms : 0.0;

    say("  present: frames=%d ms=%.1f fps=%.1f pixelcheck_mismatches=%ld", frame, ms, fps, mismatches);
    if (!gl_check("present loop")) {
        swap_fail++;
    }

    if (swap_fail || frame != PRESENT_FRAMES) {
        say("FAIL present %d of %d frames presented", frame, PRESENT_FRAMES);
    } else if (mismatches && t.shown) {
        /* With the window on screen the back buffer belongs to us, so a wrong pixel is a real defect.
         * Hidden, the pixel ownership test may legitimately discard the fragments, so the count is
         * reported but does not decide the verdict. */
        say("FAIL present %ld of %d back-buffer reads were wrong on a shown window", mismatches, frame);
    } else {
        say("PASS present %d frames in %.1f ms (%.1f fps), back-buffer reads %s",
            frame, ms, fps,
            mismatches ? "not compared (hidden window, pixel ownership undefined)" : "exact");
        verdict = 0;
    }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    target_destroy(&t);
    return verdict;
}

/* ------------------------------------------------------------------ case: threads */

#define THREAD_SIZE 64
#define THREAD_FRAMES 200
#define SHARED_ELEMS 256
#define SHARED_SEED 0x5A5A5A5Au

struct lane {
    int lane;                    /* 0 = A, writes the shared buffer; 1 = B, reads it */
    HDC dc;
    HGLRC rc;
    GLuint prog;
    GLuint ssbo;
    GLuint compute;              /* lane A only, 0 when the context is below GL 4.3 */
    int frames_done;
    long mismatches;
    int failed;
    char note[256];
};

static HANDLE g_written_event;      /* lane A: the fence for the shared buffer is published */
static HANDLE g_read_event;         /* lane B: the shared buffer has been read */
static volatile GLsync g_shared_sync;
static volatile LONG g_shared_ok;

static int lane_render(struct lane *L)
{
    struct fbo f;
    GLuint vao = 0;
    unsigned char *pix;
    GLint u_frame;
    GLint u_lane;
    int frame;

    if (!fbo_create(&f, THREAD_SIZE, THREAD_SIZE)) {
        _snprintf(L->note, sizeof(L->note) - 1, "no framebuffer");
        return 0;
    }
    pix = (unsigned char *)malloc((size_t)THREAD_SIZE * THREAD_SIZE * 4);
    if (!pix) {
        fbo_destroy(&f);
        _snprintf(L->note, sizeof(L->note) - 1, "out of memory");
        return 0;
    }
    pglGenVertexArrays(1, &vao);
    pglBindVertexArray(vao);
    pglUseProgram(L->prog);
    u_frame = pglGetUniformLocation(L->prog, "uFrame");
    u_lane = pglGetUniformLocation(L->prog, "uLane");
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    for (frame = 0; frame < THREAD_FRAMES; frame++) {
        int x;
        int y;
        int c;

        pglUniform1i(u_frame, frame);
        pglUniform1i(u_lane, L->lane);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, THREAD_SIZE, THREAD_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        for (y = 0; y < THREAD_SIZE; y++) {
            for (x = 0; x < THREAD_SIZE; x++) {
                const unsigned char *p = pix + ((size_t)y * THREAD_SIZE + (size_t)x) * 4;
                for (c = 0; c < 4; c++) {
                    if (p[c] != thread_byte(x, y, frame, L->lane, c)) {
                        L->mismatches++;
                        break;
                    }
                }
            }
        }
        L->frames_done = frame + 1;
    }

    free(pix);
    pglBindVertexArray(0);
    pglDeleteVertexArrays(1, &vao);
    fbo_destroy(&f);
    return gl_check("lane render");
}

static DWORD WINAPI lane_thread(LPVOID arg)
{
    struct lane *L = (struct lane *)arg;

    if (!wglMakeCurrent(L->dc, L->rc)) {
        _snprintf(L->note, sizeof(L->note) - 1, "wglMakeCurrent failed, GetLastError=%lu", GetLastError());
        L->failed = 1;
        if (L->lane == 0) {
            SetEvent(g_written_event);
        } else {
            SetEvent(g_read_event);
        }
        return 1;
    }
    gl_quiet_errors();

    if (!lane_render(L)) {
        L->failed = 1;
    }

    if (L->lane == 0) {
        /* Lane A writes the shared buffer on the GPU, then publishes a fence for lane B. */
        if (L->compute) {
            GLint u_seed;
            GLsync s;
            pglUseProgram(L->compute);
            u_seed = pglGetUniformLocation(L->compute, "uSeed");
            pglUniform1ui(u_seed, SHARED_SEED);
            pglBindBuffer(GL_SHADER_STORAGE_BUFFER, L->ssbo);
            pglBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, L->ssbo);
            pglDispatchCompute(SHARED_ELEMS / 64, 1, 1);
            pglMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            s = pglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            if (!s || !gl_check("lane A compute write")) {
                _snprintf(L->note, sizeof(L->note) - 1, "the shared write did not produce a fence");
                L->failed = 1;
            }
            g_shared_sync = s;
        } else {
            _snprintf(L->note, sizeof(L->note) - 1, "shared buffer skipped, context below GL 4.3");
        }
        SetEvent(g_written_event);
        if (WaitForSingleObject(g_read_event, (DWORD)g_seconds * 1000) != WAIT_OBJECT_0) {
            _snprintf(L->note, sizeof(L->note) - 1, "lane B never finished reading the shared buffer");
            L->failed = 1;
        }
    } else {
        if (WaitForSingleObject(g_written_event, (DWORD)g_seconds * 1000) != WAIT_OBJECT_0) {
            _snprintf(L->note, sizeof(L->note) - 1, "lane A never published the shared buffer");
            L->failed = 1;
        } else if (g_shared_sync) {
            GLsync s = g_shared_sync;
            GLenum w = pglClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
            if (w != GL_ALREADY_SIGNALED && w != GL_CONDITION_SATISFIED) {
                _snprintf(L->note, sizeof(L->note) - 1, "glClientWaitSync returned 0x%04X", (unsigned)w);
                L->failed = 1;
            } else {
                unsigned *data = (unsigned *)malloc(sizeof(unsigned) * SHARED_ELEMS);
                if (!data) {
                    _snprintf(L->note, sizeof(L->note) - 1, "out of memory");
                    L->failed = 1;
                } else {
                    long bad = 0;
                    int i;
                    memset(data, 0, sizeof(unsigned) * SHARED_ELEMS);
                    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, L->ssbo);
                    pglGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                                        (GLsizeiptr)(sizeof(unsigned) * SHARED_ELEMS), data);
                    for (i = 0; i < SHARED_ELEMS; i++) {
                        if (data[i] != mix32((unsigned)i, SHARED_SEED)) {
                            bad++;
                        }
                    }
                    if (!gl_check("lane B shared read")) {
                        L->failed = 1;
                    }
                    if (bad) {
                        _snprintf(L->note, sizeof(L->note) - 1,
                                  "shared buffer: %ld of %d words wrong (first got 0x%08X want 0x%08X)",
                                  bad, SHARED_ELEMS, data[0], mix32(0, SHARED_SEED));
                        L->failed = 1;
                    } else {
                        _snprintf(L->note, sizeof(L->note) - 1,
                                  "shared buffer: %d words exact after glClientWaitSync=0x%04X",
                                  SHARED_ELEMS, (unsigned)w);
                        InterlockedExchange(&g_shared_ok, 1);
                    }
                    free(data);
                }
            }
        } else {
            _snprintf(L->note, sizeof(L->note) - 1, "no fence to wait on");
        }
        SetEvent(g_read_event);
    }

    pglUseProgram(0);
    wglMakeCurrent(NULL, NULL);
    return L->failed ? 1u : 0u;
}

static int case_threads(void)
{
    struct target ta;
    struct target tb;
    struct lane la;
    struct lane lb;
    HGLRC rca = NULL;
    HGLRC rcb = NULL;
    HANDLE th[2];
    GLuint prog_a = 0;
    GLuint prog_b = 0;
    GLuint compute = 0;
    GLuint ssbo = 0;
    char vs[1024];
    char fs[1024];
    char cs[1024];
    int major = 0;
    int minor = 0;
    int verdict = 1;
    int share_ok;

    memset(&ta, 0, sizeof(ta));
    memset(&tb, 0, sizeof(tb));
    memset(&la, 0, sizeof(la));
    memset(&lb, 0, sizeof(lb));
    la.lane = 0;
    lb.lane = 1;

    if (!target_create(&ta, THREAD_SIZE, THREAD_SIZE, 0, "gltest lane A") ||
        !target_create(&tb, THREAD_SIZE, THREAD_SIZE, 0, "gltest lane B")) {
        say("FAIL threads could not make two windows");
        target_destroy(&ta);
        target_destroy(&tb);
        return 1;
    }
    rca = rc_create(ta.dc, NULL, &major, &minor, 1);
    if (!rca) {
        say("FAIL threads no core context for lane A");
        target_destroy(&ta);
        target_destroy(&tb);
        return 1;
    }
    /* Lane B's context shares lists with lane A's: the share argument of wglCreateContextAttribsARB is
     * the modern form of wglShareLists, and the one a core-profile client is expected to use. */
    rcb = rc_create(tb.dc, rca, NULL, NULL, 1);
    share_ok = rcb != NULL;
    if (!share_ok) {
        rcb = rc_create(tb.dc, NULL, NULL, NULL, 1);
        if (rcb && wglShareLists(rca, rcb)) {
            share_ok = 1;
            say("  threads: share group made with wglShareLists after the share context was refused");
        }
    }
    if (!rcb || !share_ok) {
        say("FAIL threads no shared context for lane B");
        if (rcb) {
            wglDeleteContext(rcb);
        }
        wglDeleteContext(rca);
        target_destroy(&ta);
        target_destroy(&tb);
        return 1;
    }

    /* Shared objects (programs, buffers) are made once, on this thread, so that the two lanes never
     * create or change the same object at the same time. Container objects (VAO, FBO) are not shared and
     * each lane makes its own. */
    if (!wglMakeCurrent(ta.dc, rca)) {
        say("FAIL threads wglMakeCurrent for the setup failed, GetLastError=%lu", GetLastError());
        goto cleanup;
    }
    gl_quiet_errors();
    source_of(vs, sizeof(vs), k_vs_fullscreen);
    source_of(fs, sizeof(fs), k_fs_thread);
    prog_a = make_program(vs, fs, NULL, "lane A program");
    prog_b = make_program(vs, fs, NULL, "lane B program");
    if (!prog_a || !prog_b) {
        wglMakeCurrent(NULL, NULL);
        say("FAIL threads the per-lane programs did not build");
        goto cleanup;
    }
    if (g_gl_major > 4 || (g_gl_major == 4 && g_gl_minor >= 3)) {
        source_of(cs, sizeof(cs), k_cs_fill);
        compute = make_program(NULL, NULL, cs, "shared-write compute program");
        pglGenBuffers(1, &ssbo);
        pglBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        pglBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof(unsigned) * SHARED_ELEMS), NULL,
                      GL_DYNAMIC_COPY);
        if (!gl_check("shared buffer setup") || !pglBindBufferBase || !pglDispatchCompute ||
            !pglMemoryBarrier || !pglFenceSync || !pglClientWaitSync) {
            if (compute) {
                pglDeleteProgram(compute);
                compute = 0;
            }
        }
    }
    glFinish();
    wglMakeCurrent(NULL, NULL);

    la.dc = ta.dc;
    la.rc = rca;
    la.prog = prog_a;
    la.ssbo = ssbo;
    la.compute = compute;
    lb.dc = tb.dc;
    lb.rc = rcb;
    lb.prog = prog_b;
    lb.ssbo = ssbo;

    g_written_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_read_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_written_event || !g_read_event) {
        say("FAIL threads could not create the hand-over events");
        goto cleanup;
    }
    th[0] = CreateThread(NULL, 0, lane_thread, &la, 0, NULL);
    th[1] = CreateThread(NULL, 0, lane_thread, &lb, 0, NULL);
    if (!th[0] || !th[1]) {
        say("FAIL threads CreateThread failed, GetLastError=%lu", GetLastError());
        goto cleanup;
    }
    if (WaitForMultipleObjects(2, th, TRUE, (DWORD)g_seconds * 1000) != WAIT_OBJECT_0) {
        say("FAIL threads a lane did not finish inside the %u s bound", g_seconds);
        CloseHandle(th[0]);
        CloseHandle(th[1]);
        goto cleanup;
    }
    CloseHandle(th[0]);
    CloseHandle(th[1]);

    say("  threads: lane A frames=%d mismatches=%ld%s%s", la.frames_done, la.mismatches,
        la.note[0] ? " note=" : "", la.note);
    say("  threads: lane B frames=%d mismatches=%ld%s%s", lb.frames_done, lb.mismatches,
        lb.note[0] ? " note=" : "", lb.note);

    if (la.failed || lb.failed) {
        say("FAIL threads a lane reported a failure");
    } else if (la.frames_done != THREAD_FRAMES || lb.frames_done != THREAD_FRAMES) {
        say("FAIL threads frames A=%d B=%d, expected %d each", la.frames_done, lb.frames_done,
            THREAD_FRAMES);
    } else if (la.mismatches || lb.mismatches) {
        say("FAIL threads pixel mismatches A=%ld B=%ld", la.mismatches, lb.mismatches);
    } else if (compute && !g_shared_ok) {
        say("FAIL threads the shared buffer did not survive the fence hand-over");
    } else {
        say("PASS threads 2 contexts, %d frames each, shared buffer %s", THREAD_FRAMES,
            compute ? "exact across glFenceSync/glClientWaitSync" : "skipped (context below GL 4.3)");
        verdict = 0;
    }

cleanup:
    if (g_written_event) {
        CloseHandle(g_written_event);
        g_written_event = NULL;
    }
    if (g_read_event) {
        CloseHandle(g_read_event);
        g_read_event = NULL;
    }
    if (prog_a || prog_b || ssbo || compute || g_shared_sync) {
        if (wglMakeCurrent(ta.dc, rca)) {
            if (g_shared_sync) {
                pglDeleteSync(g_shared_sync);
                g_shared_sync = NULL;
            }
            if (ssbo) {
                pglDeleteBuffers(1, &ssbo);
            }
            if (prog_a) {
                pglDeleteProgram(prog_a);
            }
            if (prog_b) {
                pglDeleteProgram(prog_b);
            }
            if (compute) {
                pglDeleteProgram(compute);
            }
            gl_quiet_errors();
            wglMakeCurrent(NULL, NULL);
        }
    }
    if (rcb) {
        wglDeleteContext(rcb);
    }
    if (rca) {
        wglDeleteContext(rca);
    }
    target_destroy(&ta);
    target_destroy(&tb);
    return verdict;
}

/* ------------------------------------------------------------------ case: compute */

#define COMPUTE_ELEMS 1024
#define COMPUTE_SEED 0x01234567u

static int case_compute(void)
{
    char cs[1024];
    GLuint prog;
    GLuint ssbo = 0;
    GLint u_seed;
    unsigned *data;
    long bad = 0;
    int first = -1;
    int i;
    int rc = 1;

    if (g_gl_major < 4 || (g_gl_major == 4 && g_gl_minor < 3)) {
        say("FAIL compute the context is core %d.%d, compute shaders need 4.3", g_gl_major, g_gl_minor);
        return 1;
    }
    if (!pglDispatchCompute || !pglBindBufferBase || !pglMemoryBarrier) {
        say("FAIL compute glDispatchCompute, glBindBufferBase or glMemoryBarrier is missing");
        return 1;
    }
    source_of(cs, sizeof(cs), k_cs_fill);
    prog = make_program(NULL, NULL, cs, "compute program");
    if (!prog) {
        say("FAIL compute the compute program did not build");
        return 1;
    }
    data = (unsigned *)malloc(sizeof(unsigned) * COMPUTE_ELEMS);
    if (!data) {
        pglDeleteProgram(prog);
        say("FAIL compute out of memory");
        return 1;
    }
    memset(data, 0, sizeof(unsigned) * COMPUTE_ELEMS);

    pglGenBuffers(1, &ssbo);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    pglBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof(unsigned) * COMPUTE_ELEMS), NULL,
                  GL_DYNAMIC_COPY);
    pglBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    pglUseProgram(prog);
    u_seed = pglGetUniformLocation(prog, "uSeed");
    pglUniform1ui(u_seed, COMPUTE_SEED);
    pglDispatchCompute(COMPUTE_ELEMS / 64, 1, 1);
    pglMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glFinish();
    pglGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(sizeof(unsigned) * COMPUTE_ELEMS), data);

    if (gl_check("compute dispatch and readback")) {
        for (i = 0; i < COMPUTE_ELEMS; i++) {
            if (data[i] != mix32((unsigned)i, COMPUTE_SEED)) {
                if (first < 0) {
                    first = i;
                }
                bad++;
            }
        }
        say("  compute: v[0]=0x%08X want 0x%08X, v[%d]=0x%08X want 0x%08X",
            data[0], mix32(0, COMPUTE_SEED), COMPUTE_ELEMS - 1, data[COMPUTE_ELEMS - 1],
            mix32((unsigned)(COMPUTE_ELEMS - 1), COMPUTE_SEED));
        if (bad == 0) {
            say("PASS compute %d words exact from %d work groups of 64", COMPUTE_ELEMS,
                COMPUTE_ELEMS / 64);
            rc = 0;
        } else {
            say("FAIL compute %ld of %d words differ, first at index %d", bad, COMPUTE_ELEMS, first);
        }
    } else {
        say("FAIL compute GL reported an error during the dispatch or the readback");
    }

    free(data);
    pglUseProgram(0);
    pglDeleteBuffers(1, &ssbo);
    pglDeleteProgram(prog);
    return rc;
}

/* ------------------------------------------------------------------ bootstrap and main */

/* The ARB entry points can only be asked for through a context, and a window's pixel format can be set
 * once, so the first context lives on a throwaway window. This is the standard Windows bootstrap. */
static int bootstrap_wgl(void)
{
    struct target t;
    HGLRC rc;

    if (!target_create(&t, 16, 16, 0, "gltest bootstrap")) {
        return 0;
    }
    rc = wglCreateContext(t.dc);
    if (!rc) {
        say("  error: wglCreateContext failed on the bootstrap window, GetLastError=%lu", GetLastError());
        target_destroy(&t);
        return 0;
    }
    if (!wglMakeCurrent(t.dc, rc)) {
        say("  error: wglMakeCurrent failed on the bootstrap context, GetLastError=%lu", GetLastError());
        wglDeleteContext(rc);
        target_destroy(&t);
        return 0;
    }
    pwglCreateContextAttribsARB =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    pwglChoosePixelFormatARB =
        (PFN_wglChoosePixelFormatARB)wglGetProcAddress("wglChoosePixelFormatARB");
    pwglGetExtensionsStringARB =
        (PFN_wglGetExtensionsStringARB)wglGetProcAddress("wglGetExtensionsStringARB");
    pwglSwapIntervalEXT = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT");
    say("  bootstrap: legacy GL_VERSION = %s", glGetString(GL_VERSION) ?
        (const char *)glGetString(GL_VERSION) : "(null)");
    say("  bootstrap: wglCreateContextAttribsARB %s, wglChoosePixelFormatARB %s, wglSwapIntervalEXT %s",
        pwglCreateContextAttribsARB ? "yes" : "NO", pwglChoosePixelFormatARB ? "yes" : "NO",
        pwglSwapIntervalEXT ? "yes" : "NO");
    if (pwglGetExtensionsStringARB) {
        const char *ext = pwglGetExtensionsStringARB(t.dc);
        say("  bootstrap: WGL extensions %u characters", ext ? (unsigned)strlen(ext) : 0u);
    }
    gl_quiet_errors();
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    target_destroy(&t);
    return pwglCreateContextAttribsARB != NULL;
}

static void usage(FILE *out)
{
    fputs("gltest - the project's own OpenGL 4.x test client\n"
          "\n"
          "  --case info|draw|present|threads|compute|all   case to run (default all)\n"
          "  --show                                         show the present window (hidden by default)\n"
          "  --seconds N                                    watchdog bound, 1..3600 (default 60)\n"
          "  --log PATH                                     also write every line to PATH\n"
          "  --negative-control                             displace every expected value by one, so\n"
          "                                                 draw, threads and compute must all fail\n"
          "  --help                                         this text\n"
          "\n"
          "Exit code 0 when every selected case passed, 1 when one failed or the watchdog fired,\n"
          "2 on a usage error.\n", out);
}

enum {
    CASE_INFO = 1,
    CASE_DRAW = 2,
    CASE_PRESENT = 4,
    CASE_THREADS = 8,
    CASE_COMPUTE = 16,
    CASE_ALL = 31
};

int main(int argc, char **argv)
{
    int selected = CASE_ALL;
    const char *log_path = NULL;
    int i;
    int passed = 0;
    int total = 0;
    HANDLE wd;
    int have_ctx = 0;
    struct target main_target;
    HGLRC main_rc = NULL;

    memset(&main_target, 0, sizeof(main_target));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--show") == 0) {
            g_show = 1;
        } else if (strcmp(argv[i], "--negative-control") == 0) {
            g_negative = 1;
        } else if (strcmp(argv[i], "--case") == 0) {
            if (++i >= argc) {
                fputs("gltest: --case needs a value\n", stderr);
                return 2;
            }
            if (strcmp(argv[i], "all") == 0) {
                selected = CASE_ALL;
            } else if (strcmp(argv[i], "info") == 0) {
                selected = CASE_INFO;
            } else if (strcmp(argv[i], "draw") == 0) {
                selected = CASE_DRAW;
            } else if (strcmp(argv[i], "present") == 0) {
                selected = CASE_PRESENT;
            } else if (strcmp(argv[i], "threads") == 0) {
                selected = CASE_THREADS;
            } else if (strcmp(argv[i], "compute") == 0) {
                selected = CASE_COMPUTE;
            } else {
                fprintf(stderr, "gltest: unknown case \"%s\"\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--seconds") == 0) {
            long v;
            if (++i >= argc) {
                fputs("gltest: --seconds needs a value\n", stderr);
                return 2;
            }
            v = strtol(argv[i], NULL, 10);
            if (v < 1 || v > 3600) {
                fprintf(stderr, "gltest: --seconds out of range: %s\n", argv[i]);
                return 2;
            }
            g_seconds = (unsigned)v;
        } else if (strcmp(argv[i], "--log") == 0) {
            if (++i >= argc) {
                fputs("gltest: --log needs a path\n", stderr);
                return 2;
            }
            log_path = argv[i];
        } else {
            fprintf(stderr, "gltest: unknown option \"%s\"\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    InitializeCriticalSection(&g_log_lock);
    g_log_ready = 1;
    if (log_path) {
        g_log_file = fopen(log_path, "w");
        if (!g_log_file) {
            fprintf(stderr, "gltest: cannot write %s\n", log_path);
            return 2;
        }
    }

    g_done_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_done_event) {
        fputs("gltest: CreateEvent failed\n", stderr);
        return 1;
    }
    wd = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    if (!wd) {
        fputs("gltest: the watchdog thread did not start\n", stderr);
        return 1;
    }

    say("gltest: start bound=%us cases=%s%s%s%s%s show=%d",
        g_seconds,
        (selected & CASE_INFO) ? "info " : "",
        (selected & CASE_DRAW) ? "draw " : "",
        (selected & CASE_PRESENT) ? "present " : "",
        (selected & CASE_THREADS) ? "threads " : "",
        (selected & CASE_COMPUTE) ? "compute" : "",
        g_show);
    if (g_negative) {
        say("  note: --negative-control, draw, threads and compute must all FAIL");
    }

    if (bootstrap_wgl()) {
        if (target_create(&main_target, 64, 64, 0, "gltest main")) {
            main_rc = rc_create(main_target.dc, NULL, &g_gl_major, &g_gl_minor, 0);
            if (main_rc && wglMakeCurrent(main_target.dc, main_rc)) {
                GLint mj = 0;
                GLint mn = 0;
                gl_quiet_errors();
                glGetIntegerv(GL_MAJOR_VERSION, &mj);
                glGetIntegerv(GL_MINOR_VERSION, &mn);
                gl_quiet_errors();
                if (mj > 0) {
                    g_gl_major = mj;
                    g_gl_minor = mn;
                }
                g_glsl_version = (g_gl_major > 4 || (g_gl_major == 4 && g_gl_minor >= 3)) ? 430 : 330;
                load_gl_functions();
                have_ctx = 1;
            }
        }
    }
    if (!have_ctx) {
        say("  error: no core OpenGL context on this adapter, every case fails");
    }

    if (selected & CASE_INFO) {
        total++;
        if (have_ctx) {
            if (case_info() == 0) {
                passed++;
            }
        } else {
            say("FAIL info no context");
        }
    }
    if (selected & CASE_DRAW) {
        total++;
        if (have_ctx) {
            if (case_draw() == 0) {
                passed++;
            }
        } else {
            say("FAIL draw no context");
        }
    }
    if (selected & CASE_COMPUTE) {
        total++;
        if (have_ctx) {
            if (case_compute() == 0) {
                passed++;
            }
        } else {
            say("FAIL compute no context");
        }
    }

    /* The window cases run with the shared setup context released, so that nothing of ours is current
     * while they make their own contexts current. */
    if (main_rc) {
        wglMakeCurrent(NULL, NULL);
    }

    if (selected & CASE_PRESENT) {
        total++;
        if (have_ctx) {
            if (case_present() == 0) {
                passed++;
            }
        } else {
            say("FAIL present no context");
        }
    }
    if (selected & CASE_THREADS) {
        total++;
        if (have_ctx) {
            if (case_threads() == 0) {
                passed++;
            }
        } else {
            say("FAIL threads no context");
        }
    }

    if (main_rc) {
        wglDeleteContext(main_rc);
    }
    target_destroy(&main_target);

    if (passed == total) {
        say("gltest: PASS %d/%d", passed, total);
    } else {
        say("gltest: FAIL %d/%d", passed, total);
    }
    SetEvent(g_done_event);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    return passed == total ? 0 : 1;
}
