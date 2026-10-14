# Kernel GDI hardware acceleration (HLK gap G1)

GDI hardware acceleration is the WDDM feature that moves the kernel GDI drawing operations of the
Canonical Display Driver onto the GPU. The WDDM feature caps table marks it mandatory for a full
graphics driver and for a render only driver. The Hardware Lab Kit tests it with the job
`GDIHW_Functional test for GDI HW ACC (v1.1)`, native and WoW64. Row G1 of
[docs/testing/hlk-certification.md](../testing/hlk-certification.md) records the gap this page
closes.

The driver answers the feature through one capability bit and one DDI function. The bit is
`DXGK_PRESENTATIONCAPS.SupportKernelModeCommandBuffer`. The function is `DxgkDdiRenderGdi`, which
translates a command buffer of GDI operations into one DMA buffer of GPU commands.

## State of this work

Both increments are in the driver.

Increment 1 carries the contract layer, the reference model of every operation, the command stream
of the two operations that the command processor performs exactly, and the host tests.

Increment 2 carries the six compute kernels for the blends, the stretch, the transparency and the
ClearType blend, as gfx1013 machine code inside the driver image, and the dispatch that runs them.
`Bc250GdiKernelsPresent` in [gdi_dispatch.c](../../driver/kmd/gdi_dispatch.c) is the one place that
reports their presence. The capability bit, the start log line and the operator gate all read that
function, and the gate `EnableGdiHwAccel` is on when the registry says nothing.

So this driver now sets `SupportKernelModeCommandBuffer`, which promises all seven operations, and
clears `DriverSupportsCddDwmInterop`.

This work goes to the train after tester.31. The reason is the capacity question of the review, and
[What bounds the command buffer](#what-bounds-the-command-buffer) answers it from the operating
system binaries.

Nothing of this has run on the lab. The host tests run the kernels' own source text on this
processor, which proves the arithmetic and the addressing and says nothing about the instructions
the compiler chose. The lab plan is
[docs/testing/gdi-hw-accel-lab.md](../testing/gdi-hw-accel-lab.md).

## Two contract sentences that shape the design

The first sentence is the return table of `DXGKDDI_RENDERGDI`. It has two rows. `STATUS_SUCCESS`
means "The submitted command is well-formed". Every other value "will lead to the OS bugcheck".

The second sentence is the guaranteed contract DMA buffer model. "The display miniport driver must
be able to translate one command buffer into exactly one DMA buffer and one patch-location list."
The same page adds that a driver which answers out of DMA buffer space makes the VRAM manager of
the operating system bugcheck the system.

Together they remove every way to report a problem. `Bc250WddmRenderGdi` therefore returns
`STATUS_SUCCESS` on every path. It never answers `STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER` and it
never uses `MultipassOffset`. A command that the driver cannot translate is counted, logged and
dropped. A dropped operation leaves a visible artefact on the screen, which is the smaller harm.

This is also why the admission in [gdi_render.c](../../driver/kmd/gdi_render.c) is exhaustive and
host tested. A mistake there cannot be answered with an error code.

## RenderGdi, not RenderKm

The GDI acceleration pages name `DxgkDdiRenderKm`. That function takes a `DXGKARG_RENDER`, which
carries a patch location list and a physical address. `DXGKDDI_RENDERGDI` "is used when submitting
Windows Graphics Device Interface (GDI) commands for contexts that support virtual addressing".
Every context of this adapter declares `VirtualAddressing`, because the adapter declares GpuMmu.

`DXGKARG_RENDERGDI` has no patch location list at all. It carries `DmaBufferGpuVirtualAddress`
instead. So the absent `DxgkDdiPatch` and the unconditional
`ContextInfo.Caps.NoPatchingRequired = 1` of `Bc250WddmCreateContext` stay as they are. The
allocation list entries carry each allocation's GPU virtual address, and the driver writes those
addresses into the commands directly.

## What the contract requires

`DXGK_RENDERKM_COMMAND` is a variable size structure. `OpCode` names the operation and
`CommandSize` gives the bytes to the next command, "including the value of OpCode and the number of
sub-rectangles in the command". Each operation carries a destination rectangle and an array of
sub-rectangles in destination coordinates. The sub-rectangles are what the driver paints.

| OpCode | Operation | Arithmetic | What this driver does |
|---|---|---|---|
| 1 | `BITBLT` | four binary raster operations over the source and the destination | `SRCCOPY` with equal extents is a DMA_DATA copy. The other three need a kernel |
| 2 | `COLORFILL` | seven raster operations over a colour and the destination | `PATCOPY` is a DMA_DATA fill. The other six need a kernel |
| 3 | `ALPHABLEND` | the Win32 `BLENDFUNCTION` tables | a kernel |
| 4 | `STRETCHBLT` | a point resample of the source | equal extents is a copy. A real stretch needs a kernel |
| 5 | `ESCAPE` | none. "Driver ignores this command" | counted and skipped |
| 6 | `TRANSPARENTBLT` | a colour key test for each pixel | a kernel |
| 7 | `CLEARTYPEBLEND` | a per channel blend through a gamma table pair | a kernel |

The limits that matter for the admission:

- A destination rectangle may overhang the destination surface. A sub-rectangle may not, and "all
  sub-rectangles are guaranteed to fit inside the destination surface".
- A `BITBLT` source rectangle "can exceed the bounds of the source surface". An `ALPHABLEND` or
  `STRETCHBLT` source rectangle never does. `TRANSPARENTBLT` states neither, and its Remarks bound
  every source sub-rectangle by `SrcRect`, so this driver holds it to the stricter rule.
- The `SrcPitch` and `DstPitch` members apply to `D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE` and to
  `D3DKMDT_GDISURFACE_EXISTINGSYSMEM` only. "Pitch should be ignored for other allocation types".
- `AlignmentShift` must be 2 or more. The driver reports 2, so every pitch is a multiple of four
  bytes.
- `MaxTextureWidthShift` and `MaxTextureHeightShift` 2 with `DXGK_TEXTURE_SIZE_SHIFT` 11 report
  8192 pixels. The admission refuses a larger extent.
- The gamma allocation of the ClearType blend has a fixed shape. It is 8 bits a pixel and 512 by 16
  pixels. Each row holds a gamma table of 256 entries and an inverse gamma table of 256 entries.
- `Gamma` is a row index from 0 to 15, or `D3DKM_INVALID_GAMMA_INDEX`. The two values select two
  different blend formulas.

Two geometric rules are exact, and the host test checks both against an independent oracle. The
`BITBLT` Remarks translate a sub-rectangle into the source as
`SrcSubRect.left = SubRect.left - DstRect.left + SrcRect.left`. The `STRETCHBLT` Remarks resample as
`Xs = truncate((Xd - DstRect.left + 0.5) * Ws / Wd + SrcRect.left)`.

[gdi_render.c](../../driver/kmd/gdi_render.c) evaluates the second rule as exact integer arithmetic.
`Xs = SrcLeft + (2 * (Xd - DstLeft0) + 1) * Ws / (2 * Wd)` is the same number for every extent pair
the admission allows. A fixed point step would not be, and the clipped sub-rectangle keeps the
original destination origin, so a clip never moves the sampled pixels.

## The candidate designs

Five designs reach a working feature. The driver takes design E.

**A. Hand written gfx1013 shader code.** Write each blend as GFX10 assembly, assemble it with the
local LLVM, and dispatch it. Rejected. The code is large, the project forbids hand written register
addresses, and nothing on the development PC can run a gfx1013 instruction. The arithmetic would
reach the lab unchecked.

**B. Report the feature and refuse the hard operations.** Set the capability bit and translate only
the copy and the fill. Rejected on the contract. `DXGK_PRESENTATIONCAPS` has no per operation
"cannot" bit. The only refusals it offers are the `No*SameBitmap*` family, which name one
allocation on both sides. Once the bit is set the operating system sends all seven operations, and
the return table gives no way to decline one.

**C. Perform the blends on the CPU inside RenderGdi.** Map both surfaces and write the pixels with
the processor. Rejected for three reasons. It is not hardware acceleration, which is the
requirement. It breaks the order of the GPU work, because the pixels would change before the
queued commands that read them run. The texture and staging allocation types are not CPU visible,
so the mapping does not exist for the surfaces the CDD uses.

**D. A graphics pipeline pass for each blend.** Draw a full screen triangle with a pixel shader per
operation. Rejected. This driver programs no graphics pipeline state at all, and a first graphics
pipeline on this part is a much larger step than a compute dispatch. The compute path already has a
positive control on the lab.

**E. Compute kernels, written once and compiled twice.** Write the per pixel arithmetic of all seven
operations once, in C that compiles as OpenCL C and as host C. Compile it for gfx1013 at build time
with the local AMDGPU clang, and compile the same text into the host test. Add the DMA_DATA fast
paths for the three cases the command processor performs exactly. Chosen.

Design E answers the two reasons the other four fail. The host test proves the exact arithmetic that
the GPU runs, because both come from one source text. The dispatch costs a fixed number of command
dwords for each sub-rectangle, which is what the guaranteed contract needs. A copy of rows costs
seven dwords for each row, so its cost has no bound that the command buffer size can predict.

## The mapping onto gfx1013

Two operations map onto the command processor exactly. Both are byte operations with no arithmetic.

- `BITBLT` with `SRCCOPY`, equal extents and one channel order is `PACKET3_DMA_DATA` with
  `SRC_SEL = SRC_ADDR_USING_L2` and `DST_SEL = DST_ADDR_USING_L2`. One packet carries one row.
  [gdi_emit.c](../../driver/kmd/gdi_emit.c) reuses the span emitter of
  [gfx_copy.c](../../driver/kmd/gfx_copy.c), which its own host test covers.
- `COLORFILL` with `PATCOPY` is the same packet with `SRC_SEL = DATA`. The dword that would hold the
  low source address holds the fill pattern. The field values come from the imported
  [nvd.h](../../driver/amdgpu-import/nvd.h), whose own comment documents the DATA selector.

A `STRETCHBLT` whose extents are equal is the copy case as well, because the resample is then the
identity translation.

Everything else is a compute dispatch. The arithmetic of all seven operations is
[gdi_pixel.h](../../driver/kmd/gdi_pixel.h), which compiles as OpenCL C, as kernel C and as host C
from one text. The kernels are [gdi_kernels.cl](../../driver/kmd/gdi_kernels.cl), one entry point
for each arithmetic class of `bc250_gdi_kernel`, and they hold no arithmetic of their own: what is
in them is which pixel a work item owns, where that pixel lies in each allocation, and the
destination to source transform.

The work group is 8 by 8 work items, which is 64 and so two whole wavefronts of 32 lanes. The grid
is the rectangle rounded up to whole work groups, and the last group of a row or a column drops its
overshoot itself. That one bound is the only check in a kernel: every other bound was proved on the
processor before the dispatch was emitted.

Two measured properties of this part shape the design. The compute path has a working positive
control on the lab (facts M49). The part has no working GPU reset (facts M53), so a kernel that
hangs the graphics queue costs a restart of the machine. Each dispatch therefore covers one
sub-rectangle and nothing more.

### How the code gets into the driver image

[tools/gpu/gdi_kernels.py](../../tools/gpu/gdi_kernels.py) compiles the kernels with clang 22.1.8 of
`toolchain/llvm-amdgpu-22.1.8` for `amdgcn-amd-amdhsa` and `-mcpu=gfx1013`, and writes
`driver/kmd/gdi_kernels.generated.h`. Three kinds of number are in that header, and none of them is
typed by hand:

- the code, one slice for each kernel, each slice at a 256 byte boundary because `COMPUTE_PGM_LO`
  carries the program address shifted right by eight. 5120 bytes in all, 1280 dwords.
- the kernel descriptor fields the dispatch programs: `COMPUTE_PGM_RSRC1` and `RSRC2`, the argument
  block size and the work group shape, read out of the code object the compiler emitted.
- the packet offsets of every `COMPUTE_*` register the dispatch writes, from
  [tools/regcalc](../../tools/regcalc), and the `COMPUTE_DISPATCH_INITIATOR` bits from AMD's own
  field masks. A `SET_SH_REG` offset is the register's dword index less
  `PACKET3_SET_SH_REG_START`. For `mmCOMPUTE_PGM_LO` that gives 0x20C, which is the number libdrm
  writes out as a literal. That agreement is the positive control of the whole derivation.

The kernels are OpenCL C and not kernel C because the driver has no compiler at run time and the
miniport is built with the Microsoft compiler, which has no AMDGPU target. A code object is
therefore data, built once on this processor and carried in the image.

The build is the gate as well. `--check` rebuilds the kernels and compares the whole generated
header byte for byte, and where the device compiler is absent it still holds the recorded hash of
the source against the source in the tree, so an edited kernel with a stale header fails on any
machine. The gate also refuses a code object that uses scratch memory, local data share, a call or
a flat address, that spills a register, that asks for a work group of another shape, or whose
argument list is not the one the dispatch writes. Every one of those is a condition the dispatch
depends on: it programs no scratch descriptor and no local data share size.

### Where the code lives, and why it may live there

A kernel mode GDI context owns nothing but the DMA buffer the operating system hands it. There is no
allocation of ours to keep a shader in, no `DxgkDdiCreateAllocation` call of our own in this path,
and the guaranteed contract model gives one buffer for one command buffer. The code therefore goes
into the DMA buffer itself, once per buffer, as the payload of a `PACKET3_NOP` that the command
processor skips.

That is only sound if the buffer's pages are executable, and on this adapter they are:
`WddmGpuMmuCaps` leaves `NoExecuteMemorySupported` clear, and the operating system reads it that way
(`evidence/windows/2026-09-21-E18-run-003/kmtprobe-234530.txt:15` and the E19 runs report
`flags 0x00000000 (ReadOnly 0, NoExecute 0, CacheCoherent 0)`). No mapping of this adapter can carry
a no execute bit, so no page of it can refuse to run an instruction.

Two placements were rejected. `InternalGpuVirtualAddressRange` is the range the operating system
uses for its own internal resources, not a range a driver may put code in. A device context
allocation through `DxgkCbCreateContextAllocation` would need `INIT_CONTEXT_RESOURCE` and residency
work of its own, and would still depend on the same executable mapping.

The instruction cache and the scalar cache are invalidated by the acquire at the head of the
buffer. Its `GCR_CNTL` carries `GLI_INV` and `GLK_INV` beside the level two invalidate. The processor
wrote the code and the arguments before the submission, and the command processor runs that acquire
before it reaches the dispatch, so the order is the one the hardware needs.

### The dispatch sequence

[gdi_dispatch.c](../../driver/kmd/gdi_dispatch.c) writes, for each admitted sub-rectangle: a
`PACKET3_NOP` whose payload is the argument block of 104 bytes, then `SET_SH_REG` writes of
`COMPUTE_TMPRING_SIZE` (no scratch), `COMPUTE_REQ_CTRL`, the four `COMPUTE_STATIC_THREAD_MGMT_SE*`
registers, `COMPUTE_PGM_LO` and `PGM_HI`, `COMPUTE_PGM_RSRC1` and `RSRC2`, `COMPUTE_PGM_RSRC3`, the
three `COMPUTE_NUM_THREAD_*`, `COMPUTE_USER_DATA_0` to `5` and `COMPUTE_RESOURCE_LIMITS`, then
`PACKET3_DISPATCH_DIRECT` with the grid and the initiator, then a `PACKET3_EVENT_WRITE` of
`CS_PARTIAL_FLUSH`. The shader type bit of every compute packet is what makes those register writes
name the compute space from the graphics ring.

The user data registers are what the kernel descriptor asked for: `s[0:3]` a private segment buffer
descriptor, written as zeros because the kernels have no private segment, and `s[4:5]` the pointer
to the argument block. The flush behind the dispatch makes the waves retire before anything behind
it reads the destination, which the next item of the same buffer or the following present may do.

The sequence follows the positive control of
[bc250_dispatch.c](../../driver/shim/bc250_dispatch.c), with three deliberate differences, because
this is an indirect buffer on the graphics ring and not a submission of ours to a kernel owned
compute queue:

- `COMPUTE_START_X/Y/Z` is not written. The initiator carries `FORCE_START_AT_000`, which is what
  RADV does for a grid with no offset, and a bit in our own packet cannot be left over from another
  context.
- `CP_COHER_START_DELAY` is not written. It is a uconfig register, which is ring wide state this
  driver does not own in a GDI buffer, and amdgpu sets it when it brings the block up.
- the compute unit mask goes out as an ordinary `SET_SH_REG` and not through the undocumented index
  3 of `SET_SH_REG_INDEX`. Mesa writes those two registers the plain way.

The initiator also carries `CS_W32_EN`, because the compiler built these kernels for a 32 lane
wavefront and the build gate pins that. A dispatch of wave32 code with that bit clear would run the
wrong program.

## How RenderGdi builds the DMA buffer

`Bc250WddmRenderGdi` in [wddm.c](../../driver/kmd/wddm.c) is the only layer that sees a WDK type.
The steps:

1. It admits the call. The context must be the GDI context, the node must be the graphics node, and
   the DMA buffer must be present, aligned and large enough.
2. It counts the sub-rectangles of the whole command buffer and settles the capacity of the call
   before it writes anything, which is the subject of the next section.
3. It walks the command buffer. `Bc250GdiCommandStep` is the structure of one command, and both
   walks use it and nothing else. It holds every offset inside `CommandLength` through
   `Bc250GdiWalkStep`, refuses a `CommandSize` that the buffer does not hold, and answers whether
   the command's sub-rectangle array lies inside the command itself.
4. It flattens each command into `BC250_GDI_OP`, which holds the fields of all six argument
   structures. `C_ASSERT` lines tie every plain constant of the contract layer to the WDK values.
5. It resolves the allocation list indices that the command names. Each index gives one surface
   record and one GPU virtual address. The lookup copies both descriptors under the object lock,
   the same discipline the GPU present producer uses.
6. It admits the operation and plans each sub-rectangle. `Bc250GdiAdmitOp` and `Bc250GdiPlanItem`
   prove that every byte the commands touch lies inside its allocation.
7. It offers each admitted sub-rectangle to [gdi_batch.c](../../driver/kmd/gdi_batch.c), which owns
   everything the DMA buffer holds: the one cache operation in front of the first item, the code
   block in front of the first dispatch, the choice between the engine path and a dispatch, and the
   tail padding to the 32 byte granule of the ring. The cache operation goes first because the CDD
   writes its bitmaps with the processor, and the acquire makes those writes visible to the engine's
   level two cache read, its instruction reads and its scalar reads.
8. It advances `pDmaBuffer` by what the batch wrote.

The two walks must agree command for command, and one function is how that is kept true. The count
of step 2 and the translation of step 3 to 8 read the same buffer. A command the translation drops
may not be counted, because the count is the budget of the whole call, and a call whose budget
cannot be guaranteed draws nothing. The counting walk once added a command's stated sub-rectangle
count before the check which holds that array inside the command. One command with a malformed count
could therefore refuse every good command beside it, which review r2 of 2026-10-14 found. The count
is now the count of what the translation will draw. That is also what keeps the number inside the
input's own bound: an admitted command of `L` bytes carries at most `(L - MinBytes) / 16`
sub-rectangles, because the array is inside the command. A host case set holds
`Bc250GdiCommandStep` to each answer, the malformed count among them, and the case set fails when
the containment check is removed.

`DxgkDdiSubmitCommandVirtual` needs no new arm. A GDI DMA buffer is a non present, non user mode
command buffer with bytes in it, which the existing stage C submits to the graphics ring. The GDI
context gets its root page table from `DxgkDdiSetRootPageTable` like any other context.

### The capacity of one call

The guaranteed contract DMA buffer model gives one DMA buffer for one command buffer and nothing
else: "The display miniport driver must be able to translate one command buffer into exactly one DMA
buffer and one patch-location list. If this translation is not possible, the user-mode command
buffer is, by definition, invalid. The display miniport driver cannot return status that indicates
it is out of DMA buffer space"
([using-the-guaranteed-contract-dma-buffer-model.md](https://learn.microsoft.com/windows-hardware/drivers/display/using-the-guaranteed-contract-dma-buffer-model),
local copy at revision 110f60ea). `DxgkDdiRenderGdi` has no status for it either: its table is
`STATUS_SUCCESS`, and every other value bugchecks. `DXGKARG_RENDERGDI` does carry a
`MultipassOffset`, which the `DxgkDdiRender` family uses together with
`STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER`, but that status is not in this function's table, so this
driver does not return it. Whether the operating system would accept it here is a question for the
lab and not for a shipped driver.

So capacity cannot be a per-item question, and a review of increment 1 reproduced exactly what
happens when it is. A 2 by 8192 ColorFill with two disjoint 1 by 8192 sub-rectangles spent 57344
dwords on the first column, and the second column no longer fitted and was dropped from a call that
reported success. The rows of one sub-rectangle are O(height), and nothing bounded them against the
sub-rectangles behind them.

Three rules fix the class, and [gdi_batch.c](../../driver/kmd/gdi_batch.c) holds all three:

1. **Every admitted sub-rectangle has a bounded cost.** One compute dispatch is 80 dwords, 81 with
   the worst-case alignment padding, whatever the rectangle's size. The per-row DMA_DATA path is
   cheaper for a short rectangle and unbounded for a tall one, so it is an optimisation and never an
   obligation: an item whose rows do not fit the dwords left over after the bounded cost of
   everything behind it is dispatched instead. `Bc250GdiItemToKernel` names the kernel that performs
   the same operation, which is `gdi_bitblt_rop` at SRCCOPY for a copy, `gdi_stretchblt` for a 1:1
   StretchBlt and `gdi_colorfill_rop` at PATCOPY for a fill.
2. **The whole batch is checked before anything is written.** `Bc250GdiBatchBegin` takes the number
   of sub-rectangles the command buffer holds and refuses the call unless the cache operation, the
   code block, one bounded dispatch for each of them and the ring padding all fit. A refused call
   writes an empty DMA buffer: it draws nothing, rather than drawing a prefix and dropping the rest.
   `GdiRenderBatchRefused` counts those calls and the start log names the numbers.
3. **The DMA buffer is sized so that rule 2 cannot fire.** The size is not chosen by hand. It is
   computed from the largest command buffer this driver undertakes to translate, by the arithmetic
   of rule 1 itself (`Bc250GdiContextDmaBytes`).

Rule 1 is what makes rule 2 a contract instead of a hope. The invariant is stated and proved in
`gdi_batch.c`: after `Begin` has said yes, no admitted sub-rectangle can be refused for want of
space. `BC250_GDI_BATCH_NO_SPACE` is therefore unreachable, and the host test asserts that over a
set of batches that includes the counterexample above.

#### Why rule 2 can never be reached

A refusal that returns success and draws nothing is a lost drawing call, so a bound on the input is
needed, not a larger cutoff. The DDI gives one. `DXGK_RENDERKM_COMMAND.CommandSize` is "the size of
the current command, in bytes. This is equal to the number of bytes from the beginning of
DXGK_RENDERKM_COMMAND up to the next command", and the same page's Remarks say that "each command
varies in length depending on the value of the **OpCode** member and the number of sub-rectangles in
the command"
([DXGK_RENDERKM_COMMAND](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_renderkm_command),
local copy `ref/ddi-display/d3dkmddi.md` at lines 27128 and 27169, WDK 10.0.26100). A length can
depend on how many sub-rectangles it has only if the array is inside the command, and `CommandSize`
reaches to the next command, so the arrays of different commands cannot overlap. `WddmGdiCommand`'s
caller enforces that reading already: a `pSubRects` that does not lie between the end of the
command's fixed part and the start of the next command is counted in `GdiRenderOutside` and the
command is dropped, because it is the one input that cannot be bounds-checked any other way.

The smallest command that can carry a sub-rectangle at all is ColorFill, at 8 bytes of `OpCode` and
`CommandSize` plus a 40-byte argument. The only smaller command is Escape, which the driver ignores
and which carries no sub-rectangle. So, with `L` for `CommandLength`:

```
sum over commands of 16 * NumSubRects <= L - 48        so    N <= (L - 48) / 16
capacity needed = acquire + codeMax + N * dispatchMax + granule - 1 = Bc250GdiCallDwords(L)
```

That is a constant factor of the input: 324 bytes of DMA buffer per 16 bytes of command buffer,
20.25 to one. So a DMA buffer of `Bc250GdiContextDmaBytes(bound)` bytes translates every command
buffer up to `bound`, and `Begin` cannot refuse inside it - not merely for the inputs anyone has
tried, but for every input the producer is able to build. The host test asserts it by walking every
possible `CommandLength` from 0 to the bound.

`BC250_GDI_CMDBUF_BOUND` is 96 KiB, and the numbers that follow from it are: 6141 sub-rectangles in
one call, 1998848 bytes of DMA buffer (499712 dwords), in the aperture segment the context already
used. The review's case - one ColorFill with 793 disjoint one-pixel sub-rectangles, which the
hand-picked 256 KiB buffer refused whole - is 12736 bytes of command buffer and is translated with
1.1 per cent of the buffer used. The densest call the bound admits uses 8.6 per cent, because the
budget reserves a full dispatch for each sub-rectangle while a one-pixel rectangle actually takes
the seven-dword row path. The buffer is sized for the worst case, which is 6141 rectangles that
each need a dispatch.

#### What bounds the command buffer

The DDI does not publish the size of the command buffer that a GDI context receives. The producer of
that buffer does. The canonical display driver `cdd.dll` is the only producer of these commands. It
allocates the buffer, it fills it, and dxgkrnl gives its length to this driver without a change. The
number is therefore a fact of the operating system binaries, and it was read from them.

The binaries read are `cdd.dll` 10.0.22621.6630 with `dxgkrnl.sys` 10.0.22621.6199, and `cdd.dll`
10.0.26100.9444 with `dxgkrnl.sys` 10.0.26100.9444. These are the 23H2 line of the lab and the 24H2
line. The 22621 file was taken by the image identity of the lab's own copy. The binaries and their
disassembly stay outside this repository, because they are Microsoft's property. The addresses below
are virtual addresses of the loaded image, as the public symbols of each build give them.

The producer's own buffer, the same at both builds:

- the producer's buffer constructor allocates the buffer
  with `EngAllocMem` and the size that its caller gives.
- the producer's device start creates the two buffers of the device, each with the literal size 0x10000.
- the producer's buffer reset puts the write pointer at the base of the buffer, and the
  limit at the base plus the literal 0x10000.
- Each command builder computes `0x30 + 16 * count` and refuses the command if the write pointer plus
  that size passes the limit. the ColorFill builder does it  and. the BitBlt builder, the TransparentBlt builder,
  the StretchBlt builder, the AlphaBlend builder and the ClearTypeBlend builder use the same
  arithmetic, the same limit and the same gate on the accumulated area.
- the producer's flush submits the bytes that were written, which is what it wrote. It puts that number at offset
  0x10 of the submission structure, the buffer pointer at 0x20 and the allocation list at 0x30.
- dxgkrnl's GDI entry gives that structure to dxgkrnl's render step as a `D3DKMT_RENDER`, eight bytes
  in. The submitted length lands in `CommandLength`.
- dxgkrnl's render step fills `DXGKARG_RENDERGDI`. The command address is `pNewCommandBuffer` plus
  `CommandOffset`, and `CommandLength` is copied with no test of its value.

So the `CommandLength` of a `DxgkDdiRenderGdi` call is at most 65536 bytes at both builds.
`BC250_GDI_CMDBUF_BOUND` of 96 KiB is above that maximum with half of it again to spare. The largest
command the producer can send is 65536 bytes, and it carries at most 4093 sub-rectangles.

The size is a literal of the producer. No step of that chain reads a value that a display miniport
driver gives, so the `DmaBufferSize` of our GDI context has no part in it.

What the producer does when a batch does not fit is this. The builder flushes the buffer and returns
0. Its caller then calls
the same builder a second time, as its fill callback does.
The buffer is empty at the second call, so each command of at most 65536 bytes is accepted. No
drawing is lost by the refusal, and a length above 65536 bytes cannot arrive.

Two more bounds of the same chain are worth the record:

- dxgkrnl refuses a GDI submission of more than 32 allocations, although the producer's own allocation list holds 256 entries.
- the producer's internal-bitmap start creates a third command buffer of 512 bytes, although the buffer reset gives each buffer the limit of 64 KiB. That
  difference is Microsoft's business. It can only make a submitted length smaller.

The bound is thus the producer's and not a guess, for these two Windows lines. A later Windows could
raise it, so the three measures below stay in the driver:

- `GdiRenderPeakCommandLength` records the largest `CommandLength` the field ever sends, so the
  bound becomes a measured number rather than a guess. Trial 3 of
  [the lab plan](../testing/gdi-hw-accel-lab.md) reports it.
- A call whose command buffer is larger than the bound is counted in `GdiRenderCapacityShort` and
  logged with its length and the capacity it got. It is a measurement and not a decision: `Begin`'s
  own exact check still decides, and it accepts such a call whenever the sub-rectangles actually in
  it fit. The same event raises `GdiCmdBytesWanted`, and every GDI context created afterwards asks
  for a DMA buffer large enough for it, so one short call cannot repeat for the rest of the boot.
- If a call is both above the bound and full enough that `Begin` refuses it, that call draws nothing:
  the rectangles it would have painted keep the pixels they had, until the next paint of that area
  reaches the driver. There is no honest alternative inside this DDI. There is no status to ask for
  more space (the return table is success only), the `MultipassOffset` continuation of the
  `DxgkDdiRender` family is not documented for this function, and a processor fallback is impossible
  here because `RenderGdi` receives GPU virtual addresses and no mapped pointers, with no callback it
  may use to map one.

`CreateContext` asks for the computed buffer only while the capability is reported, so a start
without the kernels allocates what it did before. `DXGK_CONTEXTINFO` says of this size that "the DMA
buffer can never shrink smaller than the starting size that **DmaBufferSize** specifies", and that
it can grow. The computed size is therefore a floor, and each call's own `DmaSize` is what the
translation uses.

## The allocation types and the caps that change

`D3DKMDT_GDISURFACETYPE` has eight values. `WddmGdiAllocationPolicy` in
[gdi_private.h](../../driver/kmd/gdi_private.h) refuses every type above 4, which is unchanged by
this work. The contract layer admits types 1 to 4 and names them rather than trust that policy.

| Type | Name | Admitted | Note |
|---|---|---|---|
| 1 | `TEXTURE` | yes | the DDI pitch member is ignored for it |
| 2 | `STAGING_CPUVISIBLE` | yes | the one type whose DDI pitch member applies |
| 3 | `STAGING` | yes | |
| 4 | `LOOKUPTABLE` | yes | the gamma table pair of the ClearType blend |
| 5 | `EXISTINGSYSMEM` | no | refused at `CreateAllocation` |
| 6 | `TEXTURE_CPUVISIBLE` | no | the redirection bitmap of BD-060 |
| 7, 8 | the cross adapter pair | no | refused at `CreateAllocation` |

A surface whose record carries GDI type 0 is a legacy surface of the compositor or the CDD. The
contract layer reads it as `TEXTURE`, which is the conservative choice, because the command's own
pitch member is then ignored.

The capability answer of `WddmDriverCaps` changes only while `GdiHwAccel` is true:

| Member | Value | Why |
|---|---|---|
| `SupportKernelModeCommandBuffer` | 1 | the feature itself |
| `DriverSupportsCddDwmInterop` | 0 | d3dkmddi.h reads this bit as "Driver does not support hardware GDI acceleration, but supports Cdd-Dwm interop". The two are an exclusive pair |
| `MaxTextureWidthShift`, `MaxTextureHeightShift` | 2 | 8192 pixels, the extent the admission allows |
| `NoTempSurfaceForClearTypeBlend` | 1 | one kernel reads and writes the destination, so no temporary surface is needed |
| `AlignmentShift` | 2 | unchanged |
| `SupportAllBltRops` | 0 | no ROP3 in `BITBLT` or `COLORFILL` |
| `SupportMirrorStretchBlt` | 0 | no mirrored stretch |
| `SupportMonoStretchBltModes` | 0 | only `COLORONCOLOR` arrives |
| `SupportSoftwareDeviceBitmaps` | 0 | type 6 stays refused (BD-060) |
| `SupportLinearHeap` | 0 | no linear heap out of a staging surface |
| `StagingRectStartPitchAligned` | 0 | no pitch aligned staging rectangle requirement |
| the `No*SameBitmap*` family | 1 | one allocation on both sides is refused outright |

The GDI acceleration page adds one condition of its own. "The driver should report this type of
support only if the cache-coherent GPU aperture segment exists and there's no significant
performance penalty when the CPU accesses GPU memory."

The first half is a fact of this driver's own segment description: the aperture segment is declared
cache coherent, which is also why `NoCacheCoherentApertureMemory` stays 0. What the CDD then writes
with the processor are the two CPU-visible surface types, and the mapping it writes them through is
the one `DxgkDdiAcquireSwizzlingRange` and the allocation's own segment preference give it, not a
mapping of our choosing.

The second half is NOT proved, and an earlier draft of this page claimed it held "by construction"
because the processor and the engine share one memory. That does not follow. Physical sharing says
nothing about the cache policy of a mapping or about its write-combining behaviour, and this project
has measured both ways round on this part: BD-021 and BD-022 are cache-policy and alias-scope
findings of ours, and the 2026-09-29 game110 history turned on a cached versus an uncached mapping.
A segment that is cache coherent for correctness can still be slow for the processor.

So the condition is recorded here as an assumption with a measurement attached: the lab plan
measures the processor's write rate into a CPU-visible GDI staging surface and compares it with a
plain system-memory write of the same size
([docs/testing/gdi-hw-accel-lab.md](../testing/gdi-hw-accel-lab.md), trial 5). The client of that
trial is [tools/win/gdi-cpuwrite](../../tools/win/gdi-cpuwrite/gdi-cpuwrite.c). It asks this driver
for a type-2 surface through the LB7A private data, because no user-mode process can ask dxgkrnl for
a standard GDI surface. The placement is therefore the same `WddmGdiAllocationPolicy` answer the
CDD's staging surface gets: the aperture, cached, CPU-visible. The trial records that policy, the
three access rates of each memory class and the ratio against ordinary memory. Until those numbers
exist, "no significant penalty" is a claim this driver makes to the operating system on the strength
of the segment's declaration alone, and the capability can be withdrawn with `EnableGdiHwAccel` if
the numbers say otherwise.

## Preemption and TDR

A GDI DMA buffer is an ordinary graphics queue submission. It inherits the scheduling answer the
driver already gives. `GraphicsPreemptionGranularity` is
`D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY`, so the scheduler preempts between DMA buffers and
never inside one. Nothing of this work changes that.

Two consequences follow. A single GDI DMA buffer must stay short enough that the timeout detection
of Windows does not fire while it runs. 9362 copy rows of a full screen width are about 38 MiB of
copy, which the DMA_DATA engine finishes far inside the two second default and far inside the ten
second value the control application writes. A compute dispatch of increment 2 covers one
sub-rectangle for each dispatch, which keeps the same bound.

A timeout on a GDI submission takes the same path as any other graphics timeout. The part has no
working GPU reset (facts M53), so `DxgkDdiResetEngine` refuses, and recovery is a restart of the
machine. That is the existing behaviour of every graphics submission on this driver and not a new
risk of this path. The driver counts the GDI submissions separately, so a timeout report says
whether a GDI buffer was in flight.

## What stays on the processor

Three things stay where they are.

The desktop present path does not change. A Blt present of the compositor still goes through
`DxgkDdiPresent`, the BGP1 record and the GPU copy of
[docs/design/gpu-dwm-interop-switches.md](gpu-dwm-interop-switches.md). GDI acceleration is a
different interface with a different command buffer.

Every GDI operation that the driver drops falls back to nothing. GDI has no second path once the
capability bit is set. This is why increment 1 keeps the bit clear and why the admission is tested
over every refusal class.

The CPU visible staging surfaces stay a processor route by their own definition. The CDD writes
them with the processor and the driver copies out of them. The driver never maps a texture or a
staging surface to read pixels.

## The two increments

| Increment | Content | State |
|---|---|---|
| 1 | the contract layer, the reference model, the DMA_DATA copy and fill, the host tests, the registry gate, the DDI function, the caps arm behind `Bc250GdiKernelsPresent` | in the driver |
| 2 | `gdi_kernels.cl` for gfx1013, its reproducible build, the dispatch emitter, the code object in the driver image, and `Bc250GdiKernelsPresent` returning 1 | in the driver |

The registry gate is `EnableGdiHwAccel`. The release default is 1, which follows the rule that a new
feature of a train ships on. The gate is the field off switch and the driver reads it once at a
start. The capability follows the gate and the kernels together: with the gate at 0, or with an
image that lost its kernels, a start reports the feature exactly as the release before this work
did, and `DxgkDdiRenderGdi` returns at once without touching the DMA buffer.

## Recorded contract gaps

Four readings are interpretations and not quotations. Each one is recorded here, because a future
reader must not mistake one for a contract.

**The AlphaBlend formula.** `DXGK_GDIARG_ALPHABLEND` gives no blend formula at all. Its two fields
are `SourceConstantAlpha` and `SourceHasAlpha`, which are the two members of the Win32
`BLENDFUNCTION` structure. This driver reads the Win32 `AC_SRC_OVER` tables as the contract, with a
premultiplied source when the source has alpha.

**The rounding.** No page states how a real valued blend becomes a byte. The driver rounds each
documented line once, over the whole right hand side, with ties away from zero. Rounding the
products separately can differ by one count. The HLK job runs `Gditest_hw.exe -tolerance -clipping
-all`, so the test has a tolerance mode, and the width of that tolerance is not known from the local
copies.

**The ClearType sentences.** The Remarks state two requirements after both blend formulas. A zero
alpha component must give the background component, and an alpha component of 0xFF must give the
`Color2` component. The gamma branch already states both inside its own formula. The invalid gamma
branch does not, and its formula yields `Color` where the sentence demands `Color2`. The driver
applies both sentences after either branch.

**The colour format of each Color field, one field at a time.** The three fields do not share one
rule, and reading them as if they did was the defect review r2 found.

`DXGK_GDIARG_COLORFILL.Color` is "Color in the format of the destination surface" in the WDK
10.0.26100 declaration, while the reference page's prose says A8R8G8B8 for the same field. A header
of the WDK the build uses wins over a page (CLAUDE.md, References), so the fill writes the dword as
it arrived. Its arithmetic is bitwise in the destination's order and never looks at a channel. The
two readings differ only for a red-first destination, which no CDD surface seen so far is, and
`GdiRenderColorFillRedFirst` counts that case so the lab can say whether it ever happens.

`DXGK_GDIARG_TRANSPARENTBLT.Color` is "the physical transparent color in the source surface, in
32-bit ARGB unsigned pixel format (as defined by the D3DDDIFMT_A8R8G8B8 value ...)". The two
`DXGK_GDIARG_CLEARTYPEBLEND` colours are the foreground colour "in 32-bit ARGB unsigned pixel
format", one mapped through the gamma tables and one not. Their header lines say "Foreground color
in a 32-bit surface format", which does not name the destination's format and does not contradict
the page. These three fields are therefore passed to the kernels unchanged. The kernels convert the
source, destination and alpha words to A8R8G8B8 themselves and compare or blend in that order, so a
conversion in the driver would apply the exchange twice. On a red-first destination that made the
transparency key match the pixels it was asked to keep, and copy them.

The host test states each field in bytes rather than sharing an assumption with the driver. Every
case below runs for both channel orders, with colours whose red and blue bytes differ. A PATCOPY
fill leaves the colour itself in the destination and never its exchange. A TransparentBlt key equal
to the source pixel in A8R8G8B8 suppresses that pixel, the exchange of that key copies it, and a key
no source pixel can hold copies it. A ClearType blend over an alpha surface of 0xFF puts `Color2`'s
three channels in the destination in the destination's order, and exchanging red and blue in `Color`
changes the blended pixel. The reference model carries each field's own encoding, which is what
makes these cases decide anything. Putting the conversion back into the driver fails fourteen of
them.

One assumption is also recorded, and it is the one the capacity proof rests on. The sub-rectangle
array of a command must lie inside that command, which follows from the `CommandSize` wording and
from the Remarks sentence that a command's length depends on how many sub-rectangles it has. The
driver checks it, counts a pointer that lies outside in `GdiRenderOutside` and drops that command.
Without this reading `CommandLength` would bound nothing, and the DMA buffer could not be sized
against the input at all. With it, the bound of
[the capacity section](#why-rule-2-can-never-be-reached) follows. A pointer outside the command is the only input this driver cannot bounds-check any other
way, and dropping such a command is the same answer the guaranteed contract gives a command buffer
it calls invalid by definition.

## Host tests

[driver/kmd/test/run_gdi_render.ps1](../../driver/kmd/test/run_gdi_render.ps1) builds and runs
`gdi_render_test.c` with the Simplified Development Kit headers only, and then compiles the same two
modules again with the kernel flags of the driver. `tools/quality/quick.ps1` runs it as the
`gdi-render` gate. The test holds four things:

- The arithmetic of every operation against a double precision oracle of the page's own
  expressions, over the whole input domain of each channel where that domain is finite.
- Every refusal class of the admission, each one provoked by its own malformed command.
- The destination to source transform against its defining inequality in 64 bit integers, which is
  an independent statement of the truncation and not a second copy of the formula.
- The emitted packets, decoded and replayed onto a software framebuffer, compared byte for byte with
  the reference model over the whole memory. A packet that writes one byte outside its rectangle
  fails the comparison.

The edge cases the test covers are the 1 by 1 rectangle, the largest extent the caps report, a
clipped sub-rectangle, an overlapping source and destination, every admitted pixel format, the one
byte lookup table surface, all 16 gamma rows and both ClearType branches.

[driver/kmd/test/run_gdi_kernels.ps1](../../driver/kmd/test/run_gdi_kernels.ps1) is the second gate,
`gdi-kernels`. It runs the code object gate above and then `gdi_kernel_test.c`, which takes one
operation at a time through the whole path: it plans a sub-rectangle, builds the argument block,
emits the code block and the dispatch, decodes the buffer it wrote, runs the grid that decode found,
and compares the whole memory with a reference model.

Four things make that comparison worth something:

- the decode is strict. Each `SET_SH_REG` must carry the offset and the count the emitter claims,
  the code in the buffer must equal the generated table dword for dword, the program address must
  be the slice of the kernel being dispatched, and the grid, the initiator and the flush must be
  the expected words. A packet with a field this driver never sets fails the test.
- the kernels are run from their own source text, compiled as host C over the same `gdi_pixel.h`
  arm the device compiler takes (`driver/kmd/test/gdi_kernel_host.h`). The test runs what the
  kernel file says, not a second implementation of it.
- the reference model addresses its pixels by its own road: it walks the destination rectangle and
  uses `Bc250GdiSourceX/Y` of the planned item, while the kernel rebuilds the transform from the
  extents in the argument block. A wrong field in the block, a wrong pitch or a wrong origin breaks
  the comparison.
- the comparison is the whole arena, so a kernel that writes one byte outside its rectangle fails.

The same gate holds the capacity contract of a whole call. `TestBatchCapacity` drives
`gdi_batch.c` over batches of tall columns, the shape of the review's counterexample, and asserts
three things: that `Bc250GdiBatchBegin` refuses what it cannot guarantee and writes nothing, that
after it says yes every sub-rectangle is drawn by one path or the other, and that the arena ends up
exactly as the reference model says. Each item's span in the buffer is checked as well, so an
engine item must hold the production rows byte for byte and a dispatched item must name the kernel
of its own operation. The cases are the 2 by 8192 ColorFill that increment 1 lost, the same pair at
512 rows that increment 1 handled, the same pair in a buffer one bounded path short of both, a copy
and a 1:1 stretch of the same shape, a four-column batch where the two paths mix inside one call,
and a kernel operation that needs no fallback at all.

Four negative controls were run against the gate, and each one was reverted after it failed:

- packing `Dx0` as zero instead of the sub-rectangle's offset inside `DstRect` failed the clipped
  stretch case alone.
- exchanging the gamma and the inverse gamma pointers inside the kernel failed every ClearType case.
- replacing the batch reserve with the per-item capacity of increment 1 dropped an admitted
  sub-rectangle in the one-dword-short case, which is the review's defect reproduced inside our own
  gate.
- putting the DDI reference's A8R8G8B8 reading of `ColorFill.Color` back into the driver failed five
  cases of the red-first fill.
- converting `TransparentBlt.Color` and the two `ClearTypeBlend` colours into the destination's order
  - the reading review r2 rejected - failed fourteen cases, among them the two direct witnesses: a
  key that must suppress a pixel copied it, and a full-coverage ClearType blend put `Color2`'s
  channels in the wrong order.

What the host tests do NOT prove, stated plainly: the gfx1013 machine code. They run the C and not
the instructions the build extracted from the code object, so they say nothing about instruction
selection, wavefront behaviour or the real caches. The code object's shape is gated by the build and
the instructions themselves run first on the lab.

## References

| Source | Revision | What it gives |
|---|---|---|
| `ref/windows-driver-docs/windows-driver-docs-pr/display/` | staging `110f60ea` | the GDI acceleration pages, the guaranteed contract page, the feature caps table |
| `ref/ddi-display/d3dkmddi.md` and `d3dkmdt.md` | WDK and SDK 10.0.26100 | the argument structures, their fields and their Remarks |
| `toolchain/nuget/microsoft.windows.wdk.x64/c/Include/10.0.26100.0` | 10.0.26100.0 | `d3dkmddi.h`, `d3dkmdt.h` and `dispmprt.h`, which win over a page |
| `learn.microsoft.com/windows-hardware/test/hlk/testref/47737217-dcac-431e-9a09-6f7bb398950c` | read 2026-10-14 | the HLK job, its command line and its two minute expected run time |
| `driver/amdgpu-import/nvd.h` | imported from amdgpu | the `PACKET3_DMA_DATA` field macros and the DATA selector |
