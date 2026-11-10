# Open ADL subset for AMD AGS

AMD GPU Services (AGS) is a library that many games ship next to their executable (`amd_ags_x64.dll`). Its
`agsInitialize` function loads the AMD Display Library (ADL) by name. Without ADL, `agsInitialize` returns
`AGS_NO_AMD_DRIVER_INSTALLED` (6). Some programs stop on that result. NVIDIA's OMMSample (through NRI) is one:
on our driver it stopped with "Failed to initialize AMDAGS: 6" before any feature check (lab run of
2026-10-24).

`driver/umd/adl` is our own ADL library. It implements the subset of ADL that AGS 6.3 needs, and it reports
only facts that the operating system gives for adapters that run our driver. The KMD must also reject foreign
private-capability query extents. ADL alone does not make `agsInitialize` safe with our driver.
`agsDriverExtensionsDX12_CreateDevice` then makes a plain D3D12 device with no extension bit, because our driver
has no AMD driver extension.

## Private KMD query compatibility

AGS 6.3 queries `KMTQAITYPE_UMDRIVERPRIVATE` itself while it builds GPU information. This is a separate
protocol from the public ADL structures. It first asks for 608 bytes, then for 17248 after
`STATUS_BUFFER_TOO_SMALL`. A successful reply makes it interpret offset 8 as an adapter count. Our BC25
reply has the byte count 1472 at that offset. Returning that reply caused a stack overwrite in a host
replay with the real AGS DLL, in both x64 and x86.

The KMD now rejects output capacities above `BC250_SCANOUT_CAPS_TOTAL` (1520) with
`STATUS_INVALID_PARAMETER`, before any write or firmware query. It preserves the legacy 1472-byte
reply, the 1496-byte identity reply, the 1520-byte scan-out reply and intermediate partial-trailer
queries. Smaller capacities still get `STATUS_BUFFER_TOO_SMALL`. The real AGS DLL continues through
ADL after the rejected 17248-byte retry and returns `AGS_SUCCESS` in the controlled host test.

This is an extent check, not caller authentication or a complete private-protocol namespace. It does
not identify an unrelated protocol with the same capacity. A future request-header protocol needs a
versioned migration that preserves the older UMDs kept in DriverStore.

Current readers remain compatible: the RADV WDDM2 BC25 path and `compute/hip/bc250hsa/kmt_device.c`
ask for 1472 bytes. D3D12 `adapter-contract.h`, D3D11 `dxvk/adapter-identity.h` and the router's
`ReadScanoutCaps` ask for 1520. The D3D12 KMT probe forwards that same contract. RADV's separate AMD
private path asks for 3008 and 17248 only after its BC25 identification fails. These are foreign
formats and must not receive a BC25 response.

The AGS client has test-only `--kmt-fixture` and `--kmt-legacy` modes. They replace two dynamic KMT
lookups in the client's loaded AGS image, without changing a system DLL or driver. Both use the
injected AMD snapshot. The first uses the KMD extent rule and passes. The second replays the old
unbounded reply and must be run under a debugger because it crashes. `--guard-allocations` adds a
no-access page after each callback allocation, allowing up to 15 alignment bytes. The host test
also checks the returned AMD identities, display count and VRAM, rather than only the exit status.

Hardware confirmation of the new KMD remains a separate lab gate. Host replay does not replace it.

## Library names

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
   `ADL2_Adapter_Graphic_Core_Info_Get`, the `ADL2_Overdrive*` functions and `ADL2_FreeSync_Cap`. AGS does not
   need them, so the presence rule (below) does not apply to them.
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

| `ADL_Adapter_ID_Get` | A stable ID for each GPU, see "Presence exports" |

The library exports 43 functions (`driver/umd/adl/atiadl.def`): the 17 `ADL2_*` functions, 15 `ADL_*` functions
without a context for older callers, and the 11 presence exports below. BD-351 added `ADL_Adapter_ID_Get` and the
11 presence exports for one client.

### Presence exports

The rule: the library exports a function that it cannot answer only when a known client needs the name. A client
needs the name when it does not use the library at all if the address of the name is null. Each such function:

- Returns `ADL_ERR_NOT_SUPPORTED` (-8) when the arguments are correct. It never invents a value.
- Clears its outputs on every path. It keeps the input fields of an output structure: `iSize`, and `iSpeedType` of
  `ADLFanSpeedValue`. A pointer output gets null, a count gets 0.
- Has no side effect: it allocates nothing, calls no callback and changes no context.
- Checks its arguments in this sequence: a null output pointer gives `ADL_ERR_NULL_POINTER` (-9), no context gives
  `ADL_ERR_NOT_INIT` (-2), an adapter index that is not in the snapshot gives `ADL_ERR_INVALID_ADL_IDX` (-5).
- Uses the context rules of "Contexts and memory". AMD's library allocates the output of
  `ADL_Adapter_Crossfire_Caps` and `ADL_ApplicationProfiles_ProfileOfAnApplication_Search` through the callback,
  and `ADL_ApplicationProfiles_User_Load` loads data into the context. Thus these three need a context of the
  calling module. The others use the newest context when the caller has none.

A function with a backing in the library is not in this list. A new backing moves a function out of the list and
into the table above. A presence export is never added for a client that only tests a name and continues without it.

The known client is the SystemInfo scanner of 3DMark (`FMSIScan.exe` 5.93, BD-351). Its ADL module gets the
addresses of 16 `ADL_*` names and calls `ADL_Main_Control_Create` only if all 16 addresses are not null. Without
the 12 names below (`ADL_Adapter_ID_Get` and the 11 presence exports), the scanner reports no ADL adapter. Each call site in the scanner, in the CPUID library
(`cpuidsdk.dll`) and in GPU-Z checks the result: on an error it clears its own copy or skips the step.

| Function | Client | Answer |
|---|---|---|
| `ADL_Adapter_ID_Get` | FMSIScan | `ADL_OK` and the adapter ID (below) |
| `ADL_Adapter_Crossfire_Caps` | FMSIScan (address only) | `ADL_ERR_NOT_SUPPORTED`: preferred 0, count 0, array null |
| `ADL_Adapter_Crossfire_Get` | FMSIScan (address only) | `ADL_ERR_NOT_SUPPORTED`: `ADLCrossfireInfo` cleared |
| `ADL_Display_MVPUStatus_Get` | FMSIScan | `ADL_ERR_NOT_SUPPORTED`: `ADLMVPUStatus` cleared |
| `ADL_Overdrive5_CurrentActivity_Get` | FMSIScan, CPUID library | `ADL_ERR_NOT_SUPPORTED`: `ADLPMActivity` cleared |
| `ADL_Overdrive5_ThermalDevices_Enum` | FMSIScan (address only) | `ADL_ERR_NOT_SUPPORTED`: `ADLThermalControllerInfo` cleared |
| `ADL_Overdrive5_Temperature_Get` | FMSIScan, CPUID library | `ADL_ERR_NOT_SUPPORTED`: `ADLTemperature` cleared |
| `ADL_Overdrive5_FanSpeedInfo_Get` | FMSIScan, CPUID library | `ADL_ERR_NOT_SUPPORTED`: `ADLFanSpeedInfo` cleared |
| `ADL_Overdrive5_FanSpeed_Get` | FMSIScan, CPUID library | `ADL_ERR_NOT_SUPPORTED`: `ADLFanSpeedValue` cleared |
| `ADL_Overdrive5_ODParameters_Get` | FMSIScan, CPUID library | `ADL_ERR_NOT_SUPPORTED`: `ADLODParameters` cleared |
| `ADL_ApplicationProfiles_User_Load` | FMSIScan | `ADL_ERR_NOT_SUPPORTED` |
| `ADL_ApplicationProfiles_ProfileOfAnApplication_Search` | FMSIScan | `ADL_ERR_NOT_SUPPORTED`: profile pointer null |

"Address only" means that the scanner tests the address and never calls the function.

`ADL_Adapter_ID_Get` has a backing, so it gives `ADL_OK`. The SDK says that the adapter ID "is a unique value and
will be used to determine what other controllers share the same adapter" and that zero means that the adapter is
not AMD. The library gives:

- The same ID for all entries of one GPU, and a different ID for each GPU. The adapter index is the index that
  `AdapterInfo` gives.
- A value from the PnP device instance ID of the GPU (31-bit FNV-1a, never 0). Thus the ID is the same in each
  process and after `Refresh`. If two GPUs get the same value, the library adds 1 to the value of the later GPU
  until the value is unique (after 0x7fffffff it continues at 1).
- 0 for a GPU whose vendor is not AMD.

The library has no Overdrive backing. The kernel-mode driver can report the GPU temperature and the clocks
(`bc250kmd_cli clock read`). A backing through a private KMD query is a separate change: it needs a query contract
and its tests.

`radeonSoftwareVersion` in `AGSGPUInfo` is the Crimson version string of `ADL2_Graphics_VersionsX2_Get`. The
library gives it empty, so `radeonSoftwareVersion` is an empty string. `agsCheckDriverVersion` then returns
`AGS_SOFTWAREVERSIONCHECK_UNDEFINED` (2). `driverVersion` is our driver version, for example `0.7.216.104`. AGS
reports `asicFamily` as unknown and `numCUs`, the clocks and the bandwidth as 0, because the library does not
export the functions that give them.

## Contexts and memory

- Each context holds its own snapshot. `ADL2_Main_Control_Destroy` frees the context and the snapshot.
- The rule for memory: an export that returns memory either fills a caller buffer of a size that the caller gives,
  or allocates through an allocation callback that the same caller gave for itself. It never uses an allocator that
  a different module chose. The caller frees what the callback gave. This is the ADL contract, applied to each
  caller. Keep this rule for every shim that returns memory to a caller.
- The `ADL_*` functions (and the `ADL2_*` functions with a null handle) use an implicit context. There is one
  implicit context for each calling module, with a reference count. The library finds the calling module from the
  return address of the export. The key of a module is its base address, its `SizeOfImage` and the `TimeDateStamp`
  of its PE header. `ADL_Main_Control_Create` and `ADL_Main_Control_Destroy` change the count of the context of that
  module. Thus each module gets the arrays of the callback that it gave, and can free them with its own deallocator
  (BD-347).
- A caller without an implicit context of its own (a module that did not call `ADL_Main_Control_Create`, a call
  through a helper module, code outside an image):
  - The functions that allocate through the callback get `ADL_ERR_NOT_INIT`, with their outputs cleared:
    `AdapterInfoX2_Get`, `DisplayInfo_Get`, `Modes_Get`, `DisplayMapConfig_Get`, `SLSMapIndex_Get` and
    `SLSMapConfig_Get`. The callback of a different module is never used.
  - `ADL_Main_Control_Refresh` and `ADL_Main_Control_Destroy` get `ADL_ERR_NOT_INIT`. They never change the context
    of a different module.
  - The functions that return no memory (`NumberOfAdapters_Get`, `AdapterInfo_Get` into a caller buffer,
    `Active_Get`, `MemoryInfo_Get`, `Versions_Get`) use the newest implicit context. A caller through a helper module
    thus still gets the facts.
- Each module holds its own snapshot. A `Refresh` of one module does not change what another module reads. Before
  BD-347 all modules shared one snapshot.
- The vendor contract. The SDK documentation (`ADL_SDK.html` and the reference pages of `adl.h`) says that
  `ADL_Main_Control_Create` "initializes global pointers". It says that the library allocates output arrays with the
  callback and that the user frees them. It also says that the `ADL2_*` functions exist "to assure that there is no
  interference with other ADL clients that are running in the same process". It does not say what a second
  `ADL_Main_Control_Create` does, or which callback a call uses after two of them. The 3DMark scanner works on vendor
  drivers with two such modules. Thus each of the two modules gets arrays that its own deallocator can free. One
  context per module gives that result, and it gives the result of one process-wide context to a single module.
- The library finds the module of the return address with `RtlPcToFileHeader`. That function reads the inverted
  function table under a shared lock of its own and does not take the loader lock, so a caller in a `DllMain` cannot
  cause a deadlock here. Code outside an image (generated code) has no module: all such callers share the key zero.
- When a module unloads, the library removes the implicit contexts at its base address. A loader notification
  (`LdrRegisterDllNotification`) tells the library about the unload. The notification runs with the loader lock.
  The library never waits for the loader lock while it holds its own lock. Without the notification, the key still
  protects a module that loads later at the same address: its size or time stamp is different, so it starts without
  a context and never gets the callback of the earlier module. The context of the earlier module is then a leak of
  one block until the library unloads. Only a later copy of the same file at the same address can have the same key.
- Limit: a caller that jumps to an export with a tail call has the return address of its own caller. The library
  then uses the context of that module, or for a function that returns no memory the newest one. The libraries in
  the scanner of BD-347 make ordinary calls.
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
- The compile-only SDK gate compares 26 structure sizes and alignments, and selected offsets, against the public
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

## System packaging

The package builder supports the explicit `adl.register` declaration with both x64 and x86 architectures.
It stages both libraries in the DriverStore and verifies their hashes after the package binds. The installer
adds the two ADL directories to the end of the machine `PATH`. The configuration restart makes the entries
available to new processes. The installer never overwrites or removes a system ADL DLL.

Bare-name `LoadLibrary` searches `PATH` after the system directories. A `CopyFiles` directive to DIRID 11 is
not isolated: InfVerif reports error 1322 in `/w` mode. The package therefore keeps both files in DIRID 13.
The `CopyToVmWhenNewer` subkeys below apply to GPU-P guests. They do not install ADL in the physical host's
system directories. The native-host acceptance test found no system ADL after installation and two restarts.
See [GPU paravirtualization](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-paravirtualization),
"Copying files to %windir%\\system32 and %windir%\\syswow64 in the VM".

Package members and registrations:

| DriverStore member | Guest copy destination | Registration in the adapter key |
|---|---|---|
| `adl\atiadlxx.dll` (x64) | `System32\atiadlxx.dll` | `CopyToVmWhenNewer`, value `AmdgpuAdl` = `adl\atiadlxx.dll`, `atiadlxx.dll` |
| `wow64\adl\atiadlxy.dll` (x86) | `SysWOW64\atiadlxy.dll` | `CopyToVmWhenNewerWow64`, value `AmdgpuAdl` = `wow64\adl\atiadlxy.dll`, `atiadlxy.dll` |

The two basenames are already unique. The x86 member retains the name that AGS loads and stays below `wow64`.
After a package change, the installer removes its recorded old ADL directories from `PATH` and adds the new
directories. It saves both generations before the update so a partial install can be removed. Uninstall removes
only the recorded ADL entries. The same cleanup applies when returning to a legacy package.

Coexistence:

- In a guest, "when newer" compares the file version first, then the timestamp when versions are equal. The library is
  stamped with the package's mapped Windows file version (for example 40.7.216.104), not an assumed 0.7.x
  comparison value. No relative ordering against an official AMD ADL version is assumed.
- On the host, an ADL DLL in a system directory or an earlier `PATH` entry retains precedence.
  Coexistence with an installed AMD driver is unmeasured and cannot be tested on the single-driver unit A.
  This is not a guarantee that either library can enumerate the other driver's adapter.
- `CopyToVmOverwrite` is not used. Only this package's named `AmdgpuAdl` registration is replaced. Other
  values under the shared registry subkeys are left intact.
- `build-umd-adl.ps1 -Version ... -Flavour release|lab` stamps the public DLL before its build receipt,
  through `versioninfo.json` and the shared version-stamp helper. Test DLLs and test hooks are excluded
  from both shipped flavours. Release package signing covers the DriverStore members before the catalog
  is generated. The x86 file and its version resource both name `atiadlxy.dll`.

Open questions for the packaging step, to be measured on the lab:

1. Do both architectures load the exact DriverStore files after clean install and upgrade without process overrides?
2. Does uninstall remove the recorded search entries and make the bare-name loads fail with file-not-found?
3. Does the HLK INF test accept the two guest-copy subkeys in the adapter key of our INF?

The installation acceptance kit uses an x64/x86 bare-name loader probe and reports the resolved DLL path.
AGS runs from directories containing no local ADL copy. A successful app-local test cannot satisfy these
checks. Clean-install, upgrade and post-uninstall observations are separate phases. The kit does not infer
absence from an arbitrary load failure or remove a foreign system library to make the check pass.

## Limits

- The library does not report Eyefinity, FreeSync, HDR, overdrive, clocks, compute units or an AMD software
  release. A caller that needs one of these sees the function as absent or gets `ADL_ERR_NOT_SUPPORTED`.
- The facts are a snapshot. The library gives a display change after `Create` only after
  `ADL2_Main_Control_Refresh` or in a new context.
- A program that reads `driverVersion` and expects AMD's numbering sees our version instead.
