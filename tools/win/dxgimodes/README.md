# dxgimodes - display mode lists per format

`dxgimodes.exe` prints the display modes that dxgkrnl and DXGI report for each output. It opens no window and
creates no device. It changes no mode, no owner and no setting.

## Why

3DMark Steel Nomad (D3D12) stopped on unit A with "Display mode list not found for given format" (lab session
native-caps349). The workload asks `IDXGIOutput::GetDisplayModeList` for a format. When the list is empty, the
workload stops. Up to KMD 0.7.200.1 the miniport offered VidPN source modes in `D3DDDIFMT_A8R8G8B8` only.
KMD 0.7.201.1 also offers `A8B8G8R8`, `A2B10G10R10` and `A16B16G16R16F` (`driver/kmd/display_modes.h`).
This tool shows the result of that change.

## Output

The tool writes one fact on each line. The first word names the layer:

| Prefix | Source | What it shows |
|---|---|---|
| `CCD` | `QueryDisplayConfig` | The desktop source mode. `pixelformat=4` is 32BPP. `5` is NONGDI. The desktop must stay at 4. |
| `ADAPTER`, `OUTPUT` | DXGI | Adapter name and LUID. Output name, `GetDesc1` color space and bits per color. |
| `KMT` | `D3DKMTGetDisplayModeList` | The modes that dxgkrnl collected from the miniport, with the `D3DDDIFORMAT` of each mode. |
| `DXGI` | `IDXGIOutput1::GetDisplayModeList1` | The count of modes for each DXGI format, with flags `0` and `DXGI_ENUM_MODES_SCALING`. |
| `SUMMARY` | | Outputs, empty lists of display formats, failed calls. |

`B8G8R8X8_UNORM` is not a DXGI display format. It is a negative control: its count is 0 on all drivers.
The exit code is 0 when all calls succeed, 1 when a call fails, and 2 for a bad argument. `--all` prints all
modes. Without it, the tool prints the first 8 KMT modes and the first 3 modes of each DXGI list.

## Build

```powershell
pwsh tools\win\dxgimodes\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\dxgi-modes\build
```

## Expected result on unit A

| Line | KMD 0.7.200.1 | KMD 0.7.201.1 |
|---|---|---|
| `KMT ... format=21(A8R8G8B8)` | 1 or more modes | 1 or more modes |
| `KMT ... format=32`, `31`, `113` | no line | 1 or more modes each |
| `DXGI ... B8G8R8A8_UNORM` | modes > 0 | modes > 0 |
| `DXGI ... R8G8B8A8_UNORM`, `R10G10B10A2_UNORM`, `R16G16B16A16_FLOAT` | expected 0 | expected > 0 |
| `CCD source ... pixelformat` | 4 | 4 |

The DXGI rows for 0.7.201.1 are an expectation, not a measurement. If a DXGI count stays 0 while the KMT line
for its `D3DDDIFORMAT` shows modes, DXGI does not map the formats as `display_modes.h` expects. Record that
result before you change the KMD again.
