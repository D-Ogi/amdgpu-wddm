# E10: GPU firmware through the PSP under Windows (milestone M5, first part)

State: **run 001 done on unit A, 2026-09-21. The PSP takes the firmware under Windows. H1, H2, H3, H6, H7 hold; H4
holds except one sub-claim; one statement of the safety section below was wrong (the PSP starts things).**

## Why

M5 is the first command execution. On this chip nothing executes before the PSP has been given the firmware:
PCI 1002:13FE sets `AMD_APU_IS_CYAN_SKILLFISH2` in amdgpu, which selects `AMDGPU_FW_LOAD_PSP` with
`psp_v11_0_8` (ring functions only, no boot loader interface), `autoload_supported = false`,
`boot_time_tmr = false` (SOURCE: `amdgpu_psp.c`, `soc15.c`/`nv.c` at v6.18). What amdgpu then did on unit A
is in the E03 trace (MEASURED): ring create through `MP0_SMN_C2PMSG_69/70/71/64` (ring at MC `0xF5FFFE7000`,
4 KB, answer `0x80020000`), then eleven submissions on `C2PMSG_67` between 0.273 s and 0.308 s. From the
source these are `GFX_CMD_ID_SETUP_TMR` and ten `GFX_CMD_ID_LOAD_IP_FW` in `AMDGPU_UCODE_ID` order: SDMA0,
SDMA1, CE, PFP, ME, MEC1, MEC1 jump table, MEC2, MEC2 jump table, RLC (the RLC file has a v2.0 header: no
restore lists). The spacing in the trace fits the image sizes: 0.1 ms after the 896-byte jump tables, 2 ms
after the 33 KB SDMA images, 6 ms after the 260 KB CP images (INFERRED to be the same eleven; the trace holds
register traffic, not command buffers).

Starting point under Windows (MEASURED, E02): `C2PMSG_64 = 0x80000000`, `C2PMSG_67/69/70/71 = 0`, the same
as under Linux before amdgpu: the PSP's trusted OS is up and has no ring.

The code: `psp_v11_0_8.c` and `psp_gfx_if.h` imported unmodified; `driver/shim/bc250_psp.c` follows
`amdgpu_psp.c`/`amdgpu_ucode.c`; host test `driver/shim/test/replay_psp.c` reproduces all 28 mailbox accesses
of amdgpu's PSP phase, reads included, against a strict model of the PSP. The driver part is
`driver/kmd/psp.c` (bc250kmd 0.5.2): gate `EnablePsp` (default 0, closed by every install, needs
`EnableMmio`, `EnableVram`, `EnableGart`), one escape with PLAN, LOAD, UNLOAD, registers through
`g_MmioPspAllow` only (generated from the trace: the five mailbox registers).

Firmware: linux-firmware `amdgpu/cyan_skillfish2_{sdma,sdma1,ce,pfp,me,mec,mec2,rlc}.bin`, kept outside the
repository (`P:\BC-250\ref\linux-firmware`, commit and sha256 in `PROVENANCE.txt` there). Header versions
equal what amdgpu reported on unit A (E01: ME 0x63, PFP 0x94, CE 0x25, RLC 0x0d, MEC 0x90). The driver
reads them from `C:\BC250\firmware\` on the target.

Memory, top 8 MB of VRAM, by physical address (facts M31, M32), all below the last 64 KB: TMR 4 MB at
MC `0xF5FF800000` (where amdgpu had it), staging 2 MB at `0xF5FFC00000`, GART table and scratch as in E09,
PSP ring at `0xF5FFFE7000` (where amdgpu had it), command buffer and fence in the next two pages.

One deliberate difference from amdgpu: amdgpu stages the images in GTT memory (system pages behind the
GART). Here they are staged in VRAM, which amdgpu also supports (`amdgpu.debug` bit `use_vram_fw_buf`,
SOURCE). Reason: one unknown at a time. A GART-mapped buffer is the next experiment, with the first ring.

## Hypotheses

- H1. Closed gate: `psp plan` is refused with `STATUS_DEVICE_NOT_READY`.
- H2. `psp plan` (**no register write, no VRAM touched**): four planned register writes, equal to amdgpu's
  first four in offset, order and value (`C2PMSG_69 = 0xFFFE7000`, `70 = 0xF5`, `71 = 0x1000`,
  `64 = 0x00020000`); eleven planned commands with the firmware types and sizes of the host test; sweeps of
  MP0 before and after equal (the plan changes nothing).
- H3. `psp load` without `gart enable` first: refused (`STATUS_INVALID_DEVICE_STATE`), nothing written.
- H4. `gart enable`, then `psp load`: ring create answered with the response flag and status 0 inside the
  poll budget; all eleven commands come back with their fence, `resp.status = 0`; every LOAD_IP_FW response
  carries an address inside our TMR (`0xF5FF800000 .. +4 MB`); 15 register writes (4 + 11 write pointer
  updates, `0x10 .. 0xB0`) equal to amdgpu's in offset, order and value. Command times are of the order
  amdgpu's were (0.1 to 10 ms) and ordered by size. Afterwards through the witness: `C2PMSG_64 = 0x80020000`,
  `C2PMSG_67 = 0xB0`, `69/70/71` as written, which is Linux's state after init. No protection fault bit in
  either hub. Display alive (stage 61, presents counting).
- H5. GC and MMHUB sweeps after the load differ from before only by noise, by E09's known GART differences,
  and by registers whose new value is the one Linux shows after init. (Open observation rather than
  prediction: which registers the PSP itself programs while loading. Whatever changes is listed.)
- H6. `psp unload`: DESTROY_TMR accepted (fence, status 0), ring stop answered; afterwards
  `C2PMSG_64` has the response flag and status 0 (what else it holds is not known: Linux never stopped the
  ring in our trace); a second `psp load` after it works again (the PSP takes a new ring and TMR), as amdgpu
  relies on for module reload.
- H7. Loaded, then the device is stopped (disable/enable) without `unload`: the driver unloads by itself at
  stop, before the GART restore; the new driver instance reports no ring, no TMR.

What would refute: a planned write amdgpu did not make; no response to the ring create; a command without
fence or with a status; a TMR address outside our TMR; a fault status bit; the picture disturbed; a PSP that
refuses a second ring after a stop.

## Safety

Every register write is one amdgpu made on this unit, same order, same values (the ring is at the same
address). The commands are amdgpu's commands with amdgpu's firmware, to a PSP in the state amdgpu found it
in. The memory handed to the PSP is VRAM that Windows does not know exists, above the firmware framebuffer
(checked), so a PSP writing where we pointed it cannot hit Windows memory; the TMR becomes inaccessible to
the CPU once set up (that is its purpose) and nothing of ours lives there. The sequence stops at the first
refused register, the first missing fence, the first non-zero status. Nothing is started: CP, MEC, RLC and
SDMA stay as the firmware left them (halted or idle); loading firmware executes nothing on them. The plan
runs first and is compared before anything is executed. At device stop the driver tells the PSP to forget
ring and TMR.

Known unknowns: what a second SETUP_TMR without DESTROY_TMR does (avoided: the driver tracks ring and TMR
state and refuses a second load; after a crash of the driver instance the state is lost, and then a reboot
is the clean way); whether the RLC, which the firmware leaves running (`RLC_CNTL = 1` in the trace before
amdgpu stops it), minds its image being reloaded while running: amdgpu did exactly this on this unit at the
same point (PSP load at 0.30 s, RLC stopped at 0.55 s), so it is the traced order.

If the machine hangs: power cycle. The gates stay as the script left them, but open gates only allocate
memory at start; every sequence runs on command only.

## Procedure

`e10_target.ps1`, one phase per call, logs under `C:\BC250\e10\out`: `install` -> `psp plan -Tag closed`
(H1) -> `gate -On 1` -> `sweep before`, `sweep before2` -> `psp plan` -> `sweep afterplan` (H2) ->
`psp load -Tag nogart` (H3) -> `gart enable` -> `sweep gart` -> `psp load` -> `sweep loaded` (H4, H5) ->
`psp unload` -> `sweep unloaded` -> `psp load -Tag second` (H6) -> `gate -On 1` again (device restart while
loaded, H7) -> `psp plan -Tag afterstop`, `sweep afterstop` -> `gate -On 0`.
Host: `compare.py commands --expect planned|loaded` and `compare.py state`.

## Result

Run 001, 2026-09-21, unit A, bc250kmd 0.5.2.0. Evidence: `evidence/windows/2026-09-21-E10-run-001/`
(`README.txt` there maps files to hypotheses, `comparison.txt` is `compare_run001.sh`'s output). All MEASURED.

- H1 holds: closed gate, `STATUS_DEVICE_NOT_READY`.
- H2 holds: the plan is amdgpu's first four writes in offset, order and value, eleven commands with the types
  and sizes the file headers give, nothing executed, no register outside the noise set changed.
- H3 holds: `psp load` before `gart enable` is refused (`STATUS_INVALID_DEVICE_STATE`), no write.
- H4 holds with one sub-claim refuted. The ring create is answered; all eleven commands come back with their
  fence and `resp.status = 0`; the 15 register writes equal amdgpu's 15 in offset, order and value. Times:
  SETUP_TMR 0.26 ms, SDMA 1.96 ms each, CE/PFP/ME 5.9 ms, MEC 5.95 ms, jump tables 0.13 ms, RLC 1.74 ms;
  amdgpu's spacing on the same unit was 0.2, 1.9, 1.9, 6.0, 6.0, 6.1, 6.0, 0.1, 6.1, 0.1 ms. Through the
  witness: `C2PMSG_64 = 0x80020000`, `C2PMSG_67 = 0xB0`, `69/70/71` as written: Linux's state after init.
  No protection fault bit in either hub. Picture undisturbed, presents counting.
  **Refuted:** "every LOAD_IP_FW response carries an address inside our TMR". Only the CP images do: CE
  `0xF5FF842000`, PFP `0xF5FF883000`, ME `0xF5FF8C4000`, MEC1 and MEC2 both `0xF5FF800000`; SDMA0/1, both
  jump tables and the RLC answer with address 0. `compare.py --expect loaded` therefore says NOT AS EXPECTED
  for those five lines; the check is left as written before the run.
- H5, the open observation, is the main finding. **Loading is not passive: the PSP starts what it loads.**
  Against the sweep taken after `gart enable`, 31 registers moved to the value Linux shows after amdgpu's
  init and 8 to something else. `SDMA0/1_F32_CNTL` went from 1 to 0 (halt released; `SDMA0/1_STATUS2_REG`
  differ at every sweep afterwards: the SDMA engines execute). `RLC_CNTL` went from 0 to 1 and the RLC
  registers show a running RLC that has talked to the SMU (`RLC_SMU_COMMAND = 0x13`, `RLC_GPM_STAT`
  `0x00F40016 -> 0x00B40016`, `RLC_DYN_PG_STATUS`, `RLC_STATIC_PG_STATUS`, `RLC_SERDES_*`,
  `CGTS_SA*_QUAD*_SM_CTRL_REG = 0xC0F40200`, `CC_GC_SHADER_ARRAY_CONFIG_GEN0 = 0x00FC0001`,
  `SPI_PG_ENABLE_STATIC_WGP_MASK = 7`), all as under Linux. Not as under Linux: `RLC_PG_CNTL = 8`
  (Linux 0: amdgpu clears it later in `gfx_v10_0_rlc_resume`), `RLC_SMU_ARGUMENT_1/3` (values that look like
  measurements), `RLC_RLCS_GRBM_IDLE_BUSY_STAT = 3`, `MP0_SMN_C2PMSG_60/81` (PSP-internal, differ at every
  sweep). CP_ME_CNTL and CP_MEC_CNTL did not change: the CP engines stay halted.
- H6 holds: DESTROY_TMR accepted, ring stop answered, afterwards `C2PMSG_64 = 0x80030000` and
  `C2PMSG_67/68 = 0xC0` (twelve frames); the second load is accepted like the first, same times, same TMR
  addresses. **New:** the second load meets an RLC that is running our image, and leaves it stopped and
  busy: `RLC_CNTL = 0` (1 after the first load and after the unload), `RLC_STAT = 5`, `GRBM_STATUS2` bit 24
  (RLC busy) set, `RLC_GPM_STAT = 0x00F40017`, `RLC_GPM_INT_STAT_TH0 = 0x80000000`; it stays like that
  over the device restart. amdgpu never does this (its `hw_fini` stops the RLC before the PSP
  goes). An unload does not undo what the load started: SDMA stays released and the RLC running.
  `CP_ME_CNTL = 0x15000000` and `CP_MEC_CNTL = 0x50000000` in every sweep of the run.
- H7 holds, seen through the witness: after a device restart without `unload`, `C2PMSG_64 = 0x80030000` and
  `C2PMSG_67 = 0xC0`, and the new driver instance reports no ring and no TMR.
- After a warm reboot of the target the GPU is back in the firmware state: SDMA halted, RLC registers as
  before the run, PSP mailbox zero, `C2PMSG_64 = 0x80000000`.

Consequences:
1. The sentence in Safety above, "loading firmware executes nothing on them", is wrong for SDMA and RLC. It is
   left in place as what was believed before the run. From `psp load` on there is a running RLC and two
   released SDMA engines with no ring; that state is only left by a reboot or by the next step (RLC stop and
   start, SDMA ring setup: amdgpu's order, M5 second part).
2. A load must not be repeated while the RLC runs. The driver needs the RLC stop before a second load; until
   it has one, a second load in the same boot is for experiments only.
3. SDMA, the jump tables and the RLC are not placed in the TMR by address as far as the response says; whether
   the PSP copies them somewhere else or programs them straight into the engines is not known (wishlist).
