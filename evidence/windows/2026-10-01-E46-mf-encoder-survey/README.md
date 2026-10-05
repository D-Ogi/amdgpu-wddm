# E46: Media Foundation video encoders offered on unit A (M15.11 bootstrap, read-only)

Date: 2026-10-01 12:11Z. Unit A, Windows 11 Pro build 22631, KMD 0.7.193.1, desktop on the GPU DWM route, registered
D3D12 triplet shell 6B12D522 / engine 15E3E24E / ICD 2A13235D (before the adapter107 promotion of 12:14Z; no D3D12 or
encoder object was created, so the triplet does not enter the measurement).

Method: `mf-encoder-survey.ps1` (SHA-256 A269EB5D9B2E5B27..., this directory) run elevated over SSH in session 0.
`MFStartup`, then `MFTEnumEx` for `MFT_CATEGORY_VIDEO_ENCODER` and `MFT_CATEGORY_VIDEO_DECODER` under six flag sets
(HARDWARE, ASYNCMFT, SYNCMFT, LOCALMFT, all, all with SORTANDFILTER), printing every attribute of every activation
object; then registry reads of the Media Foundation hardware-MFT keys, the Game DVR settings and policy, the
registered encoder category, the video controllers and the presence of encoder DLLs in System32. No
`IMFActivate::ActivateObject`, no registry write, no capture. GUIDs from the SDK 10.0.26100 headers in the toolchain.

Two script defects were fixed before the recorded run (earlier runs aborted, no output kept): a PowerShell cast of the
COM object to the interface (no QueryInterface; the helpers now cast in C#), and `GetBlob`'s `byte[]` marshalled as a
SAFEARRAY (AccessViolation; now `LPArray` with `SizeParamIndex`).

Output: `survey-output.txt` (SHA-256 E457FE66DF60A047...). Redacted: the PnP instance suffix of the adapter's device
path and the `LastGameActivity` blob.

Results:
- `MFT_ENUM_FLAG_HARDWARE`: 0 video encoders and 0 video decoders.
- Software video encoders: SYNC 7 (`H264 Encoder MFT` {6ca50344-051a-4ded-9779-a43305165e35}, WMVideo8/9, H263, and
  the HEIF, HEVC and VP9 extension entries without a CLSID), ASYNC 1 (`Microsoft MPEG-2 Video Encoder MFT`).
- `H264 Encoder MFT`: inputs IYUV, YV12, NV12, YUY2; output H264; six attributes, `MF_TRANSFORM_FLAGS_Attribute`
  {9359bb7e-...} = 1 (sync), no hardware URL or vendor id, no D3D11/D3D12 awareness attribute on the activation object.
- `HKLM\SOFTWARE\Microsoft\Windows Media Foundation\HardwareMFT` holds no values; no Game DVR policy key;
  `GameDVR_Enabled` = 1.
- `amfrt64.dll` and `nvEncodeAPI64.dll` absent; `mfh264enc.dll` present.

Facts: M774.
