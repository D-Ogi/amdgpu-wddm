# Wishlist for the next Linux session on unit A

Unit A now boots Windows from NVMe; Linux means booting the diagnostic stick (`tools/diagusb`) again, which
costs the owner a trip to the machine. This file collects what should be measured the next time that
happens, so that one session covers everything. Add an item the moment you notice "this would have been easy
to capture under Linux"; when an item is done, move it to the bottom with the evidence directory next to it.
Items are questions with a reason, not commands: the procedure is written when the session is planned.

Rules of the session stay as always: named registers only, streamed logs, boot history recorded (cold or
warm, what ran before), secrets and serials redacted before commit.

## Open

| # | What to capture | Why we want it | Noticed |
|---|---|---|---|
| L1 | A second pre-driver sweep after a **recorded cold start**, and a third after a recorded warm restart from a session in which amdgpu had run | Confirms or refutes the cause given in M19 (uninitialized flops vary per power-up; PSP mailbox state survives a warm restart). The E02/E03 runs did not record the boot history | 2026-09-21 |
| L2 | Engine state after amdgpu has run and the machine is warm-restarted **without** loading amdgpu: `CP_ME_CNTL`, `CP_MEC_CNTL`, `RLC_CNTL`, `RLC_STAT`, `SDMA0/1_F32_CNTL`, `SDMA0/1_STATUS_REG`, `GRBM_STATUS*`, PSP `C2PMSG_64/81` | Tells whether microcode and PSP state survive a warm restart. If they do, the first ring experiments under Windows can run before we have a PSP path of our own (init-sequence.md, observation 1) | 2026-09-21 |
| L3 | The PSP ring itself: the 11 command frames and their command buffers (command id, firmware type, sizes, order), plus `amdgpu_firmware_info` and names + SHA-256 of the `cyan_skillfish2_*.bin` files in use | The register trace shows only the ring write pointer moving 11 times. To load firmware under Windows we need the exact commands and order on this unit, and which firmware versions they carried | 2026-09-21 |
| L4 | Ring contents right after init with packet decode (KIQ, gfx, compute, SDMA): ring buffers, MQDs, write-back areas. `umr` on the stick if it can be built for Alpine, otherwise raw dumps like the `rings` directory of E03 but taken immediately after each ring test | Everything amdgpu does after the KIQ is up travels in packets the register trace cannot see (queue mapping, ring tests). This is the template for milestone M5 | 2026-09-21 |
| L5 | The same trace instrument armed around **one minimal submission**: an SDMA write-linear and a gfx/compute NOP + `WRITE_DATA` to `SCRATCH_REG0`, with ring dumps before and after | The smallest complete "first command" reference: doorbell value, write pointer handling, fence write-back | 2026-09-21 |
| L6 | Register trace of a **GPU reset** (`amdgpu_gpu_recover` through debugfs) and of suspend/resume if the platform supports it | A Windows driver needs a reset path for TDR from the first day it runs an engine; amdgpu's sequence on this SoC (mode2 through SMU? PSP reload?) is unknown to us | 2026-09-21 |
| L7 | Memory layout as amdgpu sees it: `GCMC_VM_FB_LOCATION_BASE/TOP`, `GCMC_VM_FB_OFFSET`, VRAM and GTT managers from debugfs (`amdgpu_vram_mm`, `amdgpu_gtt_mm`), where the GART table and the TMR sit, and the firmware framebuffer address and size from `screen_info` / efifb | Needed for M4 (GART) and to compare with what Windows reports through post-display ownership in E05: same framebuffer, same physical address? | 2026-09-21 |
| L8 | Display state after mode set: DTN log (`amdgpu_dm_dtn_log`), EDID, link rate and lane count, which `DIG`/`UNIPHY`/`OTG` instances carry the DisplayPort output | Mode setting of our own is late, but the capture is free while Linux is up, and it names the instances our DMU register reads should look at | 2026-09-21 |
| L9 | A clean init trace: the E03 instrument again, but with **no sweep before amdgpu loads** (or a sweep from the corrected allow-list without `*_SEM`, `*_HEADER_DUMP`, `GRBM_GFX_CNTL`) | M25: our pre-driver sweep acquired the VM invalidation semaphores and latched a GRBM read error, so the E03 trace and its "before" state describe a machine we had already disturbed. The timeout in M24 is the visible part; what else the sweep changed for amdgpu is unknown until a clean trace exists | 2026-09-21 |
| L10 | SMU tables: the driver table that `TransferTableSmu2Dram` fills (raw bytes + amdgpu's decoded metrics at the same moment), `GetEnabledSmuFeatures` result, fan and thermal limits if any | Gives the overlay real power and clock telemetry under Windows through the mailbox we already drive, and a second temperature source to check M23 against | 2026-09-21 |
| L11 | Interrupt plumbing: `/proc/interrupts` for amdgpu, MSI-X table and capability as programmed, IH ring registers and `IH_DOORBELL_RPTR` after some load | Reference for M6 | 2026-09-21 |
| L12 | M.2 root port `00:15.0` over several **cold** starts: `LnkSta`, `LnkCtl2`, AER counters, with and without the SSD seen by the firmware | M21: the NVMe link is intermittent. A pattern (only cold, only first power-up) decides whether a firmware setting or another SSD is the answer | 2026-09-21 |
| L13 | The PSP/CCP function `1022:143E`: `lspci -vv`, which Linux driver binds, its BARs | Under Windows it sits without a driver. Know what it is before deciding to ignore it | 2026-09-21 |
| L14 | Rebuild the stick with the fixed SSH host key and with the trace + sweep instruments of E03 as first-class commands | Housekeeping: every Linux session so far started with a known-hosts dance and ad-hoc scripts | 2026-09-21 |
| L15 | `grep . /sys/module/amdgpu/parameters/*` right after `modprobe amdgpu`, plus `uname -r` and `modinfo amdgpu` | Cheap confirmation of the module parameters the M4 replay had to take from source: `amdgpu_vm_size`, `vm_block_size`, `gart_size`, `agp` and `noretry`. The `noretry` question itself is already answered (6.18.52's `amdgpu_gmc_noretry_set()` tests `gc_ver >= IP_VERSION(10, 1, 0)` where mainline v6.18 tests `10, 3, 0`, so GC 10.1.3 gets `true` - which is what the trace shows), so this is a check, not an open question | 2026-09-21 |

## Done

_Nothing yet._
