# Application-specific driver state

Status: implemented on bc250-win branch `asd/a1-driver-state` (stage a1 of Advanced Shader Delivery). The host
gates pass on the development PC (2026-10-30). No lab trial on unit A has run yet.

## Purpose

A capture tool such as PIX records the driver state of an application. At replay, the tool gives that state back
to the runtime. The replay device then behaves as the device of the captured application. The D3D12 runtime
calls this state the application-specific driver state. The contract is
[Application_Specific_Driver_State_v0_07.md](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/Application_Specific_Driver_State_v0_07.md).
The DDI is CORE 0113 of the WDK 10.0.28000 header `d3d12umddi.h`.

The state of this driver is the profile that a device applies:

- The per-application settings of the control application, with their sources and the image name. See
  [per-app-graphics-settings.md](per-app-graphics-settings.md).
- The engine profile: the vkd3d-proton config flags that the engine selected for the program. This part is empty
  in this build. See "Limits".

## The DDI

| Entry | Answer |
|---|---|
| Caps 1094, `APPLICATION_SPECIFIC_DRIVER_STATE_113` | `Supported` TRUE at DDI 113 and later |
| `pfnGetApplicationSpecificDriverStateBlobSize` | The size of the blob that Get writes. 0 without a device |
| `pfnGetApplicationSpecificDriverState` | The blob. `E_INVALIDARG` for no buffer or a buffer smaller than the size |
| `pfnGetApplicationSpecificDriverBlobStatus` | What CreateDevice found. See "Blob status" |
| `pfnCheckDriverMatchingIdentifier`, type 1 | See "Identifier". The engine answers every other type |

The shell owns these entries (`driver/umd/d3d12/app-state-ddi.h`). The shell gives them to engine-ddi in
`ModernCoreEntries`. A table that has no such entries keeps the stubs of `ddi-modern-core.h`. The shell puts its
own `pfnCheckDriverMatchingIdentifier` in the table and keeps the engine's function for the other types.

## Blob layout

The blob is little-endian with no padding. The layout is in `driver/umd/d3d12/app-state.h`.

| Offset | Size | Field |
|---|---|---|
| 0 | 16 | `DriverOpaqueGUID`, `{99b14533-68f3-4be0-b0f8-1d5104d43f30}` |
| 16 | 4 | Format, 1 |
| 20 | 4 | 0 |
| 24 | 8 | The file version of the shell DLL, from its version resource. 0 when the DLL has no version resource |
| 32 | 4 | The number of bytes after this field |
| 36 | 4 | The engine part is valid (0 or 1) |
| 40 | 8 | The engine config flags. 0 when the engine part is not valid |
| 48 | 4 | The number of settings, 7 |
| 52 | 56 | One value and one source for each setting, 4 bytes each |
| 108 | 4 | The number of UTF-16 units of the image name, 0 to 259 |
| 112 | 2 per unit | The image name, with no terminator |

The first 32 bytes are `D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054`. The 16 bytes at offset 16 are
its versioning data. The release stamps the version resource after the build (BD-166). Thus the shell reads the
version from its own image at run time and does not compile it in.

## Identifier

`pfnCheckDriverMatchingIdentifier` with type `D3D12DDI_SERIALIZED_DATA_APPLICATION_SPECIFIC_DRIVER_STATE` gives:

| Identifier | Answer |
|---|---|
| This GUID and the versioning data of this build | `COMPATIBLE_WITH_DEVICE` |
| This GUID and other versioning data | `INCOMPATIBLE_VERSION` |
| Another GUID, or no identifier | `UNRECOGNIZED` |

The meanings of the config flags and of the setting values belong to one build. Thus the shell applies only a
blob of its own build.

## CreateDevice

At CreateDevice of a DDI 113 or later device, the shell calls `pfnQueryAdapterInfoCb2` with
`D3DDDI_QUERYADAPTERTYPE_APPLICATIONSPECIFICDRIVERBLOB`. The first buffer holds the largest blob of this build.

| Runtime answer | Result |
|---|---|
| No `pfnQueryAdapterInfoCb2` | NOT_SPECIFIED |
| `E_INVALIDARG`, a runtime without this query | NOT_SPECIFIED |
| `D3DDDIERR_NOTAVAILABLE`, no blob for this adapter | NOT_SPECIFIED |
| `S_OK` and a blob of this build | USED |
| `S_OK` and any other blob | IGNORED |
| `E_NOT_SUFFICIENT_BUFFER` | One more call with the size the runtime gave. Above 64 KiB, IGNORED with no second call |
| Any other failure | UNKNOWN. The driver cannot tell if a blob was set |

A blob of this build has every field in range. The shell refuses a blob with a value out of the range of
`app-settings-core.h` (`valid`), a set value with no source, or an image name with a path separator or a NUL.

The shell writes one log line for each device: `d3d12-app-state blob=<status> query=<HRESULT> size=<bytes>`.

## Blob status

| Status | Meaning |
|---|---|
| USED | The device applies the settings of the blob in place of the process settings |
| IGNORED | The runtime gave a blob that is not of this build. The device applies the process settings |
| NOT_SPECIFIED | The runtime gave no blob. The device applies the process settings |
| UNKNOWN | The query failed, or the device is older than DDI 113 |

A device that uses a blob applies these settings of the blob: `FrameRateLimit`, `VSync`, `Anisotropy`,
`MaxFrameLatency` and `FsrUpgrade`. The FSR escape sends the image name of the blob. The other two settings have
no effect in the D3D12 shell, the same as for the process settings.

Get writes the profile that the device applies. For a used blob, Get writes the same bytes again. Thus a capture
of a replay gives the state of the captured application.

## Release flavour

The feature reads no environment variable and no registry value. The process settings come from the reads that
`app-settings.h` already does. The release flavour and the lab flavour have the same code (BD-167).

## Limits

- The engine part is empty. The engine ABI 1.10 has no entry that gives or sets the config flags. A blob with a
  valid engine part is IGNORED, because this build cannot apply it (`kEngineProfileApplied`).
- The meta commands `GUID_MetaCommand_GetApplicationSpecificDriverState` and
  `GUID_MetaCommand_EnableDummyApplicationSpecificDriverStateBlob` are not implemented. `EnumerateMetaCommands`
  gives no meta commands. Thus the HLK conformance test cannot enable its dummy blob. The Get of this driver
  always gives a blob, so the test can use that blob in place of a dummy blob.
- A blob of a release does not apply to a later release. The versioning data has the file version.

## Tests

`driver/umd/d3d12/app-state-test.cpp` runs in `tools/build/build-umd-d3d12.ps1` when the DDI header has build
0119. It uses a runtime double that follows the contract above. It checks the conformance list of the contract
that a driver can meet without the runtime:

- Get gives a blob that `pfnCheckDriverMatchingIdentifier` type 1 accepts as `COMPATIBLE_WITH_DEVICE`.
- The blob of this driver gives USED. Random bytes give IGNORED. No blob gives NOT_SPECIFIED.
- A blob of another version gives `INCOMPATIBLE_VERSION` and IGNORED.

`engine-ddi/tests/caps-test.cpp` checks `Supported` TRUE. `engine-ddi/tests/modern-core-test.cpp` checks that
the CORE 0113 slots call the entries of the shell.

## Lab checks

| Check | Procedure | Pass |
|---|---|---|
| T2 | Windows developer mode is on. Process 1 creates a device, calls Get and `CheckDriverMatchingIdentifier`, and writes the blob to a file. Processes 2 and 3 call `SetApplicationSpecificDriverState` with that blob and with random bytes before their first device. Process 4 sets no blob. Each process then reads `GetApplicationSpecificDriverBlobStatus` | Process 1 COMPATIBLE. Process 2 USED, 3 IGNORED, 4 NOT_SPECIFIED |

The T0 and T1 checks of stage 0 (the pipeline archive of a replay) are in the stage 0 work. They need the engine
change that names the archive.
