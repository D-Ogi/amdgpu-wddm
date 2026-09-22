# E17 - Linux reference 3: what is left after the evidence was counted

Status: **prepared, not run, and deliberately much smaller than it started.** Nothing here has
touched the lab.

## Why this experiment shrank

E17 was planned as a nine-phase session: the page table format, the gfx ring wrapper, the ioctl
stream of a RADV process, a umr build, an offscreen triangle, and the open wishlist rows. Before
writing the procedure, the existing evidence was counted, one question at a time. The count is
`scratch/tmp/e17_inventory.md`; the short version is that **most of the session would have measured
what is already on disk**, and the rest can be computed on the development PC.

Two offline tools in `offline/` are that argument made checkable rather than asserted. Both read
`evidence/` and modify nothing.

### `offline/check_pte_from_trace.py`

`amdgpu_vm_sdma_set_ptes()` (`ref/linux-src/.../amdgpu_vm_sdma.c:187-203`) traces exactly the
arguments it hands to the SDMA packet, and the engine writes `(addr + i*incr) | flags`. The scatter
path `amdgpu_vm_copy_ptes`, which would hide the values inside an IB, fires **zero** times in all
seven of our capture files. So every entry amdgpu built in E03, E13 and E14 is reconstructible here
by arithmetic: 600 events describing **33348 page table entries**.

Run against `evidence/linux/` it prints **14 claims: 8 confirmed, 0 refuted, 6 not covered**
(`scratch/tmp/e17_pte_from_trace.txt`). The eight are exactly the list `bc250_pte.h` marks
"TRANSCRIPTION ONLY - nothing on this list has been executed on this ASIC by us":

| claim | measured |
|---|---|
| 512 entries per level, every table one 4 KB page | 58 whole-table initialisations, every one `count=512` |
| a PDE carries no permission bit, no MTYPE, no fragment | 35 directory entries, every one `0x1` exactly |
| a VM leaf's default memory type is NC, not the GART's UC | 117 NC leaves in user VMs; the 168 UC ones are the kernel VM, fact M37's aperture |
| 64 KB is `AMDGPU_PTE_FRAG(4)` - the header calls this an inference | 8 entries, all 64 KB aligned, all covering a whole multiple of 64 KB |
| four levels, PDB2/PDB1/PDB0/PTB | the PDE chain of one mapping walks the whole tree in one burst |
| 2 MB `AMDGPU_PDE_PTE` huge pages, which `bc250_pte_from_dxgk()` refuses | 15 entries, all FRAG=9 |

### `offline/ring_wrapper.py`

The gfx ring dump from E14 was always in the repository. `decode_cs.py --ring` is deliberately
conservative - "only the packet kind we are sure of is reported" - so it printed 16
`INDIRECT_BUFFER` lines and the wrapper was taken to be missing. It is not. Packet names come from
the kernel's `nvd.h`, register names from `tools/regcalc`, and `--selftest` proves the naming on a
ring built here before any real dump is believed. One RADV submission, out of bytes we already had:

```
COND_EXEC / WAIT_REG_MEM  poll memory == previous fence
WRITE_DATA [PFP]  mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32  <- 0x6ffe0001
WRITE_DATA [PFP]  mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_HI32  <- 0x00000004
WAIT_REG_MEM      write 0x00f80002 -> mmGCVM_INVALIDATE_ENG0_REQ
                  poll mmGCVM_INVALIDATE_ENG0_ACK until & 0x2   (bit 1 = vmid 1)
PFP_SYNC_ME / WRITE_DATA <the pasid LUT> / RELEASE_MEM
SWITCH_BUFFER x2 / COND_EXEC / CONTEXT_CONTROL / FRAME_CONTROL
INDIRECT_BUFFER  VMID=1  24 dwords     <- RADV's preamble
INDIRECT_BUFFER  VMID=1  64 dwords     <- RADV's main IB
FRAME_CONTROL / RELEASE_MEM (user fence) / RELEASE_MEM (scheduler fence) / SWITCH_BUFFER
```

The compute ring from E13 differs exactly where it should: engine `ME` not `PFP`, invalidation
engine 4 not 0, and none of the gfx-only packets. That is the M8 scheduler's template, twice, from
two independent captures, with no boot.

**Correction, 2026-09-22.** This section used to say that the pasid-to-VMID LUT write - `BAR5+0x04284`
on the gfx ring, `BAR5+0x0429C` on the compute ring - "has no name in `tools/regcalc`: the OSSSYS/IH
register header is not in our set". That was wrong: `third_party/linux-amdgpu/osssys_5_0_0_offset.h`
has been in the repository since the scaffold commit, and `regcalc --ip OSSSYS --reg-header
third_party/linux-amdgpu/osssys_5_0_0_offset.h reverse 0x04284 0x0429C` answers `mmIH_VMID_1_LUT` and
`mmIH_VMID_7_LUT`. `ring_wrapper.py` only ever built the default GC map, so it printed `<unnamed>`.
It now consults the OSSSYS map as well, and the same E14 dump decodes as seven LUT writes:
`mmIH_VMID_1_LUT <- 0x5B` through `mmIH_VMID_7_LUT <- 0x5A`, which is amdgpu mapping seven VMIDs to
seven pasids in one submission - exactly what `SOC15_REG_OFFSET(OSSSYS, 0, mmIH_VMID_0_LUT) + vmid`
writes (`ref/linux-src/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gfx_v10.c:130`). No register the M8
wrapper needs is unnamed any more.

## What is left for hardware, and it is not much

1. **PRT (sparse) and NOALLOC page table entries.** Across 600 events and five sessions: **zero**
   entries with either bit. RADV never asks for them in the workloads we run. They are precisely
   what WDDM will ask `bc250_pte.c` for - `ReserveGpuVirtualAddress` becomes a sparse mapping - and
   nothing but a purpose-built client can produce one. **This is the only reason to boot.**
2. **A positive control**: the entries read back out of VRAM through `amdgpu_vram`/`amdgpu_iomem`,
   rather than out of a tracepoint. Those files exist on the target and no session has ever opened
   them. It would show that the SDMA writes landed and that chaining `pe` to a physical page is
   right. It does not produce a value we do not already have to the bit.
3. Cheap, while the machine is up: L8's display leftovers, GDS/OA, one more `ip_discovery` sample.

Not in this experiment, with reasons: the MMHUB VM path (hub 1 has never been used on this unit, and
our driver is gfx only), VM fault behaviour (wanted for TDR, not for stage B), and RADV's ioctl chunk
list (derivable from `ref/mesa`'s winsys without hardware).

## Recommendation

**Do not spend a boot on this alone.** Land the offline tools, write stage B's host tests and check
them against the 33348 recorded entries first. If something comes back REFUTED, that is a boot with a
specific question, which is a better session than a broad one. Otherwise fold the `hold` phase into
the next boot that happens for another reason: it needs no internet, no packages and no umr, only the
python3 the stick already carries, and it is about fifteen minutes.

## Hypotheses, and what would refute each

| # | Hypothesis | Refuted by |
|---|---|---|
| H1 | A sparse mapping produces an entry with `AMDGPU_PTE_PRT` set and no address | any other encoding in the dumped table page for `prt4k` |
| H2 | `AMDGPU_VM_PAGE_NOALLOC` on a mapping reaches the leaf entry as the NOALLOC bit | the bit clear in the leaf for `noalloc` while `rw` differs only in that flag |
| H3 | The bytes in VRAM equal the value the tracepoint described, `(addr + i*incr) \| flags` | any mismatch between `pt_walk.py`'s read and the `set_ptes` line for the same VA |
| H4 | Each buffer's first dword is readable at the physical address its own PTE names | the witness word missing or different - then the walk is wrong and nothing else in the phase counts |
| H5 | A 2 MB mapping at a 2 MB aligned VA becomes one `PDE_PTE` entry at PDB0 | an ordinary table of 512 4 KB entries instead |
| H6 | `far512g` forces a second PDB2 entry, so the root really is walked at four levels | the address resolving inside the same PDB1 subtree |
| H7 | With no submission, `GCVM_CONTEXT1..15_PAGE_TABLE_BASE_ADDR` stay zero even though our tables exist | a non-zero base with nothing submitted, which would mean the base is written at map time and not from the ring |

H7 is a control, not a discovery: E03 already read those registers as zero with no client, and both
ring dumps show the base being written from a packet. If it comes out otherwise, the ring decode is
wrong and so is the M8 plan.

## Safety

Read-only towards the GPU throughout. No register is written. Nothing is written to a disk, the
firmware or NVRAM. `tracefs` is the only thing outside `/tmp` this session writes to, and `disarm`
puts it back.

- `e17_vm.py` makes only the ioctls an ordinary client makes - `INFO`, `GEM_CREATE`, `GEM_MMAP`,
  `GEM_VA MAP/UNMAP`, `GEM_CLOSE`. **No context, no `CS`, no doorbell**: the GPU is never asked to
  execute anything, so this program cannot hang it. Struct layouts and ioctl numbers are imported
  from E13's `dispatch.json`, not retyped.
- `pt_walk.py` opens `amdgpu_vram` and `amdgpu_iomem` `O_RDONLY` and `pread`s only addresses a PTE
  just named. Address 0 is never read. The single glob is over `dri/*/amdgpu_vram`.
- `regs2.py` with `vmlists.json` reads 412 named registers through amdgpu's own accessor under its
  mutex. `gen_vmlists.py` cannot emit an `*_INVALIDATE_ENG*_SEM` (reading one acquires the
  semaphore, facts M25) or anything of MMEA - it filters them out by construction.
- The debugfs files that DO something when read - `amdgpu_test_ib`, `amdgpu_gpu_recover`,
  `amdgpu_evict_vram`, `amdgpu_evict_gtt`, `amdgpu_benchmark`, `amdgpu_preempt_ib`,
  `amdgpu_force_sclk`, `amdgpu_gfxoff`, `ras_ctrl` - are named nowhere in this directory, and no
  debugfs directory is globbed.
- Nothing opens `/dev/mem`.
- Deliberately excluded: `modprobe -r amdgpu` and a second `modprobe` (facts M42, M55),
  `amdgpu_gpu_recover` (M53 - this part has no working reset, so a hang ends at the owner's power
  button), suspend (M56), any clock change, anything needing a display server.
- Stop and cool down above 85 C. `temp()` runs at the end of each phase.

## The way in, and the way back

In, from the running Windows system, no keyboard at the machine (L18): set `default=<n>` in the
stick's `boot/grub/grub.cfg` (keep `.orig`) for the read-only diagnostic entry, restore
`efi/boot/bootx64.efi` from `.off`, then restart.

Back: `poweroff` and a **cold** start, never `reboot` - a warm restart out of the stick once left the
Realtek NIC "Not Present", and the M.2 link is intermittent across restarts (M21). Rename
`bootx64.efi` to `.off` again before the power-off so the machine falls through to NVMe.

The owner is needed twice: the power button if something hangs, and the cold start at the end.

## Procedure

`export BC250_SSH="ssh -i <key> -o UserKnownHostsFile=<file> root@<address>"` first; nothing
lab-specific is written down in this repository.

| # | Step | min | go / no-go |
|---|---|---|---|
| 1 | `./e17_pc.sh push` | 1 | the scripts are in `/tmp/e17s` |
| 2 | `./e17_pc.sh run pre` then `pull` | 2 | it is unit A, amdgpu is not loaded, `/tmp` has room |
| 3 | `./e17_pc.sh run load` then `pull` | 3 | `Initialized amdgpu`; the trace has the init's writes. Optional - it duplicates M41/M48 and is kept only as a cheap check that this boot behaves like the four before it |
| 4 | `./e17_pc.sh run base` then `pull` | 3 | 412 registers read; `CONTEXT1..15` base still zero with no client |
| 5 | `./e17_pc.sh run hold` then `pull` | 6 | **the phase this session exists for.** The holder maps twelve buffers; the positive control (H4) must pass before anything else in the phase is believed |
| 6 | `./e17_pc.sh run extras` then `pull` | 2 | L8's leftovers and a second `ip_discovery` sample |
| 7 | `./e17_pc.sh run info` then `pull` | 2 | the `AMDGPU_INFO` answers still match facts M46 |
| 8 | `poweroff`, owner cold-starts into Windows | 3 | Windows is back and the M.2 link is up |

About twenty minutes of machine time. If step 5's control fails, stop and pull everything: the walk
is wrong, and a wrong walk is worse than no walk.

## Stop conditions

- The witness word does not read back (H4 fails): stop the phase, keep the dumps, decide on the PC.
- Any `dmesg` line with `VM_L2_PROTECTION_FAULT`, `amdgpu: ring ... timeout`, or a GPU hang: stop
  everything, pull, and call the owner. There is no working reset on this part (M53).
- Edge temperature above 85 C: stop and let it cool.
- The probe stops answering: the streamed files are what there is (`e17_pc.sh stream`, E13's lesson
  from facts M42), and the machine needs the owner's power button.

## What is in this directory

```
README.md                      this
session.sh                     six phases on the probe
e17_pc.sh                      push / run / pull / stream, from the PC
e17_vm.py                      twelve buffers at known addresses, mapped and held, submitting nothing
pt_walk.py                     our page table walker and the positive control
check_pte.py                   the offline decode of the dumped pages, CONFIRMED / REFUTED per claim
gen_vmlists.py  -> vmlists.json    412 named VM registers of both hubs
gen_pte_bits.py -> pte_bits.json   the AMD entry layout, parsed from the kernel with a citation per field
offline/check_pte_from_trace.py    bc250_pte.h against 33348 entries already in evidence/
offline/ring_wrapper.py            the ring dumps we already have, decoded in full
```

`offline/` needs no lab and no session: it runs on this PC, today, and should run in CI so that a
change to `bc250_pte.c` which contradicts the recorded entries fails here.

Dropped after the inventory, and why: `umr_build.sh`/`umr_read.sh` (a fourth decoder of entries that
already have three), `ioctl_log.c`/`build.sh` (RADV's chunk list is derivable from Mesa's source),
`vktri.c` and its shaders (untested code, and M10 does not need it yet). The git history has them if
a later session wants them back.

## Result

Not run.
