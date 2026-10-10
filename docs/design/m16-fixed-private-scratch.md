# Fixed-private scratch on gfx1013

ABI 1.3 implements bounded private frames in the HIP PM4 path. Dynamic stacks and wave64
execution remain refused. This is an offline contract implementation, not a successful GPU
measurement. No KMD change is required: scratch uses a normal resident device-local allocation.

## Shader contract

The reference is LLVM 23.1.2, revision `85ac560262434c9ccfc0c183ec22d4138ed647fb`.
[AMDGPUUsage](https://github.com/llvm/llvm-project/blob/85ac560262434c9ccfc0c183ec22d4138ed647fb/llvm/docs/AMDGPUUsage.rst)
identifies gfx1013 as Absolute flat scratch. Its initial-state table orders enabled user SGPRs as
private V# (4), dispatch pointer (2), queue pointer (2), kernarg pointer (2), dispatch ID (2),
flat scratch init (2), private segment size (1). This implementation refuses the queue and dispatch
ID requests. It fills other enabled entries densely and checks the descriptor user-SGPR count.

Flat scratch init is the scratch allocation base. The kernel prologue adds the SPI-provided
per-wave offset. On GFX10 it writes FLAT_SCR_LO/HI through S_SETREG. This is the compiler's job.
See [SIFrameLowering.cpp](https://github.com/llvm/llvm-project/blob/85ac560262434c9ccfc0c183ec22d4138ed647fb/llvm/lib/Target/AMDGPU/SIFrameLowering.cpp),
lines 610-660 and 1083-1115. The PM4 path preserves the descriptor's SCRATCH_EN flag, which enables
the per-wave offset input. A nonzero private frame without this flag is refused.

The private-size SGPR is the per-work-item size rounded to four bytes. The AQL packet still states
the requested byte size. [SIInstrInfo.cpp](https://github.com/llvm/llvm-project/blob/85ac560262434c9ccfc0c183ec22d4138ed647fb/llvm/lib/Target/AMDGPU/SIInstrInfo.cpp)
lines 9878-9923 supplies the HSA scratch V# convention: base low, base high with SWIZZLE_ENABLE,
NUM_RECORDS UINT32_MAX, and FORMAT 32_FLOAT, INDEX_STRIDE 2 for wave32, ADD_TID_ENABLE 1,
RESOURCE_LEVEL 1 and OOB_SELECT 3. The high base passed to flat scratch init excludes the swizzle bit.
A raw linear-buffer descriptor is not a scratch descriptor.

## Capacity and ownership

Mesa revision `05e6c9622e135ac2aeaf56ec70222642627e2162`,
[ac_shader_util.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/05e6c9622e135ac2aeaf56ec70222642627e2162/src/amd/common/ac_shader_util.c)
lines 1054-1083 defines WAVES as record count and WAVESIZE as stride. The stride cannot change
while a scratch buffer is in use. GFX10 WAVES is global. The per-SE division applies only to GFX11.
[ac_gpu_info.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/05e6c9622e135ac2aeaf56ec70222642627e2162/src/amd/common/ac_gpu_info.c)
lines 1645-1656 specifies 1024-byte wave-size granularity and a minimum of 32 scratch waves for
one 1024-thread wave32 workgroup. It permits reducing the resident wave count.

This first implementation uses 32 resident scratch-wave slots. It does not claim peak occupancy.
Every launch rejects a block larger than 1024 threads. Per-thread bytes are rounded to four.
Per-wave bytes are rounded to 1024. The hardware WAVESIZE field has 13 bits. Values beyond that
field are refused before allocation. Allocation size is per-wave bytes times 32. Base alignment is
4096. The complete allocation must fit in the admitted 48-bit GPU address range. For a 3516-byte
private frame, a wave32 needs 112640 bytes and the ring needs 3604480 bytes.

Each IB slot owns a separate allocation. Before replacement or reuse, its fence must have retired.
UINT64_MAX is removal, not retirement. Growth allocates first and frees the old allocation only
after successful replacement. Allocation failure preserves the old slot. Different in-flight slots
cannot share a scratch allocation. The device lock serializes slot selection and ownership changes.

A scratch dispatch flushes the open scratch-free batch and submits alone, with full PM4 state.
This preserves fence order and prevents a stride change inside a live buffer. The public batch
builder also refuses fixed-private kernels when count exceeds one. Zero-private dispatches keep
their batching and their original golden PM4 stream. The register cache includes TMPRING_SIZE.

Close requires a successful flush and a positive fence retirement before it releases the device.
On flush failure, timeout or device removal it marks the device closed and retains its allocations,
context and tracking until process exit. It does not unmap, free or destroy anything on that path.
This prevents immediate VA reuse while execution is unproven. Repeated failed closes can retain
multiple devices. This is a safety tradeoff, not successful recovery. The public close consumes the
caller handle in both cases, and later submissions through a retained handle are refused.

[FreeGpuVirtualAddress](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtfreegpuvirtualaddress)
permits a new range to occupy the released VA immediately. The public DestroyContext description
only says the context is released. This implementation does not infer a drained-hardware witness
from that wording after failed recovery. Offline close-callback tests require zero releases after
failed flush, wait timeout and removal, and require flush then wait then release on success.

The PM4 environment grows from 48 to 64 bytes. An old 48-byte environment remains accepted for
zero-private kernels. Fixed-private kernels require the complete structure. Scratch base and size
are builder inputs, not an application-owned mutable global buffer.

## Offline gates and acceptance boundary

The build compares descriptor bit positions with a pinned MIT Mesa GFX10 reference fixture, and
register masks and FORMAT with the imported AMD GFX10 headers. Host tests cover size boundaries,
wave32 and wave64 sizing arithmetic, address and allocation bounds, enabled SGPR order, fixed-frame
PM4, old-size environment admission, pending and removed fences, disjoint slot allocations,
allocation-failure preservation and retired growth. Runtime wave64 remains refused.

GPU acceptance must still use a compiler-generated spilling kernel with checked output, followed
by simultaneous devices, changing private-frame sizes and a fixed-frame llama.cpp kernel. The host
gates establish encoding and ownership policy. They do not establish hardware execution or speed.
