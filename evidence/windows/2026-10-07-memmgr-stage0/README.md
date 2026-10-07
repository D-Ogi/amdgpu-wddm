# Memory budgets per adapter and the KMD segment table on unit A (2026-10-07, memory stage 0)

Unit A, Windows 11, kernel driver 0.7.216.4 (the candidate of `evidence/windows/2026-10-07-dp-audio-rate/`),
desktop idle. One bounded run of `stage0.ps1` over SSH (`python tools/win/target.py ps stage0.ps1`), about
04:56Z. No driver change.

The question: an earlier reading gave a `NON_LOCAL` budget of 0 and a `LOCAL` budget of about 3.7 GB, and no file
in the workspace held that reading. This run queries both DXGI adapters in one run and writes each answer to its
own file.

## Files

| File | Content |
|---|---|
| `stage0.ps1` | the run: `amdgpu_wddm_d3d12caps.exe <adapter> <json>` for adapter 0 and 1, then `bc250kmd_cli vram` |
| `stage0.txt` | the run's console output: the tool hash, the matching lines of each JSON, the `vram` table |
| `caps-adapter0.json`, `caps-adapter1.json` | the complete `d3d12caps` output for adapter index 0 and 1 |

The tool is `C:\BC250\memmgr\amdgpu_wddm_d3d12caps.exe`, SHA-256 prefix `0C99B59E` (`stage0.txt`, line 1), built
from `tools/win/d3d12caps` with `build.ps1` just before the run.

## What it shows

1. **Adapter 0 is our GPU.** `GetDesc1`: `BC-250 GPU (amdgpu-wddm)`, `VendorId` `0x1002`, `DeviceId` `0x13FE`,
   `DedicatedVideoMemory` 8547139584, `SharedSystemMemory` 4148424704. `IDXGIAdapter3::QueryVideoMemoryInfo`:
   - `LOCAL`: `Budget` 7741833216, `AvailableForReservation` 4005134336, `CurrentUsage` 0, `S_OK`.
   - `NON_LOCAL`: `Budget` 3733582234, `AvailableForReservation` 1970501734, `CurrentUsage` 0, `S_OK`.
2. **Adapter 1 is the Microsoft Basic Render Driver.** `VendorId` `0x1414`, `DeviceId` `0x8C`, flags
   `SOFTWARE|ACG_COMPATIBLE|...`, `DedicatedVideoMemory` 0, `SharedSystemMemory` 4148424704:
   - `LOCAL`: `Budget` 3733582234, `AvailableForReservation` 1970501734, `S_OK`.
   - `NON_LOCAL`: `Budget` 0, `AvailableForReservation` 0, `S_OK`.
3. **The pair "LOCAL about 3.7 GB, NON_LOCAL 0" is the answer of adapter 1.** Arithmetic on these values:
   3733582234 / 4148424704 = 0.9000. 8547139584 - 7741833216 = 805306368 = 768 MiB.
4. **The KMD's segment table (`bc250kmd_cli vram`).** Four segments, dedicated 8547139584 bytes:

   | Segment | Kind | Resident | Committed | Limit |
   |---|---|---|---|---|
   | 0 | memory | 433569792 | 388861952 | 8280080384 |
   | 1 | aperture | 0 | 0 | 268435456 |
   | 2 | memory | 4284416 | 4284416 | 267059200 |
   | 3 | aperture | 459489280 | 459489280 | 18446744073709551615 |

   Totals: memory resident 437854208, committed 393146368, limit 8547139584. Aperture resident 459489280.

Both adapters report `EnumOutputs` `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` in this run (session 0, no desktop).

No file needed a change for privacy. Checked and found clean: no MAC address, serial number, UUID, IPv4 address, host
name or user name. The adapter LUID in `stage0.txt` is a per-boot local identifier.
