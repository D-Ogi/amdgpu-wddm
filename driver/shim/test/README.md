# The shim's host tests

What each suite is, and where its long-form documentation lives. The shim itself, its contract and
the replay method are described in `driver/shim/README.md`; this file covers the tests as tests.

| Script | What it runs | Asks |
|--------|--------------|------|
| `run.ps1` | `replay.c` | M4: does the GART/GMC bring-up write what Linux wrote? |
| `run_psp.ps1` | `replay_psp.c` | M5 part A: the PSP ring and the firmware load. |
| `run_gfx.ps1` | `replay_gfx.c` | M5 part B and M6: GFX10 CP/KIQ, SDMA, the interrupt enables, the fences, the dispatch. Also ADR 0008 stage C: the indirect buffer and a VMID's page directory. |
| `run_ih.ps1` | `replay_ih.c` | the interrupt ring, its decode and its pointers. |
| `run_pte.ps1` | `replay_pte.c` | the page tables. |
| `run_sdma_faults.ps1` | `sdma_faults.c` | what the SDMA bring-up does when things fail. |

The five replays all ask one question - does the shim write what unit A's Linux driver wrote, in that
order - and they ask it on the path where everything works. Their verdict is EXACT MATCH or nothing.

## Stage C inside `run_gfx.ps1`

Two of the things ADR 0008 stage C adds cannot be replayed against anything, and the arm that checks
them says so in its own banner: amdgpu programs a VMID's page directory root per job, so no bring-up
window holds those registers, and it submits no indirect buffer in the windows we recorded either.
What `check_stage_c()` does instead is compare `bc250_gfx_emit_ib()` dword for dword with the packet
AMD's own field macros build, name the page-directory registers by their `CONTEXTn` names rather than
by the `ctx_addr_distance` arithmetic the code uses, and check that every refusal leaves the ring and
the register file untouched.

It also runs one submission end to end. `backend_mem.c`'s CP stub follows a `PACKET3_INDIRECT_BUFFER`
at VMID 0 - the flat GART aperture, the one address space a host allocator can stand in for - reads
the dwords at the address the packet names and executes them like any others, so the ring test built
as an IB reaches `SCRATCH_REG0` the long way round. The same submission at VMID 1 is the control: the
stub counts it as skipped, the register keeps its seed, and the fence behind the IB still lands,
because the fence is in the ring and only the IB is fetched through a VMID.

## `run_sdma_faults.ps1` - fault injection for the SDMA bring-up

The other question: what `bc250_sdma_setup()`, `bc250_sdma_start()`,
`bc250_sdma_fence_page_alloc()` and the undo path do when something does not work. Allocations that
fail one at a time, allocations that succeed without a CPU mapping, engines that come back holding a
write pointer that is not a pointer, and an engine that refuses after the other one is already
running. The report of the first full run is `P:\BC-250\scratch\tmp\sdma_faults_report.md`.

It shares nothing with the replays. `sdma_faults.c` brings its own allocator and its own register
file and links against `driver/shim` alone, so that an allocator instrumented to fail can never reach
`backend_mem.c` and change what a replay allocates. Two things it models rather than replays:

- **the allocator**, with a live-object table, a fault at the Nth call, and a "succeeded but could
  not map it" mode for the `cpu == NULL` case `bc250_shim.h` allows. Freed memory is not given back
  to the C library: it is filled with a poison byte and kept, so that a write into it after the free
  is a fact the suite states rather than a crash it might get away with. The free itself is modelled
  exactly as the two shipped backends do it, keyed on the CPU pointer, because one of the findings is
  about what that line does with an object that has none.
- **the register file**, one value per dword index, plus a forced-read table for the one thing E15
  measured that a register file cannot hold: an SDMA engine goes on answering with the write pointer
  it holds however often the bring-up writes 0 into it (facts M59/M60). That is how a corrupt
  preserved write pointer is put in front of the adoption code.

No register sequence may change to satisfy this suite. The sequences are confirmed write-for-write
against unit A's trace and `run_gfx.ps1` says so; this suite only fails the things around them, and
it checks that an error path writes no register a working bring-up would not - the set it compares
against is learned from a successful `bc250_sdma_hw_init()` in the same process, never typed.

### Expected failures

An expectation that states how a **confirmed defect** should behave is written with `check_defect()`
and carries the defect's id:

```c
	check_defect(r != 0, "D-01",
		     "bc250_sdma_setup should refuse a write-back page it cannot address");
```

While the defect stands the suite stays green and prints `D-01 (expected) ...` on every run, so the
defect says its name without turning the build red. The day it is fixed the expectation passes, the
suite reports `XPASS` and exits non-zero - the reminder to come back here and turn the marker into a
plain `check()`. The defect ids and what each one is are in the header comment of `sdma_faults.c`.
