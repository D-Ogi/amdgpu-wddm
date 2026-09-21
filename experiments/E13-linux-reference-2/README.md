# E13: second Linux reference session on unit A (wishlist L3, L4, L5, L7, L8, L9, L10, L11, L13, L15; L6 last)

State: **run on 2026-09-21, three boots.** H1 to H4 hold; one step hung the machine (facts M40, M41, M42).
The owner offered a Linux boot while M6 (E12) was in progress.

## Why now

E12 needs a reference for the interrupt side that the E03 trace does not hold (it recorded register accesses of the
init only): how many interrupts amdgpu takes for a known submission, what the IH ring pointers and the per-queue HQD
registers look like on a live, idle GPU, and which MSI state Linux programs. E03 itself was taken after a sweep of
ours that had disturbed the machine (facts M25), so a clean init trace is owed since then (L9). M7 will need a reset
path; amdgpu's on this SoC is unknown to us (L6).

## What is captured, and what is expected

Boot entry "shell only": amdgpu blacklisted, none of our tools touches the GPU before amdgpu loads.

- `pre`: PCI view of the GPU function, of the PSP/CCP function `1022:143e` (L13) and of the M.2 root port (L12),
  `/proc/interrupts`. No GPU register is touched.
- `load`: tracefs events of module amdgpu armed before `modprobe amdgpu` (the E03 instrument), no sweep before.
  Expected (H1): the register writes of the GART, PSP, IH, GFX and SDMA steps equal E03's in offset, order and value,
  except addresses; the "Timeout waiting for sem acquire in VM flush" of E03's dmesg is gone (M25 says our sweep
  caused it). What would refute M25: the timeout is still there.
- `state`: module parameters (L15), MSI/MSI-X state as programmed (L11), firmware versions and file hashes (L3, in
  part), memory managers (L7), SMU metrics (L10), the display log (L8), ring buffers and MQDs (L4), and named registers
  through amdgpu's own `amdgpu_regs2` debugfs file: global state and the HQD registers of every queue under amdgpu's
  own SRBM selection. Expected (H2): `CPC_INT_CNTL` read under me 1, pipe n equals `CP_ME1_PIPEn_INT_CNTL` read without
  a selection (the aliasing `driver/shim/include/bc250_irq.h` assumes). (H3): the KIQ's and the compute queues'
  `CP_HQD_PQ_RPTR` on an idle GPU equal their write pointers, and the priorities of compute queue 0 are those the MQD
  dump of E03 showed (pipe priority 2, queue priority 15).
- `ib`: amdgpu's own IB tests on every ring (`amdgpu_test_ib`) with the trace armed and `/proc/interrupts`, the IH
  pointers and the ring contents before and after. Expected (H4): one IH vector and one interrupt per fence, visible
  as `amdgpu_iv` events with the client and source ids `bc250_ih.h` names; the fence packets in the ring dumps are
  the ones `bc250_gfx_emit_fence()` builds.
- `reset` (last; may hang the machine, which then needs the owner): `amdgpu_gpu_recover` with the trace armed (L6).

Not in this session: L1, L2 (they need recorded cold and warm restarts), L16 (needs a patched module), L5's SDMA
write-linear by hand, L12's series of cold starts, L14 (rebuilding the stick).

## Safety

Read-only towards the GPU except what amdgpu itself does. Registers are read by name only, from lists generated out of
the witness read list (`gen_lists.py`; no `*_SEM`, `*_HEADER_DUMP`, `GRBM_GFX_CNTL`, nothing of MMEA), and through
amdgpu's own accessor, which does the SRBM selection under its mutex. Nothing is written to a disk, the firmware or
NVRAM; the stick runs from RAM. Logs are streamed to the PC as they are produced.

## Procedure

Scripts are copied to `/tmp/e13s` on the probe over SSH; each phase is `sh session.sh <phase>`, its output teed on the
PC; `/tmp/e13` is fetched with tar at the end. MAC addresses, serial numbers and the EDID are redacted before commit.

## What was actually done

The plan above assumed the boot entry "shell only". The owner's keyboard does not work in GRUB, so the stick came up
with its default entry, mode `full` (the stick's short probe with its two restored writes, then amdgpu), and "shell
only" (`bc250.mode=off`) would not have started the network at all. The session became three boots:

| Boot | How it started | Mode | Phases |
|---|---|---|---|
| 1 | by the owner, out of Windows; cold or warm not recorded | `full` | `state`, `ib`, `pre` (with amdgpu loaded), then `reload.sh`: **hung at `modprobe -r amdgpu`** |
| 2 | cold, the owner's power button after the hang | `full` | `pre` (with amdgpu loaded), `state`, `ib` |
| 3 | warm restart out of boot 2, after `set default=2` in the stick's `grub.cfg` (original kept and restored afterwards) | `readonly`: named reads, no write, amdgpu not loaded | `pre`, `load`, `state`, `ib` |

`reset` was not run: it needs the owner's consent and a hang had already cost one trip to the power button.
Afterwards the stick's loader was renamed (`efi/boot/bootx64.efi.off`) so that the firmware would go on to the NVMe
disk; the machine did not come back by itself after that restart (see the journal).

Two things about the stick came out on the way (wishlist L14, L18): sshd there does not load the fixed ed25519 host
key, so the host key changes with every boot and was re-learned each time after checking that the machine behind the
address is unit A with the stick's command line; and busybox `od` reads nothing from amdgpu's ring files in debugfs,
`dd bs=4096 | od` does.

## Result

Evidence: `evidence/linux/2026-09-21-E13-reference-2/` (redacted with `redact.py`), one directory per boot.

- **H1 holds** (facts M41). `compare.py` against E03: all 10018 register writes of E03 are in the clean trace, same
  order, same offsets; 7 values differ (three times the dummy page's address, four writes to one register none of our
  steps touches); 478 more writes 2.2 s later are the console's mode set. No "sem acquire" timeout in any of the three
  boots. Not refuted: M25. Limits: boot 3 was a warm restart after amdgpu had run; how E03's boot started is not
  recorded either.
- **H2 holds**, with a discriminating case: `CPC_INT_CNTL` under me 2 pipe 0 reads 0 and under me 2 pipe 1
  `0x20000000`, each equal to that pipe's `CP_ME2_PIPEn_INT_CNTL`; under me 1 every pipe reads `0x05800000`.
- **H3 holds**: every active queue idles with read pointer = write pointer (compute `0x200`, KIQ `0x1400` in boot 1);
  compute queue 0 has pipe priority 2 and queue priority 15, the others 0 and 0. Not expected: a thirteenth active HQD
  (me 2 pipe 0 queue 0), which is amdkfd's HIQ (dmesg: "kfd kfd: amdgpu: added device 1002:13fe").
- **H4 holds for the interrupt half** (facts M40): 12 fences, 12 interrupts, 12 vectors, the same client, source and
  ring ids in all three boots. This is what E12 part C compares against. The packet half (the fence packets in the
  ring dumps against `bc250_gfx_emit_fence()`) is not evaluated yet; the dumps are in `boot*/rings-after-ib/`.
- amdgpu uses MSI-X here (3 entries, one used); Windows gives our miniport MSI with one message (M38).
- **`modprobe -r amdgpu` hung the machine** (facts M42, n = 1): no reference for the teardown; wishlist L17.
