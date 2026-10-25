# Open ADL subset for AMD AGS

AMD GPU Services (AGS) is a library that many games ship next to their executable (`amd_ags_x64.dll`). Its
`agsInitialize` function loads the AMD Display Library (ADL) by name. Without ADL, `agsInitialize` returns
`AGS_NO_AMD_DRIVER_INSTALLED` (6). Some programs stop on that result. NVIDIA's OMMSample (through NRI) is one:
on our driver it stopped with "Failed to initialize AMDAGS: 6" before any feature check (lab run of
2026-10-24).

`driver/umd/adl` is our own ADL library. It implements the subset of ADL that AGS 6.3 needs, and it reports
only facts that the operating system gives for adapters that run our driver. With it, `agsInitialize` succeeds.
`agsDriverExtensionsDX12_CreateDevice` then makes a plain D3D12 device with no extension bit, because our driver
has no AMD driver extension.

## File names

| Process | File name | AGS library | Search order of AGS 6.3 |
|---|---|---|---|
| 64-bit | `atiadlxx.dll` | `amd_ags_x64.dll` | `atiadlxx.dll`, then `atiadlxy.dll` |
| 32-bit | `atiadlxy.dll` | `amd_ags_x86.dll` | `atiadlxx.dll`, then `atiadlxy.dll` |

Both AGS libraries try the same two names in the same order (disassembly of the two 6.3.0 binaries). A 32-bit
process on 64-bit Windows reads `SysWOW64` for a system library name. For AGS 6.3 either name works there. The
loader of the ADL SDK samples tries `atiadlxy.dll` for a 32-bit caller after `atiadlxx.dll` fails, and AMD's own
driver puts the 32-bit library in `SysWOW64` under that name. Thus the 32-bit file is `atiadlxy.dll`, and the
64-bit file in `System32` is `atiadlxx.dll`.

## The AGS 6.3 contract

The facts in this section come from the AGS 6.3.0 binaries: `amd_ags_x64.dll` (the copy that OMMSample ships) and
`amd_ags_x86.dll`, with the SHA256 values in the table below. The host tests below check each of them.

| File | SHA256 |
|---|---|
| `amd_ags_x64.dll` 6.3.0 | `b27b070ca39dc37984fb3dde0187515d36094e72cb881d7a99bd1055befd8da2` |
| `amd_ags_x86.dll` 6.3.0 | `d7a0122abd8be157ab838a88368bd3358c8483c6b23083a28832c19dc7698bf6` |

1. `agsInitialize` loads the ADL library. If neither name loads, the result is 6.
2. It gets the addresses of nine functions. If one of them is missing, the result is `AGS_LEGACY_DRIVER` (5):
   `ADL2_Main_Control_Create`, `ADL2_Main_Control_Destroy`, `ADL2_Graphics_Versions_Get`,
   `ADL2_Adapter_NumberOfAdapters_Get`, `ADL2_Adapter_AdapterInfoX2_Get`, `ADL2_Adapter_MemoryInfo_Get`,
   `ADL2_Display_DisplayMapConfig_Get`, `ADL2_Display_SLSMapIndex_Get` and `ADL2_Display_Modes_Get`.
3. It gets the addresses of other functions too. It calls such a function only when the address is not null.
   The subset does not export them, for example `ADL2_Adapter_AdapterInfoX4_Get`, `ADL2_Adapter_MemoryInfo2_Get`,
   `ADL2_Adapter_Graphic_Core_Info_Get`, the Overdrive functions and `ADL2_FreeSync_Cap`.
4. It creates an ADL context with `ADL2_Main_ControlX3_Create` when the library has it, else with
   `ADL2_Main_Control_Create`. The allocation callback is its own. A failure gives `AGS_ADL_FAILURE` (8).
5. It reads the adapter list (`AdapterInfo`, 0x624 bytes for each entry). It keeps the entries with `iExist`
   set. It reads the vendor, device and revision from the `VEN_`, `DEV_` and `REV_` parts of `strUDID`. It makes
   one GPU from the entries with the same bus and device number. It finds the primary GPU when `strDisplayName`
   of an entry is the name of the primary display device.
6. For each GPU it reads the memory size (`ADL2_Adapter_MemoryInfo_Get`) and the version strings (X3, X2 or the
   base function). It calls `ADL2_Display_DisplayInfo_Get` without an address check, so the subset must export it.
   For each connected display it reads the current mode (`ADL2_Display_Modes_Get`).
7. It reads the display map (`ADL2_Display_DisplayMapConfig_Get`). Only a map with more than one target makes it
   ask for an Eyefinity (SLS) group. It calls `ADL2_Display_SLSMapConfig_Get` without an address check, but only
   after `ADL2_Display_SLSMapIndex_Get` gave a group.
8. The `adlAdapterIndex` of a GPU and of a display is the position of the entry in the adapter list. The subset
   gives each entry its position as `iAdapterIndex`.

The ADL functions use the C calling convention of the platform (`__cdecl` on x86). The x86 AGS binary removes the
arguments itself after each call. The allocation callback is `__stdcall`.

## What the library reports

The library finds its adapters in two steps, and both are needed:

1. SetupAPI lists the display adapters whose service is our kernel-mode driver (`bc250kmd`).
2. `D3DKMTEnumAdapters2` gives the running adapters. The library matches them to step 1 by PCI bus, device and
   function. The match gives the LUID and the segment sizes.

An adapter of another driver is never reported, and nothing in the library names a board or a device ID.
`EnumDisplayDevices` and `QueryDisplayConfig` give the display entries and the displays.

| Function | Answer |
|---|---|
| `ADL2_Adapter_NumberOfAdapters_Get` | Number of entries: one for each display source of each adapter, and one for an adapter with no source |
| `ADL2_Adapter_AdapterInfoX2_Get` | Entries from the caller's allocation callback. `strUDID` is the device instance ID, with `_` in place of `\`. The vendor is the decimal number 1002 for an AMD device, as in AMD's library |
| `ADL2_Adapter_MemoryInfo_Get` | Dedicated VRAM of the adapter (`D3DKMT_SEGMENTSIZEINFO`). Memory type empty, bandwidth 0 |
| `ADL2_Graphics_Versions_Get`, `ADL2_Graphics_VersionsX2_Get` | The driver version of the device node (SetupAPI `DriverVersion`). Catalyst version, Crimson version and web link are empty. `ADL_ERR_NOT_SUPPORTED` when no adapter runs our driver |
| `ADL2_Graphics_VersionsX3_Get` | The same strings for one adapter index |
| `ADL2_Display_DisplayInfo_Get` | One record for each active display path of the adapter: monitor name, EDID manufacturer ID, display type, output and connector type |
| `ADL2_Display_Modes_Get` | The current mode of each display: position, size, refresh rate and rotation |
| `ADL2_Display_DisplayMapConfig_Get` | One map for each display source, with its targets. A clone gives one map with two targets |
| `ADL2_Display_SLSMapIndex_Get` | Index -1 and `ADL_ERR_NOT_SUPPORTED`: there is no Eyefinity group |
| `ADL2_Display_SLSMapConfig_Get` | `ADL_ERR_NOT_SUPPORTED`. Every output is cleared |
| `ADL2_Main_Control_Refresh` | A new snapshot of the facts above |

The library exports 31 functions: the 17 `ADL2_*` functions and 14 `ADL_*` functions without a context, for
older callers (`driver/umd/adl/atiadl.def`). It does not export a function that it cannot answer truthfully.

`radeonSoftwareVersion` in `AGSGPUInfo` is the Crimson version string of `ADL2_Graphics_VersionsX2_Get`. The
library gives it empty, so `radeonSoftwareVersion` is an empty string. `agsCheckDriverVersion` then returns
`AGS_SOFTWAREVERSIONCHECK_UNDEFINED` (2). `driverVersion` is our driver version, for example `0.7.216.104`. AGS
reports `asicFamily` as unknown and `numCUs`, the clocks and the bandwidth as 0, because the library does not
export the functions that give them.

## Contexts and memory

- Each context holds its own snapshot. `ADL2_Main_Control_Destroy` frees the context and the snapshot.
- Every array that a function returns comes from the allocation callback of the context, and the caller frees it.
  This is the ADL contract.
- The `ADL_*` functions use one shared context with a reference count. `ADL_Main_Control_Create` and
  `ADL_Main_Control_Destroy` change the count.
- One lock protects context state. Discovery and caller allocation callbacks run outside that lock. Array queries
  copy the snapshot and callback while locked, then use that private copy until the query returns. A callback can
  query, refresh or destroy its context without a recursive lock or use of freed context memory.
- `DisplayMapConfig_Get` needs two caller allocations. If the second fails, it returns an error with the first
  array and its count still set. The caller owns that array and must free it even on error. ADL has no caller
  deallocation callback, so the library must not free it with a different allocator.
- When the process unloads the library, the library frees the contexts that are still open.

## Build and gates

`tools/build/build-umd-adl.ps1 -Arch x64|x86` builds the library with clang-cl 23.1.4 (`-Compiler msvc` needs
`-MsvcReason`). The C runtime is static, so the library needs no Visual C++ redistributable. The script stops on
these failures:

- The image is not of the requested architecture.
- The exports differ from `atiadl.def`. Thus a test function cannot ship.
- The library imports a DLL other than KERNEL32, USER32, GDI32, SETUPAPI and ADVAPI32.
- The unit test (`driver/umd/adl/tests/adl-unit-test.cpp`) fails. It calls every export on an injected adapter
  list, on an empty list and with bad arguments. It counts the callback allocations and the context blocks, and
  it makes 1000 Create and Destroy cycles. On x86 it measures the stack bytes that each export removes, which
  must be 0 (`__cdecl`).
- The compile-only SDK gate compares 15 structure sizes and alignments, and selected offsets, against the public
  `adl_structures.h` (`-AdlSdk` selects its include directory). The callback tests have a five-second bound and
  cover queries, refresh and destruction from an allocator, plus failure of the second map allocation.

`tools/build/test-umd-adl-ags.ps1` then runs the real AGS 6.3 library of the requested architecture with
`ags-client.exe` (`driver/umd/adl/tests/ags-client.cpp`):

| Case | ADL library | Expected |
|---|---|---|
| shipped | the shipped library | `agsInitialize` 0 with and without GPU information. 100 rounds with no growth of live heap bytes or handles. A D3D12 device through AGS with extension bits 0, one clear and a readback |
| inject | the test build with two AMD GPUs and three displays | the same, through the AGS parsers of adapters, displays and modes |
| host | the test build that takes the GPU of the host for ours (`-TestService`) | the same, on real operating system data |
| none | none | `agsInitialize` 6 |
| no-modes | the shipped library without `ADL2_Display_Modes_Get` | `agsInitialize` 5 |

`tools/quality/quick.ps1` runs both scripts for x64 and x86 (gate `umd-adl`) when the AGS SDK is in
`<BC250_ROOT>\ref\AGS_SDK-v6.3.0`. The AGS SDK is not in the repository (`THIRD-PARTY.md`).

The host run of 2026-10-25 (x64 and x86, 13 of 13 runs each as expected) is in
[evidence/windows/2026-10-25-adl-subset-host](../../evidence/windows/2026-10-25-adl-subset-host/RESULT.md).

## Packaging proposal

This part is not implemented. The INF and the installer do not change in this step.

The library must be in a directory that `LoadLibrary` searches for a bare name, that is `System32` and
`SysWOW64`. A `CopyFiles` directive to DIRID 11 is not isolated. InfVerif reports it as error 1322 in `/w` mode
(driver package isolation), so the package does not use it. The display INF has a registered mechanism instead:
the `CopyToVmWhenNewer` and `CopyToVmWhenNewerWow64` subkeys of the adapter key. Windows copies each listed
DriverStore file to `System32` or `SysWOW64` (WDK documentation, "Container support for non-DX APIs").

Proposed package members and registrations:

| DriverStore member | Copied to | Registration in the adapter key |
|---|---|---|
| `adl\atiadlxx.dll` (x64) | `System32\atiadlxx.dll` | `CopyToVmWhenNewer`, value `AmdgpuAdl` = `adl\atiadlxx.dll`, `atiadlxx.dll` |
| `wow64\adl\atiadlxy32.dll` (x86) | `SysWOW64\atiadlxy.dll` | `CopyToVmWhenNewerWow64`, value `AmdgpuAdl` = `wow64\adl\atiadlxy32.dll`, `atiadlxy.dll` |

The x86 member name ends in `32` and stays below `wow64`, as `driverstore-package.md` requires. The copy gives it
the ADL name.

Coexistence:

- Only AMD's driver installs a file with these names. "When newer" compares the file version first. Our version
  (0.7.x) is lower than the version that we expect in AMD's library (not checked), so Windows does not replace
  AMD's file with ours. AMD's library then answers for AMD's adapters, and our adapter is not in its list. On a
  system with both drivers, AGS then sees only the AMD adapter.
- `CopyToVmOverwrite` would replace AMD's file at each start of our adapter. The proposal does not use it.
- The library needs a version resource for the "newer" comparison. `tools/build/versioninfo.json` and
  `version-stamp.ps1` must stamp it like the other user-mode files. The library has no version resource today.

Open questions for the packaging step, to be measured on the lab:

1. Does Windows copy the files on the host as well as into a virtual machine, and at which time?
2. Does Windows remove the copies when the driver is removed? The uninstall path of the installer must check this.
3. Does the HLK INF test accept the two subkeys in the adapter key of our INF?

## Limits

- The library does not report Eyefinity, FreeSync, HDR, overdrive, clocks, compute units or an AMD software
  release. A caller that needs one of these sees the function as absent or gets `ADL_ERR_NOT_SUPPORTED`.
- The facts are a snapshot. The library gives a display change after `Create` only after
  `ADL2_Main_Control_Refresh` or in a new context.
- A program that reads `driverVersion` and expects AMD's numbering sees our version instead.
