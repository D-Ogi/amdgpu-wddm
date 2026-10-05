# driver/shim/

The slice of the Linux/amdgpu API that AMD's imported IP-block code needs, so that
`driver/amdgpu-import/` compiles and runs unmodified (ADR 0002), plus the GMC bring-up sequence
built on top of it. Small on purpose: only what the imports actually touch, nothing speculative.

```
include/amdgpu.h        types, register macros, struct amdgpu_device and the hub descriptors
include/bc250_shim.h    the backend contract: register read, register write, udelay, log
include/bc250_gmc.h     the bring-up entry points the driver and the test both call
include/nv.h            stand-in for amdgpu's nv.h (one prototype)
include/gc/, mmhub/     forwarding headers, so "gc/gc_10_1_0_offset.h" finds third_party/linux-amdgpu/
shim.c                  the few entry points that cannot be macros
bc250_gmc.c             setup, gart_enable, gart_disable, flush_gpu_tlb - the code under test
include/amdgpu_psp.h    the slice of amdgpu_psp.h the imported psp_v11_0_8.c compiles against
include/amdgpu_ucode.h  the firmware file headers (common, gfx v1.0, rlc v2.0, sdma v1.0)
include/bc250_psp.h     firmware loading through the PSP: the entry points (milestone M5)
include/mp/             forwarding header for "mp/mp_11_0_8_offset.h"
bc250_psp.c             ring create/stop, SETUP_TMR, LOAD_IP_FW, firmware file parsing
include/bc250_gfx.h     GFX10 CP, KIQ and ring bring-up: the entry points (milestone M5 part B)
include/bc250_sdma.h    SDMA 5.0 ring bring-up: the entry points
include/bc250_irq.h     CP and SDMA interrupt enabling: the register half of amdgpu_irq_get()
include/bc250_nbio.h    the two NBIO 2.3 doorbell registers this milestone writes
bc250_ring.c            the ring write path: alloc, write, pad, commit, doorbell
bc250_gfx.c             golden registers, constants, RLC, MQDs, KIQ, the compute and gfx queues
bc250_sdma.c            golden registers, unhalt, context switch, the two SDMA GFX rings
bc250_irq.c             the CP and SDMA interrupt-enable callbacks and the two orders they run in
bc250_nbio.c            SDMA doorbell range and the doorbell self-ring aperture
include/bc250_pte.h     DXGK_PTE -> AMD page table entry: the entry points (milestone M7 Stage B)
bc250_pte.c             the translation, the inverse and the one-line description of an entry
generated/*.inc         golden register tables cut out of the reference imports, see below
test/                   the host replay tests, see below
```

`generated/` holds nothing written by hand. `tools/import/extract_table.py` copies a named C array
verbatim out of a reference-only import in `driver/amdgpu-import/` and writes it with a banner
giving the source file, the line range, the kernel tag and commit, and a sha256 of the body. The
replay tests re-run the extraction with `--check`, so an upstream change to a table is an error and
not a silent drift. Today there are two: the GFX10 and the SDMA5 golden register tables for Cyan
Skillfish.

Each definition in `amdgpu.h` is marked `[amdgpu]` (copied from AMD's MIT sources, with the file it
came from) or `[shim]` (ours). Nothing GPL-only is copied in: `lower_32_bits`, `min`, `max` and
`ARRAY_SIZE` live in GPL-2.0 kernel headers upstream and are written here from their documented
behaviour instead. `min`/`max` are plain ternaries because MSVC has no GNU statement expressions;
the imports only pass plain member reads, so the double evaluation cannot bite.

## The backend

Everything the imported code does to the hardware funnels through exactly two functions:

```c
unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index);
void         bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value);
```

`dword_index` is what `SOC15_REG_OFFSET` produces, so the byte offset inside BAR5 is
`dword_index * 4`. `RREG32`/`WREG32` and every `*_SOC15*` macro in `soc15_common.h` end up in these
two and nowhere else, which is what makes both backends possible:

- **host** - `test/backend_trace.c`: reads are answered from unit A's pre-driver register sweep,
  writes are recorded and compared with amdgpu's own trace. No hardware.
- **kernel** (later) - the miniport: `MmioRead`/`MmioWrite` in `driver/kmd/mmio.c`, which check
  every offset against the generated allow-lists (ADR 0007) before touching BAR5.

Two more functions complete the contract:

```c
void bc250_shim_udelay(unsigned int usec);                    /* amdgpu's udelay() */
void bc250_shim_log(int level, void *dev, const char *fmt, ...);  /* dev_err / dev_warn / dev_info */
```

`bc250_shim_udelay` is the only wait the shim ever does. In the kernel it is
`KeStallExecutionProcessor`; in the host replay it is a no-op, because nothing can change a replayed
register except the code under test. It is called from bounded poll loops only, never to sleep.

## The bring-up sequence

`bc250_gmc.c` is the hardware-independent part of the GART bring-up: the same object is compiled
into the replay test and into the miniport, so the driver runs the very code the test proves. It has
no statics and no globals, and uses no CRT beyond `memset`.

```c
int  bc250_gmc_setup(struct amdgpu_device *adev, const struct bc250_gmc_inputs *in,
                     struct amdgpu_bo *gart_bo);
int  bc250_gmc_gart_enable(struct amdgpu_device *adev);
void bc250_gmc_gart_disable(struct amdgpu_device *adev);
int  bc250_gmc_flush_gpu_tlb(struct amdgpu_device *adev, u32 vmid, u32 vmhub, u32 flush_type);
```

`bc250_gmc_setup` fills `adev` the way amdgpu's `gmc_v10_0_sw_init`/`_early_init` chain did on this
part: IP versions, `cyan_skillfish_reg_base_init()`, both hub function tables and their `init()`, the
VRAM and GART placement read off the hardware, the page-table geometry. It reads registers and writes
none. The caller zeroes `adev` (it may set `adev->dev` and `adev->backend` first; both survive) and
owns the `struct amdgpu_bo` that becomes `adev->gart.bo`. Everything it cannot derive from the
hardware comes in through

```c
struct bc250_gmc_inputs {
        u64  gart_table_mc;     /* MC address of the GART table   */
        u64  mem_scratch_mc;    /* MC address of the scratch page */
        u64  dummy_page_dma;    /* DMA address of the dummy page  */
        bool noretry;           /* policy, see below              */
};
```

`bc250_gmc_gart_enable` follows `gmc_v10_0_gart_enable()`: both hubs' `gart_enable`, both hubs'
`set_fault_enable_default(true)`, then an engine-17 TLB flush on MMHUB0 and one on GFXHUB0. It
returns 0 or a negative error code; like upstream it does not fail on a flush timeout.
`bc250_gmc_gart_disable` runs both hubs' `gart_disable` in `gmc_v10_0_gart_disable()`'s order.

`bc250_gmc_flush_gpu_tlb` mirrors `gmc_v10_0_flush_gpu_tlb()` with every poll bounded by
`adev->usec_timeout` (100000, amdgpu's default) iterations of `bc250_shim_udelay(1)`: it returns
`BC250_ETIME` if the MMHUB semaphore cannot be acquired or the invalidate acknowledge never arrives,
and it **always** releases the semaphore if it took one, the ack-timeout path included. Upstream also
holds `adev->gmc.invalidate_lock` around this; the shim has no locks yet, so the miniport must
serialize the calls itself until it does. That is noted in the source at the call site too.

## The GFX, SDMA and interrupt bring-up (milestone M5 part B)

Same arrangement as the GMC: the objects the replay test links are the objects the miniport links.
The sequence follows amdgpu's on this part, and every function in the four sources names the upstream
function and line it follows, at v6.18, commit `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`. The
upstream files themselves are in `driver/amdgpu-import/` as reference-only copies, so those citations
can be checked without a kernel checkout. Why they are transcribed and not imported is in that
directory's `PROVENANCE.md`.

```c
int  bc250_gfx_setup(struct amdgpu_device *adev, const struct bc250_gfx_inputs *in);
int  bc250_sdma_setup(struct amdgpu_device *adev);

int  bc250_gfx_hw_init(struct amdgpu_device *adev);      /* golden, CAM probe, constants, RLC, CP */
int  bc250_sdma_hw_init(struct amdgpu_device *adev);     /* golden, unhalt, ctx switch, both rings */
int  bc250_irq_init_mec_pipes(struct amdgpu_device *adev);
int  bc250_irq_hw_init(struct amdgpu_device *adev);
int  bc250_nbio_enable_doorbell_selfring_aperture(struct amdgpu_device *adev, bool enable);
int  bc250_irq_late_init(struct amdgpu_device *adev);

int  bc250_gfx_hw_fini(struct amdgpu_device *adev);      /* the first failure, or 0; see Undo */
int  bc250_sdma_hw_fini(struct amdgpu_device *adev);     /* BC250_EBUSY if an engine did not drain */
void bc250_gfx_teardown(struct amdgpu_device *adev);
void bc250_sdma_teardown(struct amdgpu_device *adev);
```

That is also the order of the E03 trace, and the order matters twice over: the rings live in GART
memory, so `bc250_gmc_gart_enable()` has to have run first, and the interrupt functions decide which
registers to walk from counts that `bc250_gfx_setup()`/`bc250_sdma_setup()` fill in, so on a zeroed
`adev` they would write nothing at all.

Two things in this stage are **not** amdgpu's:

- `bc250_irq_init_mec_pipes()` is amdkfd's `kgd_init_interrupts()`. On Linux the compute pipes belong
  to KFD, which enables their timestamp and opcode-error interrupts itself. This driver owns those
  pipes, so it has to do that work; the source says where it came from rather than passing it off as
  amdgpu's.
- `bc250_nbio_enable_doorbell_selfring_aperture()` belongs to `nv_common_hw_init()`, not to the gfx
  or SDMA IP block, which is why it is a separate call and not folded into either. It returns `int`
  where upstream returns `void`, and refuses an enable with `adev->doorbell.base == 0`: on Linux that
  field is filled long before any IP block runs, in the miniport it is 0 until the caller reads it
  out of the translated resource list, and EN = 1 over a zero base would point the window the GPU
  writes its own doorbells through at physical address 0.

Two things the sequence deliberately stops short of, each named in the headers rather than left to
be discovered: the SDMA ring test (`bc250_sdma_ring_test()` exists and follows upstream, but it is a
separate call, because it needs a scratch page the traced bring-up does not allocate and it touches
no register a replay could compare); and `gfx_v10_0_setup_grbm_cam_remapping()` (the CAM probe
returns "already remapped" on unit A, so the programming path has no traced counterpart and is not
transcribed - `bc250_gfx_grbm_cam_probe()` returns an error rather than carrying on quietly).
The fence and interrupt path is M6 and is the next section.

Where the 6.18.52 kernel that produced the trace and mainline v6.18 disagree, the trace decides and
the difference is written down. There is one in this stage: the `DB_RING_CONTROL` write that 6.18.52
added to `gfx_v10_0_constants_init()`. The full note, with the diff it comes from, is at that line in
`bc250_gfx.c`.

## The interrupt ring and the fence (milestone M6)

Every ring M5 brought up signals completion the same way: the engine writes a 32-byte interrupt
vector into a ring in system memory and raises the line. Until something reads that ring, a fence is
only a value in memory nobody is waiting on. `bc250_ih.c` is the reader, and
`bc250_gfx_emit_fence()` is the one thing that makes a vector appear on purpose.

```c
int  bc250_ih_setup(struct amdgpu_device *adev, bool msi);   /* allocates; writes no register */
void bc250_ih_teardown(struct amdgpu_device *adev);
int  bc250_ih_hw_init(struct amdgpu_device *adev);           /* navi10_ih_irq_init()    */
void bc250_ih_hw_fini(struct amdgpu_device *adev);           /* navi10_ih_irq_disable() */

/* DISPATCH_LEVEL: no lock, no allocation, no sleep */
u32  bc250_ih_get_wptr(struct amdgpu_device *adev, bool *overflowed);
int  bc250_ih_decode(struct amdgpu_device *adev, u32 *rptr, struct bc250_iv_entry *out);
void bc250_ih_set_rptr(struct amdgpu_device *adev, u32 rptr);

/* pure functions over a decoded vector */
void bc250_ih_eop_ring_id(const struct bc250_iv_entry *e, u32 *me, u32 *pipe, u32 *queue);
bool bc250_ih_is_gfx_eop(const struct bc250_iv_entry *e);
bool bc250_ih_is_compute_eop(const struct bc250_iv_entry *e);
bool bc250_ih_is_kiq(const struct bc250_iv_entry *e);
bool bc250_ih_is_sdma_trap(const struct bc250_iv_entry *e, u32 *instance);

/* the fence, in bc250_gfx.h */
int  bc250_gfx_signal_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags);
```

The three middle ones are the DPC trio and the constraint on them is absolute: the miniport's DPC
runs at `DISPATCH_LEVEL`, so they use `RREG32`/`WREG32`, one doorbell write and volatile reads of
the GTT ring, and nothing else. A DPC is `get_wptr` once, `decode` in a loop while the read pointer
differs from the write pointer, and `set_rptr` once at the end. The read pointer is the caller's,
not `adev`'s, which is a deliberate departure from upstream: upstream keeps it in the ring struct
and mutates it from one thread, and that is a rule a DPC cannot be given by a comment.

The contract is written at the top of `include/bc250_ih.h` and the host test checks it rather than
restating it. The trio reaches into `adev` at exactly two places, `adev->irq.ih` and
`adev->backend`; it takes no lock, allocates nothing, logs nothing, never sleeps, and reads
GPU-written memory through volatile pointers. The registers it can touch are these three and no
others, measured by a survey in `test/replay_ih.c`:

| Register | By | When |
|---|---|---|
| `OSSSYS.IH_RB_CNTL` (read and written) | `get_wptr` | overflow only, the two-write `WPTR_OVERFLOW_CLEAR` pulse |
| `OSSSYS.IH_RB_WPTR` (read) | `get_wptr` | only when the write-back slot says overflow |
| `OSSSYS.IH_RB_RPTR` (written) | `set_rptr` | only when `use_doorbell` is false |

With a doorbell and no overflow - the ordinary case - the trio touches no register at all.

Two details make that possible and both are upstream's, not inventions. The register offsets are
dword indices carried in `ih->ih_regs` and resolved once in `bc250_ih_setup()` (upstream's
`struct amdgpu_ih_regs`, filled by `navi10_ih_init_register_offset()`), so the trio never goes
through `SOC15_REG_OFFSET()` and therefore never touches `adev->reg_offset`. And
`struct amdgpu_ih_ring` holds no pointer into itself or into `adev`, so the miniport can give its
DPC a `struct amdgpu_device` of its own - zeroed, its own backend, `irq.ih` copied by value - which
is what it does, because the escape's `adev->backend` belongs to whoever holds the lock. The one
rule with that copy is that it must never be passed to `bc250_ih_setup()` or `bc250_ih_teardown()`:
`ring_mem` and `wb_mem` are copied too, and a teardown through the copy would free memory the
original still points at. The test runs the trio through exactly such a zeroed device.

`bc250_ih_set_rptr()` rings a **32-bit** doorbell (`bc250_shim_wdoorbell32`), which is upstream's
`WDOORBELL32` at `navi10_ih.c:499` and the only narrow doorbell in this driver - everything else
(`gfx_v10_0.c:8571` and `:8605`, `sdma_v5_0.c:389`) is 64-bit. A 64-bit store at the IH's index
would also write the next dword, and `BIF_IH_DOORBELL_RANGE` opens a two-entry window, so that
dword is inside it and would reach the hardware. The host backend records the width of every
doorbell and the test insists on it.

`bc250_ih_get_wptr()` is not a pure read. An overflow is acknowledged with a two-write pulse of
`IH_RB_CNTL.WPTR_OVERFLOW_CLEAR` and the read pointer is moved to the oldest entry the hardware has
not yet trampled; that is upstream's recovery, and `*overflowed` tells the caller it happened so it
can be counted rather than only logged.

What is deliberately left out of the transcription - ih1 and ih2, `ih_soft`, `IH_CHICKEN`,
`force_update_wptr_for_self_int()`, every SR-IOV arm, `IH_STORM_CLIENT_LIST_CNTL` and
`pci_set_master()` - is listed with an upstream citation each at the top of `include/bc250_ih.h`,
so that a reader comparing against `navi10_ih.c` does not have to guess which omissions are
decisions and which are oversights.

Two NBIO registers belong to this sequence and live in `bc250_nbio.c`, because they are NBIO's and
upstream keeps them in `nbio_v2_3.c`: `bc250_nbio_ih_control()` and
`bc250_nbio_ih_doorbell_range()`. `bc250_ih_hw_init()` calls them in upstream's order.

The fence is the smallest submission that both writes a value the driver can read and raises an
interrupt, which together are the proof that the rings and the interrupt ring are connected. A gfx
or compute ring gets a `RELEASE_MEM`; the KIQ gets two `WRITE_DATA` packets, because it has no
end-of-pipe and signals by writing `CPC_INT_STATUS`. `bc250_gfx_fence_page_alloc()` hands out one
GTT page of 8-byte slots for the value to land in - a separate call and not part of
`bc250_gfx_setup()`, because allocating it there would move every MC address the traced bring-up
programs. Without `AMDGPU_FENCE_FLAG_INT` the same packet writes the value and raises nothing, which
is the control worth running first on hardware.

The SDMA engines have the same pair, and they close M36's "SDMA: rings programmed and engines
released, not tested". `bc250_sdma_emit_fence()` / `bc250_sdma_signal_fence()` follow
`sdma_v5_0_ring_emit_fence()` (`sdma_v5_0.c:523`): a 32-bit `SDMA_OP_FENCE` with `MTYPE(3)`, the
address and the sequence, then an `SDMA_OP_TRAP` with interrupt context 0 when
`AMDGPU_FENCE_FLAG_INT` is set. `bc250_sdma_ring_test()` follows `sdma_v5_0_ring_test_ring()`
(`:1012`): a `WRITE_LINEAR` of `0xDEADBEEF` over a slot seeded with `0xCAFEDEAD`, then a poll.
`bc250_sdma_fence_page_alloc()` is the separate GTT page, for the same reason as the graphics one.
The packets are built out of `navi10_sdma_pkt_open.h`, AMD's own opcode and header macros, so no
SDMA dword is typed here either.

One thing an SDMA ring does differently and the shim has to know: its write pointer is in **bytes**,
not dwords. `sdma_v5_0_ring_set_wptr()` publishes `ring->wptr << 2` (`sdma_v5_0.c:371`), so
`amdgpu_ring_commit()` shifts for `AMDGPU_RING_TYPE_SDMA` and leaves CP rings alone. A doorbell rung
in the wrong units is a ring the engine reads at four times the offset.

## Building

The same sources compile as ordinary user-mode C and with the WDK kernel flags the miniport uses
(`driver/kmd/build.ps1`); `BC250_SHIM_KERNEL` picks the kernel variant of the fixed-width types,
because the WDK's km CRT has no `<stdint.h>` and no `<stdbool.h>`. `bool` is one byte in both modes
so a struct the shim lays out has the same shape on both sides. MSVC only, C mode, `/W4 /WX` for our
own code.

The imported files are compiled with `/W4 /WX` too, minus a short list of warnings turned off **from
the build line** (never by editing an import):

| Warning | Where | Why |
|---|---|---|
| C4244 | `gfxhub_v2_0.c`, `mmhub_v2_0.c` AGP and system-aperture writes | `soc15_common.h`'s register macros are a ternary whose SR-IOV arm takes a `u32`; the aperture values are 64-bit MC addresses. That arm is never taken here (`amdgpu_sriov_vf()` is false) |
| C4701 | `mmhub_v2_0.c` `mmhub_v2_0_update_medium_grain_clock_gating()` | On the IP 2.1.x branch it reads `def`/`data` without assigning them. Upstream defect, unreachable on GC 10.1.3, not ours to fix |
| C4245 | every use of `PACKET3()` in `bc250_gfx.c` (about thirteen sites) | AMD's macro in the imported `nvd.h` yields a signed `int` with bit 31 set, and every use assigns it to a `u32`. Adding `(u32)` casts inside the transcriptions would make our text differ from upstream for no behavioural reason, so the warning goes on the build line instead |

One warning is turned off in the source rather than on the build line: C4201, nameless union, raised
by the imported `amdgpu_doorbell.h`. It is wrapped in a `#pragma warning(push / disable / pop)`
around that one `#include` in our `include/amdgpu.h`, so `/W4` stays strict everywhere else. (M5
part A took the other route for `psp_gfx_if.h` and put `/wd4201` on the build line; the two should
be made to agree.)

## The host replay test: the GART (M4)

```
pwsh driver\shim\test\run.ps1
```

Builds everything into `P:\BC-250\scratch\m4shim` (never into the repository, never onto C:),
compile-checks the same sources with the kernel flags, extracts the reference trace with

```
python tools\trace\extract_phase.py evidence\linux\2026-09-21-E03-init-trace\amdgpu-events.txt \
       --match 'GCVM|GCMC|MMVM|MMMC' --until 0.26
```

and runs `replay.exe`. The test itself decides nothing about the hardware: it calls
`bc250_gmc_setup` and `bc250_gmc_gart_enable`, and compares every write they produce with the trace
by byte offset and value, in order. Then it calls `bc250_gmc_gart_disable` as a smoke check - the
traced window ends with the GPU running, so there is nothing to compare it against; all that is
checked is that it runs and reaches 38 registers, the 19 each of the two upstream `gart_disable`
functions.

Inputs, and where each comes from, are spelled out in `test/replay.c`. Only three cannot be derived
from unit A's hardware state, because they are allocations of that one amdgpu run: the MC address of
the GART table (from the `dmesg` line "PCIE GART of 512M enabled (table at 0x000000F5FFE00000)"),
the MC address of the scratch page and the DMA address of the dummy page (from the two traced
default-address registers). Everything else - the VRAM base and size, the GART placement, the AGP
aperture, the page table geometry - is read out of the pre-driver sweep or follows from amdgpu's own
code at v6.18.

### Result, 2026-09-21

285 writes produced, 285 compared against the trace, **0 mismatches**, 0 reads that unit A's sweep
had no value for. The 66 further writes in the trace window are the repeated engine-17 invalidations
that `amdgpu_gart_invalidate_tlb()` issues after `gart_enable` returns. `gart_disable` produced its
38 writes. Both TLB flushes time out against a replayed acknowledge register - the poll bound works -
and still emit the same three writes as the trace, semaphore release included.

Is the comparison actually sensitive, or would it match anything? Two controls say it is. The test
itself runs one: the same bring-up with `noretry` flipped, which reports exactly the 30 writes that
bit reaches and nothing else. And a negative control on the input side - rerunning `replay.exe` by
hand against a copy of the sweep with `GCMC_VM_FB_OFFSET` changed from `0x270` to `0x271` - turns the
exact match into 2 mismatches, which is precisely the two `CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32` writes
(one per hub) that a 16 MB shift of the VRAM physical base can reach, and no others:

```
copy the sweep, change that one line, then
replay.exe <doctored sweep> <sweep-run2> <trace-gart.txt>
```

### noretry: MEASURED true

`adev->gmc.noretry` is a policy bit rather than a hardware fact, so it is stated here from the
measurement, with the source that explains it.

**Measured.** In the E03 trace amdgpu writes `GCVM_CONTEXT1..15_CNTL` and `MMVM_CONTEXT1..15_CNTL`
as `0x007FFE07`. Bit 7 of those registers is `RETRY_PERMISSION_OR_INVALID_PAGE_FAULT`, which the hub
code sets from `!adev->gmc.noretry`. The bit is clear, so `noretry` was **true** on unit A.

**Why.** Unit A ran Alpine's `6.18.52-0-lts` (`evidence/linux/2026-09-21-E03-init-trace/dmesg.txt`
line 1), `amdgpu` was loaded with no module parameters and the kernel command line carries no
`amdgpu.noretry`, so `amdgpu_gmc_noretry_set()` chose the default. In 6.18.52 that function tests
`gc_ver >= IP_VERSION(10, 1, 0)`; in mainline v6.18, the tag we import the hub code at, the same line
reads `IP_VERSION(10, 3, 0)`. Unit A is GC 10.1.3, so the stable kernel gives `true` where mainline
would have given `false` - which is exactly what the trace shows, and why the value has to come from
the kernel that ran rather than from the tag the imports come from.

The test runs both values and prints both. With `true` the 285 writes are identical to the trace;
with `false` exactly 30 writes differ, all of them `GCVM/MMVM_CONTEXT1..15_CNTL` and all of them in
that one bit.

## The host replay test: GFX, SDMA and interrupts (M5 part B)

```
pwsh driver\shim\test\run_gfx.ps1
```

Same method as the M4 test above, one milestone further along. It builds into
`P:\BC-250\scratch\m5-gfx`, re-runs both golden-table extractions with `--check` so a drift between
`generated/*.inc` and the imports they were cut from fails the build, compile-checks the seven shim
sources with the kernel flags, and then runs `replay_gfx.exe` over `bc250_gfx_setup`,
`bc250_sdma_setup`, `bc250_gfx_hw_init`, `bc250_sdma_hw_init` and the four stage-10 entry points.

Two windows are compared, because on unit A this is two separate things a second apart:

```
python tools\trace\extract_phase.py evidence\linux\2026-09-21-E03-init-trace\amdgpu-events.txt \
       --match '^(GC|NBIO)\.' --reads --no-fold --precision 6 --since 0.5496 --until 0.5505
python tools\trace\extract_phase.py evidence\linux\2026-09-21-E03-init-trace\amdgpu-events.txt \
       --match '^(GC|NBIO)\.' --reads --no-fold --precision 6 --since 1.5601 --until 1.5609
```

The first is stages 1 to 9 in one `hw_init`. The second is stage 10: amdkfd's per-pipe enable, the
fence driver's `amdgpu_irq_get()` per ring, the self-ring doorbell aperture, and `gfx_v10_0_late_init`.
Both are compared write for write, in order, by byte offset and value.

Five things this replay does that the M4 one does not, each declared in the header comment of
`test/replay_gfx.c` rather than left for the reader to notice:

- **Reads come from the trace.** This window starts after the GART and the PSP have run, so the
  pre-driver sweep no longer describes the hardware. The *first* read of each register is answered
  from the trace's own read record; every read after that comes from what the run itself has written,
  as on hardware. Writes in the extract are never used as answers. A read with neither a traced value
  nor a sweep value is counted and reported, and the run is called inconclusive if there is one.
  There were none.
- **Addresses.** The MQDs, ring buffers, EOP buffers, writeback slots and the clear-state buffer are
  allocated by the test, so 24 registers carry an address that cannot equal the trace. They are listed
  by name, the way `experiments/E09-gart-enable/compare.py` lists its exceptions; their offset is still
  compared, and 12 of them are separately checked against the address the test actually handed out.
- **One declared register alias.** `gfx_v10_0_check_grbm_cam_remapping()` probes whether the CAM is
  already programmed by writing `VGT_ESGS_RING_SIZE_UMD` and reading `VGT_ESGS_RING_SIZE`. The trace
  shows that aliasing directly (`0.549735 W ... 0x30900 DEADBEEF`, `0.549737 R ... 0x088c8 DEADBEEF`),
  so it is declared to the backend, from those two lines and nothing else. Both offsets are resolved
  through AMD's headers, not copied from the trace.
- **A CP stub.** Eleven ring tests submit a PM4 packet and poll `SCRATCH_REG0`. A replayed register
  file has no CP, so `test/backend_mem.c` decodes the packets off the ring at the doorbell and applies
  the one it knows, `PACKET3_SET_UCONFIG_REG` of a single register, through a path the shim cannot
  reach. Wrong opcode, wrong offset, wrong count or wrong ring position and the poll times out.
- **Two declared drops in the interrupt window.** The leading `GRBM_GFX_CNTL` 8/0 pair - a select of
  me 2 pipe 0 with no register access between the halves, which nothing in this milestone produces and
  which is left unexplained rather than imitated - and the sixteen `GCVM_CONTEXTn_CNTL`
  read-modify-writes at 1.5608, which are `gmc_v10_0`'s late init, M4's code. Both are recognised by
  shape and the test fails if the window stops looking like that.

One thing in the second window is a stand-in rather than a reproduction, and is worth knowing before
trusting it: the four `CP_ME1_PIPEn_INT_CNTL` registers are read back with the value amdkfd's enable
put there through `CPC_INT_CNTL` under a GRBM selection, and the backend does not model that
aliasing. `include/bc250_irq.h` says why the trace cannot settle whether the aliasing exists. Those
reads are answered from the trace's read record like every other first read.

Four more things it checks, none of which the two windows can reach:

- **Stage 0**, `nv_common_hw_init`'s three NBIO writes at t = 0.0383, compared against their own
  traced accesses. Only one of the three matters here: `RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN` reads 0
  under Windows (`evidence/windows/2026-09-21-E02-run-001/sweep-NBIO.log`) and with it clear no
  doorbell reaches the CP, so the first ring test would hang. The two `REMAP_HDP_*_FLUSH_CNTL`
  writes program the alias amdgpu programs, and nothing on this part ever uses it -
  `amdgpu_device_flush_hdp()` returns immediately on an x86-64 APU that is not passed through
  (`amdgpu_device.c:7278`), and 1002:13FE is registered `CHIP_CYAN_SKILLFISH|AMD_IS_APU`
  (`amdgpu_drv.c:2181`). The trace agrees: across 146135 traced accesses the aliased dwords 0x1FC00
  and 0x1FC01 are never touched. They are transcribed for fidelity, not because anything depends on
  them, and the miniport calls `bc250_nbio_enable_doorbell_aperture()` alone.
- **The GART page table entries** written by `bc250_gart.c`. That file writes memory and no
  registers, so no trace can reach it. Checked against the formula in `amdgpu_gmc_set_pte_pde()` and
  the flag chain in `amdgpu_ttm_tt_pte_flags()`, plus an 8-page bind with non-contiguous bus
  addresses, the unbind values, and six refusals that must each leave the table untouched.
- **The compute MQDs against unit A's own.** `evidence/linux/2026-09-21-E03-init-trace/rings/` holds
  the eight compute MQDs amdgpu actually left in memory, 2048 bytes each, read out of debugfs. Each
  dump is matched to the ring that says it is that me, pipe and queue - the dumps sort by (pipe,
  queue) and the rings are ordered queue-major, so pairing them by index compares the wrong pairs.
- **Teardown and re-run.** `bc250_gfx_hw_fini`, then the rings freed and allocated again as the
  miniport's FINI and RUN do, then the whole bring-up on the same adev. On Windows a second PSP load
  in one boot leaves the RLC disabled and busy (facts M35), so the kmd gets one load per boot and
  everything after it has to be undoable with registers and the KIQ alone. The engines are checked
  halted afterwards against AMD's own field masks, and every register the two fini paths touch is
  surveyed against the trace windows the miniport's allow-list is generated from - the teardown has
  no window of its own, so what it touches can only be found by running it. The free and re-allocate
  is not decoration: gpumem does not hand a freed GART range back, so the second run's rings land
  where the first run's never were, and that is what turns a stale engine fetch into a page fault
  instead of a quiet walk through someone else's memory. Two things are asserted after the second
  bring-up: no stale fetch happened, and all nine queues' MQDs sampled a read pointer of 0. The
  second of those can fail on its own - with `bc250_kcq_clear_pointers()` taken out of the teardown,
  all eight compute MQDs come back with 256, the position the CP stub leaves a queue's read pointer
  at after a ring test.
- **A compute queue that does not answer its unmap.** `bc250_gfx_unmap_queues()` failing is logged
  and survived, which leaves `bc250_kcq_clear_pointers()` in front of a queue the CP may still be
  fetching from - and zeroing a running queue's read pointer is worse than leaving a stale one. The
  arm pins one queue's `CP_HQD_ACTIVE` through the `UNMAP_QUEUES` that names it
  (`backend_hqd_pin()`), then asserts both halves: that queue keeps its read pointer, the other
  seven are back to 0, and the teardown wrote eight of each pointer register rather than nine. With
  the active check taken out of the shim it fails on both counts, which is what says it is testing
  the check and not the weather.
- **A start where the last instance left the KIQ up.** What 0.6.1's teardown leaves behind, which
  makes it the state the first 0.6.2 start finds on the lab machine. The bring-up has to notice,
  recover and come back, taking exactly one stale fetch - inside the ring the previous instance
  left - and no more.

and one pass that reports rather than checks:

- **The same bring-up from the state Windows leaves behind.** Reads answered from
  `evidence/windows/2026-09-21-E10-run-001/sweep-GC-loaded-111010.log` instead of the Linux sweep,
  because the PSP does not merely load firmware: it starts the RLC and releases the SDMA halt while
  doing so (facts M34, M35). Every write that comes out different is listed. A difference is not a
  failure and the verdict does not depend on it - the point is to find the state-dependent
  read-modify-writes here rather than on the bench.

### Result, 2026-09-21

**354 + 35 writes produced, 389 compared, 0 mismatches**, 0 invented read values, 0 address registers
wrong. 14 doorbells rung, 35 PM4 packets decoded, 11 ring tests satisfied by the stub and 0 rejected,
and every one of the 481 dwords of the KIQ MQD outside the fields `gfx_v10_0_compute_mqd_init()`
assigns is zero.

Stage 0: 3 of 3 writes identical. GART: all checks pass.

Teardown: the engines read halted - `CP_ME_CNTL 0x15000000`, `CP_MEC_CNTL 0x50000000`, `RLC_CNTL 0`,
both `SDMAn_F32_CNTL` with `HALT` set - and of the 25 registers it touches, 24 are named by a trace
window. The one that is not is **`CP_HQD_DEQUEUE_REQUEST`**, which the KIQ dequeue writes and no
traced window names, so the miniport's allow-list needs that one offset added for the teardown.

Re-run: the whole bring-up succeeds a second time on rings that were freed and allocated again, 353
writes against the first run's 354, with 13 ring tests passing across the two, and no stale fetch
surviving the teardown. It touches **no register outside the trace windows**, so a second run needs
nothing added to the miniport's table.

The one-write difference is accounted for exactly, which is the point of checking it rather than
reporting a count: `RLC_SPM_MC_CNTL`, which upstream's own `pre_data != data` guard
(`gfx_v10_0.c:8297`) skips because the field already holds what the first run put there. The test
lists every register whose write count differs, in both directions.

Unclean start: from the state 0.6.1's teardown leaves - the KIQ active with the MEC halted and a
stale read pointer of `0x737` in the register - the bring-up recovers. It takes exactly one stale
fetch, inside the ring the previous instance left behind, dequeues the queue with the engine
un-halted for the handshake, corrects the read pointer to 0 in both the register and the MQD, and
succeeds in 360 writes.

**The MQD comparison found a real bug.** All eight compute MQDs now agree with unit A on every one of
the 20 comparable fields, but they did not at first: `compute_ring[0]` was missing
`cp_hqd_pipe_priority = 2`, `cp_hqd_queue_priority = 15` and `CP_HQD_PQ_CONTROL.TUNNEL_DISPATCH`.
`amdgpu_gfx_is_high_priority_compute_queue()` makes the first of several compute queues high
priority, and a comment in `bc250_gfx.c` had asserted that no high-priority queue exists here. Unit
A's dumps say otherwise: `amdgpu_mqd_comp_1.0.0` alone has `CP_HQD_PQ_CONTROL` `0xF030890A` where
the other seven have `0xD030890A`. A register trace could never have caught this - a compute MQD is
written by the CPU into memory and handed to the CP by address, so it appears in no register access.

Eleven MQD fields are excepted: ten carry an address this test chose, and `cp_hqd_active` is the
CP's, not the driver's - amdgpu writes 0 there for a compute ring and all eight dumps read 1, because
the dumps were taken after the queues had been mapped and run. For the same reason the dumps have 56
non-zero dwords outside the fields the MQD init assigns; those are counted and reported, not
compared.

The Windows-state pass reports **0 differing writes**, with three excluded by name. `CP_MQD_CONTROL`,
`CP_HQD_PQ_CONTROL` and `CP_HQD_PERSISTENT_STATE` are GRBM-windowed, and the Windows sweep contains
no `GRBM_GFX_CNTL` write, so it read the default queue slot and reported 0 for all three. Overlaying
those zeros produces three confident-looking differences that say nothing about unit A. Settling it
needs a sweep that selects me 2 pipe 1 queue 0 before reading the HQD block.

Four controls, one per claim the register sequence does not force by itself, and all four fail:

| Control | Result |
| --- | --- |
| `async_gfx_ring = false` | 98 mismatches, 355 writes |
| `pp_gfxoff = false` | 145 mismatches, 355 writes |
| the CP stub off | `gfx_hw_init` returns `BC250_ETIME` at the first ring test |
| one compute EOP enable per ring, not per pipe | 18 mismatches, 39 writes instead of 35 |

The last one is the evidence for the only judgement call in `bc250_irq_hw_init()`: it walks the four
compute pipes rather than the eight compute rings, because `amdgpu_irq_get()` refcounts per interrupt
type and two rings share each pipe's type. The naive per-ring loop is written out in the test, not in
the shim, and produces four writes too many.

### The dumps

`run_gfx.ps1 -Out <dir>` writes three files to `<dir>\dumps`, so that what the sequence puts in
memory can be read against the kernel's own definitions instead of taken on trust:

- `kiq-mqd.txt` - the KIQ MQD and the eight compute MQDs, field by field, offsets from `offsetof()`
  over the unmodified `struct v10_compute_mqd`, with every other dword checked to be zero.
- `kiq-pm4.txt` - the PM4 stream on the KIQ ring, decoded with the opcode numbers of the imported
  `nvd.h`: `SET_RESOURCES`, one `MAP_QUEUES` per ring, and the ring tests.
- `gfx-clear-state.txt` - the clear-state stream on the gfx ring. It is 949 dwords, which is the
  `RLC_CSIB_LENGTH` the trace writes (0x3B5), plus the framing packets.

## The host replay test: the interrupt ring and the fence (M6)

```
pwsh driver\shim\test\run_ih.ps1
```

Builds `bc250_ih.c` and everything it calls both ways - user-mode for the replay, and with the WDK
kernel flags, which for this file is not a formality because three of its functions run in a DPC -
and runs `test/replay_ih.c` against unit A's E03 window at 0.2528 s. That window is the whole of
`navi10_ih_irq_init()`: twenty accesses, fifteen of them writes, OSSSYS and NBIO only, because the
IH block comes up with the common IP long before the CP.

### Result, 2026-09-21

```
trace window: 15 writes, 3 offsets seeded with their traced read values
  bc250_ih_hw_init                    : 0
  writes produced / window            : 15 / 15
  mismatches                          : 0
  address exceptions (offset only)    : 4
  address registers wrong             : 0
  reads with no recorded value        : 0
```

Four address exceptions: `IH_RB_BASE`, `IH_RB_BASE_HI`, `IH_RB_WPTR_ADDR_LO` and `_HI` carry the
ring and the write-back page, which this test allocates. Each is separately checked against the
address the test handed out, so "not compared with the trace" does not mean "not checked".
`INTERRUPT_CNTL2` is deliberately **not** an exception: it carries the dummy page, which is an input
this test is given rather than an address it chooses, so it is compared in full and matches.

`hw_fini` writes three registers, and a second `hw_init` on the same `adev` produces the same
fifteen writes - the miniport needs that, because a PSP reload in one boot forces it.

Then, on the ring that bring-up produced:

- the decode, against vectors the test builds with every field set to a different value: all
  thirteen fields of `amdgpu_ih_decode_iv_helper()` round-trip, a me-0 and a me-1 end-of-pipe are
  routed apart although they carry the same client and source id, the KIQ's vector and an SDMA1 trap
  are each recognised, and the last entry of the ring leaves the read pointer at 0 rather than at
  `ring_size`;
- `get_wptr` reads the write-back slot and touches no register when there is no overflow, reports an
  overflow only when the register agrees with the slot, moves the read pointer to `wptr + 32`, and
  acknowledges with a set-then-clear pulse of `IH_RB_CNTL` - two writes, both required;
- `set_rptr` publishes the slot and then rings the doorbell, with no MMIO write at all, and the
  doorbell is 32 bits wide at index 0x2F0, not 64 - the backend records the width so this is
  measured and not taken from the source;
- the survey of every register offset the trio touches: `IH_RB_CNTL` read and written on the
  overflow path, `IH_RB_WPTR` read there, and `IH_RB_RPTR` written only in a separate run with
  `use_doorbell` forced false, so the third entry of the list is measured rather than asserted;
- the trio run again through a second `struct amdgpu_device` that is zeroed except for `irq.ih`
  copied by value, which is what the miniport's DPC gets, with the same results;
- a fence emitted on the gfx ring, on compute ring 3 and on the KIQ: each lands its value in a GTT
  slot and produces exactly one vector, which decodes back to that ring;
- a compute dispatch on compute ring 0, which is the whole of the M6 shader test (below).

### The compute dispatch arm

`bc250_gfx_dispatch_memset()` writes 82 dwords into the ring - an `ACQUIRE_MEM`, libdrm's 18-packet
dispatch, a `CS_PARTIAL_FLUSH` - and a fence behind them. The test asserts all 82 **against a table
of libdrm's own numbers**, taken from the packet-by-packet reading in
`P:\BC-250\scratch\m6\DISPATCH-NOTES.md` section 3.3 with a `shader_test_util.c` line for each row.
That comparison is the point of the arm: the shim resolves every offset through
`gc_10_1_0_offset.h`, libdrm writes the resolved numbers out by hand, and a wrong offset would still
be a well-formed packet that the stub would still execute. Nine of the 82 dwords are addresses or
caller arguments and are checked against what the test allocated and asked for instead.

Every constant in the table is also a measurement. The same 72 dwords were submitted to unit A under
Linux through raw ioctls and filled their buffer (fact M49); the dump of what was submitted is
`evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-windows/dispatch/g1.txt:14-85`, and
our submission differs from it only in the two addresses, the fill value, and `NUM_RECORDS` and
`DIM_X` with them. `NUM_RECORDS` follows the workgroup count, as libdrm derives both from the same
`dst.size`: for one workgroup the descriptor says 0x40 records, for sixteen 0x400, so the hardware
bounds the stores to the range the dispatch is meant to cover.

One of the constants is confirmed a second time inside our own tree: packet 5 is
`0xc0017900, 0x7b, 0x20`, and those three dwords appear verbatim in `preamblecache_gfx10[]` in
`third_party/libdrm/shader_code_gfx10.h`, a different file reaching the same header encoding and the
same offset arithmetic.

The CP stub in `backend_mem.c` then executes the packets: it keeps the compute state the
`SET_SH_REG`s build, and on `DISPATCH_DIRECT` it checks the dispatch shape, `COMPUTE_SHADER_EN`,
that `COMPUTE_TMPRING_SIZE` is 0 (nothing programs a scratch base), that the two CU-mask packets and
the one uconfig packet came first, that the address in `COMPUTE_PGM_LO/HI` resolves to an allocation
whose words `memcmp` equal `bufferclear_cs_shader_gfx10`, and that the descriptor's stride is 16.
Only then does it write the records, clamped to `NUM_RECORDS` the way `OOB_SELECT` clamps them
rather than wrapping. So "the shader was copied correctly, to a 256-aligned address, and the address
was shifted the right way" is a tested statement and not an assumption.

What the arm asserts, beyond the 82 dwords: one workgroup writes its 1024 bytes and **nothing past
them** (`bc250_gfx_dispatch_check()` insists the tail is still `0xCAFEDEAD`, so a dispatch that
overran is as loud as one that did nothing), 16 workgroups fill the whole 0x4000 bytes, exactly one
dispatch was carried out, the fence behind it landed and raised exactly one vector, the padding
`amdgpu_ring_commit()` adds is all NOPs, and the whole submission produces **zero register writes**.

Five controls, each of which must fail: a dispatch of 0 workgroups, one of 17, and one on the gfx
ring are all refused with the ring untouched; with the CP stub turned off the destination keeps its
seed and `check()` fails at offset 0, which is what says the bytes came from the dispatch and not
from the setup; one corrupted dword is found at its own offset; and a shader with one word changed
is refused by the stub instead of being dispatched.

One model is declared and one is inherited. The declared one is `IH_RB_CNTL` bit 31,
`WPTR_OVERFLOW_CLEAR`, which the hardware retires as the write lands: the trace puts `C03101A0` in
at 0.252838 and reads `403101A0` back at 0.252845, and the last read-modify-write of the sequence
depends on it. `backend_add_selfclear()` corrects only the value a later read returns; the write is
still compared exactly as the driver issued it. The inherited one is the CP stub in `backend_mem.c`,
extended here to execute a `RELEASE_MEM` and deliver a vector.

Both controls fail, as they must: with MSI off the sequence produces `RPTR_REARM = 0` and two writes
differ; with the self-clearing bit not modelled the final `IH_RB_CNTL` comes out `C03301A1` instead
of `403301A1`. A fence without `AMDGPU_FENCE_FLAG_INT` writes its value and delivers nothing, and a
misaligned 64-bit fence and a 64-bit KIQ fence are both refused with nothing written to the ring.

What this test cannot say is that the ASIC raises the interrupt, or that it runs the shader. No
hardware raises an interrupt or executes an instruction anywhere in it: the stub's encoder is the
test's own code, and the "dispatch" is a `for` loop writing what the shader would write. What is
established is that the encode and the decode agree, that the packet the shim emits is the packet
upstream's emitter builds, and that the dispatch the shim emits is libdrm's dispatch dword for
dword. The rest is the first hardware run's job, and it is what the fence exists for.

## The host test: DXGK_PTE to an AMD page table entry (M7 Stage B)

```
pwsh driver\shim\test\run_pte.ps1
```

`bc250_pte.c` is the translation the full miniport needs and nothing else: under WDDM's GpuMmu
model VidMm owns the page tables and hands the driver one abstract entry at a time through
`DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE`, and each has to become the 64-bit word the hardware
walks. It reads and writes no register, allocates nothing and is safe at any IRQL, so there is no
trace to replay and no sweep to load; the whole input space is its arguments.

What takes the place of a replay is the two things it is not free to invent.

**Microsoft's half.** `replay_pte.c` includes the real `<d3dkmthk.h>`, builds a real `DXGK_PTE`,
sets one field at a time and asserts that each of the eleven lands exactly where the
`BC250_DXGK_PTE_*` macro says, that they tile all 64 bits of `Flags`, that the struct is 16 bytes
and that `PageAddress` and `PageTableAddress` are the same storage. Every translation case then
goes through a real `DXGK_PTE` rather than a hand-assembled flags word. No bit position in
`bc250_pte.h` rests on memory, and if Microsoft moves one the suite fails instead of the driver
mapping the wrong page.

**Our half.** `bc250_pte_gart_flags()` must equal `bc250_gart_pte_flags()` from `bc250_gart.c`, the
path that put `0x0003000000000077` in unit A's table and that the CP then fetched rings and MQDs
through (facts M33 and M37), and both must equal that number. `bc250_gart.c` is linked into the
test for exactly that comparison, so the two cannot drift.

The header states plainly which half of this is measured and which is transcription. The VMID 0
GART entry is measured. The multi-level tables for VMIDs 1..15 - four levels, block size 9, the PDE
carrying `VALID | SYSTEM | SNOOPED` and no permissions, `MTYPE_NC` instead of the GART's `MTYPE_UC`
- are transcribed from `gmc_v10_0.c`, `gfxhub_v2_0.c` and `amdgpu_vm*.c` at tag v6.18, with the
experiment that would settle each named next to it. It also records the three questions the DDI
does not answer: whether `PageAddress` is a byte address or a frame number, which segment id means
system memory, and which page table level is the leaf. All three are context parameters rather than
constants, and a wrong choice is a refused translation instead of a wrong page.

Eleven translations are asserted with the expected entry written out in full, twenty malformed
inputs are refused with nothing written, and every successful entry survives the round trip through
`bc250_pte_decode()`. Four controls, each a wrong implementation the rest of the suite would still
pass: the two readings of `PageAddress` must produce entries that differ by exactly the shift; the
two apertures must differ in exactly bits 50:48; `LargePage` must become `AMDGPU_PTE_FRAG(4)` and
not be dropped, which would otherwise give a valid 4 KB entry at the head of a 64 KB mapping; and a
directory entry must carry no permissions and no memory type.

What this test cannot say is that the walker accepts any of it. Nothing here has run on the ASIC:
the GART half is the same code that has, and the VM half is a careful transcription waiting for a
VMID 1 context to be brought up against it.

## What the miniport calls, in order (M5 part B)

The whole of milestone M5 part B, as the kernel-mode driver sees it. Everything takes
`struct amdgpu_device *adev`, which the miniport owns and zeroes once; everything returns 0 or a
negative `BC250_E*`. Nothing here takes a lock, allocates, maps, or sleeps - see "Serialization" in
`include/bc250_gfx.h`.

### Once per boot, before anything rings a doorbell

| Call | Needs beforehand | What it does |
| --- | --- | --- |
| `bc250_nbio_enable_doorbell_aperture(adev, true)` | nothing | One write, `RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN`. Without it no doorbell reaches the CP and the first ring test hangs. **This is the one the miniport calls.** |
| `bc250_nbio_hw_init(adev)` | `adev->rmmio_base` set to BAR5's physical address | The whole of `nv_common_hw_init()`'s three writes: the HDP flush alias as well as the doorbell aperture. Transcribed for fidelity; the alias is unused on this APU (see stage 0 above), so the miniport takes the single call instead. |

### Setup: allocate, read the hardware, write nothing

| Call | Needs beforehand | What it does |
| --- | --- | --- |
| `bc250_gmc_setup(adev, &in, &gart_bo)` | the GART table's MC address (M4) | Learns the memory layout. Reads only. |
| `bc250_gfx_setup(adev, &in)` | `bc250_gmc_setup`, a working `bc250_shim_mem_alloc` | Allocates the MQDs, rings, EOP and clear-state buffers; fills in the ring descriptors. |
| `bc250_sdma_setup(adev)` | as above | Allocates the two SDMA rings and their writeback slots. |

`struct bc250_gfx_inputs` carries the four numbers that are not readable from the hardware
(`max_shader_engines`, `max_sh_per_se`, `max_cu_per_sh`, `max_backends_per_se`) and the two module
parameters (`async_gfx_ring`, `pp_gfxoff`); the header says what the trace shows for each and which
one is stated rather than measured.

Memory comes from the miniport through `bc250_shim_mem_alloc(adev, domain, size, align, out)` in
`include/bc250_shim.h`. `BC250_MEM_VRAM` is the local frame buffer by MC address; `BC250_MEM_GTT` is
system pages the miniport has mapped through the GART with `bc250_gart_bind()`, whose PTEs must be in
place and the TLB flushed before the GPU touches them.

### Bring-up

`bc250_gfx_hw_init(adev)` then `bc250_sdma_hw_init(adev)`. The GFX one is five stages, each exposed
separately so a staged hardware run can stop after any of them and read the register state back:

| Stage | Call | Safe stopping point? |
| --- | --- | --- |
| 1 | `bc250_gfx_init_golden_registers` | yes - writes only, nothing is running |
| 2 | `bc250_gfx_grbm_cam_probe` | yes - writes a pattern and reads it back |
| 3 | `bc250_gfx_constants_init` | yes - writes only |
| 4 | `bc250_gfx_rlc_resume` | yes, and the most useful one: the RLC is running and the CP is still halted, which is the state E10 left the GPU in |
| 5 | `bc250_gfx_cp_resume` | **no** - it unhalts the CP, maps every queue through the KIQ and runs eleven ring tests. It either completes or leaves queues half mapped. |

`bc250_sdma_hw_init` is one step and is independent of the GFX stages.

### Undo

`bc250_sdma_hw_fini(adev)` then `bc250_gfx_hw_fini(adev)`. Frees nothing. The SDMA half also reports:
after the halt it reads both engines' read and write pointers and returns `BC250_EBUSY` if either
engine stopped with work still queued, because `F32_CNTL.HALT` says the engine stopped, not that it
finished, and a half-executed queue still names pages the caller is about to hand back. The GFX half
follows
`gfx_v10_0_hw_fini()` in its order: the three fault interrupt sources off, `UNMAP_QUEUES` through the
KIQ for the gfx ring and the eight compute rings with the ring test upstream does after it, **the
eight compute queues' pointer registers put back**, **the KIQ's own HQD dequeued while the MEC still
runs**, then the CP and the MEC halted. The RLC stop at the end is not upstream's and is there for
the miniport, which needs a way to stop the RLC before asking the PSP to load firmware again;
`bc250_gfx_rlc_stop(adev)` exposes that step on its own.

It returns the first failure it saw, or 0. Every step runs whatever the earlier ones did, so the
value is for the caller to report rather than to act on - there is nothing left to try. The one that
carries information is `BC250_ETIME` from the KIQ dequeue: the MEC did not answer the handshake, so
the next bring-up will have to go through the recovery branch instead.

What it leaves behind, measured by the host test rather than asserted: `CP_ME_CNTL = 0x15000000`,
`CP_MEC_CNTL = 0x50000000`, `RLC_CNTL = 0`, both `SDMAn_F32_CNTL` with `HALT` set. It touches 25
distinct registers, 24 of them already named by a trace window. The exception is
**`CP_HQD_DEQUEUE_REQUEST`**, which the KIQ dequeue writes: the miniport's generated allow-list needs
that one offset for the teardown.

#### Why the KIQ dequeue is there

The KIQ's own HQD is the queue nothing upstream dequeues: `gfx_v10_0_hw_fini()` unmaps the client
queues through the KIQ but never the KIQ itself, and the only write of `CP_HQD_ACTIVE = 0` in
`gfx_v10_0.c` is the SR-IOV one at `:7029`. That is safe on a part that drops GFX power across a
teardown or takes a reset before the next load. This one does neither, and hardware said what that
costs (E12 run 002, unit A):

**The MEC keeps its own copy of an active queue's base and read pointer. Writes to the `CP_HQD_*`
registers while it is halted do not reach that copy, and on un-halt it resumes from it.**

The second bring-up in that run programmed the new ring under halt and un-halted; the engine went
back to the *first* run's ring, which had been freed, and UTCL2 raised a VMID 0 fault at `0x444000`
= the first run's base `0x442000` plus about where its read pointer stood. An address no register in
either run names. The KIQ ring test then timed out after 165 ms.

So the teardown dequeues the KIQ while the engine can still answer: `CP_HQD_DEQUEUE_REQUEST = 1`,
poll `CP_HQD_ACTIVE` to 0, then the pointer registers zeroed. The handshake is upstream's own
(`kgd_hqd_destroy()`, `amdgpu_amdkfd_gfx_v10.c:606-619`); only the place it is called from is new.
There is no documented MEC reset to use instead - `kgd_gfx_v10_hqd_reset()` returns 0 without
touching anything, and `SOFT_RESET_CPC` exists only from gfx11 on.

#### And why the other eight queues get their pointers cleared

A second, quieter half of the same problem, and it is about a register rather than an engine.
`bc250_compute_mqd_init()` samples `CP_HQD_PQ_RPTR` under each queue's own selection, as upstream
does, so each of the eight compute MQDs is built from whatever that queue's register holds. After a
bring-up that ran the ring tests the queues idle at `rptr == wptr != 0` (fact M40), `UNMAP_QUEUES`
is not known to clear them, and nothing else in the undo did - so the second bring-up would hand
each queue an MQD saying most of the ring is pending, which is the 5222 us walk of E12 run 001, nine
times over. `bc250_kcq_clear_pointers()` zeroes `CP_HQD_PQ_RPTR`, `CP_HQD_PQ_WPTR_LO` and
`CP_HQD_PQ_WPTR_HI` for all eight after the unmap, so the sentence "the value it samples is put
there here" holds for all nine queues. All three registers are already inside a trace window, so
this adds nothing to the miniport's allow-list.

No handshake is needed for these, and none would be possible: unlike the KIQ's own HQD, these queues
have already been taken off their pipes by a running CP, and `bc250_gfx_unmap_queues()` ends in a
KIQ ring test, so the `UNMAP_QUEUES` packets are known to have completed before the registers are
written.

One deviation from upstream's behaviour is left, and it is the fallback for a KIQ found active with
the MEC already halted - what 0.6.1's teardown leaves behind, so the first 0.6.2 start on a machine
that ran 0.6.1 goes through it. There `bc250_kiq_init_register()` un-halts the MEC for the length of
the handshake, halts it again, and zeroes the read pointer instead of writing back the previous
instance's. `driver/amdgpu-import/PROVENANCE.md` carries it in full, including why a
`GRBM_SOFT_RESET.SOFT_RESET_CP` pulse is written down there rather than shipped.

Two things that were declared deviations in 0.6.1 are gone. Writing `CP_HQD_ACTIVE = 0` under halt
did nothing to the engine, which is the whole point of the measurement above; and
`mqd->cp_hqd_pq_rptr = 0` in the MQD builder treated a symptom, so it is back to upstream's sample
now that the teardown leaves a 0 there to sample. The refutation is kept in PROVENANCE rather than
deleted.

The host test carries both arms (`check_rerun()` and `check_unclean_start()` in
`test/replay_gfx.c`), and both rest on the fourth declared model,
`backend_add_mec_fetch_state()` - a replayed register file has no MEC and forgets everything the
moment a register is overwritten. `test/backend_mem.h` states what was measured for it and the one
point that was not: that a dequeue serviced by a running MEC clears the copy. That is what both the
teardown and the fallback assume, and the next hardware run is what tests it.

The same model answers reads of `CP_HQD_PQ_RPTR` from the queue `GRBM_GFX_CNTL` has selected, which
the register file cannot do - it holds one value per offset, so the nine queues would share a read
pointer and the arm above could not tell "the teardown put all nine back" from "the teardown put one
back". It answers only for queues a ring test has actually run on, so a cold boot still reads unit
A's own seeded value and the 354-write comparison is untouched.

### Interrupts (M6)

The ring comes first, and early: on unit A `navi10_ih_irq_init()` runs at 0.2528 s, before the GART
and long before the CP. So `bc250_ih_setup(adev, msi)` and `bc250_ih_hw_init(adev)` belong with the
common IP bring-up, not after the CP. `msi` is `!!adev->irq.msi_enabled` upstream; under Windows it
is what the INF's `MessageSignaledInterruptProperties` produced, and the caller states it rather
than this code assuming. `bc250_ih_hw_init()` needs the GART up (the ring is GTT memory) and
`adev->dummy_page_addr` set.

| Call | Needs beforehand | What it does |
| --- | --- | --- |
| `bc250_ih_setup(adev, msi)` | `bc250_shim_mem_alloc` | 256 KB GTT ring plus a page for the two write-back shadows. No register. |
| `bc250_ih_hw_init(adev)` | the above, the GART up, `dummy_page_addr` set | The fifteen writes of `navi10_ih_irq_init()`. |

The source enables come after the rings exist:
`bc250_irq_init_mec_pipes(adev)`, `bc250_irq_hw_init(adev)`,
`bc250_nbio_enable_doorbell_selfring_aperture(adev, true)`, `bc250_irq_late_init(adev)`, in that
order - it is the order unit A has and `bc250_nbio_enable_doorbell_selfring_aperture` really does sit
between the two irq calls, because upstream runs it from a different IP block. These only enable
sources in the CP and SDMA blocks.

The miniport owns the ISR and the DPC. The DPC is:

```c
bool overflow;
u32 wptr = bc250_ih_get_wptr(adev, &overflow);
u32 rptr = adev->irq.ih.rptr;            /* get_wptr may have moved it, on overflow */
struct bc250_iv_entry e;

while (rptr != wptr) {
        if (bc250_ih_decode(adev, &rptr, &e) != 0)
                break;
        /* route: bc250_ih_is_gfx_eop(&e), _is_compute_eop, _is_kiq, _is_sdma_trap */
}
bc250_ih_set_rptr(adev, rptr);
```

`set_rptr` once at the end, not once per entry.

### The first interrupt, on purpose

With the ring up and the matching source enabled, one call raises one end-of-pipe:

```c
bc250_gfx_fence_page_alloc(adev);
u64 addr = bc250_gfx_fence_addr(adev, 0);
bc250_gfx_signal_fence(&adev->gfx.gfx_ring[0], addr, seq, AMDGPU_FENCE_FLAG_64BIT | AMDGPU_FENCE_FLAG_INT);
/* then: bc250_gfx_fence_read(adev, 0) == seq, and one vector on the interrupt ring */
```

Run it once without `AMDGPU_FENCE_FLAG_INT` first: the value must land and nothing must arrive. That
separates "the CP executed the packet" from "the interrupt path works", which are two failures worth
telling apart on a part nobody has driven under Windows before.

## Firmware through the PSP (M5)

On 1002:13FE amdgpu loads all GPU firmware through the PSP (`AMDGPU_FW_LOAD_PSP`, `psp_v11_0_8`,
no autoload, no boot-time TMR): it creates the kernel-mode ring of the PSP's trusted OS, sends
`GFX_CMD_ID_SETUP_TMR`, then ten `GFX_CMD_ID_LOAD_IP_FW` (SDMA0, SDMA1, CE, PFP, ME, MEC1, MEC1 jump
table, MEC2, MEC2 jump table, RLC). Eleven submissions, which is the count on `MP0_SMN_C2PMSG_67` in
the E03 trace, and their spacing there (0.1 ms for the jump tables, 2 ms for SDMA, 6 ms for the
260 KB images) fits the image sizes.

`bc250_psp.c` is the hardware-independent part, compiled into the host test and into the miniport
alike. The register traffic is AMD's `psp_v11_0_8.c`, unmodified. The command path follows
`amdgpu_psp.c` function by function (each one names its upstream counterpart); two deliberate
deviations: a PSP status other than 0 is an error (upstream only warns), and the fence is polled
with a 50 us busy wait. The owner supplies the memory: ring, command buffer and fence page with CPU
pointer and MC address, the TMR's MC address, and wherever it staged the images.

`test/replay_psp.c` (`pwsh driver\shim\test\run_psp.ps1`) runs it against a model of the PSP. The
model answers the ring create as unit A's PSP answered amdgpu, parses every ring frame and command
buffer by `psp_gfx_if.h`, and writes the fence. Result: all 28 mailbox accesses of amdgpu's PSP
phase, reads included, reproduced in order and value, with no exception list (the ring sits at the
MC address amdgpu used); eleven well-formed commands with amdgpu's firmware types and the sizes the
file headers give; two control runs (a PSP that stays silent, a PSP that refuses a command) stop
the sequence where they should. What the model cannot show is what the real PSP accepts; that is
the hardware experiment's job. The firmware files are linux-firmware's
`amdgpu/cyan_skillfish2_*.bin`, kept outside this repository; their versions equal what amdgpu
reported on unit A (E01).
