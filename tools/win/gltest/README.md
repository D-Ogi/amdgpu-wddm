# gltest

The project's own OpenGL 4.x client. One C file over Win32 and WGL, no third-party loader, no window on
screen unless it is asked for. It runs from an SSH session 0 shell.

It exists for two reasons. The first is the project rule that multithreading is tested with our own clients
before a game is allowed to be the witness (2026-09-29). The second is BD-076: the installed product has no
`OpenGLDriverName` and ships no OpenGL ICD, so an OpenGL 2 or later application either fails or draws on the
CPU. When the ICD is registered, somebody must be able to say in one line whether a context came up, **which
DLL served it**, whether the pixels are the expected ones, whether two threads with two contexts in one share
group work, and whether a compute shader writes what it was told to write. That is this client.

## The contract it stands on

`opengl32.dll` finds the ICD through the display adapter's driver key. The kernel side of that lookup is
`D3DKMTQueryAdapterInfo` with `KMTQAITYPE_UMOPENGLINFO`, which returns

```c
typedef struct _D3DKMT_OPENGLINFO
{
    WCHAR               UmdOpenGlIcdFileName[MAX_PATH];
    ULONG               Version;
    ULONG               Flags;
} D3DKMT_OPENGLINFO;
```

(WDK/SDK 10.0.26100, `shared\d3dkmthk.h`, lines 1851 to 1856). The query type is line 2365. A WoW64
process gets the name of the 32-bit ICD, which is why this client is built for x64 and for x86. The ICD
itself exports the `Drv*` entry points that `opengl32` calls, `DrvSetContext` among them. Mesa's own export
list for its WGL target is `src\gallium\targets\wgl\gallium_wgl.def.in`.

gltest does not call `D3DKMTQueryAdapterInfo`. It proves which DLL won from inside the process, two ways:

1. the module that owns the address `wglGetProcAddress` returned for `glCreateShader`
   (`GetModuleHandleEx` with `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS`, then `GetModuleFileName`),
2. every loaded module that exports `DrvSetContext`, found with `EnumProcessModules` and `GetProcAddress`.

Both appear in the `info` case output. The WGL and GL enumerants in the source are copied from the Khronos
headers in `ref\mesa\include\GL` (`wglext.h` lines 62 to 200, `glcorearb.h` lines 606 to 2442) and are
declared in the file, so the tool needs the Windows SDK and nothing else.

## Build

```
pwsh tools\win\gltest\build.ps1
pwsh tools\win\gltest\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Arch x64
pwsh tools\win\gltest\build.ps1 -SkipSelfTest
```

`-Tool gltest|glprobe|both` selects the program (default `both`). The section `glprobe` below describes the
second program.

Output: `<BC250_ROOT>\scratch\build\gltest\gltest-x64.exe`, `gltest-x86.exe`, `glprobe-x64.exe` and
`glprobe-x86.exe`. Headers and import
libraries come from the SDK NuGet packages under `-Kits`, the compiler from the installed Visual Studio, as
in `tools\win\vkfillcheck\build.ps1`. Release settings: `/O2 /GL` with `/LTCG`, `NDEBUG`, static CRT and
`/Brepro`, so two builds of the same source are the same bytes.

The build keeps the previous executables under `retained\` by their hash, checks that each image is the
architecture it asked for, runs `--help` and three usage errors, then runs `--case all` on the build PC and
the negative control, and prints the SHA-256 a lab runner pins. `-SkipSelfTest` leaves out the last two.

## Cases

Each case prints one `PASS <case> ...` or `FAIL <case> ...` line after its detail lines, and the run ends
with `gltest: PASS n/n` or `gltest: FAIL n/m`. Exit code 0 when every selected case passed, 1 when one failed
or the watchdog fired, 2 on a usage error.

| Case | What it does |
| --- | --- |
| `info` | `ChoosePixelFormat` and a legacy context, then `wglCreateContextAttribsARB` for core 4.6 and down the ladder to 3.3 until one is accepted. Prints `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, `GL_SHADING_LANGUAGE_VERSION`, the profile mask and the ICD module paths. |
| `draw` | A GLSL 4.x program draws a pattern into a 256x256 RGBA8 framebuffer object, `glReadPixels` reads it back, and all 65536 pixels are compared byte for byte. Four named points are printed whatever the verdict. |
| `present` | A 640x360 window, 120 frames with `SwapBuffers` and alternating colours, a `glReadPixels` from the back buffer before each swap. Reports frames, milliseconds and frames per second. |
| `threads` | Two threads, two contexts in one share group. Each thread renders 200 frames into its own framebuffer object, with a full byte comparison per frame. Thread A then writes one shared buffer object with a compute shader. Thread B reads that buffer after `glFenceSync` and `glClientWaitSync`. The shared buffer is the only cross-context synchronisation this case measures. A context that cannot run it fails the case. It is not a pass with a note. Below GL 4.3 the case says so. |
| `compute` | One compute dispatch of 16 work groups writes 1024 words into a shader storage buffer. The words are read back and compared exactly. Needs GL 4.3. |

Every channel of every expected colour is `k/255.0` for an integer `k`, so the UNORM8 encoding is exact and
the comparison needs no tolerance.

## Options

```
--case info|draw|present|threads|compute|all   case to run (default all)
--show                                         show the present window (hidden by default)
--seconds N                                    watchdog bound, 1..3600 (default 60)
--log PATH                                     also write every line to PATH
--max-version MAJOR.MINOR                      start the context ladder at this rung
--expect-icd TEXT                              info FAILs unless the driver that serves the
                                               context carries TEXT in its path
--negative-control                             displace every expected value by one
```

A watchdog thread ends the process with code 1 when the bound passes, after it writes the reason. Every line
is flushed as it is written, so the log of a killed run is complete up to the last thing that happened.

The window is created but never shown unless `--show` is given: the development PC must not get a window
(a workspace rule), and the lab does not need one. One consequence is honest in the output: with the
window hidden, the pixel ownership test may legitimately discard fragments, so a wrong back-buffer read is
counted and printed but does not decide the `present` verdict. With `--show` it does.

`--negative-control` displaces every expected value by one, so `draw`, `threads` and `compute` must all come
back FAIL. A comparison that cannot fail proves nothing, which is why the build runs this as well.

`--expect-icd TEXT` turns the module report of the `info` case into a verdict. Both of its independent reports
have to carry the text: the module that owns the address `wglGetProcAddress` returned, and a loaded module that
exports `DrvSetContext`. Without it a run against any other vendor's OpenGL 4.x driver reports `PASS 5/5`, which
is no use to a lab arm whose whole question is which DLL answered after a registration. The build runs it both
ways round, with the name of the driver this computer has and with a name no module carries.

`--max-version MAJOR.MINOR` starts the context ladder lower than 4.6, which is how the cases that need a later
version can be seen refusing on a driver that would have given one: `compute` and the shared-buffer hand-over of
`threads` both need 4.3, and on a 4.6 driver there is no other way to reach those paths. The build uses it for
exactly that, and to run `draw` once on a core 3.3 context, which takes the GLSL 330 source of each shader.

## glprobe

`glprobe.c` is the second program of this directory. gltest says which DLL served the context. glprobe says
why a DLL did or did not serve it. It runs in four steps and prints one line per reading:

1. **The kernel's answer.** `D3DKMTQueryAdapterInfo` with `KMTQAITYPE_UMOPENGLINFO` (name, `Version`,
   `Flags`, NTSTATUS) for each adapter of `D3DKMTEnumAdapters2` with its LUID, for the primary display
   adapter (`D3DKMTOpenAdapterFromGdiDisplayName`) and for the adapter of a window DC
   (`D3DKMTOpenAdapterFromHdc`, then the DC of the window's monitor if that fails, then the primary). Each
   adapter also gets `KMTQAITYPE_CHECKDRIVERUPDATESTATUS`, which `opengl32` checks before it accepts an ICD.
2. **The registry as the kernel reads it.** `KMTQAITYPE_QUERYREGISTRY` with
   `D3DDDI_QUERYREGISTRY_ADAPTERKEY` reads the six `OpenGL*` values of the adapter's key through the kernel.
   The D3D user-mode driver names of the same adapter (`KMTQAITYPE_UMDRIVERNAME`) are the control.
3. **What opengl32 does.** `LdrRegisterDllNotification` records each DLL that the process maps and unmaps
   while `ChoosePixelFormat`, `SetPixelFormat` and `wglCreateContext` run. A vectored exception handler
   records `OutputDebugString` text (Mesa logs there on Windows). The probe counts the pixel formats as ICD,
   generic and generic-accelerated, prints the GL strings of the context, and lists the loaded modules that
   export `DrvSetContext`.
4. **The direct load.** The ICD file that the kernel named (or `--load-file PATH`) is opened for read and
   loaded with `LoadLibrary`. The probe checks the 19 `Drv*` exports and calls `DrvValidateVersion`.

The `summary` lines at the end compare these readings: the window's adapter against the adapter that names
the ICD, the kernel's answer against the registry, the load by `opengl32` against the module that serves the
context.

```
--expect-icd TEXT        the summary says if each reading carries TEXT
--load-file PATH         the direct load takes PATH (default: the file the kernel named)
--validate-version N     DrvValidateVersion of a --load-file gets N (default 1)
--no-load-icd            leave out the direct load
--show / --seconds N / --log PATH   as in gltest (default watchdog 30 s)
```

Exit code 0 when the probe ran to its end, 1 when the watchdog fired, 2 on a usage error. A finding is
in the lines, never in the exit code. The build's self-test uses this computer's own OpenGL driver as the
positive control (named, loaded, found as a module that exports `DrvSetContext`) and a name that no module
carries as the negative control.

## Two things it does not do

- gltest does not register the ICD, and it does not read the registry or the adapter. It reports what the
  process got. glprobe reads the adapter and the registry, but it does not write either.
- `present` says nothing about independent flip or about who composes the frame. It counts frames and reads
  back the colour it drew.
