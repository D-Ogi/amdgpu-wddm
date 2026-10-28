# Per-application graphics settings

Status: implemented on bc250-win branch `umd/per-app-graphics-settings` and on branch
`amdgpu-wddm/per-app-graphics-settings` of the DXVK and vkd3d-proton forks. The host gates pass on the
development PC (2026-10-08). No lab trial on unit A has run yet. The D3D12 half of `MaxFrameLatency` came later,
on branch `d3d12/frame-latency` for train b24, and waits for its own lab check.

The control application writes these settings into the registry. The driver reads them and applies them to one
application or to all applications. This document is the contract between the two sides. The Vulkan ICD settings
`WsiRoute` and `MemoryOverflow` belong to the ICD and are not in this document.

## Keys

| Key | Scope |
|---|---|
| `HKLM\SOFTWARE\amdgpu-wddm\Graphics` | The global key. Its values apply to every application |
| `HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\<image>` | The application key of one program |

`<image>` is the file name of the process image without its directory, for example `witcher3.exe`. The registry
compares key names without case, so `Witcher3.EXE` names the same key. Two programs with the same file name share
one application key. Each value is a `REG_DWORD`. A 32-bit process reads the 64-bit view of these keys, so one
key serves both kinds of process.

## Precedence

The driver takes each setting from the first source that holds a valid value:

1. The environment variable `AMDGPU_WDDM_<NAME>` of the process, where `<NAME>` is the value name in upper snake case.
2. The application key.
3. The global key.
4. The DXVK profile (the D3D11 and D3D12 shells only, not the router). See "The DXVK profile source".

When no source holds a valid value, the driver leaves the setting to the application. An environment value must be
a decimal number of 1 to 10 digits, with no sign, no space and no `0x`. The driver ignores a value outside its
range, a registry value that is not a `REG_DWORD`, and an environment value that is not a number. For each one it
writes one log line and then reads the next source.

The two shells read the settings once per process, at the first device creation. A change takes effect at the next
start of the application. The router reads `RenderOnCpu` at each `OpenAdapter` call, as it reads its `AppRouter`
key. The installed INF owns the Windows driver version.

## The DXVK profile source

DXVK keeps per-game `dxgi.*` options in its own configuration: the app profiles in its source
(`src/util/config/config.cpp`), `dxvk.conf` and `DXVK_CONFIG`. DXVK applies them in its DXGI front and its swap
chain. On our route the system DXGI owns the swap chain and the adapter identity, so DXVK's DXGI code does not run
and the options apply nowhere. The DXVK engine reads them with DXVK's own configuration code and returns them
(`Bc250DxvkEngineGetAppOptions`, engine ABI 1.5, `src/ddi/bc250_dxvk_engine.h` of the DXVK fork). A newer DXVK with
new or changed profiles therefore needs no change in the shells. The user's `dxvk.conf` and `DXVK_CONFIG` win over
the app profile, as in DXVK.

Both shells load the engine from the `d3d11` directory of the package (the D3D12 shell for this one query). They use
the options that they have a mechanism for as the lowest source (`driver/umd/app-settings/dxvk-profile.h`):

| DXVK option | Setting | Rule |
|---|---|---|
| `dxvk.maxFrameRate`, else `dxgi.maxFrameRate` | `FrameRateLimit` | n above 0: a cap of n. -1 (no limiter in DXVK): 0. -n: a cap of n. 0: no value |
| `dxgi.syncInterval` | `VSync` | 0 or 1. A negative value: no value. 2 and more: ignored with a log line |
| `dxgi.maxFrameLatency` | `MaxFrameLatency` | 1-3. 0: no value. 4 and more: ignored with a log line |

For -n, DXVK engages its limiter only when the game runs much faster than n. The shells give it as a cap of n, which
is the same limit for the games that DXVK sets it for (games whose speed depends on the frame rate above 60).
The effective-settings line marks such a value `/dxvk-profile`.

The other `dxgi.*` options do not apply on this route. The lab flavour writes one log line for each option of the
process with the reason, after the effective-settings line:

| DXVK options | Reason |
|---|---|
| `customVendorId`, `customDeviceId`, `customDeviceDesc`, `hideNvidiaGpu`, `hideNvkGpu`, `hideAmdGpu`, `hideIntelGpu` | The system DXGI reports the adapter identity from the kernel-mode driver, the same for every process |
| `maxDeviceMemory`, `maxSharedMemory` | The system DXGI reports the memory sizes from the kernel-mode driver, the same for every process |
| `enableDummyCompositionSwapchain` | Composition swap chains are the system DXGI's |
| `deferSurfaceCreation`, `tearFree`, `forceRefreshRate`, `enableHDR`, `enableUe4Workarounds` | Options of DXVK's DXGI and swap chain, which the system DXGI replaces on this route |
| any other | No mechanism on this route |

The profiles of DXVK acb78320 that set an option with a mechanism: `dxgi.maxFrameRate = -60` (six games, one with
-120, one with -1), `dxgi.syncInterval = 1` (five games, for example Persona 3 Reload, `P3R.exe`, a D3D12 game) and
`dxgi.maxFrameLatency = 1` (three games). D3D12 games get the same options as D3D11 games, as under Proton, where
DXVK's DXGI serves both.

## The settings

| Value | Environment variable | Accepted | D3D11 | D3D12 | Vulkan, OpenGL |
|---|---|---|---|---|---|
| `FrameRateLimit` | `AMDGPU_WDDM_FRAME_RATE_LIMIT` | 0, 20-300 | works | works | works |
| `VSync` | `AMDGPU_WDDM_VSYNC` | 0, 1 | works | works | works |
| `Anisotropy` | `AMDGPU_WDDM_ANISOTROPY` | 1, 2, 4, 8, 16 | works | works | works |
| `MaxFrameLatency` | `AMDGPU_WDDM_MAX_FRAME_LATENCY` | 1-3 | works | works | works |
| `PerformanceOverlay` | `AMDGPU_WDDM_PERFORMANCE_OVERLAY` | 0, 1 | works | not applied | not read |
| `RenderOnCpu` | `AMDGPU_WDDM_RENDER_ON_CPU` | 0, 1 | works, also for D3D10 | no CPU route | not read |

"Works" in this table means that the host gates pass. The lab script `settings-lab.ps1` of the work directory
checks each row on unit A.

### FrameRateLimit

The value 0 means no cap, also when the global key sets one. A value from 20 to 300 is the cap in frames per second.
Each device keeps its own clock, and the shell holds each Present until one interval has passed since the last one.
A high-resolution waitable timer sleeps to about 1 ms before the target, and short yields do the rest. A frame that
comes more than one interval late restarts the clock, so a stall does not cause a burst of frames above the cap.

The D3D11 shell waits after the Present callback returns. The D3D12 shell waits before it enters the queue domain
of the device, so no other queue operation of the device waits with the Present. The code is `FrameLimiter` in
`driver/umd/app-settings/app-settings.h`.

### VSync

The value 0 forces the sync interval 0 (`DXGI_DDI_FLIP_INTERVAL_IMMEDIATE`). The value 1 forces the sync
interval 1 (`DXGI_DDI_FLIP_INTERVAL_ONE`). Both shells set `SyncIntervalOverrideValid` and `SyncIntervalOverride`
for both values: the D3D12 shell in `D3D12DDI_PRESENT_0051`, and the D3D11 shell in `DXGIDDICB_PRESENT`.

The D3D11 shell gets those two fields only at the interface it negotiates. The WDK declares them only for the
WDDM 2.2.2 interface and later. The D3D11 runtime copies the whole structure only from interface 0xB0023
(WDDM 2.2) build 5 (`IS_DXGI1_6_1_BASE_FUNCTIONS` in `d3d10umddi.h`). The shell therefore offers the WDDM 2.2
interface to an FL12 adapter whose KMD reports WDDM 2.2 or later, with the WDDM 2.2 device table and the
DXGI 1.6.1 table that go with it
(`driver/umd/dxvk/ddi-wddm22.h`). `DeviceOwner::full_present_callback` holds the answer for the device, and the
Present path passes the whole override while it is true.

Below that interface the shell passes `VSync` 0 only, and does `VSync` 1 with its own waits. This applies to
an FL11 adapter, which offers the D3D11.1 interface alone, an older KMD (BD-153 below), and the process switch
`wddm22-ddi-off` of `AMDGPU_WDDM_D3D11_EXPERIMENT` (`driver/umd/dxvk/ddi-experiment.h`), which withholds the
WDDM 2.2 offer for a comparison. Then `d3d11.dll` 10.0.22621 gives `PresentCB_PreWDDM2_2`, which copies only the
older, shorter structure into its own `DXGIDDICB_PRESENT` (BD-099). On x64 the copy holds
`SyncIntervalOverrideValid`, which is in the tail padding of the older structure, but not `SyncIntervalOverride`.
Thus each override arrives as interval 0. An override for `VSync` 1 gave 873-928 frames/s for the intervals 1
and 2. On x86 (WOW64) the copy holds neither field, and the override has no effect.

BD-153 adds another constraint: `KMTQAITYPE_DRIVERVERSION` must report WDDM 2.2 or later on the adapter's
LUID. The driver reads it once at adapter open. Version discovery and device creation use the same result.
The current KMD reports WDDM 2.0, so its FL12 adapter offers that table as its highest version. A failed query
also withholds the optional WDDM 2.2 table and logs the failure. FL12 support stays unchanged. The Present
path uses the existing vertical-blank waits below the WDDM 2.2 interface.

The arguments of the D3D11 Present DDI do not show the interval of the application. For a swap chain in a window,
`FlipInterval` is 0 for the intervals 0, 1 and 2. Thus on the old callback the shell waits for one vertical blank
after each Present with `VSync` 1 (`VBlankPacer` in `driver/umd/dxvk/vblank-pacer.h`), and the runtime keeps the
interval of the application. It waits on the desktop output of the adapter, the primary output first. The DDI does
not name the window, so with several outputs of different refresh rates the wait follows that one output. On the
whole callback the runtime paces the frame and the shell waits for nothing.

The measurements below come from the shell on the old callback (the vertical-blank waits). On unit A at 59 Hz, the
x64 d3d11bench in a window gave 60 frames/s with `VSync` 1 for the intervals 0 and 1, and 30 frames/s for interval
2. With `VSync` 0 it gave 857-873 frames/s for the intervals 1 and 2. The x86 d3d11bench gave 60 frames/s with
`VSync` 1 for the intervals 0 and 1, and 30 frames/s for interval 2. With `VSync` 0 it gave 774 frames/s for
interval 0, 60 frames/s for interval 1 and 30 frames/s for interval 2. Thus on that callback `VSync` 1 does not
shorten interval 2, and for an x86 application `VSync` 0 does not shorten interval 1 or 2. The shell on the whole
callback is measured in the b24 lab validation.

### Anisotropy

The value is the maximum anisotropy of each sampler with a linear minification filter. The value 1 turns
anisotropic filtering off for those samplers. A sampler with a point minification filter stays as the application
made it.

- D3D11: the shell passes the DXVK option `d3d11.samplerAnisotropy` to the engine. DXVK applies it in
  `src/d3d11/d3d11_sampler.cpp`.
- D3D12: the shell sets `VKD3D_SAMPLER_ANISOTROPY` for the engine. The vkd3d-proton fork reads it at device
  creation and applies it to static samplers and to sampler descriptors. The fork limits the value to
  `maxSamplerAnisotropy` of the Vulkan device.

### MaxFrameLatency

The value is the number of Presents that a shell lets wait for the GPU. After each Present the shell waits until
the frame that is N places back has finished on the GPU. Both shells apply it.

The limit is a ceiling and never a floor. The first check of the wait costs no wait at all. An application that
keeps fewer frames in flight than the value, with its own fences or with the frame-latency waitable object of its
swap chain, finds the frame already finished. The shell then adds no wait of its own. The setting can lower the
frames in flight of such an application, never raise them.

- D3D11: the wait uses the present fence of the shell, as the swap chain of DXVK does for its own frame latency.
  It waits for the Present that is N places back. Only a lost device makes the Present fail. The code is in
  `driver/umd/dxvk/ddi-present.cpp`.
- D3D12: the runtime makes the kernel present call, so the shell holds no present fence. Its proof that a frame
  finished is a device progress snapshot (`driver/umd/d3d12/device-progress.h`), which it takes at each Present.
  The snapshot names each monitored fence of the device that the GPU has not reached yet, so everything that
  every context of the device submitted before that Present has retired once the snapshot is retired. The shell
  keeps a ring of N snapshots, one per device, and before Present N returns it waits for the snapshot of Present
  N-k. The code is `FrameLatency` in `driver/umd/d3d12/frame-latency.h`. The snapshot covers the work that
  produced that frame. It does not cover the kernel present, which the runtime makes after the shell returns, so
  the number of flips that the compositor still holds stays a matter for DXGI.

The D3D12 wait runs after the shell releases the device's queue domain, as the frame rate limit runs before it
(`device-state.h`, `QueueDomainScope`), so no other queue operation of the device waits with the present. A
refused present gets no wait. The wait polls the snapshot: a short spin first, then sleeps of 250 microseconds on
a high-resolution timer. It gives up after one second, counts the event and lets the frame through, so a fence
that never retires cannot stop the application inside the driver. After 8 frames that spend the whole budget the
gate stops for the life of the device. A fence that never retires would otherwise cost a budget in every Present,
which is one frame per second for as long as the application runs.

A lost device reports the value `0xFFFFFFFFFFFFFFFF` on the fences that the kernel wrote, and that value counts as
retired. A loss that leaves a fence below its published value instead ends the wait on the budget, and the stop
above ends the gate. The shell's own flag for a lost device is not read here, because the gate has no business
failing a present.

One device that presents on several queues shares one ring, as it shares one frame rate clock. The setting
belongs to the application, not to one swap chain.

The snapshot holds only what the Vulkan ICD publishes. The ICD publishes the value of its monitored fence after
every native submit (`radv_wddm2_cs.c`, `BC250_HOST_PUBLISH_PROGRESS`). An ICD that publishes nothing gives an
empty snapshot, which is retired at once, so the setting does nothing. The lab check reads the witness line to
prove that the gate waited, and not only that the value reached the shell.

The D3D12 shell writes four lines at most per process, one of each kind, as the witness that the gate is in force.
Each line goes to the log of `AMDGPU_WDDM_LOG` and to the debugger, as the settings line does, so a lab trial
reads them from the log file:

```
BC250 MaxFrameLatency=1: the present waited after 9 checks (frame 42)
BC250 MaxFrameLatency=1: the present gave up after 4005 checks (frame 1180)
BC250 MaxFrameLatency=1: the present proved nothing: more unretired fences than the snapshot holds (frame 7)
BC250 MaxFrameLatency=1: the gate stopped: too many frames did not finish inside the budget (frame 1310)
```

The first line is the proof that the gate is in force. The second line is a defect report: a frame did not finish
within one second. The third line says that the device had more unretired fences than the snapshot holds (8), so
the gate could prove nothing and let that frame through. It separates a gate that found the frame finished from a
gate that could not look. The fourth line says that the gate stopped itself.

### PerformanceOverlay

The value 1 shows the DXVK HUD with the items `fps`, `frametimes`, `gpuload` and `api`. The D3D11 engine draws the
HUD into each surface that the application presents, after the last draw of the frame. The engine skips a surface
that it cannot use as a color attachment, a multisampled surface, and a surface that it has not written yet.

The D3D12 shell does not apply this value. vkd3d-proton has no HUD of its own. The only overlay of a D3D12
application on this driver is the one of the application itself.

### RenderOnCpu

The value 1 sends a D3D10 or D3D11 application to the CPU UMD `bc250d3d.dll`, which is Mesa `d3d10umd` on
llvmpipe. The router applies it in `driver/umd/router/router-policy.h`, after the built-in protected list and
before the `Deny` list. The route log line gives the reason `app-render-on-cpu` and the field `render_on_cpu`.

The `Deny` list of `HKLM\SOFTWARE\amdgpu-wddm\AppRouter` works as before. Either one keeps an application on the
CPU UMD. The value 0 never moves an application off the CPU UMD that the `AppRouter` policy chose. When the
`AppRouter` value `Mode` is absent, every application stays on the CPU UMD and `RenderOnCpu` changes nothing.

Under `gpu-default` the router also applies a built-in default `Deny` list of its own (`IsCpuOnlyDialogHost` in
`router-policy.h`): the images whose window the application GPU UMD was measured not to show. It holds
`PickerHost.exe`, the File Picker UI Host, which serves the Windows firewall notification dialog and the modern
file open and save dialogs (BD-126), and `SecHealthUI.exe`, the Windows Security app, which made DWM crash in a loop
on the GPU route (BD-168 class). The route log line gives the reason `app-dialog-host`. An entry in `Allow`
lifts the built-in list for that image, because `Allow` names an image and a built-in default does not. An entry
in `Deny`, the protected list, `RenderOnCpu` and, under `allowlist`, "not listed in `Allow`" answer before it and
keep their own reasons. The list moves the route only: every window of the image moves with it, and a program on
the CPU route gets FL 10_0 (BD-088).

D3D12 has no CPU route. No CPU route through lavapipe exists in any repository of this project.

### The system Vulkan ICD

From tester.34 the system Vulkan ICD reads `FrameRateLimit`, `VSync`, `Anisotropy` and `MaxFrameLatency` by the
same contract (`src/amd/vulkan/winsys/wddm2/radv_wddm2_app_settings.h` in the Mesa fork, one reader in the 64-bit
and in the 32-bit ICD). It reads them once per process. It does not read them in `dwm.exe`, and a device of the ICD
that a D3D shell hosts does not apply them, because the shell applies them. The OpenGL ICD presents through the
64-bit system Vulkan ICD in the same process, so a 64-bit OpenGL game gets them too. The present rules are in
`src/vulkan/wsi/wsi_win32_present_policy.h`. The host test is `radv_wddm2_app_settings_test.c`.

| Value | 64-bit ICD, DXGI route | 64-bit ICD, GDI route | 32-bit ICD (GDI only) |
|---|---|---|---|
| `FrameRateLimit` | a paced wait before each present | the same | the same |
| `VSync` | 1 is FIFO, 0 is IMMEDIATE (tearing where the chain may tear) | 1 is FIFO, 0 is IMMEDIATE (no wait for the composition) | 1 is as without the value, 0 removes the wait for the composition |
| `MaxFrameLatency` | after a present, a wait of at most 1 s for the copy of the present that many presents back | no wait: each present copies a finished frame | no wait, as on the GDI route |
| `Anisotropy` | the anisotropy of each sampler with a linear minification filter and normalized coordinates | the same | the same |

The `VSync` value also replaces a present mode that the application changes per present. The effective line of the
log is `amdgpu-wddm settings: api=vulkan app=<image> ...`.

### Installed driver version

The package sets the Windows driver version during installation. There is no
runtime override. See [the version policy](#the-amd-scheme-driver-version).

## Per-application switches that are not settings

A setting of this document says what the application gets. A switch of the other kind says which of the driver's
own defaults is taken back, for a comparison or for one program that a default harms. Those live in the
`Experiment` key, not in the `Graphics` key, and they can only ever subtract from the behaviour the lab checked:

| Key | Value |
|---|---|
| `HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<image>` | `Experiment`, `REG_SZ`, a comma-separated list of tokens |
| `HKLM\SOFTWARE\amdgpu-wddm\D3D12` | the same value for every program |

The program gets the profile's list and the machine-wide list together: a profile adds to the machine-wide
switches and does not replace them. A profile of exactly `none` gives that program no switch, the machine-wide
ones included. A value outside the syntax in either key gives no switch at all. In a lab build, the environment
variable `AMDGPU_WDDM_D3D12_EXPERIMENT` of the process replaces both keys. Each default has one token,
`<name>-off`. A token the driver does not know is ignored.
`driver/umd/d3d12/ddi-trace.h` lists every default and its token, and the D3D12 shell's README says what each one
does. Two of them a tester may need:

| Token | What it takes back |
|---|---|
| `deferred-replay-off` | Command-list recording goes to the engine on the recording thread again |
| `async-pso-off` | `CreatePipelineState` makes the engine's pipeline on the calling thread again, as before BD-158 |

Asynchronous pipeline compilation is on by default, for every application. With it on, the create DDI keeps every
check and every refusal on the calling thread and the engine's create (the DXIL to SPIR-V conversion and the
shader compile) runs on a pool of `logical processors - 1` threads, so a game that creates its pipelines from one
thread compiles them on every core. `tools/win/d3d12pso` measures the difference, and the shell writes the
counters of the pool (queued, run by a worker, run by the consumer, waits and their time, failures) to its log when
the device is destroyed.

The D3D11 shell has the same mechanism under `AMDGPU_WDDM_D3D11_EXPERIMENT`
(`driver/umd/dxvk/ddi-experiment.h`), with `wddm22-ddi-off` above.

## Log lines

Each shell writes its effective settings once per process, to `OutputDebugString` and to the `AMDGPU_WDDM_LOG`
sink. Each source shows after its value. The marker `not-applied` follows a set value that the shell does not
apply:

```
amdgpu-wddm settings: api=d3d12 app=game.exe FrameRateLimit=60/application VSync=unset Anisotropy=16/global
MaxFrameLatency=2/global PerformanceOverlay=unset RenderOnCpu=unset
```

The line above is one line in the log. An ignored value gives a line before it:

```
amdgpu-wddm settings: ignored FrameRateLimit=5 from the application key (accepted: 0, 20-300)
```

The engines give these lines at the info level, which the `AMDGPU_WDDM_LOG` sink receives:

| Line | Writer |
|---|---|
| `Effective configuration:` followed by `d3d11.samplerAnisotropy` or `dxvk.hud` | DXVK |
| `amdgpu_wddm_dxvk: HUD on (fps,frametimes,gpuload,api)` | DXVK fork, at device creation |
| `amdgpu_wddm_dxvk: HUD drawn into the first presented surface` | DXVK fork, at the first Present |
| `Sampler anisotropy forced to <n>` | vkd3d-proton fork, at device creation |

The KMD gives its lines with the prefix `driver version:`.

## How the engine options get to the engines

The shells pass the engine options through environment variables. That path exists in both engines, so the engine
ABI does not change. The D3D11 shell adds its options to `DXVK_CONFIG`, after a value that the process already has.
The D3D12 shell sets `VKD3D_SAMPLER_ANISOTROPY`.

Each shell sets the variable only for the engine call that creates the device, and then puts the previous value
back. A child process of the application does not get the variable. A lock keeps two device creations of one
process from mixing their values.

## The AMD-scheme driver version

BD-152 makes the INF version an installation-owned property. The release builder
maps product package `a.b.c.d` to Windows DriverVer `(a+40).b.c.d`. For example,
product `0.7.216.103` maps to `40.7.216.103`. The embedded KMD build can differ.
The manifest records the release name, product quad, INF date/quad, embedded KMD
build/ABI and SYS hash as separate identities. The INF writes our own
`ProductPackageVersion` value. It never writes the OS-owned `DriverVersion` value
through AddReg.

Each INF field must be less than 65535. A product major above 65494 is refused,
as are an invalid quad, date or suffix. PnP still compares rank first, then date,
then version. The +40 mapping does not override a worse rank or an older date.
BD-150's date/version supersedence gate remains in the release build.

The KMD no longer reads `ReportAmdDriverVersion`, visits video registry aliases
or rewrites version properties at adapter start. The GUI has no such switch.
On upgrade, installation must establish the new package binding. The installer compares
the bound INF hash/date/quad, OS version properties, product value and loaded SYS.
Only then does it remove our obsolete `Bc250DriverVersion` and
`Bc250ReportedDriverVersion` tags. A mismatch keeps the tags for diagnosis and
fails verification. No code restores `DriverVersion` from the old backup.

The previous implementation changed that OS property at runtime. BD-104 showed
that an Unreal Engine check could read it through a video-key alias. That result
explains the number policy, but does not permit the write. The Microsoft device
installation contract prohibits changing `DriverVersion`, `DriverDate` and the
other installation-owned properties in an adapter software key.

Sources: MicrosoftDocs/windows-driver-docs `110f60ea`,
[software key](https://learn.microsoft.com/windows-hardware/drivers/install/opening-a-device-s-software-key),
[DriverVer](https://learn.microsoft.com/windows-hardware/drivers/install/inf-driverver-directive),
and [driver selection](https://learn.microsoft.com/windows-hardware/drivers/install/overview-of-the-driver-selection-process).
Host tests cover identity mismatches, bounds and rollback ranking. The Ascent
on a fresh and an upgraded install is a separate lab acceptance test.

### What Unreal Engine checks

These facts come from the `Engine/Config/BaseHardware.ini` and `WindowsPlatformMisc.cpp` files of the Unreal Engine
branches 4.26 and 5.4, read on 2026-10-08.

| Branch | Default `r.DriverDetectionMethod` | Source of the version | AMD entries |
|---|---|---|---|
| 4.26 | 4 | `DriverVersion` of the `DeviceKey` | `<=22.19.662.4` for all RHIs, `<=26.20.13031.15006` for D3D12 |
| 5.4 | 5 | `DEVPKEY_Device_DriverVersion` through the SetupAPI | `<27.20.20913.2000` for D3D11, `DriverDate` `<2-19-2024` for D3D12 and Vulkan |

Unreal Engine 4.26 shows its warning only when an entry matches. It compares the two numbers as six unsigned
integers, right-aligned: `0.7.216.18` becomes `0.0.0.7.216.18` and `22.19.662.4` becomes `0.0.22.19.662.4`, so the
first field decides. The installed `0.7.216.18` is below both entries and the warning appears. The number
`40.7.216.18` is above both entries, and no entry matches. The Ascent showed this warning on unit A on 2026-10-07 (session 465), with the
recommended version `19.20.1`, which is the `SuggestedDriverVersion` of `[GPU_AMD]` for D3D12 in the 4.26 file.

Unreal Engine 4.26 also reads `Catalyst_Version`, `RadeonSoftwareEdition` and `RadeonSoftwareVersion` of the same
key for the text of its message. The decision uses the number of `DriverVersion`, so this driver does not write
those three values.

Unreal Engine 5.4 reads the device property through the SetupAPI. That property reads the `DriverVersion` value of
the class key, which is the key that the symbolic link of the video key leads to, so installation sets the number
that method 5 sees as well. Its D3D12 and Vulkan entries compare the driver date, and the INF date of this driver
passes them. The lab check records the property next to the registry values, to check this on unit A. Method 5
reads the `DeviceKey` only when the SetupAPI finds no adapter with the name of the D3D adapter.

A future entry that matches all versions from a value upward, with `>=`, would also match the AMD-scheme number.

## Files

| File | Part |
|---|---|
| `driver/umd/app-settings/app-settings-core.h` | names, ranges, precedence, registry and environment reads |
| `driver/umd/app-settings/app-settings.h` | log lines, sync override, frame limiter, engine options |
| `driver/umd/app-settings/app-settings-test.cpp` | host test, run by `tools/build/test-umd-app-settings.ps1` in the release and the lab flavour |
| `driver/umd/app-settings/flavour-strings-check.cpp` | compile check: the release object of `log_once` holds no lab line of `dxvk-profile.h`, the lab object holds them |
| `driver/umd/dxvk/ddi-adapter.cpp`, `ddi-present.cpp` | the D3D11 shell |
| `driver/umd/d3d12/adapter.cpp`, `native-tables.cpp` | the D3D12 shell |
| `driver/umd/router/router-policy.h`, `router.cpp` | `RenderOnCpu` |
| `tools/release/installer/version-identity.ps1` | package version identity |
| `tools/release/test-version-identity.ps1` | version and rollback host tests |

The build scripts of the three UMDs run `test-umd-app-settings.ps1`. `tools/quality/quick.ps1` runs it and the
driver version test as the checks `umd-app-settings` and `driver-version`.
