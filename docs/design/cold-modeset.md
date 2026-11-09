# Cold mode set

Status: plan, 2026-11-09. Not implemented. Stages B and C are planned for a later release.

## Problem

The driver programs only a pipe that the firmware lit. When the firmware
finds no monitor at POST, it lights no pipe. Since BD-341 stage A, the
adapter then starts with the display-start state
`BC250_DISPLAY_START_NO_POST` ([start contract](wddm-startup.md#start-without-a-firmware-display)).
The GPU works, but a monitor that arrives later gets no picture. Two stages
are necessary for that picture:

- Stage B: bring the display block from its cold state to the state that a
  connector commit needs. This is the state that the firmware leaves after it
  lights a pipe.
- Stage C: let the connector find a sink, tell Windows, and commit a mode on a
  start with no inherited pipe.

Linux does both in `amdgpu_dm` for this ASIC. The code to port is the DCN
2.0.1 hardware sequencer and the DC link code at the pinned Linux revision
(`ref/linux-src`, v6.18, `7d0a66e4`).

## What exists already

The connector commit of an inherited start already programs a complete
output through `driver/shim`:

| Part | Shim function | Linux source |
|---|---|---|
| Output timing | `Bc250OptcProgram`, `Bc250OptcEnable`, `Bc250OptcDisable` | `optc1_program_timing`, `optc1_enable_crtc` |
| Plane, pipe and requestor | `Bc250Dcn201PipeApply`, `Bc250Dcn201GroupProgramBlanked`, `Bc250Dcn201RequestorLocked` | `hubp2_*`, `dpp2_*`, `mpc2_*` (DCN 2.0.1 hooks) |
| Output path | `Bc250Dcn201OutputProgram`, `Bc250Dcn201Route` | `opp2_*`, stream encoder |
| Mode plan | `bc250_dml201.c` | DML for DCN 2.0.1 |
| Link | `bc250_dp_train.c`, `bc250_dp_phy.c`, `bc250_link_atom.c` | `dp_perform_link_training`, ATOM link tables |
| Sink | `bc250_sink.c`, `bc250_hpd.c` | DP and DDC discovery, HPD |

So the gap is not the commit. The gap is the state before the first commit,
and the connector rules that require an inherited pipe.

## Stage B: the cold DCN state

Port `dcn201_init_hw` (`dc/hwss/dcn201/dcn201_hwseq.c`) to a new shim unit,
`driver/shim/bc250_dcn201_init.c`, with the same `BC250_DISP_IO` access and
allow lists as `bc250_dcn201_pipe.c`. Do the steps in the Linux order:

1. `dccg_init` and the reference clocks (`get_dccg_ref_freq`,
   `get_dchub_ref_freq`) from the ATOM crystal frequency. The connector
   already reads that frequency (`Bc250LinkAtomReferenceClock`).
2. `init_clocks` of the clock manager. On this ASIC the SMU owns DISPCLK,
   DPPCLK and DCFCLK. A message that is not on the SMU allow list needs a
   decision first (`docs/hardware.md`).
3. `bios_golden_init`: the ATOM golden settings. The existing ATOM
   interpreter runs them, if the table is in the image.
4. `link_enc->funcs->hw_init` for the DIG of the one connector.
5. `dpp_reset` of each pipe, `mpc_init`, and `hubp_init` with
   `plane_atomic_disconnect` for each pipe.
6. `tg_init` of each timing generator, and the audio `hw_init`.
7. `DIO_MEM_PWR_CTRL` and the DCN clock gating registers, as Linux writes
   them.

Rules for stage B:

- The init runs only on a `BC250_DISPLAY_START_NO_POST` start, and only
  before the first cold commit. An inherited start never runs it.
- Each register that the init writes goes into the generated allow lists
  (`tools/regcalc`, `gen_regs.py`), with its `mm*` name. No new literal
  offsets.
- The init is a host-tested shim unit, as `bc250_dcn201_pipe.c` is. A model
  of the registers compares each write with the Linux sequence.
- The plane format registers come from the mode, not from a firmware copy.
  `DcnCaptureFirmwareFormat` has nothing to copy on a cold start. Use the
  `hubp2_program_pixel_format` values for ARGB8888.

### Lab evidence that stage B needs

Each item is a measurement on unit A. Record the result as facts with
evidence before the port.

| # | Measurement | Purpose |
|---|---|---|
| B1 | A stage A start with the monitor in standby at power-on: the start record (`DisplayStart` 2), the `display cold:` lines, and the lab-flavour DCN dump. | The cold state that the init starts from. |
| B2 | The same lines and dump on a start with the monitor on (`DisplayStart` 1). | The target state of the init. The difference between B1 and B2 is the work of stage B. |
| B3 | The DCN clocks on the B1 start (`Bc250Dcn201ReadClocks`) and the SMU clock state. | Whether the firmware leaves DISPCLK and DPPCLK running when it lights nothing. |
| B4 | Linux, wishlist row L53: amdgpu boots with no monitor, then a monitor arrives. The DCN registers of B1 after `dcn201_init_hw`, and again after the first commit. | The positive control for the init sequence on this unit. |

## Stage C: the connector without an inherited pipe

1. `ConnectorStart` on a `NO_POST` start allocates the work item and the
   snapshot, and arms HPD as on a programmable start. The sink snapshot is
   empty until discovery finds a sink.
2. `connector_link.c` `OpenContext` admits a context with no inherited pipe
   on a `NO_POST` start. It keeps the board identity check. `Plan` gets no
   inherited tuple. The first commit runs the stage B init first.
3. On an HPD edge with the line high, the worker discovers the sink,
   publishes it and indicates "connected". `ConnectorChildConnected` then
   answers from the snapshot, as for an enabled connector.
4. The mode list of the source comes from the sink snapshot, not from `Post`.
   The VidPN paths read the current signal from the connector
   (`ConnectorLinkCurrentSignal`) and not `InheritedSignalValid`.
5. `CommitVidPn` commits onto the reserved buffer
   (`ConnectorBufferPrepare`, kept on a `NO_POST` start). The result is a
   runtime mode (`RuntimeDisplayChanged`, `RuntimePost`). The DCN stop and the
   PnP release already hand back a runtime mode on a `NO_POST` start.

The display-start state does not change in stage C. A mode that the driver
lights is a runtime mode, as after a connector commit on an inherited start.

## Acceptance

- Host: the stage B register model and the stage C connector tests, with a
  negative control for each step.
- Lab: a boot with the monitor in standby, then the monitor wakes. The desktop
  comes up on the monitor. A boot with the monitor on is unchanged. Then a
  PnP restart and a sleep cycle in both cases.
- The release gates of the project apply to each change.
