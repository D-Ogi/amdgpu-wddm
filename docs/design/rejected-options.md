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

## Rejected, with the reason

| Option | What it is | Why we do not use it |
|---|---|---|
| SMU hardware temperature cap, queue 3 message `0x8C` | The firmware's own temperature cap. A community project sets it and keeps a software floor of 95 C under it | The cap cannot throttle a force-pinned clock gently. The same project reports that a cap below 95 C lets the die overshoot. An emergency thermal event then wedges the GPU. Our thermal policy is in the KMD, where it can step the clock, hold the voltage and log every decision (`dpm.md`). A second, blind cap under it adds a failure mode and no control |
| SMN window in PCI config space, offsets `0xB8` and `0xBC` | A generic read and write path into the SMN address space through the host bridge's configuration registers | Never add it. It reaches every SMN address with no name and no table, which is exactly what rule 1 of `../../CLAUDE.md` forbids. Our KMD reads BAR5 through a generated allowlist of named registers (`driver/kmd/regs.generated.h`), and a second path with no allowlist would make that table decorative |
| The SMU-wedging messages: queue 0 `0x04` and `0x2E`, and any untested queue 3 handler | Message slots that exist in the firmware's tables | The same source reports that each one hangs the SMU until AC is removed. It adds two more limits, which we keep as design rules. Send not more than one mailbox message per 100 ms. Send no mailbox traffic during sustained compute. Our allowlist is five messages, and `smu.c` refuses the rest before it touches the mailbox |
| `InitiateGcRsmuSoftReset`, message `0x2E` | A soft reset of the graphics block through the SMU | It has the shape of the resets we already measured. On this part `amdgpu_gpu_recover` and `modprobe -r amdgpu` both hang unit A (M53). This part has no working GPU reset. A reset message that we cannot observe, cannot bound and cannot undo is not a recovery route. It is also one of the two wedging slots in the row above |
| The `AMD_CU_MASK` literal "the mask must contain CU2 or CU3" | A rule taken from one community encoder and quoted as a hardware property | It is not a hardware property. It restates Mesa's own legality check, which is built from `min_good_cu_per_sa` of the board that ran it, a 40-CU part. Unit A has 24 CU. Read the field on our part and compute the mask from it. A literal copied from another topology is rejected and then ignored by Mesa, which looks like success and does nothing |

## Already rejected elsewhere, kept here as one list

| Option | Why not |
|---|---|
| `ForceGfxFreq` and `UnForceGfxFreq` | The community's own measurement of that route is 202 W and 99 C, outside the 300 W supply rule and the 87 C policy. We do not need it: 800 and 900 MHz already work through `RequestGfxclk` (M785) |
| A deep idle point such as 350 MHz for 36 W | Their point leaves the voltage to the firmware. Our architecture forces a VID with every clock, and the floor is 820 mV. 350 MHz at 820 mV is therefore a large overvolt for a few watts. What stays open is one wishlist line: how low `RequestGfxclk` goes at 820 mV |
| `RequestActiveWgp`, message `0x18` | Already in our imported AMD header and already refused with a host test. Presence in a header does not authorize use |
| CPU overclocking, `WRITE_SMN 0x98`, `SCLK_MAX 2500`, floating VID offsets from queue 3 | One of these repositories reports a permanently bricked BC-250 from this method, and another ships a `DO NOT WIRE THESE UP` banner on the same messages. This is independent support for our design: one serialized SMU owner, and no return to a legacy direct writer |
| `SetCoreEnableMask 0x2C` (`0x77` to `0xFF`, eight CPU cores) | An SMU mask, not a firmware write, so the firmware-write rule does not block it. It stays outside the allowlist on purpose, with a host test. The reported gain is one Linux board, and this change needs the owner's word and per-core validation first |
| The VF curve point 900 mV at 1500 MHz | Our 1500 MHz at 919 mV is the firmware's own point (M22). Their 900 mV is an undervolt below stock with no measurement, and their 500 MHz at 700 mV is under our 820 mV floor |
