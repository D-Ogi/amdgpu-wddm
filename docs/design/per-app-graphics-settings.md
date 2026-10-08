# Per-application graphics settings

Status: implemented on bc250-win branch `umd/per-app-graphics-settings` and on branch
`amdgpu-wddm/per-app-graphics-settings` of the DXVK and vkd3d-proton forks. The host gates pass on the
development PC (2026-10-08). No lab trial on unit A has run yet.

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

When no source holds a valid value, the driver leaves the setting to the application. An environment value must be
a decimal number of 1 to 10 digits, with no sign, no space and no `0x`. The driver ignores a value outside its
range, a registry value that is not a `REG_DWORD`, and an environment value that is not a number. For each one it
writes one log line and then reads the next source.

The two shells read the settings once per process, at the first device creation. A change takes effect at the next
start of the application. The router reads `RenderOnCpu` at each `OpenAdapter` call, as it reads its `AppRouter`
key. The KMD reads `ReportAmdDriverVersion` at each adapter start.

## The settings

| Value | Environment variable | Accepted | D3D11 | D3D12 |
|---|---|---|---|---|
| `FrameRateLimit` | `AMDGPU_WDDM_FRAME_RATE_LIMIT` | 0, 20-300 | works | works |
| `VSync` | `AMDGPU_WDDM_VSYNC` | 0, 1 | works | works |
| `Anisotropy` | `AMDGPU_WDDM_ANISOTROPY` | 1, 2, 4, 8, 16 | works | works |
| `MaxFrameLatency` | `AMDGPU_WDDM_MAX_FRAME_LATENCY` | 1-3 | works | not applied |
| `PerformanceOverlay` | `AMDGPU_WDDM_PERFORMANCE_OVERLAY` | 0, 1 | works | not applied |
| `RenderOnCpu` | `AMDGPU_WDDM_RENDER_ON_CPU` | 0, 1 | works, also for D3D10 | no CPU route |
| `ReportAmdDriverVersion` | none | 0, 1 | global key only | global key only |

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
interface to an FL12 adapter, with the WDDM 2.2 device table and the DXGI 1.6.1 table that go with it
(`driver/umd/dxvk/ddi-wddm22.h`). `DeviceOwner::full_present_callback` holds the answer for the device, and the
Present path passes the whole override while it is true.

Below that interface the shell passes `VSync` 0 only, and does `VSync` 1 with its own waits. Two cases keep it
there: an FL11 adapter, which offers the D3D11.1 interface alone, and the process switch
`wddm22-ddi-off` of `AMDGPU_WDDM_D3D11_EXPERIMENT` (`driver/umd/dxvk/ddi-experiment.h`), which withholds the
WDDM 2.2 offer for a comparison. Then `d3d11.dll` 10.0.22621 gives `PresentCB_PreWDDM2_2`, which copies only the
older, shorter structure into its own `DXGIDDICB_PRESENT` (BD-099). On x64 the copy holds
`SyncIntervalOverrideValid`, which is in the tail padding of the older structure, but not `SyncIntervalOverride`.
Thus each override arrives as interval 0. An override for `VSync` 1 gave 873-928 frames/s for the intervals 1
and 2. On x86 (WOW64) the copy holds neither field, and the override has no effect.

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

The value is the number of Presents that the D3D11 shell lets wait for the GPU. After each Present the shell waits
until the Present that is N places back has completed. The wait uses the present fence of the shell, as the swap
chain of DXVK does for its own frame latency. Only a lost device makes the Present fail.

The D3D12 shell does not apply this value. A D3D12 application controls its frame latency with its own fences and
with `SetMaximumFrameLatency` of its swap chain.

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

D3D12 has no CPU route. No CPU route through lavapipe exists in any repository of this project.

### ReportAmdDriverVersion

The KMD reads this value from the global key only, at each adapter start. The value 1 makes the driver report
an AMD-scheme version number to programs that check the driver version. The section
[The AMD-scheme driver version](#the-amd-scheme-driver-version) gives the scheme and the reason.

## Log lines

Each shell writes its effective settings once per process, to `OutputDebugString` and to the `AMDGPU_WDDM_LOG`
sink. Each source shows after its value. The marker `not-applied` follows a set value that the shell does not
apply:

```
amdgpu-wddm settings: api=d3d12 app=game.exe FrameRateLimit=60/application VSync=unset Anisotropy=16/global
MaxFrameLatency=2/global/not-applied PerformanceOverlay=unset RenderOnCpu=unset
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

### The problem

Some games compare the driver version of the adapter with a minimum version for each GPU vendor. For
vendor `0x1002` they expect AMD numbering. The version `0.7.216.18` of this driver reads as a very old AMD driver.
The game then shows a warning about known driver problems, and the player must close it at each start.

### The scheme

The reported number is `(a + 40).b.c.d` for the installed number `a.b.c.d`. The installed `0.7.216.18` is reported
as `40.7.216.18`. AMD's own driver 24.3.1 of March 2024 has the number `31.0.24027.1012`, as a comment of the
Unreal Engine 5.4 `BaseHardware.ini` records. The first field 40 is above that by a margin of several years. The
other three fields stay, so a support report still identifies the build of this driver.

### The mechanism

`EnumDisplayDevices` gives the adapter's `Control\Video` key as the `DeviceKey` of each adapter. Unreal Engine
4 reads the `DriverVersion` value of that key. This key is also the software key that dxgkrnl gives the KMD in
`DXGK_DEVICE_INFO.DeviceRegistryPath`.

At each adapter start the KMD reads `ReportAmdDriverVersion` and changes three values of the software key:

| Value | Content |
|---|---|
| `DriverVersion` | the AMD-scheme number while the setting is 1, the installed number otherwise |
| `Bc250DriverVersion` | the installed number, while the setting is 1 |
| `Bc250ReportedDriverVersion` | the number that the KMD wrote, while the setting is 1 |

When the setting goes back to 0, the next start writes the installed number back and deletes the two backup values.
A new driver installation writes its own number into `DriverVersion`. With the setting on, the next start reports
the new number in the AMD scheme. With the setting off, the KMD keeps the new number and deletes only the backups.
The KMD writes only to a key whose path contains `\Control\Video\`.

The PnP driver key under `Control\Class` and the driver store keep the INF number. The installer and the control
application read the version from there, so they show the installed number. The code is `driver/kmd/driver_version.c`,
and `driver/kmd/driver_version.h` holds the decisions that the host test checks.

### What Unreal Engine checks

These facts come from the `Engine/Config/BaseHardware.ini` and `WindowsPlatformMisc.cpp` files of the Unreal Engine
branches 4.26 and 5.4, read on 2026-10-08.

| Branch | Default `r.DriverDetectionMethod` | Source of the version | AMD entries |
|---|---|---|---|
| 4.26 | 4 | `DriverVersion` of the `DeviceKey` | `<=22.19.662.4` for all RHIs, `<=26.20.13031.15006` for D3D12 |
| 5.4 | 5 | `DEVPKEY_Device_DriverVersion` through the SetupAPI | `<27.20.20913.2000` for D3D11, `DriverDate` `<2-19-2024` for D3D12 and Vulkan |

Unreal Engine 4.26 shows its warning only when an entry matches. The number `40.7.216.18` matches neither 4.26
entry, so a 4.26 game does not show it with the setting on. The Ascent showed this warning on unit A on 2026-10-07.

Unreal Engine 5.4 reads the PnP driver key, which this setting does not change. Its D3D12 and Vulkan entries
compare the driver date, and the INF date `10/07/2026` passes them. A 5.4 game on D3D11 still sees `0.7.216.18`
and shows its warning. The method falls back to the `DeviceKey` only when the SetupAPI finds no adapter with the
name of the D3D adapter.

A future entry that matches all versions from a value upward, with `>=`, would also match the AMD-scheme number.

## Files

| File | Part |
|---|---|
| `driver/umd/app-settings/app-settings-core.h` | names, ranges, precedence, registry and environment reads |
| `driver/umd/app-settings/app-settings.h` | log lines, sync override, frame limiter, engine options |
| `driver/umd/app-settings/app-settings-test.cpp` | host test, run by `tools/build/test-umd-app-settings.ps1` |
| `driver/umd/dxvk/ddi-adapter.cpp`, `ddi-present.cpp` | the D3D11 shell |
| `driver/umd/d3d12/adapter.cpp`, `native-tables.cpp` | the D3D12 shell |
| `driver/umd/router/router-policy.h`, `router.cpp` | `RenderOnCpu` |
| `driver/kmd/driver_version.c`, `driver_version.h` | `ReportAmdDriverVersion` |
| `driver/kmd/test/driver_version_test.c` | host test, run by `driver/kmd/test/run_driver_version.ps1` |

The build scripts of the three UMDs run `test-umd-app-settings.ps1`. `tools/quality/quick.ps1` runs it and the
driver version test as the checks `umd-app-settings` and `driver-version`.
