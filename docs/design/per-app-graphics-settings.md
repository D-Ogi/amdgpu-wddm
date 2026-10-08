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
| `VSync` | `AMDGPU_WDDM_VSYNC` | 0, 1 | implemented, lab check open | implemented, lab check open |
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
interval 1 (`DXGI_DDI_FLIP_INTERVAL_ONE`). The shells set `SyncIntervalOverrideValid` and `SyncIntervalOverride` in
the arguments of the Present callback. The D3D11 shell fills `DXGIDDICB_PRESENT`, and the D3D12 shell fills
`D3D12DDI_PRESENT_0051`.

The WDK declares the two `DXGIDDICB_PRESENT` fields for the WDDM 2.2.2 interface and later. The D3D11 shell reports
an older interface to the runtime. The lab must show whether DXGI reads the override from this shell. With
`VSync` 1 the frame rate of an uncapped client must drop to the refresh rate of the monitor.

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

`EnumDisplayDevices` gives the adapter's video key as the `DeviceKey` of each adapter:

```
HKLM\SYSTEM\CurrentControlSet\Control\Video\{VideoID}\0000
```

Unreal Engine 4 reads the `DriverVersion` value of that key. This key is not the one that dxgkrnl gives the KMD in
`DXGK_DEVICE_INFO.DeviceRegistryPath`: on unit A that one is the class key
`Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0000`. The first version of this code wrote only to
`DeviceRegistryPath` and refused that key, so the number never reached Unreal Engine (b23 lab 489).

The KMD finds the video keys of its adapter in two steps:

1. It opens the adapter's hardware key (`IoOpenDeviceRegistryKey` with `PLUGPLAY_REGKEY_DEVICE`) and reads the
   `VideoID` value, which holds the GUID of the video keys of this adapter.
2. It opens `Control\Video\{VideoID}` and visits each subkey whose name is four decimal digits. One adapter can have
   more than one such key, and `HKLM\HARDWARE\DEVICEMAP\VIDEO` names the same keys as `\Device\Video<n>`. The
   development PC showed four of them for one adapter. The KMD visits at most 16 and says so in the log if there
   are more.

Each numbered video key is a registry symbolic link: it holds a `REG_LINK` value `SymbolicLinkValue` whose target is
the class key of the adapter, `Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\<nnnn>`. An open that
does not ask for `OBJ_OPENLINK` follows the link, so the KMD writes the class key through the video path. Three
results follow:

- The second and later video keys of the adapter find the number in place and write nothing.
- The write also changes the version that Device Manager gives for the adapter.
- The write changes the SetupAPI property `DEVPKEY_Device_DriverVersion`, because that property reads the
  `DriverVersion` value of the same class key. The Microsoft page of that property names its registry value
  `REGSTR_VAL_DRIVERVERSION`, `DriverVersion`
  (`windows-driver-docs-pr/install/devpkey-device-driverversion.md`, staging `110f60ea`).

The driver store keeps the INF number, and so does `Bc250DriverVersion`. Because the key that the write reaches is
not the key that the path names, the KMD guards both: it opens only a path below `Control\Video`, and it reads the
name of the key that the open gives back and writes only when that name is a video key or one adapter key of
the adapter class `{4d36e968-e325-11ce-bfc1-08002be10318}`.

The two facts above were measured on the development PC on 2026-10-08, read-only, with
`RegOpenKeyEx(REG_OPTION_OPEN_LINK)` and `RegQueryValueEx("SymbolicLinkValue")` on the video keys of its graphics
adapter, and by comparing `DEVPKEY_Device_DriverVersion` of the devnode with the `DriverVersion` value of the class
key that the link named. All four numbered video keys of that adapter named the same class key.

At each adapter start the KMD reads `ReportAmdDriverVersion` and changes three values of each video key:

| Value | Content |
|---|---|
| `DriverVersion` | the AMD-scheme number while the setting is 1, the installed number otherwise |
| `Bc250DriverVersion` | the installed number, while the setting is 1 |
| `Bc250ReportedDriverVersion` | the number that the KMD wrote, while the setting is 1 |

When the setting goes back to 0, the next start writes the installed number back and deletes the two backup values.
A new driver installation writes its own number into `DriverVersion`. With the setting on, the next start reports
the new number in the AMD scheme. With the setting off, the KMD keeps the new number and deletes only the backups.
The KMD opens only a path that contains `\Control\Video\`, and it writes only when the key behind that path names a
video key or one adapter key of the adapter class.

The driver store keeps the INF number, and `Bc250DriverVersion` holds it next to the reported number, so a support
report can give both. The code is `driver/kmd/driver_version.c`, and `driver/kmd/driver_version.h` holds the
decisions and the path of the key, which the host test checks.

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
the class key, which is the key that the symbolic link of the video key leads to, so the setting changes the number
that method 5 sees as well. Its D3D12 and Vulkan entries compare the driver date, and the INF date of this driver
passes them. The lab check records the property next to the registry values, to check this on unit A. Method 5
reads the `DeviceKey` only when the SetupAPI finds no adapter with the name of the D3D adapter.

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
