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
test/                   the host replay test, see below
```

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

## Building

The same sources compile as ordinary user-mode C and with the WDK kernel flags the miniport uses
(`driver/kmd/build.ps1`); `BC250_SHIM_KERNEL` picks the kernel variant of the fixed-width types,
because the WDK's km CRT has no `<stdint.h>` and no `<stdbool.h>`. `bool` is one byte in both modes
so a struct the shim lays out has the same shape on both sides. MSVC only, C mode, `/W4 /WX` for our
own code.

The imported files are compiled with `/W4 /WX` too, minus two warnings turned off **from the build
line** (never by editing an import):

| Warning | Where | Why |
|---|---|---|
| C4244 | `gfxhub_v2_0.c`, `mmhub_v2_0.c` AGP and system-aperture writes | `soc15_common.h`'s register macros are a ternary whose SR-IOV arm takes a `u32`; the aperture values are 64-bit MC addresses. That arm is never taken here (`amdgpu_sriov_vf()` is false) |
| C4701 | `mmhub_v2_0.c` `mmhub_v2_0_update_medium_grain_clock_gating()` | On the IP 2.1.x branch it reads `def`/`data` without assigning them. Upstream defect, unreachable on GC 10.1.3, not ours to fix |

## The host replay test

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
