# Rejected design options

Each row below is a route that somebody can propose again. Each one exists on this hardware, a community
project uses it, and it looks like a short cut. We examined each one and rejected it. The row gives the
reason, so that the next reader does not repeat the examination and a refusal needs no new argument.

This page lists rejected routes. It is not a list of open questions. For open questions see
[`../linux-session-wishlist.md`](../linux-session-wishlist.md). For the rules that these decisions
follow, see [`../hardware.md`](../hardware.md) ("Limits for our experiments") and
[`dpm.md`](dpm.md) ("SMU allowlist").

PROVENANCE: facts below come from community repositories. We cite their findings. We copy no code and
no text from them, and no AMD or ASRock firmware image goes near our tree.
`cachenetics/project-ariel` at `97b8692`, GPL-2.0-only.
`bc250-collective/amd_smu_reverse_engineering` at `3f47768`, no LICENSE file, all rights reserved, read only.
`bc250-collective/bc250_smu_oc` at `327014d`, MIT.
`MTSistemi/bc250-vaapi` at `4c667c5`, GPL-3.0-only.
`rw-r-r-0644/bc250-core-unlock` at `569785a`, MIT.
`62fixolab/Latest-Bazzite-AMD-BC-250-Patched-Images` at `b0366d9`, MIT.
`MTSistemi/SkillFishOS` at `7d1628c`, GPL-3.0-only.

## Rejected, with the reason

| Option | What it is | Why we do not use it |
|---|---|---|
| SMU hardware temperature cap for the GPU, queue 3 messages `0x8C` and `0x20` | The firmware's own cap over the forced GPU clock. A community project sets it and keeps a software floor of 95 C under it | The cap cannot throttle a force-pinned clock gently. The same project reports that a cap below 95 C lets the die overshoot. An emergency thermal event then wedges the GPU. Our GPU thermal policy is in the KMD, where it can step the clock, hold the voltage and log every decision (`dpm.md`). A second, blind cap under it adds a failure mode and no control. **Corrected in 0.7.210:** the CPU cap `0x8B` is a different message with its own readback (`0x40`), and it is admitted at 85 to 100 C, because on the CPU rail the firmware is the only thing that throttles and our driver has no policy of its own there |
| SMN window in PCI config space, offsets `0xB8` and `0xBC` | A generic read and write path into the SMN address space through the host bridge's configuration registers | Never add it. It reaches every SMN address with no name and no table, which is exactly what rule 1 of `../../CLAUDE.md` forbids. Our KMD reads BAR5 through a generated allowlist of named registers (`driver/kmd/regs.generated.h`), and a second path with no allowlist would make that table decorative |
| The SMU-wedging messages: queue 0 `0x04` and `0x2E`, and any queue 3 handler the read stage has not reached | Message slots that exist in the firmware's tables | The same source reports that each one hangs the SMU until AC is removed. **Narrowed in 0.7.215:** `0x04` is `SetDriverTableDramAddrHigh`, and amdgpu sends it with the address of its own table at every Linux start of unit A (`../init-sequence.md`, E03). The metrics table reader sends it once per owner start with the address of the driver's own page, and its list refuses every other argument (`dpm.md`, "Power reading"; `EnableSmuMetrics` 0 removes it). `0x04` with any other argument, and `0x2E`, stay rejected. It adds two more limits, which we keep as design rules. Send not more than one mailbox **setter** per 100 ms, and a getter not faster than the firmware's turnaround of 10 ms (`BC250_CPU_MESSAGE_GAP_MS`, `BC250_CPU_GETTER_GAP_MS`; 0.7.210 sent getters at the setter's rate too, which held the surface for 1.9 s per read stage and bought nothing, since a getter settles nothing). Send no mailbox traffic during sustained compute. Our allowlist is the five clock messages of queue 0, five more CPU messages on queue 0 and nine on queue 3 (0.7.210, `hardware.md`), and the three metrics table messages `0x04` to `0x06` of queue 0 with fixed arguments (0.7.215), and `smu.c` refuses every other message before it touches the mailbox. A queue 3 setter waits for this start's own read stage to answer first |
| `InitiateGcRsmuSoftReset`, message `0x2E` | A soft reset of the graphics block through the SMU | It has the shape of the resets we already measured. On this part `amdgpu_gpu_recover` and `modprobe -r amdgpu` both hang unit A (M53). This part has no working GPU reset. A reset message that we cannot observe, cannot bound and cannot undo is not a recovery route. It is also one of the two wedging slots in the row above |
| The `AMD_CU_MASK` literal "the mask must contain CU2 or CU3" | A rule taken from one community encoder and quoted as a hardware property | It is not a hardware property. It restates Mesa's own legality check, which is built from `min_good_cu_per_sa` of the board that ran it, a 40-CU part. Unit A has 24 CU. Read the field on our part and compute the mask from it. A literal copied from another topology is rejected and then ignored by Mesa, which looks like success and does nothing |
| The Super I/O configuration pair `0x2E`/`0x2F` | The standard way to find and configure a Super I/O chip: an enter sequence, a logical-device select, register access, an exit. Every Linux hardware-monitor driver and every Windows monitoring tool starts there | The DSDT drives the same pair itself, under ACPI mutex `\_SB.PCI0.SBRG.SIO1.MUT0`, at every device-tree rescan and at every sleep and resume. A kernel driver cannot take an ACPI mutex, the enter/select/exit sequence has no abort, and an interleaved sequence leaves the chip in configuration mode with somebody else's logical device selected. We do not need it: the base addresses are constants the DSDT declares, and the chip identity is readable from the EC window alone (`fan.md`). Our reader touches only the EC window at base + 4, and `bc250_hwmon_base_allowed()` refuses any base outside the three declared windows |

## Already rejected elsewhere, kept here as one list

| Option | Why not |
|---|---|
| `ForceGfxFreq` and `UnForceGfxFreq` | The community's own measurement of that route is 202 W and 99 C, outside the 300 W supply rule and the 87 C policy. We do not need it: 800 and 900 MHz already work through `RequestGfxclk` (M785) |
| A deep idle point such as 350 MHz for 36 W | Their point leaves the voltage to the firmware. Our architecture forces a VID with every clock, and the floor is 820 mV. 350 MHz at 820 mV is therefore a large overvolt for a few watts. What stays open is one wishlist line: how low `RequestGfxclk` goes at 820 mV |
| `RequestActiveWgp`, message `0x18` | Already in our imported AMD header and already refused with a host test. Presence in a header does not authorize use |
| CPU overclocking, `WRITE_SMN 0x98`, `SCLK_MAX 2500`, floating VID offsets from queue 3 | One of these repositories reports a permanently bricked BC-250 from this method, and another ships a `DO NOT WIRE THESE UP` banner on the same messages. This is independent support for our design: one serialized SMU owner, and no return to a legacy direct writer. Still rejected in 0.7.210, every part of it: the generic SMN write and the absolute voltage identifiers stay off the allowlist, and our clock control is a limit with a voltage readback under it |

## Reversed, with the reason and the guard that replaced it

A row here was rejected once and is admitted now. The route did not become safe by itself: each one gained a
bound, a readback and a way back, and the owner asked for the capability (2026-10-05 and 2026-10-06). The old
reason stays readable, because it is the reason the guard exists.

| Option | Rejected because | Admitted in 0.7.210 because |
|---|---|---|
| `SetCoreEnableMask 0x2C` (`0x77` to `0xFF`, eight CPU cores) | An SMU mask and not a firmware write, so the firmware-write rule never blocked it. It needed the owner's word and per-core validation first, and the reported gain was one Linux board | The owner pre-approved the CPU core unlock (2026-10-05) and asked for the control surface (2026-10-06). The allowlist admits the two masks `0x77` and `0xFF` and no other pattern, because another pattern suggests a real harvest of defective cores. The change needs a Windows restart and carries the two-mark boot guard, so a mask the machine does not survive costs the mask and not the machine. `QueryCorePstate 0x0C` and `ReadCoreMHz 0x43` validate the new cores per core, which is the validation the old row asked for |
| A V/F curve point under the table's line (the community's 900 mV at 1500 MHz) | Our 1500 MHz at 919 mV is the firmware's own point (M22). Their 900 mV was an undervolt below stock with no measurement, and their 500 MHz at 700 mV is under our 820 mV floor | The undervolt is bounded instead of free: at most 25 mV under the table's line at that clock, never under 820 mV, level 5 pinned at 820 mV, and the curve must not fall as the clock rises (`bc250_clock_curve_check`). It runs as a trial that the kernel reverts by itself, and a boot guard drops it if the start it ran was never healthy. Their 700 mV at 500 MHz stays refused: it is 120 mV under the floor and nothing admits it |

## The CPU messages that stay out (0.7.210)

The CPU control surface of 0.7.210 admits three setters on queue 3 and the core mask on queue 0. Every other CPU
message a community project sends stays off the allowlist, each with its own reason, and `cpu_test.c` sends every
one of them in both directions to prove the refusal. The hazard behind this whole table is one report of a
permanently bricked board: its method was a raised CPU clock with the voltage left to scale freely.

| Message | What it is reported to be | Why it stays out |
|---|---|---|
| `0x0F`, `0x10`, `0x4D`, `0x4E` | Writes of an absolute voltage identifier for the CPU and SoC rails | An absolute identifier makes a wrong value reachable in one message, with nothing between it and the rail. Our undervolt is a relative scale with a bound, and `0x36` reads the result back. This is the class of message that killed the community board |
| `0x49`, `0x4A` | Voltage offsets with no matching getter | A write we cannot read back is a write we cannot undo on evidence. The admitted scale `0x50` has `0x36` behind it |
| `0x52`, `0x53`, `0x6D` | Clock-stretching controls | Clock stretching is how we detect an unstable undervolt (`0x43` against the clock asked for). A driver that also set it would be reading its own output |
| `0x35`, `0x36` queue 0 (`SetSoftMinCclk`, `SetSoftMaxCclk`) | Soft per-core clock limits, `(core << 20) | MHz` | Named in the header so that a reader does not take queue 3's `0x36` for them, and not on the allowlist (0.7.211). No code path sends either: the surface lowers the firmware's own boost ceiling with `0x8F` instead, which leaves the voltage to the firmware. An admitted message with no caller is an open door with nobody watching it |
| `0x25`, `0x26` | Per-core clock forcing |
 It leaves the voltage to the firmware at a clock we chose, which is the bricking method with extra steps. The admitted ceiling `0x8F` lowers the firmware's own limit instead, so the firmware keeps choosing the voltage |
| `0x77`, `0x8E` | The board's protection limits (current, power) | Raising a protection limit is the opposite of a bound. The 300 W supply rule is a rule about the supply, not about the firmware's opinion of it |
| `0x9A` | An extra-voltage flag | No readback, no documented range, and its name is a promise to add voltage. Nothing in our design needs more voltage than the table's own line |
| `0x98` | A generic SMN write | It reaches every SMN address with no name and no table. Rule 1 of `CLAUDE.md` forbids it, and the community reports it hanging on argument 0 |
| `0x28` to `0x30` | A guarded block of nine entries | They carry a different configuration word in the firmware's own table, which the community reads as a boot flag nobody has. A guarded entry sent without its flag is an untested handler, and untested queue 3 handlers hang the SMU until AC is removed |
