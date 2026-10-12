# DCN201 mode planner

PROVENANCE: Linux AMD Display Core, MIT, revision `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`.

The [pinned AMD Display Core sources](https://github.com/torvalds/linux/tree/7d0a66e4bb9081d75c82ec4957c50034cb0ea449/drivers/gpu/drm/amd/display/dc)
contain the original contracts listed below.

`reference/manifest.json` records the original paths and SHA-256 values. Run
`python tools/import/extract_dml201.py --check` to verify the originals and generated files.
The numerical sources remain unchanged. Two platform adaptations are generated:
portable variadic logging macros, and a NaN result for the math helpers' zero-divisor assertion.
The planner rejects non-finite outputs. Other AMD assertions record a failure in the caller's workspace.

The planner admits one progressive, full-size linear RGB8 plane with physical addressing and no DCC.
It has no MMIO, allocation, firmware call or global mutable state. The caller supplies an aligned heap
workspace and protects floating-point state. A successful plan does not prove a hardware mode change.
The caller must program clocks, watermarks, RQ/DLG/TTU and OTG sync before unblanking.

## Inputs and their origin

- Timing, pixel clock and output type come from the selected target mode and detected link.
- Pitch is the scanout allocation's pitch in pixels, not bytes. The plane uses 32 storage bits per pixel.
- DCHUB reference clock comes from the firmware crystal, `REFCLK_CNTL` and
  `DCHUBBUB_GLOBAL_TIMER_CNTL`. AMD accepts an enabled global timer and a 40-60 MHz result.
- Dentist VCO comes from `CLK4_CLK_PLL_REQ.FbMult_int`, multiplied by 100000 kHz.
  The planner requires this input. It does not silently substitute the upstream 3 GHz fallback.
- DCN201 IP limits and SoC clock/latency bounds come from `dcn201_resource.c`.
  `BboxDefaults=1` records this origin. They are not measurements of the current hardware.
  The one-pipe restriction changes the available DPP/OTG count to one.
  It does not change the numerical algorithms.

Native Linux selects DML20 for DCN201. Its DCN201 clock manager does not use the `pp_smu` argument or
refresh this bounding box from SMU clock tables. It programs DISPCLK/DPPCLK through dentist and
caches the requested DCF/SOC/DRAM values. Those cached values are not hardware readback.
The DCN201 DPP DTO callback is a no-op because firmware manages that block.
The KMD must establish its actual operating clock contract before applying a computed plan.
C-state is disabled, matching the explicit DCN201 branch in `dcn20_calculate_dlg_params`.

## Source references

All paths below are under `drivers/gpu/drm/amd/display/dc/` in the pinned revision.

| Contract | Source |
| --- | --- |
| DCN201 IP and SoC bounds | `resource/dcn201/dcn201_resource.c:76-233` |
| DCN201 uses DML20 | `dml/display_mode_lib.c:44-49,102-104` |
| Native clock sources and programming | `clk_mgr/dcn201/dcn201_clk_mgr.c:84-217` |
| Crystal and DCHUB reference | `hwss/dcn201/dcn201_hwseq.c:239-255`, `hubbub/dcn20/dcn20_hubbub.c:560-589` |
| Pipe input mapping | `dml/dcn20/dcn20_fpu.c:1314-1727` |
| Watermarks, then sync and RQ/DLG/TTU | `dml/dcn20/dcn20_fpu.c:1140-1246,1730-1844` |

The isolated host test covers admitted modes, reference-clock changes, bounds and unsupported plans.
It verifies timing-dependent register values so that inherited DLG values cannot stand in for a new calculation.
It does not test register programming, memory-service latency, clock transitions or display output.

## Hardware application

`bc250_dcn201_pipe.c` imports the DCN201 hardware hooks through `extract_dcn201_pipe.py`.
`reference/pipe/manifest.json` pins the originals. The extractor also emits register lists through
`tools/regcalc`. `pipe_registers_readonly.json` separates status and clock-source reads from writes.

The wrapper admits only HUBP0, DPP0, MPC0, OPP0, OTG0 and DIG0. It verifies all field ranges before
the first write. The caller blanks and drains the plane, serializes the full mode change, and supplies
the ATOM crystal clock. A failed write requires a full replay of the previous plan.
The wrapper preserves the first I/O error and restores the FIFO override even after a clock error.
All polls consume the caller's bounded stall budget.

The following adaptations are explicit:

- Upstream array selectors are fixed to pipe and OTG zero.
- DCN201 omits VM-only NOM0-3 and cursor1. An absent field accepts only a zero payload.
- The wrapper rejects a power-gated HUBP instead of treating its skipped drain wait as success.
- The wrapper has no 3 GHz or 600 MHz fallback when a clock-source read is invalid.
- It disables stutter to match the computed plan, which has no C-state admission.
- Only RGB8 video and black output are exposed. Other imported test-pattern branches have no public entry point.

| Hardware contract | Source |
| --- | --- |
| HUBP blank and drain | `hubp/dcn10/dcn10_hubp.c:41-68` |
| DCN201 RQ, DLG and TTU hooks | `hubp/dcn201/dcn201_hubp.c:66-110`, `hubp/dcn20/dcn20_hubp.c:246-295` |
| DCN201 watermark programming | `hubbub/dcn201/dcn201_hubbub.c:53-75` |
| Dentist divider transitions and FIFO workaround | `clk_mgr/dcn20/dcn20_clk_mgr.c:73-213` |
| No DPP DTO write on DCN201 | `dccg/dcn201/dcn201_dccg.c:46-55` |
| MPC flow count, OPP clock and black output order | `hwss/dcn20/dcn20_hwseq.c:777-797,922-966` |
| DCN201 MPC and OPP hooks | `dcn201/dcn201_mpc.c:42-60`, `dcn201/dcn201_opp.c:42-57` |
| Stream attributes before link enable and unblank | `link/link_dpms.c:2506-2650` |
| OTG and DIG front/back routing | `dio/dcn10/dcn10_stream_encoder.c:1489-1496`, `dio/dcn10/dcn10_link_encoder.c:1344-1363` |

`run_dcn201_pipe.ps1` links the real display accessors and tests the generated allowlist.
It verifies exact timing-dependent values, both special dentist transitions, and injected MMIO failures.
Mock MMIO proves sequencing and error handling. It does not prove physical clocks or a visible image.
