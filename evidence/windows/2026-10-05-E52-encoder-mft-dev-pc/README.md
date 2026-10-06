# E52 stage 0: the H.264 encoder MFT on the development PC (host baseline, not unit A)

Date: 2026-10-05. Machine: the development PC, Windows 11 Pro build 26200.9445, NVIDIA GeForce RTX
4090, display driver 32.0.15.9597. **No stage of this encoder has run on unit A's GPU.** This
directory is the host baseline that the lab run of
[experiments/E52](../../../experiments/E52-m15-11-encoder-mft-lab/README.md) compares against. Every
number here comes from the NVIDIA GPU of the development PC.

Nothing was registered machine wide. The test registers the transform inside its own process with
`MFTRegisterLocal` and removes it on every exit path. The registry reads below are reads. The lab was
not touched.

## The artifact that produced these numbers

Built by `driver/umd/mft-h264/build.ps1` with MSVC 14.44.35207 and SDK 10.0.26100.0. `/Brepro` is on
every `cl` and `link` call, so the same sources give the same bytes. Two clean builds into two empty
output directories gave the same three hashes. `build.txt` is the log of the second one.

| File | Bytes | SHA-256 |
|---|---|---|
| `amdgpu_wddm_mft_h264.dll` | 454656 | `DA890BBB819EE2B09EFD2F2ADCE6B91CFB47B269BAD623D8327C83A398E216A2` |
| `mfthost.exe` | 570368 | `93B2A97CE41D5ED90F9B8B5A44248E1CD1EC19FBD90B81AC9C22AF46BF350725` |
| `mftreg.exe` | 412672 | `4C55F72BF52B99AEED2B6DC26C2C103AA532C329A98CAF5D8EBAD5F49B58610F` |

The DLL imports five DLLs: `d3d11.dll`, `dxgi.dll`, `KERNEL32.dll`, `MFPlat.DLL` and `ole32.dll`.

## Files

| File | What it holds |
|---|---|
| `build.txt` | the clean build: toolchain versions, the four gates, the hashes above |
| `all.txt`, `all-2.txt`, `all-3.txt` | three repeats of `mfthost --all --width 1280 --height 720 --frames 6 --qp 26 --deblock`, each `ALL PASS`, exit 0 |
| `sweep.txt` | `tests/sweep.ps1`, the conformance sweep |
| `sw-1.txt` to `sw-6.txt` | six repeats of `mfthost --sinkwriter` at the same settings |
| `ffprobe.txt` | `ffprobe` on the `.mp4` that the sink writer wrote |
| `mftreg-enum.txt` | the H.264 encoders of this machine, and of each video adapter |
| `mftreg-inf.txt`, `mftreg-show.txt` | the registration bytes the driver package has to write, and the same blobs in readable form |
| `mf-transform-registry.txt` | how this machine spells its Media Foundation transform registration |

## Results

The oracle in every encode case is the inbox Windows H.264 decoder MFT. A case passes when the
encoder's own GPU reconstruction is sample-for-sample identical to what that decoder produces.

1. **Conformance sweep: 68 of 68 cases pass, 0 fail** (`sweep.txt`).
2. **1280x720, qp 26, deblocking on, 6 pictures: 6 of 6 bit exact**, nnz agreement exact with 0
   disagreements, 215930 bytes, 8637200 bit/s at 30 Hz, mean PSNR(Y) 47.21 dB. The same bytes in all
   three repeats.
3. Per picture, in the three repeats: 8.66, 6.09 and 6.00 ms in total. The device's own timestamps
   measure 4.14, 4.03 and 4.05 ms of dispatches. The GPU stage around them takes 7.73, 5.18 and
   5.09 ms, of which the readback waits 5.11, 4.80 and 4.78 ms. The CPU half takes 0.93, 0.92 and
   0.91 ms, of which CAVLC takes 0.89, 0.88 and 0.87 ms. The first repeat ran while other work loaded
   the machine. The dispatch time and the CPU time repeat to about 3 %. The total does not.
4. **The asynchronous hardware MFT contract holds end to end**: found through `MFTEnumEx` with
   `MFT_ENUM_FLAG_HARDWARE`, `MF_TRANSFORM_ASYNC` set, streaming refused before
   `MF_TRANSFORM_ASYNC_UNLOCK`, an odd frame size refused on both media types, a 36 byte
   `MF_MT_MPEG_SEQUENCE_HEADER` on the output type and the same 36 bytes in front of the first access
   unit, `ICodecAPI` round trips, CABAC refused, a key frame with its own parameter sets after a
   drain, 6 fed, 7 `NeedInput`, 6 `HaveOutput`, 6 outputs, drain complete, and the inbox decoder
   accepted 6 of 6 access units. 173283 bytes, 4.66, 4.74 and 4.83 ms per picture through the
   interface.
5. `DllCanUnloadNow` answers `S_FALSE` while a transform or a class object of ours lives, and `S_OK`
   after the last one dies.
6. The VUI colour description is the client's own, byte for byte, over 5 input types.
7. `MFT_ENUM_ADAPTER_LUID` on `IMFTransform::GetAttributes` is an 8 byte blob and holds
   `00000000:000243c1`, which is the LUID of the adapter of the device the test handed over.
   `mftreg-enum.txt` reports the same LUID for the NVIDIA adapter.
8. **Direct3D 11 texture input: 4 of 4 cases pass** (BGRA plain, BGRA slice 2 of 4, NV12 plain, NV12
   slice 2 of 4). The NV12 cases decode at PSNR(Y) 42.76 dB against the exact bytes written into the
   slice. The driver accepted the NV12 textures with bind flags 0x8 and 0x208.
9. **Against the inbox software `H264 Encoder MFT`**, same source, CBR 6 Mbit/s, 1280x720: ours 178882
   bytes at PSNR Y/Cb/Cr 44.89/44.03/42.36 dB, the inbox encoder 185964 bytes at
   45.85/46.72/46.53 dB. Those figures repeat exactly. The time per picture does not: ours 6.03, 6.14
   and 5.86 ms against the inbox 6.35, 6.67 and 6.97 ms in the three repeats. Three repeats on a
   shared development PC decide no speed ratio.
10. **Sink writer: 6 of 6 repeats exit 0**, each writing the same 179781 bytes, with 6 pictures
    through our transform every time. `ffprobe` reads the file as `h264 / Constrained Baseline /
    1280x720 / yuv420p / 6 frames`.

Streams of the run retained in `P:\bc-250\scratch\m15\video-encode\out\reg-1005`, outside the
repository: `mfthost-encode.264` 215930 bytes
(`25042BA85F184BD281182B17F30A22F281AA6DC66151555B496E25C0DE491BA7`), `mfthost-mft.264` 173283 bytes
(`DB8B24E5AD77AF38A32897325C77145FDCDCB3D4F46DD0489DA881E51FF4B44D`), `mfthost-sinkwriter.mp4` 179781
bytes (`5506438270A19D49A8751EEFBD7567AA21CC3496AE2D417AECB2A1A992123839`).

## What the enumeration says about a per-adapter registration

From `mftreg-enum.txt` and `mf-transform-registry.txt`, both read on this machine:

1. The machine offers 3 H.264 encoder MFTs: `NVIDIA H.264 Encoder MFT`, the inbox software `H264
   Encoder MFT` and `Microsoft AVC DX12 Encoder`.
2. `MFTEnum2` with `MFT_ENUM_ADAPTER_LUID` set to the NVIDIA adapter's LUID returns 2 of the 3: the
   NVIDIA hardware encoder and the Microsoft AVC DX12 encoder. The inbox software encoder is not in
   that answer.
3. `MFTEnum2` with the LUID of the Microsoft Basic Render Driver adapter returns 0 encoders.
4. `MFTEnum2` refuses a LUID without `MFT_ENUM_FLAG_HARDWARE` with `0x80070057`, which is
   `E_INVALIDARG`, as its reference page states.
5. Media Foundation publishes `MFT_ENUM_ADAPTER_LUID` on none of the 3 activation objects.
6. `NVIDIA H.264 Encoder MFT` is registered machine wide under
   `HKLM\SOFTWARE\Classes\MediaFoundation\Transforms`, with `MFTFlags` 0x00000004, 96 bytes of
   `InputTypes`, 64 bytes of `OutputTypes` and a 318 byte `Attributes` blob. That blob does not hold
   the GUID of `MFT_ENUM_ADAPTER_LUID`.
7. Neither display adapter software key under
   `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}` holds an
   `MFT0` value, or any other value whose name starts with `MFT`.
8. `HKLM\SOFTWARE\Classes\MediaFoundation\Transforms` holds 58 class id subkeys, and its `Categories`
   subkey holds 9 category subkeys. All 67 names are written without braces. The COM key of the same
   NVIDIA class under `HKLM\SOFTWARE\Classes\CLSID` keeps its braces, and the same class id without
   braces does not exist there.
9. `HKLM\SOFTWARE\Microsoft\Windows Media Foundation\HardwareMFT` holds `EnableDecoders` 1 and
   `EnableEncoders` 1, so this machine disables no hardware MFT.

Point 2 with point 6 and point 7: a machine-wide registration is returned by a per-adapter
enumeration on this machine, and the display driver package that installed that registration wrote no
per-adapter value. `driver/umd/mft-h264/INSTALL.md` records what this means for our own
registration.

Out of scope here: whether Game Bar, Windows Camera or Chromium choose the transform. That needs a
machine-wide registration, which belongs to the lab and to E52.
