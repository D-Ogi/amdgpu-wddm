# gfx1013 does not do variable rate shading (2026-10-24)

Question: does the gfx1013 GPU of the BC-250 do coarse shading when the driver gives it the GFX10.3 variable
rate shading (VRS) programming? Linux and Mesa treat the part as GFX10.1, which has no VRS. Before this run,
"VRS 0" in the D3D12 caps ([M564](../../../docs/facts/d3d.md#m564)) and in the defaults audit of 2026-10-04 (item
I) followed from the hardware generation only.

**Result: no.** All four GFX10.3 routes were programmed into the command stream of one draw, and the log of the
driver shows the values that it wrote: the per-draw rate, the per-vertex rate with its combiner, the rate
attachment through HTILE, and the DB override. On every route the GPU shaded each pixel once (1x1). The same
client on another GPU shades coarse fragments as expected. Fact: [M922](../../../docs/facts/hardware.md#m922).

## Method

- **Probe ICD.** A lab-flavour RADV (`amdgpu_wddm_radv.dll`) from mesa-wddm branch `diag/bc250-vrs-probe`,
  commits 0877b385 and 844b7372, on the release line d97c4a06. Only on GFX1013, only in the lab flavour, and only
  when `RADV_BC250_VRS_PROBE` is set, RADV takes its GFX10.3 VRS paths, one bit per register group:

  | Bit | Name | Registers |
  |---|---|---|
  | 0x01 | ON | exposes `VK_KHR_fragment_shading_rate` and the GFX10.3 rate code in the shaders. Writes no VRS register |
  | 0x02 | DRAW | `GE_VRS_RATE` (uconfig 0x3098C), `VGT_DRAW_PAYLOAD_CNTL.EN_VRS_RATE` (bit 6) |
  | 0x04 | COMB | `PA_CL_VRS_CNTL` (context 0x28848), the combiners |
  | 0x08 | VTX | `PA_CL_VS_OUT_CNTL.USE_VTX_VRS_RATE` (bit 28) |
  | 0x20 | OVR_2X2 | `DB_VRS_OVERRIDE_CNTL` (context 0x28064) = OVERRIDE 2x2 on every draw |
  | 0x40 | HTILE | `DB_HTILE_SURFACE.VRS_HTILE_ENCODING` (bits 19-20), the rate-to-HTILE copy |

  With a mask set, the ICD prints `BC250 VRS PROBE: <register>=<value>` for each VRS value that it writes into
  the command buffer, and `FSR_API=<value>` for the API state that the values come from (width [0:7], height
  [8:15], combiner 0 [16:23], combiner 1 [24:31]). The client sets `AMDGPU_WDDM_LOG=stderr`, so these lines
  reach its stderr. The ICD is loaded by absolute path into the client process only. The installed driver, the
  DriverStore and the registry stay unchanged.
- **Client** ([`source/vrsprobe.c`](source/vrsprobe.c), [`.vert`](source/vrsprobe.vert),
  [`.frag`](source/vrsprobe.frag), built by [`source/build-client.ps1`](source/build-client.ps1), clang-cl
  23.1.4). One full-screen triangle on a 256 x 256 R32G32B32A32_UINT target, one draw, one submit, a 5 s fence
  timeout and a 20 s process deadline. The fragment shader takes a unique id from one atomic counter and writes
  the id, its FragCoord (x16) and its ShadingRateKHR to each pixel that it covers. A pipeline statistics query
  counts the fragment shader invocations as a second witness. A coarse 2x2 fragment leaves the same id in four
  pixels, and the counter ends at a quarter of the pixels.
- **Arms.** plain (no shading rate state), pipe1x1, pipe2x2, pipe2x1, pipe1x2 (pipeline rate), prim2x2 (vertex
  shader writes 2x2, combiner 0 REPLACE), primkeep (the same with combiner 0 KEEP: control), att2x2 and attnd2x2
  (rate attachment, left half 1x1, right half 2x2, combiner 1 REPLACE, with and without a depth attachment).
- **Steps.** [`source/run-arms.ps1`](source/run-arms.ps1) runs a list of arms under one mask and stops at the
  first error exit or new KMD fault or timeout line. Each step took less than 12 s.
- **Image check.** [`source/check-dump.py`](source/check-dump.py) on the raw readback: every pixel must hold the
  FragCoord of its shading rate (the pixel centre at 1x1, the centre of the coarse fragment otherwise). The
  readback files themselves are not kept here (1 MiB each).
- **Decision rule, written before the run.** VRS works for a route when its arm gives about 1/4 (2x2) or 1/2
  (2x1, 1x2) of the 1x1 invocations, uniform blocks, a counter equal to the pipeline statistics, and coarse
  FragCoord centres. VRS is absent when the arm equals the 1x1 control.
- **Register risk.** Steps with the bits 0x02 to 0x40 write addresses and bits that GC 10.1 does not define
  (see the register map below). They were run with these limits: writes only through the PM4 command stream,
  only the values that RADV computes for GFX10.3, one draw per process, the steps in the order from no write to
  the most writes.

## Positive control (development PC, NVIDIA GeForce RTX 4090)

The same client binary, system Vulkan loader: all 9 arms give their expected shape. pipe2x2 gives 16384 of
65536 invocations in uniform 2x2 blocks with rate 0x5, pipe2x1 and pipe1x2 give 32768, prim2x2 gives 2x2,
primkeep gives 1x1, att2x2 and attnd2x2 give 1x1 left and 2x2 right (40960 invocations). The counter equals the
pipeline statistics in every arm, and `check-dump.py` puts every pixel at its expected centre. Log:
[`positive-control/nvidia-dev-pc.txt`](positive-control/nvidia-dev-pc.txt).

## Unit A results

Unit A, Windows 11 22631 with the installed driver (KMD unchanged), Tctl 52 C at the start of the p2 steps. No GPU FAULT line and no timeout
line in the KMD log in any step, no device loss, every step `stop=none`.

### Probe ICD of 844b7372 (C043A7A8), 12:39-12:40Z

The register values are those of the client's draw. Each arm: 65536 invocations = 65536 pipeline statistics
invocations = 65536 pixels, 65536 distinct ids, no uniform 2x2 block, `rate_read` 0x0 on every pixel, and all
65536 pixels at the 1x1 FragCoord centre.

| Step (mask) | Arm | FSR_API | VRS values written for the draw | Result |
|---|---|---|---|---|
| p2s1 (0x01) | pipe1x1, pipe2x2 | not logged | no VRS register | 1x1 |
| p2s2 (0x03) | pipe1x1 | 0x00000101 | GE_VRS_RATE 0x00, VGT_DRAW_PAYLOAD_CNTL 0x40 | 1x1 |
| p2s2 (0x03) | pipe2x2 | 0x00000202 | GE_VRS_RATE 0x11, VGT_DRAW_PAYLOAD_CNTL 0x40 | 1x1, 2x2 expected |
| p2s2 (0x03) | pipe2x1 | 0x00000102 | GE_VRS_RATE 0x01, VGT_DRAW_PAYLOAD_CNTL 0x40 | 1x1, 2x1 expected |
| p2s2 (0x03) | pipe1x2 | 0x00000201 | GE_VRS_RATE 0x10, VGT_DRAW_PAYLOAD_CNTL 0x40 | 1x1, 1x2 expected |
| p2s3 (0x07) | pipe2x2 | 0x00000202 | GE_VRS_RATE 0x11, PA_CL_VRS_CNTL 0x0 | 1x1, 2x2 expected |
| p2s3 (0x07) | prim2x2 | 0x00010101 | PA_CL_VRS_CNTL 0x1, PA_CL_VS_OUT_CNTL 0x01200000 (no vertex rate bit) | 1x1 (control) |
| p2s3 (0x07) | primkeep | 0x00000101 | PA_CL_VRS_CNTL 0x0 | 1x1 |
| p2s4 (0x0F) | prim2x2 | 0x00010101 | PA_CL_VRS_CNTL 0x1, PA_CL_VS_OUT_CNTL 0x11200000 (USE_VTX_VRS_RATE) | 1x1, 2x2 expected |
| p2s4 (0x0F) | primkeep | 0x00000101 | PA_CL_VRS_CNTL 0x0, PA_CL_VS_OUT_CNTL 0x11200000 | 1x1 |
| p2s6 (0x47) | att2x2 | 0x01000101 | PA_CL_VRS_CNTL 0x40 (HTILE combiner OVERRIDE), HTILE bit | 1x1 both halves, 2x2 right expected |
| p2s6 (0x47) | attnd2x2 | 0x01000101 | PA_CL_VRS_CNTL 0x40, HTILE bit | 1x1 both halves, 2x2 right expected |

`GE_VRS_RATE`: RATE_X [0:1], RATE_Y [4:5], 1 means 2 pixels. `PA_CL_VRS_CNTL`: VERTEX [0:2], PRIMITIVE [3:5],
HTILE [6:8] combiner modes (PASSTHRU 0, OVERRIDE 1). Logs: `logs/p2s1` to `logs/p2s6`, summary
`logs/p2-run.log` (times in local time, UTC+2).

### Probe ICD of 0877b385 (2FD8921D), 11:24-11:27Z

- s0 (mask not set): `fsr_extension=0`, plain 1x1. s1 (0x01): extension exposed, every arm 1x1.
- s2, s2b, s3, s4 and s6 **do not test coarse shading**. This ICD wrote GE_VRS_RATE 0 and PA_CL_VRS_CNTL 0 in
  every arm of s2b, s3, s4 and s6. The cause: RADV removes the shading rate dynamic state below GFX10.3. Commit
  844b7372 corrects this. s2 ran the same ICD and mask as s2b, without the register log.
  s0, s1 and s2 ran with an older `run-arms.ps1` that did not set `AMDGPU_WDDM_LOG`, so their logs have no
  register lines.
- **s5 (0x21) is valid.** The ICD wrote `DB_VRS_OVERRIDE_CNTL` = 0x00000051 for the draw of pipe1x1: combiner
  mode [0:2] = 1 (OVERRIDE), rate X [4:5] = 1, rate Y [6:7] = 1, that is an override to 2x2 (gc_10_3_0_sh_mask.h,
  Mesa gfx103.json). This is the value that RADV writes for its own flat-shading 2x2 override on GFX10.3. Result:
  65536 invocations, 1x1, every pixel at the 1x1 FragCoord centre.

Logs: `logs/s0` to `logs/s6`.

## What GC 10.1 has at these places

[`source/reg-at-vrs.py`](source/reg-at-vrs.py) against the Linux `gc_10_1_0` and `gc_10_3_0` `_offset.h` and
`_sh_mask.h` headers and the Mesa register tables `gfx10.json` and `gfx103.json` (Mesa d97c4a06):

| GFX10.3 name | Address or bit | GC 10.1 | Next to it in GC 10.1 |
|---|---|---|---|
| DB_VRS_OVERRIDE_CNTL | context 0x28064 | no register | DB_Z_READ_BASE_HI at 0x28068 |
| PA_CL_VRS_CNTL | context 0x28848 | no register | PA_STATE_STEREO_X at 0x28844 |
| GE_VRS_RATE | uconfig 0x3098C | no register | GE_USER_VGPR_EN at 0x30988. Not in the Linux 10.3 header either. Mesa defines it by hand for gfx103 and gfx11 |
| PA_CL_VS_OUT_CNTL.USE_VTX_VRS_RATE | bit 28 | no field. The 10.1 fields end at bit 27 | |
| VGT_DRAW_PAYLOAD_CNTL.EN_VRS_RATE | bit 6 | no field. The 10.1 fields are bits 0-4 | |
| DB_HTILE_SURFACE.VRS_HTILE_ENCODING | bits 19-20 | no field | |

Every probe write went to an address or a bit that GC 10.1 leaves unassigned. No write went to another named
register or field.

## MEASURED and CONJECTURE

MEASURED:

- On unit A, the GFX10.3 per-draw rate, per-vertex rate, HTILE attachment rate and DB override programming
  gives no coarse shading on gfx1013: the invocation count, the ids, the reported rate and the FragCoord equal
  the 1x1 control in every arm.
- The writes cause no KMD fault or timeout and do not change the 1x1 result of the draw.
- The client finds coarse shading on a GPU that has it.

CONJECTURE:

- gfx1013 has no coarse shading hardware. The run cannot tell whether nothing decodes these addresses and bits,
  or whether something decodes them with no effect. The GC 10.1 tables have no VRS register or field at all.
- The attachment arms did not log the `DB_HTILE_SURFACE` value or the rate-to-HTILE copy, only the HTILE bit and
  `PA_CL_VRS_CNTL` = 0x40. An attachment route that works alone is unlikely when the other three routes give
  1x1, but this run does not prove it absent.
- An enable for VRS could still exist in registers at other addresses, or in a fuse, strap or firmware setting.
  No public source names such a setting for a GFX10 part. A search for one would write registers of unknown
  meaning, which the hardware rules of this project forbid.

## Consequence

The D3D12 caps stay at `VariableShadingRateTier` 0, and `VK_KHR_fragment_shading_rate` stays unexposed on
gfx1013. An emulation (shading at 1x1 and reporting 1x1, as `bc250_vrs_noop` does) is a separate API decision,
not a hardware feature.

## Files

- `logs/<step>/step.txt`: the step summary (hashes of the client and the ICD as found on the lab, the KMD log
  counts before and after, the RESULT, VERDICT, counter and register lines).
- `logs/<step>/<arm>.txt.out`, `.txt.err`: the raw stdout and stderr of the client. `<arm>.txt` is the two
  joined.
- `logs/p2-run.log`: the console output of the p2 steps.
- `source/`: the exact sources of the client binary and the scripts of this run.
- `SHA256SUMS.txt`: the hashes of the artifacts and of these files.

Artifacts:

| Artifact | SHA256 |
|---|---|
| probe ICD, 844b7372 (p2 steps) | `C043A7A843097EF4F18690EC154E507797078099BEFA87CF15A149B8CBD632F3` |
| probe ICD, 0877b385 (s steps) | `2FD8921D82DEA6F7DB8158B01167F84856E555F393507CB65B8F33289FDA42CD` |
| `vrsprobe-x64.exe` | `33CB2C414B13B62BEEF7CF50A90A401B3C5A86BC5BA95C36C2A5143AAACD81A3` |
