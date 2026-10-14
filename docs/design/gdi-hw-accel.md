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

Increment 1 is in the driver. It carries the contract layer, the reference model of every operation,
the command stream of the two operations that the command processor performs exactly, and the host
tests. The capability bit stays clear, because the bit promises all seven operations.

Increment 2 is specified here and not written. It carries the six compute kernels for the blends,
the stretch, the transparency and the ClearType blend. `Bc250GdiKernelsPresent` in
[gdi_render.c](../../driver/kmd/gdi_render.c) is the one place that reports their presence. The
capability bit, the start log line and the operator gate all read that function.

Nothing of this has run on the lab. The lab plan is
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

Everything else is a compute dispatch of increment 2. The arithmetic of all seven operations is
[gdi_pixel.h](../../driver/kmd/gdi_pixel.h), which compiles both as OpenCL C and as host C. The
kernel source will be `driver/kmd/gdi_kernels.cl`, with one entry point for each arithmetic class of
`bc250_gdi_kernel`. The build compiles it with
`toolchain/llvm-amdgpu-22.1.8/mingw64/bin/clang.exe` for `amdgcn-amd-amdhsa` and `-mcpu=gfx1013`,
and records the exact revision beside the code object. The dispatch itself follows the positive
control in [bc250_dispatch.c](../../driver/shim/bc250_dispatch.c), which programs the `COMPUTE_*`
registers with `SET_SH_REG` and starts the grid with `DISPATCH_DIRECT`.

Two measured properties of this part shape increment 2. The compute path has a working positive
control on the lab. The part has no working GPU reset (facts M53), so a kernel that hangs the
graphics queue costs a restart of the machine. Increment 2 therefore keeps each dispatch bounded to
one sub-rectangle, with a grid the driver computes from the admitted rectangle.

## How RenderGdi builds the DMA buffer

`Bc250WddmRenderGdi` in [wddm.c](../../driver/kmd/wddm.c) is the only layer that sees a WDK type.
The steps:

1. It admits the call. The context must be the GDI context, the node must be the graphics node, and
   the DMA buffer must be present, aligned and large enough.
2. It walks the command buffer. `Bc250GdiWalkStep` holds every offset inside `CommandLength` and
   refuses a `CommandSize` that the buffer does not hold.
3. It flattens each command into `BC250_GDI_OP`, which holds the fields of all six argument
   structures. `C_ASSERT` lines tie every plain constant of the contract layer to the WDK values.
4. It resolves the allocation list indices that the command names. Each index gives one surface
   record and one GPU virtual address. The lookup copies both descriptors under the object lock,
   the same discipline the GPU present producer uses.
5. It admits the operation and plans each sub-rectangle. `Bc250GdiAdmitOp` and `Bc250GdiPlanItem`
   prove that every byte the commands touch lies inside its allocation.
6. It emits one cache operation for the whole DMA buffer, before the first copy. The CDD writes its
   bitmaps with the processor into cache coherent aperture memory, and the acquire makes those
   writes visible to the engine's level two cache read.
7. It emits the packets of each admitted sub-rectangle, pads the tail to the 32 byte granule of the
   ring with the command processor's own no operation word, and advances `pDmaBuffer`.

`DxgkDdiSubmitCommandVirtual` needs no new arm. A GDI DMA buffer is a non present, non user mode
command buffer with bytes in it, which the existing stage C submits to the graphics ring. The GDI
context gets its root page table from `DxgkDdiSetRootPageTable` like any other context.

The DMA buffer size is the one number the guaranteed contract forces. `BC250_GDI_DMA_BYTES` is 256
KiB, which is 65536 dwords. That holds 9362 copy rows, about eight full screen blits of 1080 rows,
or about a thousand compute dispatches. `CreateContext` asks for it only while the capability is
reported, so a start without the kernels allocates what it did before. `GdiRenderOverflow` counts
the command buffers that still did not fit, and the lab says whether that number is ever nonzero.

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
performance penalty when the CPU accesses GPU memory." The aperture segment is declared cache
coherent, which is also why `NoCacheCoherentApertureMemory` stays 0. The processor and the GPU share
one memory on this part, so the second half holds by construction.

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
| 2 | `gdi_kernels.cl` for gfx1013, its reproducible build, the dispatch emitter, the code object in the driver image, and `Bc250GdiKernelsPresent` returning 1 | specified here |

The registry gate is `EnableGdiHwAccel`. The release default is 1, which follows the rule that a new
feature of a train ships on. The gate is the field off switch and the driver reads it once at a
start. The capability follows the gate and the translator together, so increment 1 reports the
feature exactly as the previous release did.

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

**The ColorFill colour format.** The DDI reference states that `Color` is "in 32-bit ARGB unsigned
pixel format (as defined by the D3DDDIFMT_A8R8G8B8 value)". The trailing comment in d3dkmddi.h says
"Color in the format of the destination surface" instead. The two readings differ only for a red
first destination, which no CDD surface seen so far is. The driver follows the page and counts the
red first case, so the lab can say whether it ever happens.

One assumption is also recorded. The sub-rectangle array of a command must lie inside that command,
which follows from the `CommandSize` wording. The driver checks it, counts a pointer that lies
outside and drops that command.

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

## References

| Source | Revision | What it gives |
|---|---|---|
| `ref/windows-driver-docs/windows-driver-docs-pr/display/` | staging `110f60ea` | the GDI acceleration pages, the guaranteed contract page, the feature caps table |
| `ref/ddi-display/d3dkmddi.md` and `d3dkmdt.md` | WDK and SDK 10.0.26100 | the argument structures, their fields and their Remarks |
| `toolchain/nuget/microsoft.windows.wdk.x64/c/Include/10.0.26100.0` | 10.0.26100.0 | `d3dkmddi.h`, `d3dkmdt.h` and `dispmprt.h`, which win over a page |
| `learn.microsoft.com/windows-hardware/test/hlk/testref/47737217-dcac-431e-9a09-6f7bb398950c` | read 2026-10-14 | the HLK job, its command line and its two minute expected run time |
| `driver/amdgpu-import/nvd.h` | imported from amdgpu | the `PACKET3_DMA_DATA` field macros and the DATA selector |
