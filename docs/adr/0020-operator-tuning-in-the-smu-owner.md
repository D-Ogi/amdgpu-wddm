# ADR 0020: operator tuning lives inside the single SMU owner

Date: 2026-10-06. Status: accepted (owner, 2026-10-06: build the Tuner parity the reference Control Center has -
a GPU voltage and frequency curve, a CPU undervolt and a CPU clock - beside the fan control). It reopens two rows
of [`../design/rejected-options.md`](../design/rejected-options.md) and extends ADR 0014 point 2.

Implemented in KMD 0.7.210: the two escapes, the shim policy, the host tests, the command-line tool and the
application. **No value in it is measured on unit A yet.** Every CPU message semantic and every CPU range is
REPORTED by community projects, so the driver refuses every CPU setter until a queue 3 getter has answered in the
same start, and the lab plan reads before it writes
([`../design/tuner.md`](../design/tuner.md), "What the lab must answer first").

## Context

ADR 0014 moved power management into the driver and set the order: a fixed point first, then load-driven
scaling, and only messages on the allowlist. That is done. The driver owns the clock, the governor scales it
between 500 and 2000 MHz, and the allowlist is five messages (`docs/design/dpm.md`).

What the driver does not offer is a way for a person to change the operating point. The voltage column of the
V/F table is a `const` array. The GUI's own page carries a clock ceiling and a CU mode, and two rows that say
"later": fan control and voltage. The owner's requirement WU-042 asks for the opposite: change clocks or
voltage inside an exposed range, learn the effect on noise, temperature, performance and stability first, and
restore the standard settings.

The reference operating system for this board ships that control today: a curve chart with draggable knots, a
25 s trial that keeps the old curve on disk unless the operator presses Keep, a CPU panel with clock, undervolt
in voltage-identifier steps and a temperature limit, a guided undervolt search, an 8-core unlock and a UMA
split panel. The owner asked for parity with the parts that our rules allow.

Three findings make this cheaper than it looks, and they are in `scratch/tuner/facts-stack.md` and
`scratch/tuner/facts-smu-cpu.md`:

1. **A curve is a run-time change.** The clock grid is fixed at 100 MHz steps, so a curve changes only the
   millivolt column. The clock transaction already takes a clock and a voltage per call, and a same-clock,
   different-voltage request already works through the imported path.
2. **The CPU mailbox needs no new window into the chip.** It is the firmware's queue 3. Its three registers sit
   in the same BAR5 aperture the driver already maps, and all three are already in the generated read
   allowlist. The PCI configuration SMN window that `rejected-options.md` forbids is not needed.
3. **Our own imported AMD header already names CPU messages on queue 0.** `RequestCorePstate`,
   `QueryCorePstate`, `SetSoftMinCclk`, `SetSoftMaxCclk`, `SetCoreEnableMask` and `GetEnabledSmuFeatures` are
   named, not guessed, and each has a handler in both shipped firmware images.

Against that sits the strongest warning in any community source: raising the CPU frequency without undervolting
let the voltage identifier scale uncapped and permanently bricked one BC-250. The reported ceiling is 1.325 V.
No CPU message has ever been sent on unit A.

## Decision

1. **Operator tuning is escapes into `bc250kmd`, and nothing else.** The kernel driver stays the single SMU
   owner. There is no raw user mailbox, no second writer, and no return of a legacy direct writer. Two typed
   escapes carry the whole feature: one for the GPU curve, one for the CPU.

2. **The GPU V/F curve is a run-time change with its bounds in the shim.** The curve is 11 voltages, one per
   level from 1000 to 2000 MHz. Levels below 1000 MHz keep 820 mV, because there is no anchor under the lowest
   one. A candidate is refused unless every value is 820 to 1000 mV, no value is more than a bounded depth
   under the shipped line, the voltage never falls as the clock rises, and the encoded identifier never
   inverts. The default curve is the shipped table, so nothing changes for a start that asks for nothing.

3. **Apply is a trial that the kernel reverts.** The escape stores a candidate with a serial and writes nothing
   to the registry. The governor thread applies it at its next 25 ms tick through the one checked transaction.
   When the deadline passes without a Keep, the tick restores the previous curve. A Keep inside the window
   persists, behind the two-mark boot guard that the CU mode and the DPM mode already use. A stop or a pause
   cancels a trial first. **A value under test is never written to disk**, so a hang boots the last known good
   set. There is no trial while the governor is off, because nothing would revert it.

4. **The CPU surface opens in stages, and reads before it writes.** Queue 3 gets its own transport instance,
   its own firmware-state tracking and its own message allowlist, consulted by the one place that reaches the
   mailbox. A clock transaction can never send a CPU message, and a CPU transaction can never send a clock
   message. The allowlist admits six getters, the CPU temperature cap, the maximum boost clock and the
   curve-scale undervolt, each with a checked argument range. Everything else is refused by number, with a host
   test that names the refusals. **No CPU setter runs in a start until a queue 3 getter has answered in that
   start.**

5. **The CPU path releases the owner lock between messages**, one message per 100 ms, with the temperature
   re-read before each one, and it refuses to start while the GPU is busy. The GPU clock transaction keeps its
   whole-transaction lock, because its two messages must not be split. The reason for the difference: a CPU
   sequence of three messages would otherwise block the governor's 25 ms tick, and with it the thermal policy,
   for 300 ms or more.

6. **A CPU voltage is never forced to an absolute value.** The undervolt scales the firmware's own curve and the
   applied voltage is read back after every change. Above 1300 mV the driver reverts. The reported bricking
   ceiling of 1.325 V is a refusal line, never a target. The messages that force an absolute identifier, that
   take a floating-point argument, or that change clock stretching stay rejected.

7. **The clock control is a limit before it is a raise.** The release control lowers the maximum boost clock
   inside a range whose top is the recorded stock value. A raise above stock is a lab experiment under the
   owner's hardware pre-approval, and it never runs without an undervolt in force and a voltage readback under
   the refusal line. The firmware offers no getter for either, so the driver caches what it wrote and the
   baseline is recorded before the first write.

8. **The 8-core unlock goes through our own named message and a restart.** `SetCoreEnableMask` on queue 0 can
   write the stock mask back. The community's generic SMN write through queue 3 cannot, and with argument 0 it
   hangs the firmware. The control is a registry value, a two-mark boot guard and the next Windows restart,
   exactly like the CU mode. The core-presence mask itself is not readable on Windows, so the GUI shows the
   effect - the processor count - and never the mask.

9. **The failure signs are named, and the driver and the app both watch them.** A machine check through the
   WHEA provider in the system event log, clock stretching seen from two independent routes, and a wrong
   checksum from a new CPU load client of ours. Any one of them stops a trial and reverts it.

10. **Nothing writes BIOS, SPI flash, CMOS or UEFI variables.** The reference's UMA split panel is therefore out
    of scope, and so is the pre-boot core unlock.

11. **The GUI keeps ordinary-user words.** The Graphics page keeps its plain controls. The curve chart, the CPU
    card and the core card sit behind one "Advanced tuning" disclosure, with a countdown, a Stop button, a Keep
    button and one "Restore standard settings" button. Driver internals, firmware status codes and message
    numbers appear only in the support report.

12. **Every inferred message semantic is measured before a write depends on it.** The design document carries
    the table: the fact, its class, the write it blocks and the readback that settles it. Each measured message
    gets a row in `docs/facts/hardware.md` before the next setter runs.

## Consequences

- `driver/shim/bc250_clock.c`'s point check changes from "at least the table's voltage" to "on the grid, inside
  the voltage band", and the per-level depth rule moves into a new whole-curve check. The table itself becomes
  the default curve, and the governor holds the active one.
- Two readers in `driver/kmd/dpm.c` must read the active curve instead of the shipped table: `DpmResyncLevel`
  and the 1000 ms readback comparison. A missed reader drops the clock to the floor, which is the likeliest
  defect of this work, so a host test asserts that a curve change does not trigger a resync.
- `driver/kmd/smu.c`'s two mailbox accessors admit three more offsets, all of them already in the generated read
  allowlist. The write path then admits exactly six offsets, three per queue, by name.
- `docs/hardware.md`'s voltage rule becomes a band with a depth bound, and its SMU allowlist grows a CPU
  section with one row per message and its argument range. `docs/design/dpm.md` gains a curve section.
- `design/rejected-options.md` records two reversals with their dates (`SetCoreEnableMask 0x2C`, and the V/F
  point 900 mV at 1500 MHz as a bounded operator request), three corrections. The `0x2C` row carries a wrong credit.
  `0x2E` has no handler in either firmware image, so it can only answer "unknown command". M791 cites a
  firmware image that is not unit A's version. The page also gains nine new refusals from the CPU queue.
- `bc250control.dll` grows two exports, `Bc250DpmCurve` and `Bc250Cpu`, which is how the application reaches the
  two escapes. The runtime tune escape (0.7.185) still has a command in the tool and no export in the library.
- New host tests: the curve bounds and its trial state machine in the DPM test, same-clock voltage changes in
  the clock test, a new CPU test with its allowlist negatives and its guided-search state machine, the new
  structures in the GUI's offset test, and the load client's own logic test.
- The lab plan is seven steps of at most three minutes each, one change per step, the first of them read-only and
  a precondition of the rest, with the plug sampled, the 87 C rules in force and a cropped camera frame before and
  after every hot run (`scratch/tuner/lab-tuner.ps1`, local).
- Two things stay open until the lab answers them, and the GUI's words depend on both: whether this part's
  effective CPU clock is near 2.75 GHz or near 3.5 GHz, and whether `SetCoreEnableMask` does anything at all on
  a harvested part.

## References

- ADR 0014 (clocks through the SMU, extended here at its point 2), ADR 0007 (bring-up through gated escapes).
- `docs/design/dpm.md`, `docs/design/cu-mode.md`, `docs/design/rejected-options.md`, `docs/hardware.md`.
- `driver/shim/bc250_clock.c`, `driver/shim/bc250_dpm.c`, `driver/shim/bc250_smu.c`, `driver/kmd/smu.c`,
  `driver/kmd/dpm.c`, `driver/kmd/guard.c`, `driver/kmd/bc250kmd_escape.h`.
- `driver/amdgpu-import/smu_v11_8_ppsmc.h` (MIT, the names of the queue 0 CPU messages).
- Owner's requirement WU-042, `ref/gui-requirements-owner-2026-10-04`.
- The full design, the facts with their provenance, and the lab plan: `scratch/tuner/DESIGN.md`,
  `scratch/tuner/facts-stack.md`, `scratch/tuner/facts-smu-cpu.md` (local, outside the repository).
- Community findings, facts only, no code copied: `bc250-collective/bc250_smu_oc` (MIT),
  `bc250-collective/amd_smu_reverse_engineering` (no licence, read only), `rw-r-r-0644/bc250-core-unlock`
  (MIT), `cachenetics/project-ariel` (GPL-2.0-only), `MTSistemi/SkillFishOS` (GPL-3.0-only).
