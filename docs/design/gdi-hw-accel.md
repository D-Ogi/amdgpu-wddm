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
function, and the gate `EnableGdiHwAccel`, a setting of the driver state catalog, is on at its
factory value.

So this driver now sets `SupportKernelModeCommandBuffer`, which promises all seven operations, and
clears `DriverSupportsCddDwmInterop`.

This work was held back from tester.31 and tester.32. The reason is the capacity question of the review.
[Where the bound comes from](#where-the-bound-comes-from) answers it from what one indirect buffer
can carry, which is the hardware's own limit on any translation here, [Which DMA buffer a call lies
in](#which-dma-buffer-a-call-lies-in) says how the dwords already in a buffer are established rather
than guessed, and [How far the bound is from the ordinary
producer](#how-far-the-bound-is-from-the-ordinary-producer) says how much room that leaves against
the producer the operating system binaries state, in the four files that were read. The capability
still admits inputs this translation drops: [What this capability still does not
complete](#what-this-capability-still-does-not-complete) states that limitation, with the routes out
as a plan. The trains before it do not carry the function or
the cap: [Whether DxgkDdiRenderGdi can be called
today](#whether-dxgkddirendergdi-can-be-called-today) states what was established about that and
what was not.

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

`DxgkDdiRenderKm` is in the table all the same. d3dkmddi.h describes
`SupportKernelModeCommandBuffer` as "Driver supports RenderKm DDI", and an adapter that set the bit
with a NULL `DxgkDdiRenderKm` did not start on the lab: `CM_PROB_FAILED_POST_START`, 4 ms after the
`DRIVERCAPS` reply and before any segment query (2026-10-25). `Bc250WddmRenderKm` translates
nothing. No context of this adapter is without virtual addressing, so no call is expected. A call
is counted, logged and dropped with an empty DMA buffer, and the stop line
`wddm: gdi renderkm calls<n> dropped` reports it.

The interop answer does not change the desktop route. The `DXGK_PRESENTATIONCAPS` page says: "If
the display miniport driver supports GDI hardware acceleration, DriverSupportsCddDwmInterop is
ignored. In this case the driver must support present CDD operations to DWM texture allocations
that are created by the user-mode driver." The driver clears the bit while acceleration is on, and
its CDD-to-DWM present path stays as `InteropStart` set it.

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
3. It walks the command buffer. `Bc250GdiWalkCommand` of
   [gdi_walk.c](../../driver/kmd/gdi_walk.c) is the structure of one command, and both walks call it
   and nothing else. It holds every offset inside `CommandLength`, refuses a `CommandSize` that the
   buffer does not hold, and answers whether the command's sub-rectangle array lies inside the
   command itself. Its four steps are in that order for a reason, and the order is the contract:
   the header is known to fit, then the opcode alone gives the fixed size of the command
   (`Bc250GdiOpMinBytes`), then that size is checked against `CommandSize` and the rest of the
   buffer (`Bc250GdiCommandSize`), and only then is the union member the opcode names read. Review
   r1 of 2026-10-14 found the read happening before the check, which read up to 40 bytes past
   `CommandLength` when the producer's buffer ended inside its last command. The walk is a file of
   its own because that order is what the `gdi-walk` gate holds, against an inaccessible page.
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
input's own bound: an admitted command of `L` bytes carries at most `(L - 8) / 16` sub-rectangles,
because the array is inside the command and the eight bytes are its header. A host case set holds
each answer of the walk, the malformed count among them, and the case set fails when the containment
check is removed.

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
3. **The capacity of a call is bounded end to end, and the DMA buffer is sized to that bound.**
   Everything this driver writes into a GDI context's DMA buffer leaves as a single indirect
   buffer, whose length field is 20 bits wide, so no translation may write more than
   `BC250_GDI_IB_DWORDS_MAX` dwords however large the buffer is. A longer stream is not truncated:
   the ring emitter refuses the submission whole and the work stays outstanding for recovery. So
   `Bc250GdiCallCapacity` gives each call the smallest of three numbers, the space dxgkrnl reports
   for it, that encodable length, and that length less the dwords already written into the same
   buffer by earlier calls. The third number is established for the buffer the call names, and the
   next section is how. `Bc250GdiBatchBegin` clamps anything larger than that. The row path of
   rule 1 budgets inside the result, so a large DMA buffer cannot grant it unbounded space. The DMA buffer
   size is then computed from the largest command buffer whose worst case fits both ceilings
   (`Bc250GdiContextDmaBytes`), which is what keeps rule 2 unreachable inside the bound. A command
   buffer above the bound is the one case where rule 2 can still fire, and the two sections below
   say what the bound is and what happens above it.

Rule 1 is what makes rule 2 a contract instead of a hope. The invariant is stated and proved in
`gdi_batch.c`: after `Begin` has said yes, no admitted sub-rectangle can be refused for want of
space. `BC250_GDI_BATCH_NO_SPACE` is therefore unreachable, and the host test asserts that over a
set of batches that includes the counterexample above.

#### Which DMA buffer a call lies in

The last two numbers of rule 3 need a buffer identity model. `DXGKARG_RENDERGDI` supplies a CPU
pointer, a GPU virtual address and a byte length. `DXGKARG_RENDER` requires the driver to advance
the CPU pointer past its written output. Those descriptions alone do not establish that the GPU
address advances by the same amount, or that an untracked aligned address starts a new allocation
generation. They do not establish the order of render calls and submissions for one buffer.

The extent map below is the implementation's operating model, not an OS lifetime guarantee.
Its arithmetic controls establish behavior for supplied sequences. Validation on each supported
Windows path must still join the CPU cursor, GPU address, reported size, context, submission range
and an independent buffer-lifetime witness. No such runtime trace accompanies this change.

Two reviews have built the counterexamples that shaped this. A review of r4 found that the lowest
address seen since the last submission is not an identity: with two independent buffers 8 MiB apart,
each holding one small paint, the distance between the allocations looked like two million dwords of
prefix and the second call was dropped. A review of r5 found that adjacency is not an identity
either: an empty buffer that began exactly where the first one ended, with its own full size, was
taken for a continuation of it, and the ordinary producer's 64 KiB worth of drawing was refused in a
buffer that was entirely free.

The map treats a buffer as its recorded extent. `Bc250GdiDmaClaim` places the window of a call inside the
extents of the buffers this context holds output in, and reserves the slot of that buffer until the
call publishes what it wrote or gives the claim back:

| What the window is | Result | Dwords ahead |
|---|---|---|
| inside the extent of a buffer we hold output in, exactly where that output ended | append | what we wrote into that buffer |
| inside no extent, on a 4 KB boundary, with a slot free | fresh | none |
| inside an extent, anywhere else in it, or in a buffer another call is translating into | unknown | the call is dropped |
| inside no extent and off the 4 KB boundary, or across the start of a buffer we hold | unknown | the call is dropped |
| inside no extent, with every slot taken | full | the call is dropped |

The room a claim answers with is what the buffer has left behind the window, which is the smaller of
that extent and the length the call reports. A call that continues inside a buffer therefore cannot
be handed the whole buffer's length by a `DmaSize` that names the buffer rather than the window,
whichever of the two readings the operating system means. `Bc250GdiDmaPublish` records what the call
wrote against the generation of its own claim, so a claim can never record into a slot that was
released and taken again in between. `Bc250GdiDmaAbort` gives back a claim that wrote nothing, which
every refused call does, and frees the slot when the buffer held nothing before.
`Bc250GdiDmaSubmitted` releases a buffer by the address range of the submission that took it, so a
submission of one buffer cannot clear the record of another. It leaves a reserved slot alone
(`GdiRenderDmaRaced`) rather than retiring a buffer under a call that is still deciding its length.
An empty submission leaves the entire map unchanged, including a slot with the same base address.
`DXGK_SUBMITCOMMANDFLAGS.ContextSwitch` permits zero-length DMA submissions. These carry no
buffered commands. This contract edge does not assert that the matching-address case occurred on the lab.

Two ordering questions remain open. A submission during an active append is counted but not saved
for retirement after that append. A submission starting inside an extent after its recorded base
does not retire that slot. The reviewed fields do not establish whether either sequence occurs on
this GDI path. Serial helper tests are not proof of complete render/submit concurrency.

The driver knows the prefix it recorded within this model. That does not prove the complete OS
allocation's lifetime or contents. A context holds `BC250_GDI_DMA_SLOTS` buffers at a time,
eight, counting the buffer of every call in flight. That is an implementation bound of this driver
and not a limit of the DDI, and the ninth buffer drops its call rather than forget a buffer or steal
a reservation. Every result is counted (`GdiRenderDmaAppend`, `GdiRenderDmaUnknown`,
`GdiRenderDmaFull`, `GdiRenderDmaRaced`), so the field says which orderings the operating system
actually produces, and the host test drives all of them, both reviews' counterexamples with their own
numbers included. No trial of this capability has reported any of these counters yet, so none of
these orderings is claimed to happen or claimed not to: they are the states the code is prepared
for, and the counters are how a trial would say which of them the operating system produces.

The record lives in the per-context map. Per-DMA private data remains an alternative.
`DXGK_CONTEXTINFO.DmaBufferPrivateDataSize` describes storage initialized to zero when the DMA
buffer is created and not accessed by VidMm during that buffer's lifetime. `DXGKARG_RENDERGDI`
exposes a private-data pointer and the remaining byte count for the operation. Using that storage
would require explicit cursor/bounds handling and a checked handoff to submission. The existence
of a cursor is not evidence against the documented lifetime, nor does the current map implement
that alternative.

Contract references: MicrosoftDocs/windows-driver-docs-ddi snapshot
`7515063cea4c9e98db6a92986c5b4ddb0463fd16`, the pages for
[DXGKARG_RENDERGDI](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_rendergdi),
[DXGKARG_RENDER](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_render),
[DXGK_CONTEXTINFO](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_contextinfo),
[DXGKARG_SUBMITCOMMANDVIRTUAL](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_submitcommandvirtual)
and [DXGK_SUBMITCOMMANDFLAGS](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_submitcommandflags).

A publication that is refused is treated as a dropped call and not as a detail of bookkeeping: the
dwords stay where they are, `pDmaBuffer` does not move, the submission carries what was already there
and nothing of ours, and `GdiRenderDmaLost` is counted with the other drops. The claim makes it
unreachable, which is why it is a counter and not a recovery path.

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
command's header and the start of the next command is counted in `GdiRenderOutside` and the
command is dropped, because it is the one input that cannot be bounds-checked any other way.

A sub-rectangle array may overlap the fixed part of its own command, which one producer does (see
"The second producer, and the shape it builds"), so the floor of the count is the eight-byte header
and not the opcode's fixed part. So, with `L` for `CommandLength`:

```
sum over commands of 16 * NumSubRects <= L - 8         so    N <= (L - 8) / 16
capacity needed = acquire + codeMax + N * dispatchMax + granule - 1 = Bc250GdiCallDwords(L)
                = 1359 + 81 * N dwords
```

That is a constant factor of the input: 324 bytes of DMA buffer per 16 bytes of command buffer,
20.25 to one. So a DMA buffer of `Bc250GdiContextDmaBytes(bound)` bytes translates every command
buffer up to `bound`, whatever is inside it and whoever built it, and `Begin` cannot refuse inside
it - not merely for the inputs anyone has tried, but for every input this contract admits. The host
test asserts it by walking every possible `CommandLength` from 0 to the bound.

#### Where the bound comes from

The bound is this driver's own number, and it is deliberately not read off a producer. It is the
largest candidate on this driver's own ladder of command-buffer sizes, the floor doubled while it
still fits, whose worst case fits both ceilings: the hardware's, which is what one indirect buffer
can carry, and the policy one, which is the 32 MiB this driver puts on a GDI context's DMA buffer.

| | command buffer | sub-rectangles | worst case | fits one indirect buffer | DMA buffer asked for |
| --- | --- | --- | --- | --- | --- |
| `BC250_GDI_CMDBUF_BOUND` | 192 KiB | 12287 | 996606 dwords | yes, of 1048568 | 3989504 bytes |
| the next candidate | 384 KiB | 24575 | 1991934 dwords | no, refused | would have been 7970816 |
| `BC250_GDI_CMDBUF_FLOOR` | 96 KiB | 6143 | 498942 dwords | yes | 1998848 bytes |

The hardware ceiling is the one that chooses the bound. `PACKET3_INDIRECT_BUFFER`'s length field is
20 bits (the imported AMD definition in `driver/amdgpu-import/nvd.h`), so 1048575 dwords is the
longest stream `bc250_gfx_emit_ib` accepts and 1048568 is that number at the eight-dword granule the
batch pads to. The next candidate's DMA buffer would have been well inside the 32 MiB policy
ceiling. What it cannot do is reach the ring. The constant is held against AMD's own macro by a
compile-time assertion in `gdi_dispatch.c` and again in the host test, and the host test holds both
ends of the bound: the worst case of the bound fits, the next candidate's does not
(`Bc250GdiContextDmaBytes(BC250_GDI_CMDBUF_BOUND * 2)` is 0), and the refusal is the encodable
length and not the policy ceiling.

The size asked for is bounded a second time, by policy. `Bc250GdiContextBound` takes the aperture
this start was granted and steps the bound down by halves until the DMA buffer is at most
`BC250_GDI_DMA_APERTURE_SHARE` of it, an eighth, so that a GDI context cannot eat the segment its
own surfaces live in. That share is a policy constant, not a proof that the segment can afford the
buffer: concurrent contexts, their own buffers and every other allocation compete for the same
segment, and no measurement here establishes the right fraction. At every aperture this driver
accepts, 256 MiB and up, an eighth is 32 MiB or more and the whole 3.8 MB buffer fits, so the step
down changes nothing in that range. It engages only below about 32 MiB of aperture, which this
driver does not accept today. The host test drives the whole range in 1 MiB steps, checks that the
answer never falls when the aperture grows, and pins the two ends of the step (32 MiB of aperture
keeps the bound, 16 MiB falls to the floor).

The capacity of each call comes from the space that call reports, whatever that is, so nothing here
depends on the size asked for being granted. `DXGK_CONTEXTINFO` says the buffer "can never shrink
smaller than the starting size that `DmaBufferSize` specifies", so `GdiRenderDmaShort` is the check
of that sentence and not the handling of an allocation failure: it counts a call that was given the
start of a buffer and reported less space than this context asked for. A call that continues inside
a buffer reports the window it has left, which is smaller by design and never counted there. A call
that finds the buffer already filled to the encodable length is counted in `GdiRenderIbFull`, and it
drops the paint it was given, which the lab plan treats as a correctness result and not as an
observation about the operating system.

The review's case - one ColorFill with 793 disjoint one-pixel sub-rectangles, which the hand-picked
256 KiB buffer refused whole - is 12736 bytes of command buffer, 6.5 per cent of the bound. The
second review's case, 6153 sub-rectangles in 98496 bytes, which the 96 KiB design refused by 40
dwords, is 50.1 per cent of it. The third review's case, 128 separate 1 by 1200 strips in 2096
bytes, is translated whole at this bound in 993312 dwords, 118 of the strips on the row path and the
other ten dispatched where the capacity stops the rows. With an unbounded row budget the same input
produced 1075208 dwords, which no indirect buffer can carry. The densest call the bound itself admits uses the whole buffer,
because the budget reserves a full dispatch for each sub-rectangle while a one-pixel rectangle
actually takes the seven-dword row path. The buffer is sized for that worst case, not for the
rectangles a real paint has.

#### What happens above the bound

A command buffer larger than the bound is not refused for being large. `Begin`'s own exact check
still decides, and it accepts such a call whenever the sub-rectangles actually in it fit the
capacity the call was given. The capacity of a call is the capacity of what is left of its DMA
buffer, so the bound is a statement about a buffer and not about a command length alone: a call
inside the bound can still be dropped when earlier calls have taken part of the same buffer, and
`GdiRenderIbFull` is that case. The window a call is given can also be too small for the
smallest batch this driver writes, which `GdiRenderDmaSmall` counts: that is a refusal of output
space and not a malformed input, so it leaves the admission block of unsafe arguments and goes
through the same accounting as every other drop, with the input it lost. Whatever the cause, the
behaviour of a dropped call is defined, counted and visible:

- the call draws nothing. It does not draw a prefix, which would leave a half-painted window.
- `WddmGdiDropCall` is the one exit for all of it, so `GdiRenderBatchRefused` counts every dropped
  call and `GdiRenderOmittedSubRects` adds the sub-rectangles lost with it, whichever limit stopped
  the call. The guard log names the reason, the count, the dwords needed, the capacity, the dwords
  ahead and the command length. `GdiRenderCapacityShort` counts the calls whose worst case was
  larger than the capacity they were given, which is a measurement and not a drop by itself.
- `GdiRenderDwords` counts what the batches generated and `GdiRenderDwordsCommitted` what was handed
  to the submitter. A call dropped after its stream was already written counts in the first and not
  in the second, so no counter implies that discarded output reached the ring.
- `GdiCmdBytesWanted` raises the size that every GDI context created afterwards asks for, up to what
  the aperture share and both ceilings allow. The room above the bound is narrow by construction:
  one indirect buffer carries the worst case of 200 KiB and not of 256 KiB, whatever the aperture
  is, so a request above that is ignored and the ladder's own bound is used.
- `GdiRenderScopeRefused` counts a dropped call whose command buffer was at or below the one the
  ordinary producer allocates, with a line of its own in the log, and every drop reaches it through
  the same exit. A nonzero value is a correctness result: an ordinary paint was lost. A zero value
  is not a witness that nothing was lost, because a longer call is a paint as well and
  `GdiRenderOmittedSubRects` is where those are counted. `GdiRenderAboveProducer` counts the calls
  longer than that command buffer, which is the other half of the same question.

What that recovery cannot do is stated here rather than implied: it cannot fix the call it
happened on, and it cannot enlarge the buffer of a context that already exists. `DXGK_CONTEXTINFO`
does allow the DMA buffer to grow and shrink after the context is created, above the starting size
`DmaBufferSize` names. What this implementation has is no mechanism to use that. It has no
`RenderGdi` continuation and no on-demand enlargement: it asks for a size once, at
`CreateContext`, and then uses the space each call reports. A later context can also land on a
smaller bound again if the aperture or either ceiling says so. The counters are how the field tells us it happened. They do not fix anything, and they
are not evidence that the omitted drawing ever reached the screen.

The documented `RenderGdi` return table permits only success. Other statuses lead to a bugcheck.
The shortage/retry statuses of `RenderKm` and `Render` cannot be applied to this DDI merely because
their argument structures also contain `MultipassOffset`. No reviewed contract establishes such
a continuation for `RenderGdi`. Do not add an insufficient-DMA return as a capacity fix.

This implementation has no CPU fallback: it binds GPU addresses and has no checked CPU mapping
and cache-ownership route for these operations. That source limitation does not prove that every
possible fallback architecture is excluded. Likewise, the success-only return contract does not
authorize omitted drawing. A fix must account for every admitted operation in the output and for
the ownership and completion of the resources it uses.

The local DDI snapshot cited above and the current
[RenderGdi return table](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_rendergdi)
agree on this restriction (checked 2026-10-22). The separate
[RenderKm contract](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_renderkm)
documents an insufficient-buffer status. Its guaranteed-contract paragraph is conditional on that
mode. It is not evidence that a different return code is legal for RenderGdi.

#### What a bound above one indirect buffer would take

The bound stops at what one indirect buffer can carry, so it is worth writing down what the two ways
past that cost, and why neither is in this revision.

The first is a more compact translation. The per-item cost is 81 dwords, of which 46 are register
writes and most of those are the same for every sub-rectangle of one command: the scratch size, the
request control block, the compute unit masks, the thread counts and the resource limits do not
change at all, and the program address and the two resource dwords change only with the kernel. A
state block emitted once per command, with the argument block, the user data pointer, the dispatch
and the flush left per sub-rectangle, costs about 44 dwords once and 39 for each rectangle. The
worst case of a command buffer then falls from 5.06 dwords per byte to about 2.44, and the bound
moves to about 420 KiB. What that assumes is that the compute registers keep their values between
the dispatches of one indirect buffer. Nothing in this project has measured that on gfx1013, the
host test's model would have to carry the hoisted state to keep checking each dispatch, and the
gain is in a range no measured producer reaches.

The second is several indirect buffers for one DMA buffer. `GfxSubmitIb` emits exactly one
`PACKET3_INDIRECT_BUFFER` for a submission, and that path is the one every user-mode submission of
this driver goes through. Splitting means segment boundaries carried from the translation to the
submission (`DXGKARG_RENDERGDI` does have `pDmaBufferPrivateData`, and a GDI context already
receives private data for the software Present packet), the addresses and cache operations of each
segment, and the fence and the completion of the whole submission rather than of its first part. It
is a change to the shared submission path that every user-mode submission goes through, and it would
need a review and a lab round of its own. Most of it can be tested here: the packets, the segment
boundaries, the overflow arithmetic, the ownership of each segment and the failure paths are host
work, like the rest of this file's contract. What needs the lab is whether the hardware executes
several indirect buffers of one submission the way the design says, and that the completion of the
whole submission is still a single fence.

There are two independent capacity obligations. Splitting a stream into encodable indirect buffers
removes the single-packet length limit only. It does not increase `DmaSize`, restore space consumed
by earlier calls, or prove the allocation lifetime of a separately stored command segment. Conversely,
a larger DMA allocation does not make its complete length encodable in one hardware packet.
Any segmented design must prove both bounds, preserve the order of overlapping drawing operations,
retain every segment and referenced resource until GPU completion, and signal the submission fence
only after all segments finish. Arbitrary splits through packets, inline code or argument data are
not valid segment boundaries. A compact translation needs its own worst-case expansion proof and
pixel-equivalence checks. Neither option is implemented by this documentation correction.

So this revision takes neither, and the limitation that follows is stated in the next section
instead of being argued away.

#### What this capability still does not complete

This driver reports `SupportKernelModeCommandBuffer`, and the operating system may then send any
command buffer the DDI admits. This translation does not complete all of them. A dense call above
the bound is dropped as a whole, counted, and not drawn. The r3 review's 14000 one-pixel rectangles
in 224080 bytes are the standing example: 1135366 dwords of worst case, which no single indirect
buffer can carry at any DMA buffer size, so no size of allocation closes it. This is a limitation of
the .32 implementation, not a closed question, and no counter here closes it either: a refusal
counter is a diagnostic of the gap and not an enforcement of a scope.

What is established is narrower, and it is worth having: the bound is three times the command buffer
the canonical display driver allocates in the four files that were read, and the allocation is a
literal of that producer on all four. The diagnostic producer inside dxgkrnl is not bounded by any
of that, and this driver's own bound is what covers it up to 192 KiB.

The route out, as a plan with its tests and not as work already done:

1. the compact translation above, which moves the bound to about 420 KiB and would complete the
   example. Its gate is the register-persistence assumption: a lab round that runs one indirect
   buffer with a hoisted state block and many dispatches, against the host model carrying the same
   state, and the host test extended to check each dispatch against that model.
2. several indirect buffers for one DMA buffer, which removes the 20-bit length limit of one
   indirect buffer instead of moving it. It does not remove every finite bound: the DMA
   allocation and the representation of its segments still bound what one call may write. Its
   gates are a bounded representation of those segments in the private data of the DMA buffer, the
   ownership and lifetime of each segment, the completion of a split submission as one fence, and
   the host tests named above before any lab round.
3. or an enforceable scope: the capability is reported only where the admitted input is bounded by
   something this driver can rely on, and the behaviour outside it is right by construction rather
   than counted. Nothing in the DDI offers that today, which is why it is a plan and not a design.

A second bound of this implementation belongs in the same list, although it is not a bound of the
DDI: one GDI context is tracked in at most eight DMA buffers at a time, counting the buffer of
every call in flight, and a ninth drops its call. Nothing in the DDI says that eight is enough,
and no trial has yet reported the counter either way, so it is a number this driver chose and
counts (`GdiRenderDmaFull`). A dropped call is lost drawing whichever of these bounds stopped it,
which is why the lab plan treats any of them as a failed trial.

Until one of those lands, the honest statement is the one this section makes, and
`GdiRenderOmittedSubRects` is the number that says how often the gap was reached.

#### How far the bound is from the ordinary producer

The bound above does not need a producer's number, but it is worth knowing how much room it leaves.
The DDI does not publish the size of the command buffer that a GDI context receives. The canonical
display driver is the producer of these commands on the ordinary path: it allocates the buffer, it
fills it, and dxgkrnl gives its length to this driver without a change. So the size is a fact of the
operating system binaries, and it was measured in them.

The binaries measured are `cdd.dll` 10.0.22621.6630 with `dxgkrnl.sys` 10.0.22621.6199 (the 23H2
line of the lab), `cdd.dll` 10.0.26100.9444 with `dxgkrnl.sys` 10.0.26100.9444 (the 24H2 line),
`cdd.dll` 10.0.26100.8875 as shipped on 26200 (the 25H2 line, which is an enablement package over
24H2 and keeps the 26100 file-version base at another servicing level), and, as a control newer than
anything this driver supports, `cdd.dll` 10.0.28000.2804 of the current Canary line. They are
Microsoft's property, so the binaries and their analysis stay outside this repository, in a local
working directory with its own ledger entry. This page keeps the behaviour the driver relies on and
no part of that analysis. It is a measurement of four files, not a contract, and it is not what the
capacity rests on.

What those builds do, the same in all of them:

- The device gets two command buffers of 65536 bytes each. The size is a literal of the producer:
  no step of the chain reads a value that a display miniport driver returns, so the `DmaBufferSize`
  of our GDI context has no part in it.
- A command is the DDI header plus the fixed part of its operation plus 16 bytes for each
  sub-rectangle, and the six fixed parts are 0x30 for ColorFill, 0x48 for AlphaBlend, StretchBlt and
  TransparentBlt, and 0x50 for BitBlt and ClearTypeBlend. These are the same six numbers that this
  driver computes from the WDK union as the header plus the member the opcode names, which is why
  `Bc250GdiOpMinBytes` is the producer's own fixed part and not a guess.
  [gdi_walk.c](../../driver/kmd/gdi_walk.c) asserts the equality at build time. On this path the
  sub-rectangle array follows the fixed part of its own command.
- A command that would pass the end of the buffer is refused, and only the bytes that were written
  are submitted. dxgkrnl copies that length into `CommandLength` with no test of its value.

So a `DxgkDdiRenderGdi` call that comes from this chain, on these four files, carries at most
65536 bytes. `BC250_GDI_CMDBUF_BOUND` is three times that. What this does not establish is said
plainly: four exact files are four files. The supported scope of this driver is 23H2, 24H2, 25H2 and
newer general-availability builds, and every one of those lines is serviced, so no sample of
binaries states the allocation of a build nobody has read. The 26300 build of the lab OS matrix is
one such build, and no public index serves that line yet. For the 26100 and 26200 files the whole
chain from the allocation to the submission was read. For the Canary control only the allocation
sites were. It is a statement about the ordinary path of the images that were measured, and not
about every producer: the diagnostic producer
inside dxgkrnl supplies a command buffer of its own (see "The second producer, and the shape it
builds"), and nothing in the chain bounds that length. This is why the bound is derived from the
driver's DMA ceiling instead.

A command of 65536 bytes carries at most 4093 sub-rectangles, from the smallest fixed part of the
six. The largest fixed parts leave 4091. This driver counts with its own floor, the eight-byte
header, which admits 4095 in 65536 bytes: more than that producer can write into that buffer, which
is the safe direction for a capacity gate. Its own limit tests are its own protection and not ours.
This driver's span check is what protects its reads, and a size that wrapped need not fail a size
check merely because it wrapped. No reachable overflow of the producer's arithmetic was found, and
none is assumed either way.

What that producer does when a batch does not fit was measured as well, because it decides whose
loss a refusal is. The builder flushes the buffer and refuses the command. Its caller retries once,
and the buffer is empty at that second call, so the one capacity obstacle the first call met is
gone. If the second call also refuses, that caller drops the rectangles it had accumulated. Its
diagnostic for that case is conditional, on a flag of its own and on the kernel debugger being
enabled, so where neither holds that loss leaves no trace. The retry is a single retry, and the loss is
Windows' own. What matters for the size question is the other half: the builder never writes a
command larger than its own 65536-byte buffer, whatever its caller does with a refusal.

Two more bounds of the same chain are worth the record, both of them smaller than they look:

- 32 is not a ceiling on the allocations of a GDI submission. dxgkrnl compares the count with 32
  and, above it, takes an array of pool memory instead of one on the stack. The number is the
  threshold between those two allocations, not a refusal. The producer's own allocation list holds
  256 entries.
- The producer keeps a third command buffer of 512 bytes for its internal bitmaps, and that buffer
  is small because of what it carries: one BitBlt with one rectangle, which is 0x50 bytes plus 16,
  so 96 bytes in a 512-byte buffer. It can only make a submitted length smaller.

The distance is thus measured and not guessed, for these four files. A servicing revision of any
line is a new binary, 26300 was not read, and a later Windows could raise the literal, which would
eat into the distance without touching the capacity argument. The measures below stay in the driver
because the distance is not what the capacity rests on, and because they are what would report a
producer the scope above does not cover:

- `GdiRenderPeakCommandLength` records the largest `CommandLength` the field ever sends, so the
  bound becomes a measured number rather than a guess. Trial 3 of
  [the lab plan](../testing/gdi-hw-accel-lab.md) reports it.
- A call whose command buffer is larger than the bound is counted in `GdiRenderCapacityShort` and
  logged with its length and the capacity it got, and it raises `GdiCmdBytesWanted` for the GDI
  contexts created afterwards. What that can and cannot do is in "What happens above the bound": it
  does not fix the call, it does not resize an existing context, and a later context can still
  land on a smaller bound.
- A refused call and the sub-rectangles it omitted are counted, so the field reports the event
  instead of leaving a silently unpainted window. The same section says why there is no alternative
  inside this DDI.
- `GdiRenderAboveProducer` counts every call longer than the 65536 bytes above, and
  `GdiRenderScopeRefused` every refusal at or below it. The first says the field has a producer this
  measurement did not read. The second says the bound is wrong, which is the stronger statement and
  the one that must stay at zero.

`CreateContext` asks for the computed buffer only while the capability is reported, so a start
without the kernels allocates what it did before. `DXGK_CONTEXTINFO` says of this size that "the DMA
buffer can never shrink smaller than the starting size that **DmaBufferSize** specifies", and that
it can grow above it. The computed size is therefore a floor on what the OS allocates, each call's
own `DmaSize` is what the translation uses, and this driver has no mechanism of its own to ask for
growth or to continue a call in a second buffer.

#### The second producer, and the shape it builds

The canonical display driver is not the only code that builds these commands. dxgkrnl has a producer
of its own for the Windows driver tests, and review r2 of 2026-10-14 measured two things about it.
One branch of it takes a command buffer from its caller and tests that length only for nonzero,
which is why the bound of this driver cannot be read off a producer at all. Another branch builds an
80-byte BitBlt whose `pSubRects` points at the `DstRect` embedded in the same command, 0x18 bytes
in, rather than at an array behind the fixed part. That array therefore overlaps the fixed part of
its own command.

That shape is admitted, not refused, and it is why the count floor of this driver is the header.
The containment floor of
[gdi_render.c](../../driver/kmd/gdi_render.c) is the eight-byte header, not the opcode's fixed part,
so an array anywhere between the header and the end of the command is inside the command, and the
count is still bounded by `(CommandSize - 8) / 16`. Every read stays inside the bytes the command
declares, which is the property that matters. Which part of the command the rectangles sit in is the
producer's business. The host test holds the shape in both its answers: three rectangles at 0x18 of
an 80-byte command are admitted by the walk, four are not (`TestEmbeddedSubRects` of
[gdi_walk_test.c](../../driver/kmd/test/gdi_walk_test.c)).

This producer is gated, which is what the HLK configuration has to arrange. Both of its entries ask
the same gate, and that gate answers yes in one of two ways: a flag of the calling process, not of
the adapter, or a registry value under a graphics drivers key whose content a machine prepared for
the Windows driver tests has. If neither enabling condition is satisfied, the shown entry is denied
and this producer does not run.
The value's name and the rest of the measurement are in the local notes beside the binaries, outside
this repository.

Being enabled is not being executed, and the two must not be confused in a result. A set
per-process bit or a matching registry value makes the gate succeed. Neither states that a test
called the escape, that it took the embedded-BitBlt branch, or that the call reached `RenderGdi`.
The absence of the registry value does not exclude the per-process route either, so configuration is
recorded as configuration, and the lab plan says how. What this driver gets from the shape is only that it is admitted: the
host test proves that structural answer and nothing about drawing (three rectangles at 0x18 of an
80-byte command are admitted, four are not). A lab trial would be needed to say that such a command
was ever sent, and a command total or a peak length would not identify this shape: that claim needs
a witness of the shape itself. The driver
needs no switch for it either way.

#### Whether `DxgkDdiRenderGdi` can be called today

It cannot, in the driver that tester.30 shipped and in the rc5 line of tester.31. The reason is this
driver's own caps, not the operating system:

- `DXGK_PRESENTATIONCAPS.SupportKernelModeCommandBuffer`, which the DDI reference describes as
  "Supports kernel-mode command buffer", is never set in either build. It is the answer by which the
  canonical display driver learns that it may build GDI commands for this adapter at all. Both
  builds answer the exclusive other half of that pair, `DriverSupportsCddDwmInterop`.
- Neither build has a `DxgkDdiRenderGdi` or a `DxgkDdiRenderKm` member in its device-functions
  table. The word does not appear in the source at `b614581b` or at `cb4c3420`. dxgkrnl zeroes that
  table before it asks the driver to fill it, so both pointers are NULL and there is nothing to
  call. That is a statement about those two source trees, not about an rc5 artifact that was never
  built from them.

This branch is not the same shape. It fills the `DxgkDdiRenderGdi` member unconditionally (and
`DxgkDdiRenderKm`, which translates nothing, see above), and sets
the cap only while `GdiHwAccel` is on, which is the feature switch. So the function exists in the
table of every build of this branch, and the guard that is actually established is two-fold: the
canonical display driver learns from the cap whether it may build these commands at all, and the
function itself returns success without touching the GPU while the switch is off
(`Bc250WddmRenderGdi`'s first test in [wddm.c](../../driver/kmd/wddm.c)). Those two cover the ordinary
`cdd.dll` route. They are not a proof that every diagnostic route into this function is excluded:
the second producer above is gated by Windows and not by our cap, and what our switch gives there is
the early return and not the absence of a call. Every lab number in this page has to come from a
build with the switch on.

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
| 1 | the contract layer, the reference model, the DMA_DATA copy and fill, the host tests, the off switch, the DDI function, the caps arm behind `Bc250GdiKernelsPresent` | in the driver |
| 2 | `gdi_kernels.cl` for gfx1013, its reproducible build, the dispatch emitter, the code object in the driver image, and `Bc250GdiKernelsPresent` returning 1 | in the driver |

The gate is `EnableGdiHwAccel`, a setting of the driver state catalog
([driver/state/catalog.json](../../driver/state/catalog.json)). The factory value is 1, which follows
the rule that a new feature ships on. The gate is the field off switch and the driver reads it once
at a start. The capability follows the gate and the kernels together: with the gate at 0, or with an
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

`TestIbBound` of the same gate holds rule 3, the end-to-end bound, with the three cases review r3
built. The 128 strips of 1 by 1200 in 2096 command bytes are translated whole, twice: at the
capacity a context of this bound has, and with the whole policy ceiling offered as capacity, which
is what the reproducer did. Both runs assert that every strip is drawn by one path or the other and
that the finished stream survives the ring emitter's own predicate, which is AMD's length macro over
the dwords the batch wrote. The 14000 one-pixel rectangles in 224080 command bytes are refused whole
at both capacities, because no indirect buffer can carry their worst case, and the 98313 rectangles
of review r2 are refused as before. The clamp of `Bc250GdiBatchBegin` is asserted directly as well: handed
a capacity of any value up to 0xFFFFFFFF, the batch works inside the encodable length. The same three cases run
against the review's own probe, which links the production walker, planner and batch emitter
(`scratch/gdi-hw/ib-bound-probe-r4` in the local workspace, the review's copy with the expectation
table of its main function rewritten), and it prints 993312 dwords for the strips at this bound, 118 of them on the row
path and ten dispatched.

`TestDmaMap` holds the placement of a call inside its DMA buffer, with the cases both reviews asked
for and their own numbers. Review r4's counterexample: two independent buffers of one context, 8 MiB
apart, each take a paint of their own and neither is read as a continuation of the other, a
submission of the first releases that buffer and leaves the second exactly as it was, and a
submission that names neither releases nothing. Review r5's two counterexamples: a 3989504-byte
buffer filled to its own size, and an empty buffer that starts exactly where it ends, which is its
own buffer with its own room and takes the 64 KiB worst case of the ordinary producer. And a claim
that has not published yet, which keeps its slot while every other slot is used, refuses the ninth
buffer, and publishes its own dwords afterwards, while the claim of a call that is gone cannot
publish into the slot that was released and taken again. Eight buffers allocated back to back are eight buffers,
which is the adjacency case over and over. Three calls in a row continue inside one buffer at the
offsets the emitter actually leaves, each one placed by the length of the calls before it, and their
sum stays inside the encodable length and survives the length macro. A buffer filled to that length
leaves the next call a capacity of zero, which is the counted drop and not an overflow, and the
publication of more than that length is refused. A window inside output of ours, a window at the
start of a buffer we already hold output in, a window off the 4 KB boundary, a window across the
start of a buffer of ours, and a ninth buffer are the cases that drop their call instead of assuming
an empty buffer.

The source-level probe of the reviews runs against the new admission prefix as well
(`scratch/gdi-hw/prefix-probe-r6` in the local workspace, whose prefix is taken out of `wddm.c`
mechanically so that no line of it is transcribed): both independent buffers reach `BatchBegin`, the
adjacent empty buffer reaches it with nothing ahead of it, a continuation inside a recorded extent is
admitted as an append, and the three refusals - a window of a buffer another call holds, a context
with every slot taken, and a window too small for the smallest batch - each return success with one
dropped call, one dropped call inside the input scope and one sub-rectangle omitted, and with the
malformed-input counter untouched.

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

[driver/kmd/test/run_gdi_walk.ps1](../../driver/kmd/test/run_gdi_walk.ps1) is the third gate,
`gdi-walk`, and it is about reads rather than pixels. It compiles the production
[gdi_walk.c](../../driver/kmd/gdi_walk.c) as it is, slicing the GDI block of the kit's own
`shared/d3dkmddi.h` into a header of the name the file includes, and puts every command at the end of
a committed page whose next page is inaccessible. A read of one byte past `CommandLength` is then an
access violation and the case fails. It covers a buffer that ends inside its last command at four
lengths, a header that does not fit, an opcode outside the table, an escape on its header alone, a
whole command whose last byte is the last byte of the page, the embedded sub-rectangle shape of the
second producer, a pointer outside the command in both directions, and a two-command walk whose
second command is a fragment.

Two controls make that gate worth something. The first is inside the test: it reads the inaccessible
page itself and fails if that does not raise an access violation, and it reads the last byte of the
command buffer and fails if that does. The second was run by hand on 2026-10-14: with the decode
moved back in front of the size check, which is the defect review r1 found, six checks fail with an
access violation, and none fails with the check in its place.

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
