# Installing the H.264 encoder MFT

What the installer has to put on the machine, and what it has to write, so that Game Bar, Windows
Camera and Chromium find the transform. Nothing here is done by the driver INF today; this page is the
specification for it. The bytes of every binary value come from `tools/mftreg`, which builds them with
the same code the transform itself uses, so the registration and the live object cannot drift apart:

```
<build output>\mftreg.exe --inf      INF AddReg lines
<build output>\mftreg.exe --reg      the same as a .reg file, for a bounded lab experiment
<build output>\mftreg.exe --show     the blobs in readable form
<build output>\mftreg.exe --enum     what MFTEnum2 reports for hardware H.264 encoders on this machine
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

Two routes exist. The second is the one a display driver package should use.

### Route A - machine wide, what `mftreg --inf` emits today

This is `MFTRegister`'s own on-disk shape. The transform is listed for the whole machine, with no
adapter behind it.

```
HKEY_LOCAL_MACHINE\SOFTWARE\Classes\MediaFoundation\Transforms\{A32438F0-0D79-4CA9-A5BF-9F3C80837253}
    (Default)       REG_SZ        BC-250 H.264 Encoder MFT
    MFTFlags        REG_DWORD     0x00000006
    InputTypes      REG_BINARY    <96 bytes: 3 MFT_REGISTER_TYPE_INFO pairs>
    OutputTypes     REG_BINARY    <32 bytes: 1 MFT_REGISTER_TYPE_INFO pair>
    Attributes      REG_BINARY    <the attribute blob: MF_TRANSFORM_ASYNC, MF_SA_D3D11_AWARE,
                                   MF_SA_D3D_AWARE, MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE,
                                   MFT_ENUM_HARDWARE_URL_Attribute, MFT_ENUM_HARDWARE_VENDOR_ID_Attribute>

HKEY_LOCAL_MACHINE\SOFTWARE\Classes\MediaFoundation\Transforms\Categories\{f79eac7d-e545-4387-bdee-d647d7bde42a}\{A32438F0-0D79-4CA9-A5BF-9F3C80837253}
    (no values; the key's presence is the membership)
```

Take the exact `InputTypes`, `OutputTypes` and `Attributes` bytes from `mftreg --inf` (INF `AddReg`
lines, `FLG_ADDREG_BINVALUETYPE` = `0x00000001`) or from `mftreg --reg`. Do not hand-write them: the
attribute blob is an `MFGetAttributesAsBlob` serialisation and a wrong byte makes the whole entry
unreadable to Media Foundation.

Removal deletes both keys. `mftreg --unregister-global` does it through `MFTUnregister`, and it needs
both `--unregister-global` and `BC250_ALLOW_HKLM_MFT=1` in the environment.

### Route B - per adapter under the display driver's software key (preferred, not implemented yet)

The Windows frame server binds a hardware encoder to the GPU it belongs to by reading `MFT0` to
`MFT9` from the display adapter's own software key. A hardware MFT shipped inside a display driver
package is registered this way, and only this way is it correctly attributed to our adapter. In
`driver/kmd/bc250kmd.inf` it is one `AddReg` line in the `DDInstall` section, next to the commented
out `Bc250_UserModeDriver` section:

```
[Bc250_MediaFoundation]
HKR,,"MFT0",0x00000001,f0,38,24,a3,79,0d,a9,4c,a5,bf,9f,3c,80,83,72,53
```

`HKR` in a `DDInstall.AddReg` section is the device's software key, so the value lands under
`HKLM\SYSTEM\CurrentControlSet\Control\Video\{<adapter GUID>}\0000`. Step 1 is still required: the
value names the class, and COM has to be able to create it. With route B, route A's
`MediaFoundation\Transforms` keys are not written at all.

Route B is not wired up yet because nothing of this component has run on unit A, and a per-adapter
registration takes effect for every client on the machine as soon as the driver is installed. The lab
plan ([experiments/E50](../../../experiments/E50-m15-11-encoder-mft-lab/README.md)) uses route A under
`mftreg --register-global`, registered before a trial and removed after it, so that a failure cannot
survive the trial.

## Step 3: nothing else

- Media Foundation's `HardwareMFT` gate needs no change: on unit A that key holds no values, so
  hardware MFTs are not switched off (facts M774).
- Game DVR needs no policy change: `GameDVR_Enabled` is already 1 (facts M774).
- No service, no scheduled task, no resident process.

## Verifying an installation

```
mftreg.exe --enum
```

lists what `MFTEnum2` returns for hardware H.264 encoders. A correct installation shows
`BC-250 H.264 Encoder MFT` with the hardware URL `amdgpu_wddm://h264-encoder/0`. If it is absent,
check in this order: the DLL is present and loadable, `HKCR\CLSID\{A32438F0-...}\InprocServer32`
points at it, and the `Categories` key of step 2 route A exists (or `MFT0` of route B).

## Diagnostics in the field

The transform writes nothing unless `BC250_MFT_TRACE` is set in the environment of the recording
process. With it set, every `ProcessMessage`, `ProcessInput`, `ProcessOutput` and event queue
operation is traced, which is what found the sink writer deadlock. Belongs in the support report, not
in the user interface.
