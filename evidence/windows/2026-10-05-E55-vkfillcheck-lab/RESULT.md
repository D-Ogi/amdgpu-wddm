# E55: vkfillcheck on unit A: the deployed ICDs, the BD-068 candidate and the b18 ICD

Date: 2026-10-05, 15:17-15:21 lab clock (13:17-13:21Z). Unit A, release 0.7.207.100-tester.12 (train b18r1),
kernel driver 0.7.207.1, 40 CU, GPU desktop route. No game and no other trial in flight. Tctl 64.9 C before.

Programs: `vkfillcheck-x64.exe` B74E6F69 and `vkfillcheck-x86.exe` 6AF93F20 (branch `tools/vkfillcheck`
e01442b5), in `C:\BC250\vkfillcheck`. Each run loads one ICD file by `--icd` with `--deadline 40` (30 for the
b18 run). Nothing was installed or registered: the candidate and the b18 ICD were loaded from `C:\BC250\tmp`.

## Files

| File | ICD loaded (the `icd module` line) | Result |
|---|---|---|
| `vkfill-deployed-x64.txt` | `d3d12\amdgpu_wddm_radv.dll` 72E1D192 (Mesa 091ad563) | 864 PASS, 0 FAIL, 16 SKIP, exit 0 |
| `vkfill-deployed-x86.txt` | `wow64\d3d11\amdgpu_wddm_radv.dll` E03A79CA (Mesa 091ad563, x86) | 864 PASS, 0 FAIL, 16 SKIP, exit 0 |
| `vkfill-candidate-x64.txt` | candidate 45712A13 (Mesa fork `amdgpu-wddm/upstream-2026-10-05` with the key fix) | 864 PASS, 0 FAIL, 16 SKIP, exit 0 |
| `vkfill-candidate-x86.txt` | candidate A8A7FE3C (same source, x86) | 864 PASS, 0 FAIL, 16 SKIP, exit 0 |
| `vkfill-b18-x64.txt` | b18 ICD 013DA4B0 (Mesa merge 7b17e614, BD-068) | 16 PASS, 848 FAIL, 16 SKIP, exit 1 |

All five runs name `AMD BC-250 (RADV GFX1013)` as device 0. Every run took less than 4 s.

## What it shows

1. **The gate finds BD-068 on the hardware.** The b18 ICD writes wrong bytes in every fill, copy and
   whole-size fill case. The 16 passing cases are the small `update` cases (4 to 252 bytes) into host
   memory. The small `update` cases into device-local memory fail at the `prefill` stage: the client writes
   their sentinel with a copy, and the copy uses the same buffer meta shader.
   In 348 of the 848 failed cases the wrong bytes also reach the guard bytes before `dstOffset`: the shader
   writes outside the range the application asked for. No GPU fault occurred in this run: a wild store
   faults only when it reaches an unmapped address (BD-068).
2. **The candidate pair passes the full matrix**, x64 and x86, with the same counts as the deployed b17 ICDs.
3. **The deployed ICDs pass.** The release on the lab has no transfer-content defect that this matrix
   covers.

Commands (from `bc250-win`, lab scripts kept in the local workspace):
`python tools/win/target.py ps <workspace>/scratch/train/b19-vkfill.ps1` (the four PASS runs) and
`python tools/win/target.py ps <workspace>/scratch/train/b19-vkfill-neg.ps1` (the b18 run).
