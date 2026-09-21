# What amdgpu does to unit A during init, and what that means for our driver

Source: the register trace of experiment E03 (`evidence/linux/2026-09-21-E03-init-trace/amdgpu-events.txt`),
summarized by `tools/trace/summarize_init.py` into `init-sequence-unitA.generated.md` (the full phase table;
regenerate, do not edit). This page is the reading of that table. Everything below is MEASURED in the sense
"these host register accesses happened in this order on unit A"; what each step *means* is taken from the
amdgpu source and marked as such where it matters.

Limits of the instrument: the trace shows host MMIO only. It does not show what PSP, SMU, RLC and the CP
microcode do in response, nor register writes that travel inside command packets on a ring (KIQ, gfx, SDMA).

## The sequence

10018 writes, 136117 reads, 1.74 s from the first access to the last.

| t [s] | step | writes | what it is (from source) |
|---|---|---|---|
| 0.000 | `MM_INDEX` / `MM_DATA` walk, `0xFFFF0000` upward, 2560 reads | 2560 | reads 10 KiB from the top of VRAM through the index/data pair: the IP discovery table |
| 0.038 | `REMAP_HDP_*_FLUSH_CNTL`, doorbell aperture enable | 3 | NBIO setup |
| 0.039 | `GCVM_*` / `GCMC_*` and the same for `MMVM_*` / `MMMC_*` | 280 | GART: page table base registers (`LO32 = 0x6FE00001`, `HI32 = 0x4`), VM context 0 range, system aperture, fault handling, L2 control. **Both hubs (GC and MMHUB) get the same values** |
| 0.039 - 0.251 | 99 844 reads of `MMVM_INVALIDATE_ENG17_SEM`, every one returns 0 | 0 | the first MMHUB TLB flush tries to take the invalidation semaphore (`gmc_v10_0_flush_gpu_tlb`, `usec_timeout` = 100000 polls), **never gets it**, gives up after 0.21 s and flushes anyway. All 20 later flushes get it at the first read (value 1) and release it with a write of 0. `dmesg.txt` line 817 carries the matching "Timeout waiting for sem acquire in VM flush" |
| 0.25 | `*_INVALIDATE_ENG17_REQ` / `_SEM` rounds | ~70 | TLB flushes while GART tables are filled |
| 0.253 | `IH_RB_*`, `INTERRUPT_CNTL*`, `BIF_IH_DOORBELL_RANGE` | 15 | interrupt handler ring |
| 0.253 | `MP0_SMN_C2PMSG_69/70/71`, then `_64 = 0x20000` | 4 | PSP: ring address low/high, size `0x1000`, "create ring" |
| 0.273 - 0.308 | `MP0_SMN_C2PMSG_67 = 0x10, 0x20 ... 0xB0` | 11 | PSP ring write pointer: **11 commands in 35 ms**. From source these are TMR setup and firmware loads (SDMA, CE, PFP, ME, MEC, RLC ...) |
| 0.309 | `MP1_SMN_C2PMSG_66` = 3, 2, 4, 5, 0x3D, 0x3D, 6 | 21 | SMU: GetDriverIfVersion, GetSmuVersion, SetDriverTableDramAddrHigh/Low (`0xF4`, `0x8CF000`), GetEnabledSmuFeatures twice (params 0 and 1), TransferTableSmu2Dram. Numbers checked against `smu_v11_8_ppsmc.h` |
| 0.310 - 0.55 | display core (DMU) | ~4400 | DP AUX traffic (EDID, DPCD, link training: `DP_AUX0_*` 3210 writes, DDC GPIO 1949), PHY (`RDPCSTX`), then pipe programming |
| 0.550 | GC golden settings: `GE_FAST_CLKS`, `CGTT_*`, `CB_HW_CONTROL_3/4`, `CH_PIPE_STEER`, `DB_DEBUG*`, `GB_ADDR_CONFIG = 0x44`, `GL2C_*`, `PA_SC_*`, `SQ_*`, `TA_CNTL_AUX`, `UTCL1_CTRL`, `VGT_*` | ~40 | `gfx_v10_0_init_golden_registers` for Cyan Skillfish |
| 0.550 | `GRBM_GFX_INDEX` broadcasts, `SH_MEM_*` per VMID through `GRBM_GFX_CNTL`, `GDS_VMID*` | ~150 | constants init, per-VMID shader memory windows |
| 0.550 | `RLC_CNTL = 0` ... `RLC_CNTL = 1`, `RLC_CSIB_*`, `CP_MEC_CNTL = 0`, `CP_ME_CNTL = 0` | ~15 | RLC restart with the clear-state buffer, **CP and MEC un-halted by plain register writes** |
| 0.550 | `CP_HQD_*` under `GRBM_GFX_CNTL` select, doorbell ranges | ~50 | KIQ queue brought up by direct MMIO; every other queue is then mapped *through* KIQ packets (invisible here) |
| 0.550 | `SCRATCH_REG0 = 0xCAFEDEAD` x 11 | 11 | ring tests: each ring is asked to overwrite the scratch register |
| 0.550 | `SDMA0_*` / `SDMA1_*`, `BIF_SDMA*_DOORBELL_RANGE` | ~90 | both SDMA engines: ring base, rptr address, doorbells, un-halt |
| 1.56 | interrupt enables (`CP_INT_CNTL_RING0`, `CPC_INT_CNTL`, `SDMA*_CNTL`), `*VM_CONTEXT1-15_CNTL` | ~60 | late init: per-process VM contexts enabled, fault interrupts on |
| 1.561 | SMU message 6 again | 3 | table transfer |
| 1.57 - 1.74 | display core again | ~3200 | second detect + the actual mode set (plane, `HUBP`, `MPCC`, `OTG`) |

## Observations that shape the driver

1. **No host write ever touches a microcode port** (`CP_*_UCODE_DATA`, `RLC_GPM_UCODE_DATA`, `SDMA*_UCODE_DATA`:
   zero writes). On this SoC firmware goes in through the PSP ring only. A Windows driver therefore needs
   (a) the PSP ring protocol, (b) the firmware images from linux-firmware (`cyan_skillfish2_*.bin`, kept out
   of the repo like the VBIOS), (c) a TMR. HYPOTHESIS to test before building that: after a warm restart from
   Linux the engines may still hold their microcode (facts M19 saw PSP mailbox state survive), which would
   allow first ring experiments without any PSP work. `CP_ME_CNTL = 0x15000000` under Windows (M18) says the
   engines are halted, not that they are empty.
2. **The order is short and mostly mechanical**: GART -> IH -> PSP firmware -> SMU tables -> golden registers ->
   RLC -> un-halt CP/MEC -> KIQ by MMIO -> everything else through KIQ -> SDMA -> interrupts. About 900 writes
   outside the display core. This is the skeleton for milestones M4 (GART, IH) and M5 (first command).
3. **Two thirds of all writes are display**, and almost all of those are AUX channel traffic. For M3 we
   take none of it: the firmware has already trained the link and set a mode, and a display-only miniport
   keeps that framebuffer (experiment E05). Mode setting of our own is a late, separable task.
4. **SDMA is the cheapest first engine**: no microcode port, no clear-state buffer, a ring base, a read
   pointer address and a doorbell, and its ring test is a single write packet. It is a better "first command
   executed under Windows" than the gfx ring, provided its firmware is alive (see 1).
5. `SCRATCH_REG0` is amdgpu's own ring-test target. It is the natural first *write* under Windows as well
   (roadmap: control write), since a working driver writes it eleven times in a row with no side effect.
6. The first MMHUB TLB flush costs amdgpu 0.21 s of polling a semaphore that is never granted. Our driver
   must not copy that wait blindly: bound it, and find out (M4) whether the semaphore is needed at all before
   MMHUB has been touched.
