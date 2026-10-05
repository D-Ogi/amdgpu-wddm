# Installing the H.264 encoder MFT

What the installer has to put on the machine, and what it has to write, so that Game Bar, Windows
Camera and Chromium find the transform. The driver INF writes none of it today. This page is the
specification for it. The bytes of every binary value come from `tools/mftreg`, which builds them with
the same code the transform itself uses, so the registration and the live object cannot drift apart:

```
<build output>\mftreg.exe --inf      INF AddReg lines
<build output>\mftreg.exe --reg      the same as a .reg file, for a bounded lab experiment
<build output>\mftreg.exe --show     the blobs in readable form
<build output>\mftreg.exe --enum     the H.264 encoders of this machine, and of each video adapter
```

## Identifiers

| Item | Value |
|---|---|
| Transform CLSID | `{A32438F0-0D79-4CA9-A5BF-9F3C80837253}` |
| as `REG_BINARY`, little endian | `f0 38 24 a3 79 0d a9 4c a5 bf 9f 3c 80 83 72 53` |
| Friendly name | `BC-250 H.264 Encoder MFT` |
| Category (video encoders) | `{f79eac7d-e545-4387-bdee-d647d7bde42a}` |
| `MFTFlags` | `0x00000006` = `MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT` |
| Hardware URL | `amdgpu_wddm://h264-encoder/0` |
| Vendor id | `VEN_1002` |
| Input types | NV12, IYUV, ARGB32 (major type Video) |
| Output type | H264 (major type Video) |

## Files

| File | Destination |
|---|---|
| `amdgpu_wddm_mft_h264.dll` | `%SystemRoot%\System32\` (DIRID 11), beside the other user-mode parts of the package |

The DLL imports only `d3d11.dll`, `dxgi.dll`, `kernel32.dll`, `MFPlat.DLL` and `ole32.dll`. It links
the C runtime statically, so it adds no redistributable requirement of its own.

## Step 1: the COM in-process server

Media Foundation activates the transform through COM, so the class has to be registered:

```
HKEY_CLASSES_ROOT\CLSID\{A32438F0-0D79-4CA9-A5BF-9F3C80837253}
    (Default)              REG_SZ      BC-250 H.264 Encoder MFT
HKEY_CLASSES_ROOT\CLSID\{A32438F0-0D79-4CA9-A5BF-9F3C80837253}\InprocServer32
    (Default)              REG_SZ      %SystemRoot%\System32\amdgpu_wddm_mft_h264.dll
    ThreadingModel         REG_SZ      Both
```

In an INF, `HKCR` is written as `HKLM,"SOFTWARE\Classes\..."`. The DLL exports
`DllGetClassObject` and `DllCanUnloadNow`, so `regsvr32` is not needed and must not be used: the DLL
deliberately exports no `DllRegisterServer`, because a driver package registers through its INF.

## Step 2: make it visible to a client that asks for a hardware encoder

Route A below is the registration the driver package writes. A shipping display driver package on the
development PC uses the same shape, measured. Route B was named as the preferred shape in an earlier
version of this page. That claim is withdrawn, and the reason is in route B.

### Route A - machine wide, what `mftreg --inf` and `mftreg --reg` emit

This is `MFTRegister`'s own on-disk shape. The transform is listed for the whole machine.

```
HKEY_LOCAL_MACHINE\SOFTWARE\Classes\MediaFoundation\Transforms\A32438F0-0D79-4CA9-A5BF-9F3C80837253
    (Default)       REG_SZ        BC-250 H.264 Encoder MFT
    MFTFlags        REG_DWORD     0x00000006
    InputTypes      REG_BINARY    <96 bytes: 3 MFT_REGISTER_TYPE_INFO pairs>
    OutputTypes     REG_BINARY    <32 bytes: 1 MFT_REGISTER_TYPE_INFO pair>
    Attributes      REG_BINARY    <the attribute blob: MF_TRANSFORM_ASYNC, MF_SA_D3D11_AWARE,
                                   MF_SA_D3D_AWARE, MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE,
                                   MFT_ENUM_HARDWARE_URL_Attribute, MFT_ENUM_HARDWARE_VENDOR_ID_Attribute>

HKEY_LOCAL_MACHINE\SOFTWARE\Classes\MediaFoundation\Transforms\Categories\F79EAC7D-E545-4387-BDEE-D647D7BDE42A\A32438F0-0D79-4CA9-A5BF-9F3C80837253
    (no values. The presence of the key is the membership)
```

The class id and the category carry no braces here. Measured on the development PC on 2026-10-05: all
58 class id keys and all 9 category keys of that machine are written without braces, and three of the
class id keys belong to the NVIDIA display driver package. The COM key of step 1 keeps its braces,
because `SOFTWARE\Classes\CLSID` on the same machine is written that way. Evidence:
`evidence/windows/2026-10-05-E52-encoder-mft-dev-pc/mf-transform-registry.txt`.

Take the exact `InputTypes`, `OutputTypes` and `Attributes` bytes from `mftreg --inf` (INF `AddReg`
lines, `FLG_ADDREG_BINVALUETYPE` = `0x00000001`) or from `mftreg --reg`. Do not hand-write them. The
attribute blob is an `MFGetAttributesAsBlob` serialisation, and one wrong byte makes the whole entry
unreadable to Media Foundation. The category membership line uses `FLG_ADDREG_KEYONLY`
(`0x00000010`), which creates a key and ignores the value
(`ref/windows-driver-docs/windows-driver-docs-pr/install/inf-addreg-directive.md`).

`mftreg --inf` names each key in full with `HKLM`. `HKR` is wrong for these keys, and the next section
says why.

One difference from that precedent is open. We publish `MFTFlags` 0x00000006, which is
`MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT`. The three NVIDIA hardware encoders of the
development PC publish 0x00000004, which is `MFT_ENUM_FLAG_HARDWARE` alone. `_MFT_ENUM_FLAG` states
that every MFT belongs to exactly one of the three processing categories, and that a hardware MFT
always processes data asynchronously. Our value therefore also answers a client that asks for
asynchronous software transforms. `mfthost --mft` records which flag combination finds the transform,
and the sink writer stage runs on the same flags, so a change of this value has to be measured against
both records first.

`mftreg --unregister-global` removes both keys through `MFTUnregister`. It needs both
`--unregister-global` and `BC250_ALLOW_HKLM_MFT=1` in the environment.

### What a per-adapter enumeration needs

A client that wants the encoder of one particular GPU calls `MFTEnum2` with the
`MFT_ENUM_ADAPTER_LUID` attribute set to that adapter's LUID. That call also needs
`MFT_ENUM_FLAG_HARDWARE`, and `MFTEnum2` answers `E_INVALIDARG` without it.

The registration holds no LUID, and it cannot hold one. Windows assigns a LUID at boot, and the value
does not survive a restart. Media Foundation answers the per-adapter question itself.

Measured on the development PC on 2026-10-05 with `mftreg --enum`
(`evidence/windows/2026-10-05-E52-encoder-mft-dev-pc/mftreg-enum.txt`):

1. The machine offers 3 H.264 encoder MFTs: the NVIDIA hardware encoder, the inbox software `H264
   Encoder MFT` and the Microsoft AVC DX12 encoder.
2. `MFTEnum2` with the LUID of the NVIDIA adapter returns 2 of them. The NVIDIA hardware encoder is
   one of the 2. The inbox software encoder is not.
3. `MFTEnum2` with the LUID of the Microsoft Basic Render Driver adapter returns 0 encoders.
4. The NVIDIA hardware encoder is registered machine wide, in route A's shape. Its `Attributes` blob
   holds no `MFT_ENUM_ADAPTER_LUID`, and neither display adapter software key of that machine holds
   an `MFT0` value or any other `MFT*` value.
5. Media Foundation publishes `MFT_ENUM_ADAPTER_LUID` on none of the 3 activation objects.
6. The same LUID without `MFT_ENUM_FLAG_HARDWARE` answers `0x80070057`, which is `E_INVALIDARG`.

Point 2 and point 4 together say that a machine-wide registration is not excluded from a per-adapter
enumeration. Route A is therefore enough for a client that asks per adapter, on that machine and that
build of Windows.

The transform answers the same question for a client that already holds the object. It publishes
`MFT_ENUM_ADAPTER_LUID` on `IMFTransform::GetAttributes` as soon as it knows which adapter runs the
encode. The attribute's documented data type is `LUID`, so the value is an 8 byte blob, and a UINT64
of the same bits would answer `MF_E_INVALIDTYPE` to a client's `GetBlob`. `mfthost --mft` checks the
type, the size and the value against the adapter of the device the client handed over.

Sources: `MFTEnum2` (learn.microsoft.com/windows/win32/api/mfapi/nf-mfapi-mftenum2) and the
`MFT_ENUM_ADAPTER_LUID` data type (learn.microsoft.com/windows/win32/medfound/mft-enum-adapter-luid),
both read on 2026-10-05. The local snapshot `ref/windows-driver-docs` carries no page about Media
Foundation transform registration. A search of the whole snapshot for `MFTEnum2`, `MFT0`,
`SoftwareSettings` and "hardware MFT" returns nothing.

### Route B - per adapter under the display driver's software key (unverified, not implemented)

An earlier version of this page said that the Windows frame server reads `MFT0` to `MFT9` from the
display adapter's software key, and that a hardware MFT inside a display driver package is registered
this way and only this way. Both statements are withdrawn. The first has no source. The measurement
above contradicts the second.

`MFT0` is documented for a camera. "The driver MFT is also known as MFT0 because it is the first MFT
applied to the video stream captured from the camera"
(learn.microsoft.com/windows-hardware/drivers/devapps/creating-a-camera-driver-mft, read 2026-10-05).
That page registers the class id of an AVStream capture device, not of a display adapter.

The earlier version also named the wrong key. In a `DDInstall` AddReg section `HKR` is the device's
software key. For a display adapter that key is created under the class GUID
`{4d36e968-e325-11ce-bfc1-08002be10318}`
(`ref/windows-driver-docs/windows-driver-docs-pr/install/inf-addreg-directive.md`, the HKR table, and
`.../display/adding-software-registry-settings.md`). It is not
`HKLM\SYSTEM\CurrentControlSet\Control\Video\{<adapter GUID>}\0000`, which the earlier version named.
An installer written from that sentence writes `MFT0` where nothing reads it.

A per-adapter route may still exist, and it may still be the better shape for a driver package. It
needs a measurement first, which belongs to
[experiments/E52](../../../experiments/E52-m15-11-encoder-mft-lab/README.md). Until that measurement
exists, the driver package uses route A. The lab plan registers route A with
`mftreg --register-global` before a trial and removes it after the trial, so a failure cannot survive
the trial.

## Step 3: nothing else

- Media Foundation's `HardwareMFT` gate needs no change: on unit A that key holds no values, so
  hardware MFTs are not switched off (facts M774).
- Game DVR needs no policy change: `GameDVR_Enabled` is already 1 (facts M774).
- No service, no scheduled task, no resident process.

## Checking an installation

```
mftreg.exe --enum
```

lists the H.264 encoders of the machine, first for the whole machine and then for each video adapter
in turn. A correct installation lists `BC-250 H.264 Encoder MFT` with the hardware URL
`amdgpu_wddm://h264-encoder/0` in both listings, and the adapter listing that holds it is our own
adapter. If the transform is absent, check in this order:

1. the DLL is present and loadable.
2. `HKCR\CLSID\{A32438F0-0D79-4CA9-A5BF-9F3C80837253}\InprocServer32` points at it.
3. The transform key and the `Categories` key of step 2 exist, with the class id written without
   braces.

## Diagnostics in the field

The transform writes nothing unless `BC250_MFT_TRACE` is set in the environment of the recording
process. With it set, every `ProcessMessage`, `ProcessInput`, `ProcessOutput` and event queue
operation is traced, which is what found the sink writer deadlock. Belongs in the support report, not
in the user interface.
