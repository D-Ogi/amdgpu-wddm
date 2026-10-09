# M16 route B: HIP on gfx1013, one design for two builders

Date 2026-10-08. Milestone M16, gate G5. Owner decisions D014 (start M16) and D015 (PAL is excluded
for every purpose). Roadmap revision R0006. Route B of the M16 route comparison: our own thin HIP
runtime on our own Windows driver.

This document merges two design notes of the same spike, one about submission and one about the
loader and the host application binary interface. It is written so that two builders can work at the
same time without talking to each other. Layer 1 and layer 2 meet at one header, `bc250hsa.h`, whose
final text is section 3.9. The header is frozen by this document. A change of the header needs a new
revision of this document, because the other builder reads it as a contract.

No part of this design uses PAL, and no part depends on an AMD user-mode component.

Status words. MEASURED: the spike ran it and an artifact holds the result. READ: it comes from a
primary source in this workspace. DECIDED: a design choice of this document. OPEN: nobody knows yet.

## Sources

| What | Source |
|---|---|
| The route comparison, the kill criteria K1 to K5 and steps 1 to 3 | The M16 route document of the spike (local research workspace) |
| The measured compiler, metadata and descriptor contract | The M16 findings document and its proof artifacts (local research workspace) |
| Our own compute bring-up on this chip | Facts M38 (RLC, KIQ, MEC, eight compute queues, eleven ring tests), M40 (compute queue interrupts and ring ids) |
| A real gfx1013 PM4 dispatch with cache control and a fence | Fact M49, `driver/shim/include/bc250_dispatch.h`, `driver/shim/bc250_dispatch.c` |
| Client PM4 through VidMm page tables from a user-mode context | Fact M80, `driver/kmd/wddm.c`, `driver/kmd/umd_blob.h` |
| No compute IP on this part | Fact M50, `driver/contract/bc250_umd_submit.h` |
| No working GPU reset on this part | Fact M53 |
| The Vulkan compute baseline to beat | Fact M816 (llama.cpp on our Vulkan backend: 1813.1 prompt and 202.1 generation tokens per second on TinyLlama 1.1B Q4_0 at 1000 MHz) |
| The D3DKMT call sequence to copy | Our Mesa fork, branch `amdgpu-wddm/b19-icd` head `154050d5`, files `src/amd/vulkan/winsys/wddm2/radv_wddm2_winsys.c`, `radv_wddm2_bo.c`, `radv_wddm2_cs.c` |
| Register names and offsets | `third_party/linux-amdgpu/gc_10_1_0_offset.h`, and the Mesa fork's `src/amd/common/sid.h` and `src/amd/registers/gfx10.json` |
| Code object, kernel descriptor, relocation and metadata rules | LLVM `llvm/docs/AMDGPUUsage.rst`, revision `85ac5602` |
| The dispatch packet order of a proven gfx10 test | libdrm `libdrm-2.4.114`, commit `b9ca37b3`, `tests/amdgpu/shader_test_util.c` |
| The HIP host ABI that clang emits | clang `lib/CodeGen/CGCUDANV.cpp` of LLVM 22.1.8, and `clang/docs/ClangOffloadBundler.rst` |
| The ROCm device library for gfx1013 | `ROCm/llvm-project`, branch `release/rocm-rel-7.2`, head `f58b06dc`, directory `amd/device-libs`, NCSA licence |

Line numbers move. This document names a file and a symbol, and it names a line only where the spike
notes already recorded one.

`clang/lib/CodeGen/CGCUDANV.cpp` is an upstream file. This workspace holds no copy of it, because the
local `ref/llvm-project` checkout is sparse and carries no `clang` source. Every statement here about
the registration ABI was therefore measured against clang 22.1.8 itself, with a compiled program.

---

## 1. The two layers and the seam

```
   application (a HIP program, llama.cpp)
        |  the HIP host ABI that clang emits
   +----v--------------------------------------------------+
   | Layer 2: amdhip64.dll                                  |  branch m16/hip-runtime
   |  registration, launch, streams, events, errors, props  |  C and C++
   +----|--------------------------------------------------+
        |  bc250hsa.h, and nothing else of the submission path
   +----v--------------------------------------------------+
   | Layer 1: bc250hsa (static library) + hipprobe tool     |  branch m16/hip-dispatch
   |  device, memory, loader, kernarg, PM4, fence, wait     |  C11
   +----|--------------------------------------------------+
        |  D3DKMT, with our three private blobs
   +----v--------------------------------------------------+
   | bc250kmd, unchanged                                    |
   +-------------------------------------------------------+
```

Three properties of the seam, and each one is a rule for both builders.

1. `bc250hsa.h` names no HIP type, no Mesa type and no D3DKMT type. Layer 2 therefore compiles and
   runs its tests against a mock implementation of the same header, on a development machine with no
   BC-250 adapter.
2. Layer 1 holds no HIP semantics. It has one queue, one fence and one bounded wait. Streams,
   events, the per-thread error state and the lazy module load belong to layer 2.
3. Layer 1 reads no registry value, no file and no environment variable. Every policy arrives as a
   parameter. Layer 2 owns the policy.

---

## 2. The two notes disagreed. Here is the resolution

| # | Design A said | Design B said | DECIDED | Why |
|---|---|---|---|---|
| 1 | User-mode WDDM submission. No kernel driver change. | The kernel driver escape takes seven values instead of its fixed shader. | **User-mode WDDM submission. The kernel driver does not change.** | A read of the submit path found no refusal. `WddmSubmitUmdImpl()` parses only the private blob, whose whole output is `num_ibs`, `ib_va`, `ib_bytes` and `single_ib` (`driver/kmd/umd_blob.h`). It has three conditions only: the blob parses, it names one indirect buffer, and the node is node 0. Fact M80 is the measurement. B wrote its sentence from the route document's step 1, which was drafted before anyone read that path. The seven values of B survive as the fields of `bc250hsa_dispatch`. |
| 2 | The dispatch rides the gfx ring on node 0, with `CONTEXT_CONTROL` added. | The dispatch rides the escape's compute rings `BC250_FENCE_RING_COMPUTE0 + 0..7`. | **Node 0, the graphics ring.** | Three independent statements. Fact M50 says that Mesa sees no COMPUTE IP on this part. The Mesa fork's `radv_wddm2_cs.c` returns at once for any hardware IP that is not GFX, and it forces node 0. `driver/contract/bc250_umd_submit.h` states that `AMDGPU_HW_IP_GFX` is the only choice. The compute rings exist, but only inside the kernel-owned escape of decision 1, which this design does not take. |
| 3 | `COMPUTE_PGM_RSRC2.LDS_SIZE` must be computed by the host. | The dispatch copies `COMPUTE_PGM_RSRC1` to `3` out of the kernel descriptor instead of computing them. | **Both, in this order: copy `RSRC1` and `RSRC3` unchanged, copy `RSRC2` and then write the computed `LDS_SIZE` field into bits 23:15.** | MEASURED: `reduce256` has 1024 bytes of local memory and its descriptor's `GRANULATED_LDS_SIZE` is 0, because the command processor normally writes that field from the AQL packet. There is no AQL packet here. A host that copies `RSRC2` unchanged runs the kernel with no local memory. `bc250hsa_lds_size_field()` is the one place that computes it. |
| 4 | The kernel argument buffer is aligned to `.kernarg_segment_align`. | It is aligned to at least 16 bytes, which is the stricter documented rule. | **`max(.kernarg_segment_align, 16)`, reported as `kernarg_align`.** | The measured metadata gives 8 for `vadd`, and `AMDGPUUsage.rst` requires at least 16 for the allocation. The stricter rule wins and the field carries the result, so no caller repeats the arithmetic. |
| 5 | Two headers, `bc250_gpu.h` for the device and `bc250_co_loader.h` for the loader, with their own prefixes and their own error enumerations. | The same split, from the other side. | **One public header, `bc250hsa.h`, one prefix `bc250hsa_`, one status enumeration.** | Two headers with two error spaces would make layer 2 translate twice. The loader keeps its allocator seam as `bc250hsa_allocator` and `bc250hsa_module_load_alloc()`, which is what made B's split useful: the loader test runs with no device. |
| 6 | Negative integer error macros. | A positive enumeration of loader errors. | **One `bc250hsa_status` enumeration, zero for success and negative values for failure, with the loader codes in their own range at -20 and below.** | One rule for every call, and `bc250hsa_status_string()` names them all. |
| 7 | The caller unbundles the fat binary, because the HIP runtime already walks it. | The unbundler belongs beside the loader. | **The unbundler is a separate call in the same header, `bc250hsa_unbundle()`, and `bc250hsa_module_load()` takes a plain ELF image.** | Both notes wanted the same shape. Layer 2 calls the unbundler, keeps the view and loads it lazily when the program starts its first kernel. |
| 8 | The loader asks the caller for memory through `BC250_CO_ALLOC_*` flags. | The device memory flags are `BC250_GPU_MEM_*`. | **One flag set, `BC250HSA_MEM_*`, used by the device path and by the loader.** | One vocabulary. `BC250HSA_MEM_EXEC` carries the 4096-byte base and the rule that code is never write combined. |
| 9 | About 600 lines for the D3DKMT layer, about 1200 lines for the whole step-1 tool. | About 1000 lines for the loader and about 200 for a MessagePack reader. | **The size table of section 3.1.** | The two notes measured different parts. Together they are about 2900 lines of C for layer 1, tests apart. |
| 10 | The wait slices are 1 second to a total of 120 seconds. | Not addressed. | **The library keeps 1000 and 120000 as its defaults, and the step-1 tool passes 1000 and 10000.** | The long default exists because a healthy wait can sit behind another process's engine reset, measured at 14.6 seconds (defect K225). A lab trial must stay under three minutes, so the tool shortens the total by hand, which the interface allows. |

Nothing else in the two notes conflicts. Where only one note covered a subject, this document keeps
its decision and names it below.

---

## 3. Layer 1: bc250hsa

### 3.1 Components, and what each one costs

| Component | File | Lines, estimated | Needs a GPU to test |
|---|---|---|---|
| D3DKMT device: adapter, caps, device, paging queue, context, fence | `kmt_device.c` | 600 | yes |
| Allocation, GPU address, residency, host mapping, the address window | `kmt_memory.c` | 450 | yes |
| ELF code object loader and relocations | `co_loader.c` | 550 | no |
| MessagePack reader for the metadata note | `co_msgpack.c` | 200 | no |
| Metadata to `bc250hsa_kernel`, and the kernel descriptor fields | `co_metadata.c` | 300 | no |
| Kernel argument packer | `kernarg.c` | 250 | no |
| PM4 dispatch builder, the buffer resource and the user SGPR plan | `pm4_dispatch.c` | 400 | no |
| Submission, the command ring, the bounded wait, the fault query | `submit.c` | 350 | yes |
| Status strings, counters, the log hook | `status.c` | 120 | no |
| Step-1 tool `hipprobe` | `tools/hipprobe.c` | 450 | yes, except `--selftest` |

About 3200 lines in all, and about 1800 of them are testable with no GPU. The MessagePack reader
stays small because the metadata note uses maps, arrays, strings, integers and booleans only.

### 3.2 The submission path

DECIDED. Nine D3DKMT calls, in this order, copied from our own Vulkan winsys. The private blobs are
fixed by `driver/contract/bc250_umd_submit.h` and `driver/contract/bc250_umd_private.h`. Layer 1
keeps its own copy of their field layout in one header, `kmt_blobs.h`, as the Mesa fork keeps
`radv_wddm2_bc250.h` for the Vulkan side.

| Step | D3DKMT call | What it carries | Copy from (`amdgpu-wddm/b19-icd`) |
|---|---|---|---|
| 1 | `D3DKMTOpenAdapterFromLuid` | the LUID, or the first BC-250 | `radv_wddm2_winsys.c` |
| 2 | `D3DKMTQueryAdapterInfo`, `KMTQAITYPE_UMDRIVERPRIVATE` | 1472 bytes, magic `BC25`: the device properties of `bc250hsa_props` | `radv_wddm2_winsys.c` |
| 3 | `D3DKMTCreateDevice` | - | `radv_wddm2_winsys.c` |
| 4 | `D3DKMTCreatePagingQueue` | the paging fence that every allocation waits on | `radv_wddm2_winsys.c` |
| 5 | `D3DKMTCreateAllocation2` | the `BC2A` blob: size, physical alignment, preferred heap, cache intent | `radv_wddm2_bo.c` |
| 6 | `D3DKMTMapGpuVirtualAddress` | base 0 inside this device's window, `Protection.Write` | `radv_wddm2_bo.c` |
| 7 | `D3DKMTMakeResident` plus a wait on the paging fence | `MustSucceed` | `radv_wddm2_bo.c` |
| 8 | `D3DKMTCreateContextVirtual` | node 0, the `BC2C` blob, `HwQueueSupported = 0` | `radv_wddm2_cs.c` |
| 9 | `D3DKMTCreateSynchronizationObject2` | `D3DDDI_MONITORED_FENCE`, unshared, both addresses kept | `radv_wddm2_cs.c` |
| 10 | `D3DKMTSubmitCommand` | the `BC2S` blob: one indirect buffer, its GPU address and its byte count | `radv_wddm2_cs.c` |

DECIDED. Layer 1 does not reuse the `radeon_winsys` vtable of RADV. That abstraction drags
`util_vma`, `vk_sync`, `ac_gpu_info`, the Mesa build system and the Vulkan object model into a HIP
runtime that needs none of them. The part worth copying is the call sequence, not the abstraction.

One constraint from the kernel driver, quoted in `driver/kmd/wddm.c`: "One IB is already the ring's
whole capacity". A second submission that arrives before the first fence waits inside the display
driver interface. Layer 1 therefore serialises submissions per device and keeps a ring of command
buffer slots, each slot retired by its own fence value before it is written again. The Vulkan driver
ships on the same rule.

Why the escape path (a new mode of `BC250_FENCE_MODE_DISPATCH`) is **not** the route: the escape
runs a fixed shader from kernel-owned GART memory at VMID 0 with no allocator, no residency and no
per-process address space. It would grow the one component that must stay stable on a chip with no
working GPU reset (fact M53). It can never be the product path, because a shipping runtime cannot
ask a display miniport escape to run user code. It is also more work, not less, because it still needs
the same descriptor parsing, the same register program and the same fence, plus an in-kernel copy of
every buffer. Keep it in reserve for one case only: a named refusal of the user-mode path, with a
file and a line. The spike found none.

### 3.3 Memory and residency

| Buffer | Heap | Alignment | Flags |
|---|---|---|---|
| The loaded code object | GTT for step 1, VRAM later | 4096 for the base. The linker already aligns the entry to 256 and the descriptor to 64 | `BC250HSA_MEM_EXEC` |
| Kernel arguments | GTT, host visible | `kernarg_align`, which is at least 16 | `BC250HSA_MEM_HOST` |
| Application data (`hipMalloc`) | VRAM | 4096, raised to 64 KiB at 64 KiB and to 256 KiB at 256 KiB, as the winsys does | `BC250HSA_MEM_DEVICE` or `BC250HSA_MEM_MAPPABLE` |
| The private segment buffer | VRAM | 256 | one zeroed 4 KiB allocation per device |
| The command ring | GTT, host visible | 4096 | `BC250HSA_MEM_HOST`, eight slots of 64 KiB |

Rules that layer 1 keeps.

- Every allocation is made resident for its whole life in build 1, which is what the winsys calls
  `all_resident`. WDDM moves residency out of the submission call, so there is no per-submission
  buffer list.
- `D3DKMTMakeResident` and `D3DKMTMapGpuVirtualAddress` are asynchronous. The paging fence wait after
  each one is not optional.
- A write combined mapping needs a store fence before the submission. `bc250hsa_write_barrier()` is
  that fence, and `bc250hsa_dispatch_submit()` emits one by itself before it submits.
- The code allocation is never write combined, because the loader writes it with ordinary stores and
  the instruction fetch must see them.
- Layer 1 keeps its own GPU address window and never shares one with the Vulkan driver in the same
  process.
- The entry point register holds the address shifted right by 8, so the code allocation must be
  256-byte aligned at least. The high half is **not** `address32_hi`: a build that kept a preamble
  value of `0x80` fetched a shader at the wrong address.

OPEN for the lab: whether `D3DKMTLock2` works on a VRAM allocation on this part. Device local memory
here is a carve-out of system memory, so a mapping should be possible, and the winsys proves the
mapping for GTT only. Until a trial answers it, `BC250HSA_MEM_DEVICE` reports no host pointer and
`bc250hsa_copy_to_device()` refuses it with `BC250HSA_EUNSUPPORTED`. Layer 2 therefore allocates
`hipMalloc` memory with `BC250HSA_MEM_MAPPABLE` in build 1 and falls back to GTT when the mapping
fails. A device-side copy, with a copy kernel of ours or with the copy node, is later work: node 1 is
the copy engine and is not a user-mode queue.

The 8 GiB device-local segment ceiling that the Vulkan path meets is a kernel driver and memory map
subject. The route document already makes it a stop criterion for M16. Nothing here changes it.

### 3.4 The code object loader

DECIDED. Our own loader, about 750 lines with the MessagePack reader. Not `amdhsaloader`, which is
about 384 KB of source and near 10 000 lines, and whose client context is shaped around HSA concepts
that we do not have: an HSA instruction set architecture handle, HSA memory regions, an HSA agent and
AMD's own option parser. Taking it means taking those types, which is the coupling that makes the
other routes large. The NCSA licence of that code is permissive, so a line of it may be copied later
with one PROVENANCE line.

What the loader does, every step MEASURED on the spike's own code objects.

1. Check the header: `e_machine` is `EM_AMDGPU` (0xE0), `e_ident[EI_OSABI]` is `ELFOSABI_AMDGPU_HSA`
   (0x40), and `e_flags & EF_AMDGPU_MACH` is `EF_AMDGPU_MACH_AMDGCN_GFX1013` (0x42) or
   `EF_AMDGPU_MACH_AMDGCN_GFX10_1_GENERIC` (0x52). The measured header of the spike's object reads
   `Flags [ (0x142) EF_AMDGPU_FEATURE_XNACK_ANY_V4 (0x100), EF_AMDGPU_MACH_AMDGCN_GFX1013 (0x42) ]`.
2. Accept `e_ident[EI_ABIVERSION]` 3 (code object version 5) or 4 (version 6). Clang 22 writes 4 by
   default and `-mcode-object-version=5` writes 3.
3. Allocate one `BC250HSA_MEM_EXEC` range that covers every `PT_LOAD` segment, from the lowest
   `p_vaddr` to the highest `p_vaddr + p_memsz`, rounded up to 4096. The measured span of the `vadd`
   object is 0 to 0x42B1, so 0x5000 bytes.
4. Copy `p_filesz` bytes of each segment to `base + p_vaddr` and zero the rest of `p_memsz`. The zero
   fill is not optional: `.bss` and `.relro_padding` have `p_filesz` 0 and a non-zero `p_memsz`.
5. Apply `.rela.dyn`. MEASURED: a linked gfx1013 code object needs exactly one relocation type,
   `R_AMDGPU_RELATIVE64` (type 13, calculation `B + A`), and the probe produced two records, both in
   `.data.rel.ro`, from an array of device function pointers. The `vadd` object has none. The loader
   writes `*(uint64_t*)(base + r_offset) = base + r_addend` and refuses every other type, and also
   refuses a record that names a symbol, because a named symbol means an unresolved external from a
   `-fgpu-rdc` build with no device link step.
6. Read the `NT_AMDGPU_METADATA` note and keep one record per kernel: the argument list, the segment
   sizes, the register counts, the wave size and the `.symbol` name.
7. Resolve each `.symbol` ("`<kernel>.kd`") in `.dynsym` and report `base + st_value` as the
   descriptor address. MEASURED on the `vadd` object: `vadd` is a `FUNC` at 0x1E00 and `vadd.kd` is an
   `OBJECT` of 64 bytes at 0xC80.

The loader needs no global offset table, no procedure linkage table, no lazy binding, no symbol
versioning, no `DT_NEEDED` and no initialiser array. MEASURED: the dynamic table of the `vadd` object
holds only `SYMTAB`, `SYMENT`, `STRTAB`, `STRSZ`, `GNU_HASH`, `HASH` and `NULL`, and the object with
relocations adds `RELA`, `RELASZ`, `RELAENT` and `RELACOUNT`.

The offload bundle reader, about 80 lines. The format is a 24-byte magic string
`__CLANG_OFFLOAD_BUNDLE__`, an 8-byte entry count, then per entry an 8-byte offset, an 8-byte size,
an 8-byte identifier length and the identifier, then the code objects. Three MEASURED traps that a
naive reader does not see:

1. The host entry has size 0.
2. The host entry has the **same** offset as the device entry. A reader that keys entries by offset,
   or that asserts that offsets do not repeat, fails here.
3. The host entry identifier names `x86_64-unknown-linux-gnu` even on a Windows compile, because
   clang writes a fixed placeholder triple. Never match on the host triple.

The matching rule, in this order:

1. Check the magic. Refuse `CCOB`, a compressed bundle, by name.
2. Skip every entry whose size is 0.
3. Accept an identifier that starts with `hip-` or `hipv4-`, that holds the triple
   `amdgcn-amd-amdhsa`, and whose target id is `gfx1013`, `gfx10-1-generic`, or one of those two with
   feature suffixes.
4. Prefer the exact processor over the generic one.
5. Check that the body starts with `\x7fELF`.
6. Hand the view on, with no copy.

The measured device code object starts at offset 4096 inside the fat binary, which agrees with
clang's `HIPCodeObjectAlign`.

### 3.5 The kernel descriptor

The descriptor is 64 bytes, 64-byte aligned. MEASURED byte for byte against the spike's three
descriptors, and read against `AMDGPUUsage.rst`.

| Offset | Size | Field | `vadd`, measured | Where it goes |
|---|---|---|---|---|
| 0 | 4 | `GROUP_SEGMENT_FIXED_SIZE` | 0 | `group_segment_bytes`, and the `LDS_SIZE` computation |
| 4 | 4 | `PRIVATE_SEGMENT_FIXED_SIZE` | 0 | `private_segment_bytes`, and `COMPUTE_TMPRING_SIZE` |
| 8 | 4 | `KERNARG_SIZE` | 0x1C = 28 | a cross-check of `.kernarg_segment_size` |
| 12 | 4 | reserved | 0 | refuse a non-zero value |
| 16 | 8 | `KERNEL_CODE_ENTRY_BYTE_OFFSET` | 0x1180 | `entry_va = descriptor_va + offset`. 0xC80 + 0x1180 = 0x1E00, which is the measured `.text` start |
| 24 | 20 | reserved | 0 | refuse a non-zero value |
| 44 | 4 | `COMPUTE_PGM_RSRC3` | 0 | copied unchanged |
| 48 | 4 | `COMPUTE_PGM_RSRC1` | 0xE0AF0000 | copied unchanged |
| 52 | 4 | `COMPUTE_PGM_RSRC2` | 0x8C | copied, then `LDS_SIZE` written into bits 23:15. Bits 5:1 are the user SGPR count, measured 6 |
| 56 | 2 | `KERNEL_CODE_PROPERTIES` | 0x0409 | the `ENABLE_SGPR_*` bits that drive the user data plan |
| 58 | 2 | `KERNARG_PRELOAD` | 0 | refuse a non-zero value: this build does not preload |
| 60 | 4 | reserved | 0 | refuse a non-zero value |

`KERNEL_CODE_PROPERTIES` 0x0409 is bit 0 `ENABLE_SGPR_PRIVATE_SEGMENT_BUFFER`, bit 3
`ENABLE_SGPR_KERNARG_SEGMENT_PTR` and bit 10 `ENABLE_WAVEFRONT_SIZE32`, which agrees with the
assembly of the same kernel: `.amdhsa_user_sgpr_count 6`, a private segment buffer in `s[0:3]`, the
kernel argument pointer in `s[4:5]`, the workgroup index in `s6`, wave size 32 and workgroup
processor mode 1.

The user SGPRs are dense, in the documented order: private segment buffer (4 registers), dispatch
pointer (2), queue pointer (2), kernel argument pointer (2), dispatch id (2), flat scratch init (2),
private segment size (1). `bc250hsa_plan_user_sgprs()` walks the enable bits in that order and places
each item at the next free `COMPUTE_USER_DATA` register. Do not hard-code `s[0:3]` and `s[4:5]`: that
is right for the three measured kernels only because they enable exactly the first and the fourth
item. An enabled item that this build does not program is refused with `BC250HSA_EUNSUPPORTED`.

The dispatch pointer is programmed, and it was not until 2026-10-09. MEASURED on llama.cpp's built
`ggml-hip.dll`: 1752 of its 7105 gfx1013 kernels enable `ENABLE_SGPR_DISPATCH_PTR` and the other
5353 do not, and the reason is one builtin. `__builtin_amdgcn_workgroup_size_x`, which is what HIP's
`blockDim.x` becomes, is a 16-bit load from the AQL kernel dispatch packet, so a kernel with a
run-time block size reads the packet while a kernel with a compile-time one does not. A PM4 dispatch
has no packet, and all 1752 were refused by name: every `k_get_rows`, every `soft_max_f32`, half of
the `k_bin_bcast` set and half of the `mul_mat_q` set, which is defect BD-110 and the whole reason
no model ran in part 3B. Section 7.1 of the header is the answer: the packet goes at the end of the
kernel argument buffer the caller already allocates for each dispatch, it states that dispatch and
nothing else, and the two registers hold its address. The four items still refused are the queue pointer,
the dispatch id, the flat scratch init and the private segment size.

The private segment buffer is a requirement even when `PRIVATE_SEGMENT_FIXED_SIZE` is 0, because
clang always requests it. Layer 1 points it at one zeroed, resident 4 KiB allocation per device. RADV
does the same with its own zero buffer object. The 128-bit resource for a raw buffer on gfx10.1 is
the libdrm-proven word 3 value `0x1104BFAC`: format 0x4B (32_32_32_32 uint), `RESOURCE_LEVEL` 1,
which gfx10.1 needs, `OOB_SELECT` 1 and type 0. `bc250hsa_buffer_resource()` builds it.

### 3.6 Kernel arguments

DECIDED. The packer reads the metadata `.args` list and nothing else. It never computes an offset of
its own.

The explicit arguments. `hipLaunchKernel` receives `void** args`, one pointer per parameter in
declaration order, because the kernel stub that clang emits stores the address of each parameter into
a local array. The packer walks the list and the array in step: for each entry that is not a hidden
kind, copy `entry.size` bytes from `args[j]` to `kernarg + entry.offset` and advance `j`. A
`global_buffer` entry takes the device pointer by value, which is why its size is 8.

The hidden arguments. MEASURED: a kernel that does not read the implicit argument pointer has no
implicit block at all (`vadd` has `kernarg_segment_size` 28, the four explicit arguments only), and
when the block exists the metadata states its layout (`writeGridSize` has 264, which is 8 explicit
bytes and a 256-byte block). Measured offsets of that kernel:

| `.value_kind` | `.offset` | `.size` | What the packer writes |
|---|---|---|---|
| `hidden_block_count_x/y/z` | 8, 12, 16 | 4 | the work-group count per dimension, which is `bc250hsa_launch.grid` unchanged |
| `hidden_group_size_x/y/z` | 20, 22, 24 | 2 | the workgroup size, per dimension |
| `hidden_remainder_x/y/z` | 26, 28, 30 | 2 | 0, because a HIP grid is a whole number of workgroups and has no partial one |
| `hidden_global_offset_x/y/z` | 48, 56, 64 | 8 | 0, because HIP has no global offset |
| `hidden_grid_dims` | 72 | 2 | 1, 2 or 3 |

Read the first row twice. `hidden_block_count_*` is **not** the grid size in work items.
`AMDGPUUsage.rst` states it for code object v5: "The grid dispatch work-group count for the X
dimension is passed in the kernarg ... This is not the same as the value in the AQL dispatch packet,
which has the grid size in work-items" (lines 5726 to 5750 of the revision in section Sources). A
packer that multiplies the grid by the block here gives every kernel `gridDim.x * blockDim.x` for
`gridDim.x`. A kernel of the `if (i < n)` shape hides that error, and a kernel that divides work by
`gridDim.x` computes wrong numbers with no fault and no message. `hidden_remainder_*` is the size of
the partial last workgroup, which this interface cannot express, so it is 0 and not a modulo.

Note where the block starts and where it stops. In this kernel it starts at offset 8, behind the one
explicit argument, so no offset of the table is the offset of the same field in another kernel: the
same `hidden_block_count_x` sits at 32 in the four-argument `vadd` of the layer-2 fixture. The block
also has holes, because the compiler writes an entry only for a field the kernel reads: bytes 32 to
47 of this kernel are named by no entry at all. Both are the measured reason why no fixed structure
may describe the block, and why the packer reads the list.

`hidden_dynamic_lds_size` takes `bc250hsa_launch.dynamic_group_bytes`, which is the same number that
decision 3 adds to `COMPUTE_PGM_RSRC2.LDS_SIZE`. Every other hidden kind that this build does not
fill is zeroed and counted:
`hidden_hostcall_buffer`, `hidden_printf_buffer`, `hidden_heap_v1`, `hidden_multigrid_sync_arg`,
`hidden_queue_ptr`, `hidden_private_base`, `hidden_shared_base`,
`hidden_default_queue` and `hidden_completion_action`. An unknown key is zeroed, counted and named in
the log, and the dispatch continues, because a later code object version may add a key. A non-zero
`hostcall_buffer_requests` counter is the measurement that answers kill criterion K4, which is
device-side `printf`. We read that answer from a counter and not from a crash.

### 3.7 The PM4 dispatch stream

Register offsets. `SET_SH_REG` names a register by its offset from the packet base. For the compute
block the arithmetic is `offset = (byte address - 0xB000) / 4 = mm<name> - 0x19A0`. Layer 1 resolves
every offset through `third_party/linux-amdgpu/gc_10_1_0_offset.h`, as `driver/shim/bc250_dispatch.c`
does. The table below is the control, not the source.

| Register | Byte address | AMD header dword | `SET_SH_REG` offset |
|---|---|---|---|
| `COMPUTE_START_X/Y/Z` | 0xB810-0xB818 | 0x1BA4-0x1BA6 | 0x204-0x206 |
| `COMPUTE_NUM_THREAD_X/Y/Z` | 0xB81C-0xB824 | 0x1BA7-0x1BA9 | 0x207-0x209 |
| `COMPUTE_PGM_LO` / `_HI` | 0xB830 / 0xB834 | 0x1BAC / 0x1BAD | 0x20C / 0x20D |
| `COMPUTE_PGM_RSRC1` / `RSRC2` | 0xB848 / 0xB84C | 0x1BB2 / 0x1BB3 | 0x212 / 0x213 |
| `COMPUTE_RESOURCE_LIMITS` | 0xB854 | 0x1BB5 | 0x215 |
| `COMPUTE_STATIC_THREAD_MGMT_SE0/SE1` | 0xB858 / 0xB85C | 0x1BB6 / 0x1BB7 | 0x216 / 0x217 |
| `COMPUTE_TMPRING_SIZE` | 0xB860 | 0x1BB8 | 0x218 |
| `COMPUTE_STATIC_THREAD_MGMT_SE2/SE3` | 0xB864 / 0xB868 | 0x1BB9 / 0x1BBA | 0x219 / 0x21A |
| `COMPUTE_REQ_CTRL` | 0xB888 | 0x1BC2 | 0x222 |
| `COMPUTE_PGM_RSRC3` | 0xB8A0 | 0x1BC8 | 0x228 |
| `COMPUTE_SHADER_CHKSUM` | 0xB8A8 | 0x1BCA | 0x22A |
| `COMPUTE_USER_DATA_0` to `_15` | 0xB900-0xB93C | 0x1BE0-0x1BEF | 0x240-0x24F |

The three offsets that libdrm writes as literals, 0x204, 0x20C and 0x240, fall out of the same
arithmetic, which is the control that the table is right.

The stream, one dispatch. Every `SET_SH_REG`, `SET_SH_REG_INDEX` and `DISPATCH_DIRECT` header carries
the shader-type bit, which is bit 1 of a type-3 header and selects the compute register space. The one
`SET_UCONFIG_REG` does not.

| # | Packet | Payload | Why |
|---|---|---|---|
| 1 | `CONTEXT_CONTROL` | 0x80000000, 0x80000000 | the graphics ring needs it, and it is a no-operation on a compute ring. `BC250HSA_DISPATCH_GFX_RING` |
| 2 | `ACQUIRE_MEM` | `CP_COHER_CNTL` 0, size 0xFFFFFFFF / 0xFFFFFF, base 0, poll 0x0A, `GCR_CNTL` with `GL2_INV`, `GL2_WB`, `GLM_INV`, `GLM_WB`, `GL1_INV`, `GLV_INV`, `GLK_INV`, `GLI_INV` | the kernel driver's ring emits no cache acquire in front of an indirect buffer. Without it the shader can be fetched through a stale instruction cache line |
| 3 | `SET_SH_REG` 0x204, 3 | 0, 0, 0 | `COMPUTE_START_X/Y/Z` |
| 4 | `SET_SH_REG` 0x22A, 1 | 0 | `COMPUTE_SHADER_CHKSUM` |
| 5 | `SET_SH_REG` 0x222, 6 | six zeros | `COMPUTE_REQ_CTRL` and the five behind it |
| 6 | `SET_UCONFIG_REG` `CP_COHER_START_DELAY`, 1 | 0x20 | a performance knob of `ACQUIRE_MEM`. Droppable |
| 7 | `SET_SH_REG_INDEX` index 3, 0x216, 2 | 0xFFFFFFFF, 0xFFFFFFFF | `STATIC_THREAD_MGMT_SE0/SE1`, every compute unit. Droppable with `BC250HSA_DISPATCH_NO_CU_MASK` |
| 8 | `SET_SH_REG_INDEX` index 3, 0x219, 2 | 0xFFFFFFFF, 0xFFFFFFFF | `SE2/SE3`, the same |
| 9 | `SET_SH_REG` 0x20C, 2 | `entry_va >> 8`, `(entry_va >> 40) & 0xFF` | `COMPUTE_PGM_LO` and `_HI` |
| 10 | `SET_SH_REG` 0x212, 2 | `RSRC1`, `RSRC2` with the computed `LDS_SIZE` | section 3.5 and decision 3 |
| 11 | `SET_SH_REG` 0x228, 1 | `RSRC3` | gfx10 and later |
| 12 | `SET_SH_REG` 0x218, 1 | 0, or the waves and wave size encoding | `COMPUTE_TMPRING_SIZE`. 0 when the kernel needs no scratch |
| 13 | `SET_SH_REG` 0x207, 3 | `block.x`, `block.y`, `block.z` | `COMPUTE_NUM_THREAD_X/Y/Z` |
| 14 | `SET_SH_REG` 0x240, n | the user data plan of section 3.5 | one packet per run of consecutive registers |
| 15 | `SET_SH_REG` 0x215, 1 | 0 | `COMPUTE_RESOURCE_LIMITS` |
| 16 | `DISPATCH_DIRECT` | `grid.x`, `grid.y`, `grid.z` in workgroups, then the initiator | the initiator is `COMPUTE_SHADER_EN` plus `CS_W32_EN` when the kernel descriptor says wave32, which every kernel of this build does. `FORCE_START_AT_000` with `BC250HSA_DISPATCH_START_AT_000` |
| 17 | `EVENT_WRITE` | `EVENT_TYPE(CS_PARTIAL_FLUSH)`, `EVENT_INDEX(4)` | the command processor waits for the waves |
| 18 | `RELEASE_MEM` | the 8-dword completion write of section 3.8 | this dispatch's fence value |
| 19 | type-3 `NOP`, count field 0x3FFF | as many as needed | the graphics ring pads an indirect buffer to 8 dwords. amdgpu and the shim call `BC250HSA_CP_NOP` a PACKET2 NOP. It is a type-3 NOP |

Packets 1 to 17 without the user data are the 72 dwords of fact M49 plus the graphics-ring
`CONTEXT_CONTROL`. Packets 7 and 8 are the first to drop if the command processor refuses the
opcode.

What the kernel driver adds around the buffer: an `INDIRECT_BUFFER` packet and its own ring fence, a
`RELEASE_MEM` with `CACHE_FLUSH_AND_INV_TS`, `GCR_GL2_WB` and `GCR_GLM_WB`, and nothing else. So the
host must emit its own acquire in front, which is packet 2, and it needs no second flush behind packet
18 for the central processor to read the results.

### 3.8 The bounded wait, and a lost device

The completion value, the fast path: the indirect buffer writes the monitored fence itself. The
synchronisation object returns both a host address and a GPU address for its value. The last packet is
a 64-bit `RELEASE_MEM` to the GPU address, bit for bit our Vulkan driver's own progress write:

```
dword 0:    PKT3(RELEASE_MEM, 6, 0)
dword 1:    0x06603514   /* CACHE_FLUSH_AND_INV_TS, EVENT_INDEX 5, GLM_WB, GLM_INV,
                            GL2_WB, SEQ 1, cache policy 3 */
dword 2:    0x40000000   /* DATA_SEL 2 (a 64-bit value), INT_SEL 0, DST_SEL 0 (memory) */
dwords 3-4: the fence GPU address, 8-byte aligned
dwords 5-6: the value
dword 7:    0
```

Both constant dwords carry a static assertion in the Vulkan driver, and run A0 of `tools/win/monfence`
measured the whole mechanism on unit A: the host mapping held the value 64 microseconds after the
write, the operating system wait woke, and a second context's GPU wait on the same fence released.
Eight dwords are already a multiple of the graphics padding.

The slow path, `D3DKMTSignalSynchronizationObjectFromGpu2` after the submission, is for a fence that
another process waits on. Layer 1 does not use it. Never mix the two on one fence: a kernel signal of
an older value can land after a newer GPU write.

The wait, and this is the one piece of policy that layer 1 keeps for itself:

1. Read the fence's host mapping. If it already holds the value, return. Most waits end here.
2. Otherwise wait on the operating system object with an asynchronous event.
3. Wait in slices of `slice_ms`, to a total of `total_ms`. After every slice, re-read the fence value
   and ask `D3DKMTGetDeviceState` with `D3DKMT_DEVICESTATE_EXECUTION` whether the device is still
   active. Stop early only on a real loss: the fence reads `UINT64_MAX`, or the device is not active.

Why slices and not one long wait: when another process hangs the engine, this device's work waits
behind it until the scheduler resets the engine and resubmits. That took 14.6 seconds in lab trial D1
of kernel driver 0.7.216.17 with `TdrDelay` 10, and a single 10-second wait turned a healthy wait into
a lost device (defect K225). A runtime that copies the naive form reports `hipErrorLaunchTimeOut` on
healthy work. Defaults 1000 and 120000 therefore stay in the library.

A defect not to copy: the older Mesa tree's fence wait reads an `ExecutionState` field without ever
calling `D3DKMTGetDeviceState`. Copy the `b19-icd` form.

On a lost device, layer 1 marks the device, refuses every later call with `BC250HSA_EDEVICELOST` and
never resubmits. `bc250hsa_query_fault()` reads the page fault block, which gives the faulting virtual
address, the error code and the pipeline stage. That is the first diagnostic for a wrong descriptor or
a wrong user data register. The kernel driver's own refusals are visible in its log ring: a refused
submission writes "umd submit fence N not run: <reason>" and names which condition failed. Every
step-1 trial pulls that ring.

Timeout safety. A kernel that does not finish inside the operating system timeout is a device reset for
the whole machine. The lab runs `TdrDelay` 10. Step-1 kernels stay far under one second: 1048576 floats
is microseconds of work. The host never polls a GPU address without a bound, and never spins on a fence
while it holds a lock that the completion path needs.

### 3.9 The public header

This text is the contract between the two layers. The file in the repository,
`compute/hip/include/bc250hsa.h`, is normative, and this section is a copy of it.
`compute/hip/build.ps1 -CheckDoc` compares the two and fails the build when they differ, in the style
of the constant checks in `tools/win/monfence/build.ps1`.

```c
/* bc250hsa.h - the gfx1013 compute submission layer of the BC-250 Windows driver.
 *
 * Milestone M16, gate G5, route B (our own thin HIP runtime). The design that this
 * header belongs to is docs/design/m16-hip-route-b.md. Layer 1 of that design builds
 * this interface. Layer 2 (amdhip64.dll) uses this interface and nothing else of the
 * submission path. The hipBLAS shim and the step-1 host tool use it as well.
 *
 * What this layer owns: one WDDM device on the BC-250 adapter, its memory, the code
 * object loader, the kernel argument packer, the PM4 dispatch stream, the batching of
 * dispatches into one indirect buffer, one monitored fence and one bounded wait.
 *
 * What this layer does not own: HIP semantics, streams, events, modules per process,
 * and the offload bundle policy of a compiler. Those belong to layer 2.
 *
 * Rules of the interface:
 *   1. Every function returns BC250HSA_OK (0) or a negative bc250hsa_status.
 *      bc250hsa_fence_read and bc250hsa_status_string are the two exceptions; they
 *      cannot fail.
 *   2. No call waits without a bound. A wait states its slice and its total.
 *   3. No HIP type, no Mesa type and no D3DKMT type appears here. A test build can
 *      therefore replace the device half with a mock and keep the loader.
 *   4. Every structure that the caller allocates and fills carries struct_bytes as its
 *      first field. The library refuses a size it does not know with BC250HSA_EINVAL.
 *      Structures that the library owns (bc250hsa_kernel, bc250hsa_arg) carry no size
 *      field; BC250HSA_ABI_VERSION covers them. bc250hsa_mem is a plain handle.
 *   5. One device serialises its submissions. The kernel driver runs one indirect
 *      buffer at a time (driver/kmd/wddm.c, "One IB is already the ring's whole
 *      capacity"), so a second dispatch waits for a free ring slot under the same
 *      bound as bc250hsa_wait.
 *   6. Nothing in this header reads or writes a registry value, a file or an
 *      environment variable. The caller passes every policy as a parameter.
 */

#ifndef BC250HSA_H
#define BC250HSA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of this interface. A build of layer 2 records it and refuses a library
 * that reports another major value. bc250hsa_abi_version() returns the value that the
 * library was built with. */
#define BC250HSA_ABI_VERSION_MAJOR 1u
#define BC250HSA_ABI_VERSION_MINOR 2u   /* 1.1 added section 8.1, batching;
                                         * 1.2 added section 7.1, the AQL dispatch packet
                                         * that ENABLE_SGPR_DISPATCH_PTR needs */

uint32_t bc250hsa_abi_version_major(void);
uint32_t bc250hsa_abi_version_minor(void);

/* ---------------------------------------------------------------------------------
 * 1. Status
 * ------------------------------------------------------------------------------- */

typedef enum bc250hsa_status {
    BC250HSA_OK = 0,

    /* General */
    BC250HSA_EINVAL = -1,        /* a parameter, a size field or an alignment is wrong */
    BC250HSA_ENOMEM = -2,        /* no host memory, or no device memory of that size */
    BC250HSA_ENODEV = -3,        /* no BC-250 adapter, or the device is already closed */
    BC250HSA_ETIMEOUT = -4,      /* the bounded wait ran out and the device still lives */
    BC250HSA_EDEVICELOST = -5,   /* a reset, a page fault, or the fence read UINT64_MAX */
    BC250HSA_EUNSUPPORTED = -6,  /* the request needs a part of the design not built yet */
    BC250HSA_EOS = -7,           /* a Windows call failed; bc250hsa_last_os_status() has it */
    BC250HSA_ENOTFOUND = -8,     /* no kernel, no symbol or no bundle entry of that name */
    BC250HSA_EBUSY = -9,         /* no free command ring slot inside the bound */

    /* Code object and bundle */
    BC250HSA_EBADELF = -20,          /* not ELF64, not EM_AMDGPU, or not ELFOSABI_AMDGPU_HSA */
    BC250HSA_EWRONGTARGET = -21,     /* e_flags names another processor than this device */
    BC250HSA_EBADABIVERSION = -22,   /* EI_ABIVERSION is not 3 (v5) and not 4 (v6) */
    BC250HSA_EBADRELOC = -23,        /* a type other than R_AMDGPU_RELATIVE64, or a named symbol */
    BC250HSA_EBADMETADATA = -24,     /* no NT_AMDGPU_METADATA note, or it does not parse */
    BC250HSA_EBADBUNDLE = -25,       /* the offload bundle header does not parse */
    BC250HSA_ECOMPRESSEDBUNDLE = -26 /* a compressed bundle ("CCOB"); rebuild without
                                      * --offload-compress */
} bc250hsa_status;

/* A short English name of a status. Never NULL. */
const char* bc250hsa_status_string(bc250hsa_status status);

/* The NTSTATUS of the last Windows call that failed on this thread, or 0. Only
 * meaningful right after a call returned BC250HSA_EOS. */
int32_t bc250hsa_last_os_status(void);

/* ---------------------------------------------------------------------------------
 * 2. Log hook
 * ------------------------------------------------------------------------------- */

#define BC250HSA_LOG_ERROR 0u
#define BC250HSA_LOG_WARN  1u
#define BC250HSA_LOG_INFO  2u
#define BC250HSA_LOG_TRACE 3u

typedef void (*bc250hsa_log_fn)(void* ctx, uint32_t level, const char* message);

/* Installs one log sink for the process. fn NULL removes it. The library never writes
 * to a stream of its own. Messages hold no application data. */
void bc250hsa_set_log(bc250hsa_log_fn fn, void* ctx, uint32_t max_level);

/* ---------------------------------------------------------------------------------
 * 3. Counters
 * ------------------------------------------------------------------------------- */

/* Process counters. They answer design questions without a debugger, and
 * hostcall_buffer_requests is the measurement that answers kill criterion K4 of the
 * route document, which is device-side printf. */
typedef struct bc250hsa_counters {
    uint32_t struct_bytes;
    uint64_t modules_loaded;
    uint64_t dispatches_built;
    uint64_t submissions;
    uint64_t submissions_refused;
    uint64_t waits;
    uint64_t waits_fast;              /* the fence mapping already held the value */
    uint64_t waits_timed_out;
    uint64_t device_losses;
    uint64_t hidden_args_zeroed;
    uint64_t unknown_arg_kinds;
    uint64_t hostcall_buffer_requests;
    uint64_t dynamic_stack_refusals;
    /* Section 8.1. submissions / dispatches_built is the number this interface is
     * measured by: one submission per dispatch is one user-to-kernel transition per
     * kernel. batches_submitted counts the submissions that carried more than one
     * dispatch, and dispatches_batched the dispatches that rode in them. */
    uint64_t batches_submitted;
    uint64_t dispatches_batched;
} bc250hsa_counters;

bc250hsa_status bc250hsa_counters_read(bc250hsa_counters* out);
void            bc250hsa_counters_reset(void);

/* ---------------------------------------------------------------------------------
 * 4. Device
 * ------------------------------------------------------------------------------- */

typedef struct bc250hsa_device bc250hsa_device;
typedef struct bc250hsa_module bc250hsa_module;

#define BC250HSA_OPEN_NO_GPU_SUBMIT 0x1u /* open everything but the context and the fence;
                                          * for a host test of the loader on a machine
                                          * with no BC-250 adapter */

typedef struct bc250hsa_open_params {
    uint32_t struct_bytes;
    uint32_t flags;
    uint32_t luid_low;            /* 0 and 0: the first BC-250 adapter */
    int32_t  luid_high;
    uint32_t ring_slots;          /* command buffer slots, 0 takes 8 */
    uint32_t ring_slot_bytes;     /* 0 takes 65536 */
    uint32_t va_window_gib;       /* the GPU address window of this device, 0 takes 64 */
} bc250hsa_open_params;

/* Opens the adapter, creates the device, the paging queue, one node-0 context and one
 * unshared monitored fence, and allocates the command ring and the zero page that backs
 * the private segment buffer. params NULL takes every default.
 *
 * A note for a caller that tells a device pointer from a host pointer by its value,
 * which is what layer 2 does for hipMemcpyDefault: while the GPU address window lies
 * inside the host user address space of a 64-bit Windows process (0 to
 * 0x00007FFFFFFFFFFF), a GPU virtual address can hold the same value as a host pointer
 * of the same process. A window above that range removes the ambiguity. Until the
 * window is there, the caller must pass the direction of a copy and must not guess it. */
bc250hsa_status bc250hsa_open(const bc250hsa_open_params* params, bc250hsa_device** out);

/* Waits for the last submission under the default bound, frees every allocation this
 * device still owns, and closes the adapter. Safe with dev NULL. */
void bc250hsa_close(bc250hsa_device* dev);

typedef struct bc250hsa_props {
    uint32_t struct_bytes;
    char     name[64];                  /* "AMD BC-250" */
    uint32_t gfx_ip_major;              /* 10 */
    uint32_t gfx_ip_minor;              /* 1 */
    uint32_t gfx_ip_rev;                /* 3 */
    uint32_t pci_bus, pci_device, pci_function;
    uint32_t wave_size;                 /* 32 */
    uint32_t cu_count;                  /* compute units the driver reports as enabled */
    uint32_t se_count;
    uint32_t max_workgroup_size;
    uint32_t max_workgroups_per_dim;
    uint32_t lds_bytes_per_workgroup;   /* 65536 on this part */
    uint32_t waves_per_cu;
    uint64_t vram_bytes;
    uint64_t visible_vram_bytes;
    uint64_t gtt_bytes;
    uint32_t gfx_clock_khz;             /* the clock the driver reports now */
    uint32_t mem_clock_khz;
    uint32_t mem_bus_width;
    uint32_t kmd_version[4];            /* the kernel driver version, four parts */
} bc250hsa_props;

bc250hsa_status bc250hsa_props_read(bc250hsa_device* dev, bc250hsa_props* out);

/* ---------------------------------------------------------------------------------
 * 5. Memory
 * ------------------------------------------------------------------------------- */

#define BC250HSA_MEM_DEVICE    0x0u /* device local, no host mapping asked for */
#define BC250HSA_MEM_HOST      0x1u /* host visible and cached */
#define BC250HSA_MEM_HOST_WC   0x2u /* host visible and write combined */
#define BC250HSA_MEM_EXEC      0x4u /* holds instructions: 4096-aligned base, never
                                     * write combined */
#define BC250HSA_MEM_MAPPABLE  0x8u /* device local, and a later bc250hsa_map may work */
#define BC250HSA_MEM_ZERO      0x10u/* the library zeroes the range before it returns */

typedef struct bc250hsa_mem {
    uint64_t va;        /* the GPU virtual address that the packets use */
    void*    host;      /* the host mapping, or NULL */
    uint64_t bytes;     /* the size the device sees, after alignment */
    uint32_t flags;     /* the flags this allocation was made with */
    uint32_t reserved;
    void*    opaque;    /* the library's handle; never read it */
} bc250hsa_mem;

/* Creates the allocation, maps a GPU address inside this device's window, makes it
 * resident, and waits for the paging fence. On return the memory is usable by a
 * submission. alignment 0 takes the natural one for the flags. */
bc250hsa_status bc250hsa_alloc(bc250hsa_device* dev, uint64_t bytes, uint64_t alignment,
                               uint32_t flags, bc250hsa_mem* out);

/* Frees the allocation. The caller must know that no submission still reads it. */
bc250hsa_status bc250hsa_free(bc250hsa_device* dev, bc250hsa_mem* mem);

/* A host mapping for an allocation made without one. Refuses BC250HSA_MEM_DEVICE
 * without BC250HSA_MEM_MAPPABLE, with BC250HSA_EUNSUPPORTED.
 *
 * out is the one result of this call. On BC250HSA_OK the mapping is in *out, and
 * out NULL is BC250HSA_EINVAL. This implementation also stores the pointer in
 * mem->host, but that field is an implementation detail: a caller reads *out and
 * keeps the pointer in its own copy of the handle. A caller that reads mem->host
 * after a map instead of *out works with this implementation of the header and can
 * fail with the next. bc250hsa_unmap takes the mapping back, and it clears
 * mem->host of the handle the caller gave it. */
bc250hsa_status bc250hsa_map(bc250hsa_device* dev, bc250hsa_mem* mem, void** out);
bc250hsa_status bc250hsa_unmap(bc250hsa_device* dev, bc250hsa_mem* mem);

/* Drains the write buffers of the host. Call it after a write into a write combined
 * allocation and before the submission that reads it. */
void bc250hsa_write_barrier(void);

/* Host to device and device to host copies through the host mapping. They refuse an
 * allocation with no mapping, with BC250HSA_EUNSUPPORTED: a copy engine and a copy
 * kernel are later work, named in the design. */
bc250hsa_status bc250hsa_copy_to_device(bc250hsa_device* dev, const bc250hsa_mem* dst,
                                        uint64_t dst_offset, const void* src, uint64_t bytes);
bc250hsa_status bc250hsa_copy_from_device(bc250hsa_device* dev, void* dst,
                                          const bc250hsa_mem* src, uint64_t src_offset,
                                          uint64_t bytes);

/* An allocator seam, so that the loader runs with no device. The device path passes
 * its own implementation. A host test passes one over malloc with a synthetic base
 * address. */
typedef struct bc250hsa_allocator {
    void* ctx;
    bc250hsa_status (*alloc)(void* ctx, uint64_t bytes, uint64_t alignment, uint32_t flags,
                             bc250hsa_mem* out);
    void            (*free)(void* ctx, bc250hsa_mem* mem);
} bc250hsa_allocator;

/* The device's own allocator, for a caller that holds the loader and the device apart. */
bc250hsa_status bc250hsa_device_allocator(bc250hsa_device* dev, bc250hsa_allocator* out);

/* ---------------------------------------------------------------------------------
 * 6. Code objects
 * ------------------------------------------------------------------------------- */

/* The kinds of the metadata .args list. The packer knows every field by its kind and
 * never by a fixed offset: in a measured kernel hidden_block_count_z and
 * hidden_group_size_x share one offset. */
typedef enum bc250hsa_arg_kind {
    BC250HSA_ARG_BY_VALUE = 0,
    BC250HSA_ARG_GLOBAL_BUFFER,
    BC250HSA_ARG_DYNAMIC_SHARED_POINTER,
    BC250HSA_ARG_SAMPLER,
    BC250HSA_ARG_IMAGE,
    BC250HSA_ARG_PIPE,
    BC250HSA_ARG_QUEUE,
    BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X,
    BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Y,
    BC250HSA_ARG_HIDDEN_BLOCK_COUNT_Z,
    BC250HSA_ARG_HIDDEN_GROUP_SIZE_X,
    BC250HSA_ARG_HIDDEN_GROUP_SIZE_Y,
    BC250HSA_ARG_HIDDEN_GROUP_SIZE_Z,
    BC250HSA_ARG_HIDDEN_REMAINDER_X,
    BC250HSA_ARG_HIDDEN_REMAINDER_Y,
    BC250HSA_ARG_HIDDEN_REMAINDER_Z,
    BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_X,
    BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Y,
    BC250HSA_ARG_HIDDEN_GLOBAL_OFFSET_Z,
    BC250HSA_ARG_HIDDEN_GRID_DIMS,
    BC250HSA_ARG_HIDDEN_DYNAMIC_LDS_SIZE,
    BC250HSA_ARG_HIDDEN_PRINTF_BUFFER,
    BC250HSA_ARG_HIDDEN_HOSTCALL_BUFFER,
    BC250HSA_ARG_HIDDEN_HEAP_V1,
    BC250HSA_ARG_HIDDEN_DEFAULT_QUEUE,
    BC250HSA_ARG_HIDDEN_COMPLETION_ACTION,
    BC250HSA_ARG_HIDDEN_MULTIGRID_SYNC_ARG,
    BC250HSA_ARG_HIDDEN_QUEUE_PTR,
    BC250HSA_ARG_HIDDEN_PRIVATE_BASE,
    BC250HSA_ARG_HIDDEN_SHARED_BASE,
    BC250HSA_ARG_HIDDEN_OTHER   /* a key this build does not know: zero fill and count */
} bc250hsa_arg_kind;

#define BC250HSA_AS_NONE     0u
#define BC250HSA_AS_GLOBAL   1u
#define BC250HSA_AS_PRIVATE  2u
#define BC250HSA_AS_LOCAL    3u
#define BC250HSA_AS_CONSTANT 4u
#define BC250HSA_AS_GENERIC  5u

typedef struct bc250hsa_arg {
    uint32_t offset;        /* .offset, bytes from the start of the kernarg buffer */
    uint32_t size;          /* .size */
    uint16_t kind;          /* bc250hsa_arg_kind */
    uint8_t  address_space; /* BC250HSA_AS_* */
    uint8_t  value_align;   /* .value_align, or 0 */
} bc250hsa_arg;

/* Everything a dispatch and a packer need for one kernel. The library owns this
 * structure; it lives as long as its module. */
typedef struct bc250hsa_kernel {
    const char* name;                   /* the metadata .name, the device side name */
    uint64_t descriptor_va;             /* the loaded .symbol object, 64-byte aligned */
    uint64_t entry_va;                  /* descriptor_va + KERNEL_CODE_ENTRY_BYTE_OFFSET */
    uint32_t kernarg_bytes;             /* .kernarg_segment_size */
    uint32_t kernarg_align;             /* max(.kernarg_segment_align, 16) */
    uint32_t group_segment_bytes;       /* .group_segment_fixed_size */
    uint32_t private_segment_bytes;     /* .private_segment_fixed_size */
    uint32_t max_flat_workgroup_size;
    uint16_t sgpr_count;
    uint16_t vgpr_count;
    uint8_t  wave_size;                 /* 32 on this part */
    uint8_t  workgroup_processor_mode;
    uint8_t  uses_dynamic_stack;
    uint8_t  user_sgpr_count;           /* read out of COMPUTE_PGM_RSRC2 bits 5:1 */
    uint32_t compute_pgm_rsrc1;         /* copied out of the kernel descriptor */
    uint32_t compute_pgm_rsrc2;         /* copied; the dispatch writes LDS_SIZE into it */
    uint32_t compute_pgm_rsrc3;         /* copied */
    uint16_t kernel_code_properties;    /* the ENABLE_SGPR_* bits */
    uint16_t reserved;
    uint32_t arg_count;                 /* every entry, explicit and hidden */
    uint32_t explicit_arg_count;        /* the entries that hipLaunchKernel supplies */
    uint32_t hidden_arg_count;
    const bc250hsa_arg* args;           /* arg_count entries, in metadata order */
} bc250hsa_kernel;

/* The offload bundle reader. It copies nothing: image points into fatbin.
 * target_id NULL takes "gfx1013", and an exact processor wins over a generic one.
 * Refuses a compressed bundle by name, with BC250HSA_ECOMPRESSEDBUNDLE. */
bc250hsa_status bc250hsa_unbundle(const void* fatbin, size_t fatbin_bytes,
                                  const char* target_id,
                                  const void** image, size_t* image_bytes);

/* Loads one ELF code object onto the device: checks the header, allocates one
 * executable range that covers every PT_LOAD segment, copies p_filesz and zeroes the
 * rest of p_memsz, applies the R_AMDGPU_RELATIVE64 relocations, reads the
 * NT_AMDGPU_METADATA note and resolves every kernel descriptor from .dynsym. It does
 * not keep the image pointer, and it does not unbundle. */
bc250hsa_status bc250hsa_module_load(bc250hsa_device* dev, const void* image,
                                     size_t image_bytes, bc250hsa_module** out);

/* The same loader against a caller's allocator, so that a host test runs it with no
 * device and no GPU. */
bc250hsa_status bc250hsa_module_load_alloc(const bc250hsa_allocator* alloc,
                                           const void* image, size_t image_bytes,
                                           bc250hsa_module** out);

void bc250hsa_module_unload(bc250hsa_module* mod);

uint32_t                bc250hsa_module_kernel_count(const bc250hsa_module* mod);
const bc250hsa_kernel*  bc250hsa_module_kernel_at(const bc250hsa_module* mod, uint32_t index);
const bc250hsa_kernel*  bc250hsa_module_kernel_by_name(const bc250hsa_module* mod,
                                                       const char* name);

/* A device global of the loaded object, for __hipRegisterVar. */
bc250hsa_status bc250hsa_module_symbol(const bc250hsa_module* mod, const char* name,
                                       uint64_t* va, uint64_t* bytes);

/* The loaded range of the object, for a fault report. */
bc250hsa_status bc250hsa_module_range(const bc250hsa_module* mod, uint64_t* va,
                                      uint64_t* bytes);

/* ---------------------------------------------------------------------------------
 * 7. Kernel arguments
 * ------------------------------------------------------------------------------- */

typedef struct bc250hsa_launch {
    uint32_t struct_bytes;
    uint32_t grid[3];               /* workgroups, not work items */
    uint32_t block[3];              /* work items per workgroup */
    uint32_t dynamic_group_bytes;   /* added to group_segment_bytes */
} bc250hsa_launch;

typedef struct bc250hsa_pack_result {
    uint32_t struct_bytes;
    uint32_t bytes_written;
    uint32_t explicit_args_written;
    uint32_t hidden_args_zeroed;
    uint32_t unknown_arg_kinds;
    uint32_t hostcall_buffer_requested; /* 1: the kernel asks for a host call service */
    uint32_t dispatch_packet_requested; /* 1: the kernel enables ENABLE_SGPR_DISPATCH_PTR,
                                         * and the packet was written at the offset below */
    uint32_t dispatch_packet_offset;    /* where in the buffer the 64-byte packet is */
    char     first_unknown_kind[32];    /* the metadata key, for the log */
} bc250hsa_pack_result;

/* ---------------------------------------------------------------------------------
 * 7.1 The AQL dispatch packet
 *
 * A kernel may enable ENABLE_SGPR_DISPATCH_PTR, and the compiler does so for every
 * kernel that reads its own workgroup size: `__builtin_amdgcn_workgroup_size_x`, which
 * is what HIP's blockDim becomes, is a 16-bit load from the AQL kernel dispatch packet
 * and not an implicit kernel argument. MEASURED on the ggml-hip backend of llama.cpp:
 * 1752 of its 7105 gfx1013 kernels enable the bit, among them every k_get_rows kernel,
 * which is the first launch of a decode (evidence/m16/step3b-2026-10-09, defect BD-110).
 *
 * A PM4 dispatch has no packet of its own, so this layer writes one. It belongs to the
 * launch, so it goes at the end of the kernel argument buffer the caller already
 * allocates per launch: bc250hsa_kernarg_requirements adds the 64 bytes and the
 * alignment, bc250hsa_kernarg_pack fills it from the same bc250hsa_launch the kernel
 * arguments come from, and the caller passes its address in bc250hsa_dispatch. One
 * allocation, one lifetime, and the packet retires with the dispatch that reads it.
 *
 * Every field the packet holds is one this layer knows: the workgroup size, the grid in
 * work items, the two segment sizes, the kernel object and the kernel argument address.
 * completion_signal is 0, because this layer completes through its own fence and not
 * through an HSA signal, and the two reserved fields are 0.
 * ------------------------------------------------------------------------------- */

#define BC250HSA_AQL_PACKET_BYTES 64u
#define BC250HSA_AQL_PACKET_ALIGN 64u

/* The size and the alignment that the caller must allocate for one launch. It covers
 * the kernel arguments and, for a kernel that enables ENABLE_SGPR_DISPATCH_PTR, the
 * AQL dispatch packet behind them. */
bc250hsa_status bc250hsa_kernarg_requirements(const bc250hsa_kernel* kernel,
                                              uint32_t* bytes, uint32_t* alignment);

/* Fills the kernel argument buffer from the metadata .args list and from nothing else.
 * args is the array that hipLaunchKernel receives: one pointer per explicit argument,
 * in declaration order. The packer walks the list and the array in step, writes every
 * hidden field it knows from launch, and zeroes every hidden field it does not know.
 * It never computes an offset of its own. kernarg may be a plain host buffer, which is
 * what the host test uses.
 *
 * kernarg_va is the GPU address that this buffer will have in bc250hsa_dispatch. It is
 * written into the AQL dispatch packet of section 7.1 and used for nothing else, so a
 * caller that packs into a plain host buffer passes 0 and the packet's kernarg_address
 * field reads 0. */
bc250hsa_status bc250hsa_kernarg_pack(const bc250hsa_kernel* kernel,
                                      const bc250hsa_launch* launch,
                                      void* const* args, uint32_t arg_count,
                                      void* kernarg, uint32_t kernarg_bytes,
                                      uint64_t kernarg_va,
                                      bc250hsa_pack_result* result);

/* ---------------------------------------------------------------------------------
 * 8. Dispatch
 * ------------------------------------------------------------------------------- */

#define BC250HSA_DISPATCH_GFX_RING   0x1u /* node 0 is the graphics ring: emit
                                           * CONTEXT_CONTROL first. The device path sets
                                           * it by itself; the golden test sets it by
                                           * hand */
#define BC250HSA_DISPATCH_NO_CU_MASK 0x2u /* drop the two SET_SH_REG_INDEX packets */
#define BC250HSA_DISPATCH_START_AT_000 0x4u /* add FORCE_START_AT_000 to the initiator */
#define BC250HSA_DISPATCH_NO_ACQUIRE 0x8u /* leave out ACQUIRE_MEM; diagnosis only */
#define BC250HSA_DISPATCH_NO_FENCE   0x10u/* leave out RELEASE_MEM; the caller fences */
#define BC250HSA_DISPATCH_LIGHT_BARRIER 0x20u /* between two dispatches of one batch, emit
                                               * the level-0 and level-1 invalidate only.
                                               * Section 8.1 */
#define BC250HSA_DISPATCH_FULL_STATE 0x40u /* write the whole compute state for every
                                            * dispatch of a batch, not only the registers
                                            * that changed since the one before it.
                                            * Section 8.8; the switch a lab arm compares
                                            * against */

typedef struct bc250hsa_dispatch {
    uint32_t struct_bytes;
    uint32_t flags;
    const bc250hsa_kernel* kernel;
    uint64_t kernarg_va;            /* a resident buffer of kernarg_bytes, already filled */
    /* The AQL dispatch packet of section 7.1, where bc250hsa_kernarg_pack wrote it:
     * kernarg_va + bc250hsa_pack_result.dispatch_packet_offset. It is needed only by a
     * kernel that enables ENABLE_SGPR_DISPATCH_PTR, and such a kernel with 0 here is
     * refused rather than started with a register that points nowhere. */
    uint64_t dispatch_packet_va;
    bc250hsa_launch launch;
} bc250hsa_dispatch;

/* Builds the indirect buffer into a free slot of this device's command ring, submits it
 * on the node-0 context and returns the fence value that this dispatch writes. It does
 * not wait for the dispatch. It may wait for a free ring slot, under the bound of
 * bc250hsa_wait's defaults, and returns BC250HSA_EBUSY if none frees.
 *
 * With batching on (section 8.1) the dispatch is appended to the indirect buffer this
 * device is building and the submission happens at a flush point. The returned fence
 * value is then the value of the whole batch: it is a value this device has promised
 * and not yet submitted, every wait for it submits the batch first, and a caller
 * cannot tell the difference apart from the timing.
 *
 * It refuses, and submits nothing: a zero grid or block, a block product above
 * max_flat_workgroup_size, local memory above the device limit, uses_dynamic_stack,
 * a kernarg_va that is not aligned to kernarg_align, a kernel that enables
 * ENABLE_SGPR_DISPATCH_PTR with no dispatch_packet_va, and a kernel whose enabled user
 * SGPRs this build does not program (BC250HSA_EUNSUPPORTED in the last three cases). */
bc250hsa_status bc250hsa_dispatch_submit(bc250hsa_device* dev,
                                         const bc250hsa_dispatch* dispatch,
                                         uint64_t* fence_value_out);

/* ---------------------------------------------------------------------------------
 * 8.1 Batching
 *
 * One dispatch per indirect buffer is one D3DKMTSubmitCommand, and therefore one
 * user-to-kernel transition and one kernel driver submission, per kernel. A decode step
 * of a language model issues hundreds of small kernels, so that cost is paid hundreds of
 * times per token. The kernel driver runs one indirect buffer at a time
 * (driver/kmd/wddm.c refuses a blob whose num_ibs is not 1), so the answer is not more
 * indirect buffers per submission: it is more dispatches per indirect buffer.
 *
 * With batching on, consecutive dispatches of this device append into one indirect
 * buffer, separated by the barrier of BC250HSA_DISPATCH_LIGHT_BARRIER or by the full
 * acquire, and one RELEASE_MEM at the end writes one fence value for the whole buffer.
 * Every dispatch of the batch therefore reports that one value, which keeps the rule a
 * caller needs: when the fence reaches the value a dispatch reported, that dispatch has
 * finished.
 *
 * The batch is submitted at the first of these:
 *   - bc250hsa_flush, bc250hsa_close, or a bc250hsa_wait for a value the device has not
 *     submitted yet;
 *   - bc250hsa_free, bc250hsa_map, bc250hsa_unmap, a copy through the host mapping, or a
 *     module load or unload, because each of them changes memory a dispatch of the open
 *     batch may name;
 *   - max_dispatches dispatches, or max_ib_dwords dwords, in the buffer;
 *   - a dispatch that arrives max_hold_us or more after the batch was opened.
 * The library runs no thread of its own, so the time cap is read when a call arrives.
 * Nothing a program can observe stays behind an open batch: every read of the device
 * goes through one of the flush points above.
 *
 * A submission that fails with an open batch loses work this library already promised.
 * It therefore marks the device lost, where the next call reports it, instead of
 * returning an error to a caller that did not make that dispatch.
 * ------------------------------------------------------------------------------- */

#define BC250HSA_BATCH_DISPATCHES_MAX     256u
#define BC250HSA_BATCH_DISPATCHES_DEFAULT  32u
#define BC250HSA_BATCH_HOLD_US_DEFAULT   1000u

typedef struct bc250hsa_batch_policy {
    uint32_t struct_bytes;
    uint32_t enabled;         /* 0: one submission per dispatch, as build 1 did */
    uint32_t max_dispatches;  /* 0 takes BC250HSA_BATCH_DISPATCHES_DEFAULT */
    uint32_t max_ib_dwords;   /* 0 takes what one command ring slot holds */
    uint32_t max_hold_us;     /* 0 takes BC250HSA_BATCH_HOLD_US_DEFAULT */
    uint32_t light_barrier;   /* 1: BC250HSA_DISPATCH_LIGHT_BARRIER between dispatches */
} bc250hsa_batch_policy;

/* Rule 6 of this header keeps policy out of the library: the caller decides. A change
 * submits an open batch first, so the policy of a dispatch is the policy its buffer was
 * opened with. */
bc250hsa_status bc250hsa_batch_policy_set(bc250hsa_device* dev,
                                          const bc250hsa_batch_policy* policy);
bc250hsa_status bc250hsa_batch_policy_get(bc250hsa_device* dev, bc250hsa_batch_policy* out);

/* Submits the open batch, if there is one. fence_value_out, when it is not NULL,
 * receives the value of that submission, or the last submitted value when nothing was
 * open. It is BC250HSA_OK with batching off and with an empty batch. */
bc250hsa_status bc250hsa_flush(bc250hsa_device* dev, uint64_t* fence_value_out);

/* --- the pure builder, for the golden test and for a failure report --------------- */

#define BC250HSA_PM4_MAX_DWORDS 128u

typedef struct bc250hsa_pm4_env {
    uint32_t struct_bytes;
    uint32_t flags;                     /* the same BC250HSA_DISPATCH_* bits */
    uint64_t fence_va;                  /* 8-byte aligned, the monitored fence */
    uint64_t fence_value;
    uint32_t private_segment_rsrc[4];   /* the 128-bit buffer resource for s[0:3] */
    uint32_t ib_pad_dwords;             /* 8 on the graphics ring, 0 takes 8 */
} bc250hsa_pm4_env;

/* Writes the dispatch of section 8 of the design as PM4 dwords into the caller's
 * buffer. It touches no device and no operating system, so a host test compares the
 * result with a golden stream. */
bc250hsa_status bc250hsa_pm4_build_dispatch(const bc250hsa_dispatch* dispatch,
                                            const bc250hsa_pm4_env* env,
                                            uint32_t* dwords, uint32_t dword_capacity,
                                            uint32_t* dwords_written);

/* The same stream for count dispatches in one indirect buffer: one head (the graphics
 * ring CONTEXT_CONTROL and the full acquire), count dispatch bodies with the barrier of
 * env->flags between them, and one completion write at the end. count 1 writes exactly
 * what bc250hsa_pm4_build_dispatch writes, dword for dword. A batch of n dispatches
 * needs at most n * BC250HSA_PM4_MAX_DWORDS dwords. */
bc250hsa_status bc250hsa_pm4_build_batch(const bc250hsa_dispatch* dispatches, uint32_t count,
                                         const bc250hsa_pm4_env* env,
                                         uint32_t* dwords, uint32_t dword_capacity,
                                         uint32_t* dwords_written);

/* COMPUTE_PGM_RSRC2.LDS_SIZE for this part: align(bytes, 512) / 512. The command
 * processor writes this field from the AQL packet, and a PM4 path must write it by
 * hand, because the kernel descriptor holds 0. */
uint32_t bc250hsa_lds_size_field(uint32_t group_segment_bytes, uint32_t dynamic_group_bytes);

/* The 128-bit buffer resource of a raw buffer on gfx10.1, for the private segment
 * buffer in s[0:3]. */
bc250hsa_status bc250hsa_buffer_resource(uint64_t va, uint64_t bytes, uint32_t out_dwords[4]);

#define BC250HSA_MAX_USER_SGPR 16u

typedef struct bc250hsa_user_sgpr_plan {
    uint32_t struct_bytes;
    uint32_t count;                             /* registers in use, at most 16 */
    uint32_t value[BC250HSA_MAX_USER_SGPR];     /* COMPUTE_USER_DATA_0 upward */
} bc250hsa_user_sgpr_plan;

/* Walks the ENABLE_SGPR_* bits of the kernel in the order of the AMDGPU documentation
 * and places each item at the next free COMPUTE_USER_DATA register. It refuses an
 * enabled item that this build does not program, with BC250HSA_EUNSUPPORTED, and it
 * refuses ENABLE_SGPR_DISPATCH_PTR with dispatch_packet_va 0 for the same reason: a
 * register that points nowhere is worse than a stated refusal. */
bc250hsa_status bc250hsa_plan_user_sgprs(const bc250hsa_kernel* kernel, uint64_t kernarg_va,
                                         uint64_t dispatch_packet_va,
                                         const uint32_t private_segment_rsrc[4],
                                         bc250hsa_user_sgpr_plan* out);

/* ---------------------------------------------------------------------------------
 * 9. Completion
 * ------------------------------------------------------------------------------- */

/* The last value that the GPU wrote, read straight from the fence's host mapping. It
 * never enters the kernel and it never fails. UINT64_MAX means a lost device. */
uint64_t bc250hsa_fence_read(bc250hsa_device* dev);

/* The value the last dispatch of this device writes. 0 before the first one. With
 * batching on it covers a dispatch that is still in the open batch, because that is what
 * a caller means by "everything this device owes"; bc250hsa_wait for it submits the
 * batch. */
uint64_t bc250hsa_fence_last_submitted(bc250hsa_device* dev);

/* Waits until the fence reaches value. A value this device has promised and not yet
 * submitted submits the open batch first (section 8.1). It reads the host mapping
 * first, and most waits end there. It then waits in slices of slice_ms, up to total_ms in all, and between
 * two slices it re-reads the fence and asks the operating system whether the device
 * still runs. slice_ms 0 takes 1000 and total_ms 0 takes 120000, which are the
 * measured defaults of our Vulkan driver: one long wait turns a healthy wait behind
 * another process's reset into a lost device. A test or a short trial passes a smaller
 * total by hand.
 *
 * Returns BC250HSA_OK, BC250HSA_ETIMEOUT (the device still lives) or
 * BC250HSA_EDEVICELOST. */
bc250hsa_status bc250hsa_wait(bc250hsa_device* dev, uint64_t value,
                              uint32_t slice_ms, uint32_t total_ms);

/* ---------------------------------------------------------------------------------
 * 10. Diagnosis
 * ------------------------------------------------------------------------------- */

typedef struct bc250hsa_fault {
    uint32_t struct_bytes;
    uint32_t device_lost;       /* 1: this device no longer runs work */
    uint64_t faulted_va;
    uint32_t general_error;
    uint32_t device_error;
    uint32_t fault_flags;
    uint32_t pipeline_stage;
    uint64_t fence_value;       /* the value at the time of the query */
    uint64_t fence_expected;    /* the last submitted value */
} bc250hsa_fault;

/* Asks the operating system for the execution state, and for the page fault block when
 * the state reports a fault. It changes nothing and it is safe at any time. */
bc250hsa_status bc250hsa_query_fault(bc250hsa_device* dev, bc250hsa_fault* out);

/* The dwords of the last indirect buffer that this device submitted, for a failure
 * report. The pointer stays valid until the next submission. */
bc250hsa_status bc250hsa_last_ib(bc250hsa_device* dev, const uint32_t** dwords,
                                 uint32_t* count);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* BC250HSA_H */
```

---

## 4. Layer 2: amdhip64.dll

### 4.1 The entry points of step 2

DECIDED. 38 exported names. MEASURED against a real program: a plain HIP vector addition compiles and
links against exactly this set with no ROCm and no AMD driver on the machine.

Registration and kernel start, which clang's code generation emits and which no application calls directly:

| Name | Signature, from clang's HIP code generation |
|---|---|
| `__hipRegisterFatBinary` | `void** (void*)`, and it receives the wrapper, not the fat binary |
| `__hipRegisterFunction` | `int (void**, const void*, char*, const char*, int, void*, void*, void*, void*, int*)`. MEASURED: clang passes the host stub, the device name twice, -1 and six null pointers |
| `__hipRegisterVar` | `void (void**, void*, char*, const char*, int, size_t, int, int)`. The size field is `size_t` for HIP, not `int` |
| `__hipRegisterManagedVar` | `void (void**, void*, void*, const char*, size_t, unsigned)`. A stub that reports a missing capability in step 2 |
| `__hipUnregisterFatBinary` | `void (void**)` |
| `__hipPushCallConfiguration` | `(dim3, dim3, size_t, hipStream_t)`, called by the `<<<>>>` lowering |
| `__hipPopCallConfiguration` | `int (dim3*, dim3*, size_t*, void**)`, called by the stub |
| `hipLaunchKernel` | `hipError_t (const void*, dim3, dim3, void**, size_t, hipStream_t)` |

Device and context, 6: `hipInit`, `hipGetDeviceCount`, `hipSetDevice`, `hipGetDevice`,
`hipGetDeviceProperties`, `hipDeviceSynchronize`.

Memory, 9: `hipMalloc`, `hipFree`, `hipHostMalloc`, `hipHostFree`, `hipMemcpy`, `hipMemcpyAsync`,
`hipMemset`, `hipMemsetAsync`, `hipMemGetInfo`.

Streams, 5: `hipStreamCreate`, `hipStreamCreateWithFlags`, `hipStreamDestroy`,
`hipStreamSynchronize`, `hipStreamWaitEvent`.

Events, 6: `hipEventCreate`, `hipEventCreateWithFlags`, `hipEventDestroy`, `hipEventRecord`,
`hipEventSynchronize`, `hipEventElapsedTime`.

Errors, 4: `hipGetLastError`, `hipPeekAtLastError`, `hipGetErrorString`, `hipGetErrorName`.

`hipGetDeviceProperties` reports `sharedMemPerBlock` 65536 and `warpSize` 32. The 65536 comes from the
acceptance path: llama.cpp's integer matrix multiply kernels refuse themselves below 48 KiB and every
prompt matrix multiply then falls back to hipBLAS. 32 is MEASURED: every kernel of the spike reports
wave size 32. The error state is per thread. `hipGetLastError` clears it and `hipPeekAtLastError` does
not.

Section 4.9 holds the measured step-3 set, which is 13 names and not the 14 of the estimate below.
The paragraph that follows is the estimate of the spike, and it is kept because the difference is
the useful part.

Step 3 adds 14 names, which the spike measured from the application: `hipDeviceGetAttribute`,
`hipDeviceGetPCIBusId`, `hipFuncSetAttribute`, `hipOccupancyMaxActiveBlocksPerMultiprocessor` (on the
FlashAttention dispatch path, so not optional), `hipOccupancyMaxPotentialBlockSize`,
`hipHostGetDevicePointer`, `hipHostRegister`, `hipHostUnregister`, `hipMallocManaged`, `hipMemAdvise`,
`hipMemcpy2DAsync`, `hipMemcpyPeerAsync` with `hipDeviceCanAccessPeer`, `hipDeviceEnablePeerAccess` and
`hipDeviceDisablePeerAccess` as stubs that report no peer, `hipLaunchHostFunc`, and
`hipLaunchCooperativeKernel` as a stub that reports a missing capability. Three build options remove
the hardest parts: `GGML_HIP_GRAPHS=OFF` removes 11 graph names, `GGML_HIP_NO_VMM=ON` removes 10
virtual memory names, and `GGML_CUDA_FORCE_MMQ=ON` keeps quantized matrix multiplies out of hipBLAS.
So step 3 is 52 exported functions, 5 of them honest stubs.

### 4.2 The registration ABI

MEASURED. The 24 bytes of `.hipFatBinSegment` are these four fields, with magic 0x48495046
("HIPF"), version 1 and a null fourth field:

```c
struct { int magic; int version; void* gpu_binary; void* unused; };
```

`__hipRegisterFatBinary` receives the address of this wrapper and **not** the fat binary. The runtime
checks the magic, refuses the CUDA magic 0x466243b1 with a clear message, and follows the third field
to the bundle.

The host object also defines, per translation unit: `__hip_fatbin_wrapper` in `.hipFatBinSegment`,
`__hip_fatbin` in `.hip_fatbin` aligned to 4096, `__hip_gpubin_handle_<cuid>` as one null pointer,
`__hip_cuid_<hash>` as one byte in `.bss`, a module constructor in `.CRT$XCU` and a module destructor.
Clang's own comment states that "the name, size, and initialization pattern" of the handle variable is
part of the HIP ABI, and the constructor calls `__hipRegisterFatBinary` only when the handle is null, so
one process with several translation units registers one time.

DECIDED. `__hipRegisterFatBinary` returns an opaque module handle that layer 2 owns. It calls
`bc250hsa_unbundle()` at once, because a wrong target must be reported early, and it calls
`bc250hsa_module_load()` lazily at the first kernel start, so a process that never starts one pays
nothing. `__hipRegisterFunction` records the mapping from the host stub address to the device kernel
name, because `hipLaunchKernel` receives the host stub address as its first argument and must find the
kernel from it.

`atexit` appears in the undefined symbols because clang compiles the host side with
`-fno-use-cxa-atexit` on this target, so the module destructor is registered with the C runtime.

### 4.3 The import library

MEASURED. Clang's HIP driver appends a bare `amdhip64.lib` to the Windows link line by itself. So our
import library must be named exactly `amdhip64.lib` and must sit on the library search path. A program
needs `-L<directory>` and no other linker flag. Passing the library as a positional argument fails
twice: `-x hip` applies to every input, so clang parses the archive as HIP source, and the driver still
adds its own copy of the name.

The library is built with no MSVC tool:

```
llvm-dlltool -m i386:x86-64 -d amdhip64.def -l amdhip64.lib -D amdhip64.dll
```

MEASURED: 9100 bytes from the 38 names. Six names arrive **without** the `__imp_` prefix: the five
registration names and `hipLaunchKernel`, because clang creates them with its runtime-function helper,
which makes a fresh declaration and ignores `__declspec(dllimport)` in the header.
`__hipPushCallConfiguration` is the one registration-side name that is decorated, because the `<<<>>>`
lowering calls the declared function. Consequence: the import library must carry the short jump thunks
as well as the `__imp_` data symbols. `llvm-dlltool` writes both. A hand-made library of `__imp_`
symbols alone does not link.

### 4.4 The minimal `hip_runtime.h`

DECIDED and MEASURED. The file ships as `compute/hip/include/hip/hip_runtime.h` and contains, in this
order:

1. The attribute macros `__device__`, `__host__`, `__global__`, `__shared__`, `__constant__`,
   `__managed__`, `__forceinline__` and `__launch_bounds__`. Clang defines none of them.
2. `dim3`, with a constructor that defaults every field to 1 and accepts one argument, because
   `<<<n, m>>>` passes an `int`.
3. `hipError_t`, `hipMemcpyKind`, `hipStream_t`, `hipEvent_t`, `hipDeviceProp_t` and the flag
   constants.
4. The declaration of `hipLaunchKernel` at global scope. Clang looks that name up in the translation
   unit and reports "Can't find declaration for hipLaunchKernel" when it is absent. MEASURED, that
   exact error.
5. The declarations of `__hipPushCallConfiguration` and `__hipPopCallConfiguration`.
6. The 38 host entry points.
7. `threadIdx`, `blockIdx`, `blockDim` and `gridDim`. MEASURED: each must exist in **both** compiler
   passes, because clang parses the body of a `__global__` function in the host pass as well and
   reports "use of undeclared identifier 'blockIdx'" there. Each coordinate is an empty structure with
   a `__device__` conversion operator that calls the matching AMDGPU builtin.
8. `__syncthreads`, as a release fence, `__builtin_amdgcn_s_barrier` and an acquire fence.
9. MEASURED trap: `__declspec(dllimport)` must disappear in the device pass, which targets
   `amdgcn-amd-amdhsa` and warns on every declaration. The guard is
   `#if defined(_WIN32) && !defined(__HIP_DEVICE_COMPILE__)`.

Step 3 adds the device-side math set, the atomic set, the warp shuffle set, `__half` and the vector
types. llama.cpp uses all of them. They resolve through the ROCm device library, so they are header
work and not runtime work.

### 4.5 How HIP maps onto bc250hsa

| HIP | Layer 2 does this |
|---|---|
| `hipInit`, `hipGetDeviceCount` | `bc250hsa_open()` once per process, lazily. One device |
| `hipGetDeviceProperties` | `bc250hsa_props_read()`, plus the fixed fields of section 4.1 |
| `hipMalloc` | `bc250hsa_alloc()` with `BC250HSA_MEM_MAPPABLE` |
| `hipHostMalloc` | `bc250hsa_alloc()` with `BC250HSA_MEM_HOST` |
| `hipMemcpy`, `hipMemcpyAsync` | `bc250hsa_copy_to_device()` or `_from_device()`, after a wait on the stream's last fence value. Asynchronous copies are synchronous in build 1, which is legal and slow |
| `hipMemset` | a host fill through the mapping in build 1, a fill kernel later |
| `__hipRegisterFatBinary` | `bc250hsa_unbundle()`, then `bc250hsa_module_load()` at the first kernel start |
| `__hipRegisterFunction` | the stub-to-name map |
| `hipLaunchKernel` | `bc250hsa_kernarg_requirements()`, a kernarg buffer from a per-stream pool, `bc250hsa_kernarg_pack()`, then `bc250hsa_dispatch_submit()`. The returned fence value becomes the stream's last value |
| `hipStreamCreate` | a software stream: an ordered list of submissions and one last fence value. One hardware queue stays underneath |
| `hipStreamSynchronize`, `hipDeviceSynchronize` | `bc250hsa_wait()` on that value. The legacy null stream waits for the whole device, and a stream waits for the event it still carries |
| `hipStreamWaitEvent` | a host wait on the recorded value before the next submission of this stream, because there is one hardware queue and submissions are already ordered |
| `hipEventRecord` | records what the stream owes and, when that value already retired, a host timestamp |
| `hipEventElapsedTime` | the difference of two host timestamps. Each one is taken at the moment its fence value retired, which is the first wait or fence read that passes it. A GPU timestamp is later work, named in section 7 |
| `hipGetLastError` | the per-thread error state, written by the status translation below |

Two rules of this table that a reader can read wrongly. First, the value a stream owes is not always
its own last submission: the legacy null stream owes the last submission of the device, because HIP
says that it synchronises with every other stream, and a stream that carries an event wait and
nothing else owes that event. Second, an event's host timestamp belongs to the moment its value
retired. A timestamp taken when a program asks about the event would make every measurement of two
retired events microseconds apart, whatever the kernels did.

Layer 2 owns the wait policy, because `bc250hsa.h` rule 6 keeps policy out of layer 1. One bound
holds for the process, and every wait of layer 2 passes it. It is read one time, at the first call,
from the environment variables `BC250_HIP_WAIT_TOTAL_MS` and `BC250_HIP_WAIT_SLICE_MS`. Absent or 0
takes the library defaults of 1000 and 120000 milliseconds. A lab trial must stay inside three
minutes, so the step-2 program takes `--wait-total <ms>` and sets the variable before its first HIP
call.

The status translation, one table and no judgement: `BC250HSA_OK` to `hipSuccess`, `EINVAL` to
`hipErrorInvalidValue`, `ENOMEM` to `hipErrorOutOfMemory`, `ENODEV` to `hipErrorNoDevice`, `ETIMEOUT`
to `hipErrorLaunchTimeOut`, `EDEVICELOST` to `hipErrorIllegalAddress` after a fault query, and to
`hipErrorContextIsDestroyed` without one, `EUNSUPPORTED` to `hipErrorNotSupported`, `ENOTFOUND` to
`hipErrorInvalidDeviceFunction`, `EBUSY` to `hipErrorNotReady`, and every code at -20 and below to
`hipErrorInvalidImage`.

### 4.6 The device library and the build command

MEASURED by the spike, which answers open question 3 of the findings document. The ROCm device library
builds for gfx1013 with our own clang 22 from the `release/rocm-rel-7.2` branch of `ROCm/llvm-project`:
ninja exits 0 and writes 71 bitcode files, among them `ocml.bc` (214604 bytes), `ockl.bc` (243500
bytes) and `oclc_isa_version_1013.bc`. The `__ocml_sin_f32` program that failed before now links two
ways, through `--hip-device-lib-path` and through explicit `-mlink-builtin-bitcode`.

Three walls, each already passed and each recorded here so that nobody meets it twice.

1. The `amd-staging` branch does **not** build with upstream clang. `ockl` fails on `workitem.cl`
   (`amdhsa_abi.h` is absent) and on `alrs.cl` and `dm.cl` (`__builtin_amdgcn_wave_shuffle` is absent).
   Both come from AMD's own clang. Build the release branch and re-check `amd-staging` when upstream
   clang catches up.
2. For the plain triple with no processor, clang 22 reports that `__builtin_amdgcn_lerp` needs the
   `lerp-inst` target feature and that `__builtin_amdgcn_cubema` needs `cube-insts`. Adding
   `-DCLANG_OPTIONS_APPEND=-mcpu=gfx1013` passes both. The cost is one bitcode set per processor, which
   is acceptable for one target.
3. `prepare-builtins`, a small tool of that build, does not compile in this workspace, because our
   clang is a MinGW build with no C++ standard library headers here. The tool only changes the linkage
   of every external definition to `linkonce_odr`, and a script does the same through the bitcode
   disassembler and assembler. The linkage matters only for a `-fgpu-rdc` build, which we do not use.
   OPEN and small: write the real tool once a MinGW C++ toolchain or an MSVC build of LLVM exists.

One licence line, as the owner's rule says: the ROCm device library and the ROCm loader carry the
University of Illinois NCSA Open Source License, which is permissive. We build the bitcode from source
and record the revision.

The complete command that builds a HIP program against our runtime, MEASURED:

```
clang -x hip --offload-arch=gfx1013 --target=x86_64-pc-windows-msvc
      -nogpuinc -I<the directory that holds hip/hip_runtime.h>
      --hip-device-lib-path=<the directory that holds ocml.bc and the oclc files>
      -O2 -std=c++17 -fms-runtime-lib=dll
      -o app.exe app.hip -L<the directory that holds amdhip64.lib>
```

MEASURED condition: clang compiles the **device** pass with the host include search path as well, so
the MSVC and Windows SDK include and library paths must be in the environment or the device pass fails
on `<stdio.h>`. A developer command prompt, or the same environment import that our build script
performs, is therefore a hard requirement of a Windows HIP build. That is a normal Windows condition
and not a gap of our runtime. `-fhip-new-launch-api` is the clang 22 default, so layer 2 implements
`hipLaunchKernel` only. The legacy `hipSetupArgument` and `hipLaunchByPtr` path stays unwritten.

### 4.7 The process lock, and the waits

Layer 2 keeps one lock over the process state: the device, the modules, the host stubs, the
allocation table, the kernel argument pool and the streams. One lock is the right size here,
because layer 1 has one hardware queue and serialises its submissions anyway (`bc250hsa.h`,
rule 5). A finer lock would buy no parallel submission.

Above that lock there is one rule. **No thread holds the lock while it waits for the device.** The
first build of layer 2 did. The result is what a lock over a wait always is: a program whose
second thread cannot call the runtime at all while the first one waits. A wait here is a whole
dispatch long. Design section 3.8 states the rule for layer 1 already ("never spins on a fence
while it holds a lock that the completion path needs"). This section states it for layer 2.

A wait therefore has three parts:

1. read the fence value to wait for, with the lock held,
2. wait in `bc250hsa_wait`, with the lock open,
3. take the lock again and update the state.

`bc250hip::Guard` is the lock and the only way to wait, so no entry point can wait by itself and
forget the rule. Layer 1's wait may be entered from several threads at once: it creates its own
operating system event per call and its counters are interlocked.

Step 3 is the reason the three parts matter. The lock must not be held across a wait, but the
state must still be consistent when the wait ends, because another thread owned it in the
meantime. Two answers, and no third:

| What a call still needs after its wait | The answer |
|---|---|
| a stream, an event | a reference count on the object. The handle holds one. A call that waits holds one more (`StreamRef`, `EventRef`). `hipStreamDestroy` and `hipEventDestroy` take the handle's reference away and clear its magic word. The handle dies at once, and the object dies when the last waiter lets go of it |
| the kernel of a host stub, an allocation of the table, a buffer of the kernel argument pool | a second lookup. The kernel is resolved again, because another thread may have unregistered the fat binary. The allocation table is read again, because another thread may have freed the same pointer. A pool buffer that a thread waits for carries a claim, so no other thread takes it |

Two more consequences, both of them HIP's own semantics and not a compromise:

- `hipDeviceSynchronize`, `hipMemcpy` and `hipFree` wait for the device's last submitted value as
  it stood when the call started. Work that another thread submits during the wait belongs to the
  next call.
- A synchronize clears the event wait that it waited for and not a later one that another thread
  asked for in the meantime.

One wait is left inside the lock, and layer 2 cannot reach it. A submission whose command ring
is full waits for the oldest slot inside `bc250hsa_dispatch_submit`, under the same bound and
under layer 1's own device lock (`bc250hsa.h`, rule 5). Layer 2 holds the process lock across a
submission, because a submission is a system call and not a wait, and the contract gives it no
ring slot to read. It happens only with eight dispatches of one process in flight, and it
blocks the other threads' submissions in layer 1 anyway. The fix belongs to layer 1: wait for a
free slot without holding the device lock. It is open work of step 3 and is not needed before a
multithreaded application queues eight dispatches.

The owner's instruction of 2026-09-29 is that multithreading is measured with our own clients
before an application is asked to exercise it. `compute/hip/tests/host/hip_threads_client.h` is
that client, with a negative control that restores the lock over the wait. Section 5.4 lists what
each of the two builds must measure.

#### 4.7a What threads can and cannot buy on this device

One hardware queue is not a detail of the lock. It is the shape of the device, and it decides what
a second thread can win. Our node-0 context submits one indirect buffer at a time, the kernel
driver runs one at a time (`driver/kmd/wddm.c`, "One IB is already the ring's whole capacity"),
and two dispatches inside one batched buffer have a barrier between them (section 8.1). So a
compute-bound dispatch has the whole device to itself, and *n* of them cost *n* times their own
device time whatever the thread count. What threads buy is the host side: the packing, the
submission and the waiting of one thread overlap the device time of another's work, which is why
the lock must stay open across a wait.

MEASURED, unit A, 2026-10-09 (`hipthreads` phase B, three policies): four threads with four waits
each took 578.9, 583.8 and 585.5 ms where a serialised runtime takes 591.0 to 592.1 ms. That is
the device time of sixteen 37 ms dispatches, with about 1 % of host cost on top of it instead of a
penalty, and it is the right answer for one queue. The mock backend measures 812 ms against
3239 ms, because it retires every dispatch on a timer of its own and therefore has as many in
flight as the threads give it. The two numbers do not compare, and phase B's "four times" verdict
belonged to the mock alone: it asked the hardware for parallelism that one queue cannot give
(defect BD-111). The client now takes `device_overlaps_dispatches` from its caller: the host test
and the mock-DLL run of the sample set it and keep the four-times check, which is a real test of
the lock there, and the lab run leaves it off and checks instead that the threads cost nothing on
top of the serialised device time. Phase A is what proves the lock property on the hardware, and
it passed in all three runs: 8, 29 and 48 operations of the other threads completed inside one
thread's wait.

What would buy real concurrency on this part is a second queue, not a better lock: the
asynchronous compute pipes of the MEC, which this kernel driver does not expose to user mode
today. That is open work with its own cost measurement, and nothing in layer 2 has to change for
it. Until then the honest statement is the one above, and a performance claim about thread count
on a compute-bound workload is a claim about the host side only.

### 4.8 What llama.cpp's ggml-hip backend asks of the host

MEASURED 2026-10-09, offline, with two tools in `scratch\m16-hip\step3\tools`. `inventory.py`
reads every `.cu` and `.cuh` file of `ggml/src/ggml-cuda`, keeps each call of a name of the CUDA
or HIP host interface, resolves the CUDA name to the HIP name through the backend's own
`ggml-cuda/vendors/hip.h`, and compares the result with `amdhip64.def`. `device_scan.py` does the
same for the device side. The checkout is llama.cpp `b86d2f07`, which is the revision that the
Vulkan measurements of fact M816 and the Strata work already use.

The host interface of the backend is 95 distinct calls. They divide like this.

| Group | Count | What we do |
|---|---|---|
| Already exported by `amdhip64.dll` after step 2 | 25 | nothing |
| Removed by a build option | 19 | `GGML_HIP_GRAPHS=OFF` removes 8 graph and capture names, `GGML_HIP_NO_VMM=ON` removes 10 virtual memory names, `GGML_CUDA_FA=OFF` removes `hipOccupancyMaxActiveBlocksPerMultiprocessor` |
| Compiled out by the backend itself | 1 | `hipLaunchHostFunc`, which sits inside `#if 0` |
| Added in step 3 (section 4.9) | 13 | implemented |
| hipBLAS and rocBLAS | 11 | open work, section 4.10 |
| The CUDA-only paths and the macro names of the backend | 26 | nothing: they never reach a HIP build |

Two names of the spike's estimate are therefore not needed, and two more are needed only with
FlashAttention on. `hipDeviceDisablePeerAccess` is named in `vendors/hip.h` and called nowhere,
and `hipOccupancyMaxPotentialBlockSize` is the same.

### 4.9 The entry points of step 3

DECIDED. 13 names, which take the HIP part of `amdhip64.def` from 38 to 51. Each one is in the link line of
ggml-hip, so each one must exist, and five of them say no.

| Name | What it does here |
|---|---|
| `hipDeviceGetAttribute` | 13 attributes, each one the same value as the field of `hipGetDeviceProperties` that carries it. An attribute this build does not know is `hipErrorInvalidValue` and never a guessed zero |
| `hipDeviceGetPCIBusId` | the bus identifier of the adapter, from the three numbers layer 1 reads. A short buffer truncates and reports success, as CUDA and HIP both do |
| `hipDeviceCanAccessPeer` | one device, so the answer is 0 and a second device number is `hipErrorInvalidDevice` |
| `hipDeviceEnablePeerAccess` | `hipErrorInvalidDevice`: a device cannot peer with itself, and there is no other |
| `hipFuncSetAttribute` | records a dynamic group memory ceiling for one kernel, and refuses a value above the hardware limit at the call that asks for it. The ceiling is advice on this part: there is no 48 KiB default to lift, and layer 1 checks each dispatch |
| `hipLaunchCooperativeKernel` | `hipErrorNotSupported`. One hardware queue, so nothing can start every workgroup of a grid at the same time |
| `hipHostGetDevicePointer` | the GPU address of memory `hipHostMalloc` returned, interior pointers included |
| `hipHostRegister` | `hipErrorNotSupported`. Layer 1 has no entry point that takes a range of a program's own memory |
| `hipHostUnregister` | `hipErrorHostMemoryNotRegistered` |
| `hipMallocManaged` | `hipErrorNotSupported`, which is the code the backend falls back on |
| `hipMemAdvise` | `hipErrorNotSupported`, and the backend ignores the result |
| `hipMemcpy2DAsync` | a real row-by-row copy under one wait. The bytes between the rows stay as they were |
| `hipMemcpyPeerAsync` | within device 0 it is a device-to-device copy. Any other device number is `hipErrorInvalidDevice` |

Each refusal was chosen against the code that receives it, not against the specification alone:

- `hipMallocManaged` must answer exactly `hipErrorNotSupported`. With that code
  `ggml_cuda_device_malloc` falls back to `hipMalloc` and writes one warning line. Any other
  error code becomes an abort of the program.
- `hipHostRegister` is called only with `GGML_CUDA_REGISTER_HOST` in the environment.
  `ggml_backend_cuda_register_host_buffer` clears the error and runs without registered host
  memory, which costs one copy through a staging buffer per upload.
- `hipLaunchCooperativeKernel` is guarded at run time by the attribute
  `hipDeviceAttributeCooperativeLaunch`, which this runtime answers 0. The entry point exists
  because the backend links against it.

Two more things belong to the same step. `hipStreamPerThread` and `hipStreamLegacy` are handle
values that no stream object can have, and the backend passes `hipStreamPerThread` to
`hipMemcpyPeerAsync`. Both now resolve to the one default stream of the process, which is
stricter than HIP asks and is legal. And `hipDeviceProp_t` already carries every field the
backend reads, `gcnArchName` first of all: the backend parses `gfx1013` out of it to pick its
kernels.

### 4.10 What stops the backend from building today

Two walls, both MEASURED 2026-10-09 with `scratch\m16-hip\step3\probe\try-ggml-hip.py`.

**Wall 1, the package files.** The CMake configure stops at `ggml/src/ggml-hip/CMakeLists.txt`
line 46, `find_package(hip REQUIRED)`: "Could not find a package configuration file provided by
hip". Lines 47 and 48 ask for `hipblas` and `rocblas` in the same way. So our tree needs
`hip-config.cmake`, `hipblas-config.cmake` and `rocblas-config.cmake` of its own, with the
targets `hip::host`, `hip::device`, `roc::hipblas` and `roc::rocblas`, and a `hip_VERSION` of at
least 6.1, which line 55 enforces.

**Wall 2, the device headers.** One translation unit compiled by hand, with no CMake, stops at
`ggml/src/ggml-common.h` line 51: `'hip/hip_fp16.h' file not found`. Our `include/hip` holds one
header, `hip_runtime.h`. The backend needs `hip/hip_fp16.h`, `hip/hip_bf16.h` and
`hipblas/hipblas.h` as well.

The size of wall 2 is measured and not estimated. `device_scan.py` finds 134 device-side names in
the backend's kernels, and 30 of them are the C library of mathematics. Those 30 are no longer
work: see section 4.6 and the probe below. The rest divides like this.

| Group | Names | Note |
|---|---|---|
| half and bfloat16 types, operators and conversions | 26 | `_Float16` and `__bf16` are native types of this compiler, so these are our own small header and not new hardware |
| vector types and their makers (`float2`, `int4`, `make_float4`) | 16 | our own header. `dim3` is the one of them we already have |
| cross-lane functions (`__shfl`, `__shfl_xor`, `__all`, `__any`, `__ballot`, `__popc`, `__byte_perm`) | 9 | MEASURED below: every one of them compiles for gfx1013 |
| atomics (`atomicAdd` and its system form) | 2 | MEASURED below |
| cached and non-temporal loads, fast-math names, the CUDA 4-bit float types | 13 | the three `__nv_fp4` names belong to a CUDA-only path |
| matrix core and wave matrix instructions | 22 | not for this part, and the backend already guards them by architecture |
| dot product (`__dp4a`) | 3 | MEASURED below: gfx1013 has no dot instruction, so the backend's integer fallback is the only path |
| scheduling and barrier builtins | 13 | clang supplies them, as the probe proves. `__syncthreads` is already in our header |

So the remaining work of step 3 is a HIP device header set of about 100 names and a hipBLAS shim
of 11 entry points. It is not a hardware question and it is not a compiler question.

### 4.11 What the device probes measured

Four probes, `scratch\m16-hip\step3\probe\run-probe.py`, all offline, all for gfx1013, all
against the device library of section 4.6 and clang's own HIP math headers. Nothing of AMD's
product stack is installed on the machine that ran them.

| Probe | Result |
|---|---|
| 48 names of the mathematics library, and the half-precision set | a 14256-byte code object, 0 undefined symbols |
| the same source with `-nogpulib` | the link fails on `__ocml_expm1_f32`, `__ocml_log1p_f32` and `__ocml_rsqrt_f32`. This is the negative control: it proves the device library carries the work and that clang does not expand it in place |
| cross-lane, atomic, permute and scheduling builtins | a 6208-byte code object, 0 undefined symbols |
| `__builtin_amdgcn_sdot4` | "needs target feature dot1-insts". gfx1013 has no dot-product instruction |

One result of the third probe is a number for the roadmap. The generated instructions of that
code object are `ds_bpermute_b32` for a cross-lane move, `v_perm_b32` for a byte permute, one
native `global_atomic_add` for the integer addition, and `global_atomic_cmpswap` for the float
addition. So every floating-point `atomicAdd` on this part is a compare-and-swap loop. The
backend uses that operation in its split-k matrix multiply paths, so the loop is on the hot path
of a prompt and not in a corner.

Two names of the CUDA interface are missing from clang's HIP math headers and belong to ours:
`__float_as_int` and `__float_as_uint`, with their two inverses. One builtin each.

How the probes reach those math headers is worth one paragraph, because it decides the shape of
our include tree. clang holds the HIP mathematics itself, in
`lib/clang/22/include/__clang_hip_math.h`, and `__clang_hip_runtime_wrapper.h` pulls it in. clang
force-includes that wrapper only when it recognises a ROCm installation, so the probes name the
wrapper on the command line with `-include`. The wrapper then includes `"hip/hip_version.h"`,
which a ROCm installation supplies. Ours supplies it as well: the probe tree holds a
`hip/hip_version.h` of 24 lines that states the interface version our runtime implements. With
that one file, our include directory and the bitcode directory, `--rocm-path` makes clang behave
as it does against a product installation, and the device library links by itself. The file
belongs in `compute/hip/include/hip` as soon as the device headers of section 4.10 are written.

### 4.12 Both walls of 4.10 are down, and the one wall that is left

MEASURED 2026-10-09, offline, no lab unit.

**Wall 1 is down.** `compute/hip/cmake` holds `hip-config.cmake`, `hip-config-version.cmake`,
`hipblas-config.cmake` and `rocblas-config.cmake`, and `compute/hip/tools/make-rocm-root.py`
assembles them with our include directory, our import library and the device library bitcode into
one ROCm-shaped root. With `CMAKE_PREFIX_PATH` and `ROCM_PATH` on that root, llama.cpp's configure
prints "HIP and hipBLAS found" and goes on. `hip_VERSION` is 6.2.0, which is what
`include/hip/hip_version.h` states: above the 6.1 floor of `ggml/src/ggml-hip/CMakeLists.txt` line
55, and below the 6.3 at which the backend starts to include `hip/hip_fp8.h`.

One option of `hip::device` decides whether the whole backend compiles, and it is worth stating
why. clang force-includes `__clang_hip_runtime_wrapper.h` only when it recognises the
`--rocm-path` root as a product installation, and ours is not one: no `bin/hipconfig`, no
`.hipVersion`. Without that wrapper every call of the mathematics library fails to resolve, which
is 1431 of the 1993 errors of the first whole-tree build. `hip::device` therefore carries
`-include __clang_hip_runtime_wrapper.h`, and the failing translation units fell from 144 to 44 on
that one option alone.

**Wall 2 is down.** `compute/hip/include/hip` now holds `hip_version.h`, `hip_vector_types.h`,
`hip_fp16.h`, `hip_bf16.h` and `hip_cooperative_groups.h` beside `hip_runtime.h`, and
`compute/hip/include/hipblas/hipblas.h` holds the BLAS interface. Every include of the backend
resolves, and all 144 translation units of `ggml-hip` compile for gfx1013 (60 MB of objects). Five
traps the headers had to answer, each one found by a failed build and written into the header that
answers it:

1. MSVC compiles the host half of the runtime, so the vector types need `__declspec(align(n))` and
   not `__attribute__((aligned(n)))`, with the alignment of each type written out.
2. `warpSize` is a `static constexpr int`, not a macro. As a macro it rewrote `prop.warpSize` in
   our own sample.
3. `__half` offers one implicit conversion, to `float`. With `operator _Float16()` as well,
   `int32_t(x)` in `convert.cuh` is ambiguous.
4. The host side of the float-to-half conversion is integer arithmetic, because this toolchain
   ships no compiler-rt builtins for x86-64 Windows and `_Float16` arithmetic on the host asks for
   `__truncsfhf2`. The device side still uses `v_cvt_f16_f32`.
5. `__hgt2_mask` is absent on purpose. ggml defines it itself for HIP (`common.cuh:704`), so a
   second definition is an error.

Two host entry points joined the 13 of section 4.9 for reasons the backend does not state in its
own guards. `hipOccupancyMaxActiveBlocksPerMultiprocessor` is called from
`fattn-common.cuh:1137`, which compiles even with FlashAttention off. `hipStreamBeginCapture` is
called at `ggml-cuda.cu:4598`, outside the `#ifdef USE_CUDA_GRAPH` block that ends at line 4589:
an upstream portability defect, answered here with `hipErrorNotSupported`. HIP is therefore 53
names, and the module definition file is 55: the two counter calls of section 8.7
(`bc250hipGetCounters` and `bc250hipResetCounters`) are ours and not HIP, and they are exported
from the same DLL. `build-runtime.ps1` gates that number.

**The wall that is left: hipBLAS has no implementation.** The link of `ggml-hip.dll` asks for ten
names: `hipblasCreate`, `hipblasDestroy`, `hipblasSetStream`, `hipblasGemmEx`,
`hipblasGemmBatchedEx`, `hipblasGemmStridedBatchedEx`, `hipblasSgemm`, `hipblasSgemmBatched`,
`hipblasSgemmStridedBatched` and `hipblasStrsmBatched`. The header answers the compiler, and
nothing answers the linker yet. `compute/hipblas/README.md` holds the route chosen for it, the
measurement behind the choice and the smallest next step. One note for that work, because it is
easy to make a mistake with: with `GGML_CUDA_NO_FA=1` the attention matrix multiplies go through
`hipblasGemmStridedBatchedEx`, so the GEMM must really compute. FlashAttention on moves attention
onto ggml's own kernels instead.

### 4.13 The hipBLAS wall is down: bc250hipblas

MEASURED 2026-10-09, offline, no lab unit. `compute/hipblas` holds the library, and
`compute/hipblas/README.md` holds the whole of it: the type table, the kernel shape, the tests
and the open performance work. This section records the decisions that belong in the design and
the two new walls the build found.

**What was built.** `bc250hipblas.dll`, one translation unit of HIP C++ for gfx1013 over our own
HIP runtime, 11 exported names (the ten the link asks for, plus `hipblasStatusToString`, which
the header declares). Two kernels: a strided batched GEMM and one that takes arrays of pointers
in device memory, because that is how ggml passes a batch (`ggml-cuda.cu:1588`,
`out-prod.cu:100`). One workgroup of 16 x 16 work items per 16 x 16 tile of C, the reduction in
steps of 16 through local memory, 2 KB of local memory, eight wave32 waves.

**Three decisions worth keeping.**

1. **One translation unit, because of `-fno-gpu-rdc`.** `hip::device` of our CMake package
   compiles with `-fno-gpu-rdc`, so each translation unit's device code is self-contained and a
   kernel in one file cannot be started from another. Everything three readers share is in a
   header instead (`src/gemm_core.h`): the kernel, the host test and the device test all use the
   same index arithmetic, the same tile phases and the same type table.
2. **A refusal, never a guess.** Five type combinations are implemented and every other one
   answers `HIPBLAS_STATUS_NOT_SUPPORTED`, as does `HIPBLAS_OP_C` and an unknown algorithm
   selector. The reason is `GGML_CUDA_FORCE_MMQ=ON`: a quantized type that MMQ does not cover
   still reaches `hipblasGemmEx` through `ggml_cuda_mul_mat_cublas`, so an unknown
   `hipblasDatatype_t` read as f32 would be a wrong number in a model's output with no message,
   while a refusal is a stated abort through ggml's own `CUBLAS_CHECK`.
3. **The accumulator is f32 even when the compute type is `HIPBLAS_R_16F`.** More accurate than
   the letter of the type, never less, and it is what this part wants: gfx1013 has no f16 dot
   instruction. The result is rounded to f16 one time, on the store. `alpha` and `beta` are read
   in the compute type, as hipBLAS states, so a `R_16F` call passes two `__half`
   (`ggml-cuda.cu:1399-1402`).

**What the tests can and cannot prove.** The mock backend of layer 2 records a dispatch and
executes no instruction, so no kernel runs on the development PC. The host test therefore runs
the kernel's own three phases serially over every work item of every workgroup, which is what
the two `__syncthreads` of the kernel promise, and compares the result with an independent
reference: 550 checks, 0 failures, over 8 shapes with odd sizes, 4 transpositions, both betas,
three element combinations, batch counts 1, 3 and 5 in both batch modes, `k == 0`, and a
negative control per case. The device test program runs through the interface instead: against
the mock it checks every refusal and proves that a refused call builds no dispatch (446 checks,
0 failures, 43 dispatches expected and 43 recorded, every one a 16 x 16 workgroup), and on unit
A it checks the numbers. Until that lab arm runs, this library is proven on the processor and
unproven on the GPU.

**Two new walls, both in llama.cpp's own build and both down.**

1. **`llama-cli` is not a target at b86d2f07, and neither is one single front end.** The command
   line program of that revision is the target `llama-app` (`bin\llama.exe`), which carries the
   old `llama-cli`, `llama-completion` and `llama-bench` front ends as subcommands. A separate
   `llama-cli` target exists under `tools/cli`, and it is configured only with
   `LLAMA_BUILD_SERVER=ON`, which also pulls in `tools/ui`, whose CMake file downloads prebuilt
   assets from Hugging Face - which an offline build cannot do. `llama-app` does not link without
   the server either: its CMake file links `llama-server-impl` and `llama-cli-impl`
   unconditionally. The two programs that do build offline are `llama-completion` and
   `llama-bench`, they are exactly the two the Vulkan comparison build already holds, and between
   them they are the greedy generation and the benchmark the lab plan needs. The lab plan names
   those two.
2. **`LLAMA_OPENSSL=ON` breaks the host compilation on this machine.** cpp-httplib's
   `find_package(OpenSSL)` finds the MSYS2 installation of the development PC, and CMake then
   puts `-isystem C:/msys64/mingw64/include` on every `llama-common` translation unit. That
   directory holds MinGW's `<stdint.h>`, which shadows the Windows SDK one, and MSVC's
   `vcruntime.h` fails on "unknown type name 'uintptr_t'". `-DLLAMA_OPENSSL=OFF` is the answer
   and costs nothing: a lab trial reads a local model file.

### 4.14 What the whole stack does against the mock, and the one ceiling that stops it

MEASURED 2026-10-09, offline. With `bc250hipblas.dll` and the mock build of `amdhip64.dll` beside
them, `llama-completion.exe` and `llama-bench.exe` start, load `ggml-hip.dll`, open our runtime
and enumerate the device:

```
ggml_cuda_init: found 1 ROCm devices (Total VRAM: 8192 MiB):
  Device 0: AMD BC-250 (mock device), gfx1013 (0x1013), VMM: no, Wave Size: 32, VRAM: 8192 MiB
```

Two walls stood between the link and that line, and both are instructive.

**The event contract, and it was ours.** `ggml_backend_cuda_device_event_synchronize` creates an
event with `hipEventCreateWithFlags(hipEventDisableTiming)` and synchronizes it before anything
records it. Our runtime answered `hipErrorInvalidHandle`, which is a stated abort through ggml's
own `CUDA_CHECK`, and `llama-bench` stopped at its first HIP call. HIP returns success there: an
event that nothing recorded is complete, waits for nothing and holds nothing back, which is what
our own `hipStreamWaitEvent` already answered. The rule was in the wrong place, in the shared
helper that waits for an event's value. It now lives in `hipEventElapsedTime` alone, which is the
one path that really needs a record, because without one there is no timestamp to subtract. The
host test covers all three calls on an unrecorded event (`test_hip_mock`, 232 checks).

**The mock backend's ceiling, and it is only the mock's.** A bounded `llama-bench` run then
reaches the start of a kernel and stops with `hipErrorInvalidImage`. The cause is not the code object
and not the product loader: the mock backend is a fixed-size test double, with
`MOCK_MAX_KERNELS 16`, `MOCK_MAX_SYMBOLS 128` and `MOCK_NAME_MAX 96`, and ggml's code objects are
far above all three. MEASURED with `scratch\m16-hip\step3\probe\mock-ceiling.py` over the 144
translation units of `ggml-hip`:

| | ggml-hip, gfx1013 | the mock's ceiling |
|---|---|---|
| translation units with a gfx1013 image | 139 of 144 | - |
| kernel descriptors in all | 7105 | - |
| kernel descriptors in one translation unit, most | 352 (`mmvf.cu`) | 16 |
| defined symbols in one image, most | 2476 (`mmvf.cu`) | 128 |
| longest symbol name | 176 characters (`binbcast.cu`) | 96 |

84 of those translation units carry more than 16 kernels, 84 more than 128 symbols and 89 a name
longer than 96 characters. The product loader of layer 1 (`bc250hsa/co_loader.c`,
`co_metadata.c`) allocates its kernel and symbol tables from the metadata's own counts and has no
such ceiling, so this wall is between the development PC and a model, not between the driver and
one. **Therefore: a model does not run against the mock, and the first inference of this route is
a lab arm.** Raising the mock's tables to the metadata's counts is named work of its own. It buys
an offline proof of the whole argument path of the backend, and it is not needed for the lab arms
of part 3B.

---

## 5. Repository placement, build and tests

### 5.1 Directory layout

One new top-level directory. Nothing outside it changes, and no driver component changes.

```
compute/
  hip/
    README.md                     what this is, how to build it, what it does not do yet
    build.ps1                     layer 1: the static library, the host tests, the step-1 tool
    build-runtime.ps1             layer 2: amdhip64.dll, amdhip64.lib, the mock tests
    include/
      bc250hsa.h                  the frozen contract of section 3.9
      hip/hip_runtime.h           the minimal HIP header of section 4.4
    bc250hsa/
      kmt_blobs.h  kmt_device.c  kmt_memory.c
      co_loader.c  co_msgpack.c  co_metadata.c  co_internal.h
      kernarg.c    pm4_dispatch.c  pm4_regs.h    submit.c  status.c
      internal.h
    runtime/
      amdhip64.def                55 names: sections 4.1, 4.9 and 8.7
      hip_module.cpp  hip_launch.cpp  hip_memory.cpp  hip_stream.cpp
      hip_event.cpp   hip_device.cpp  hip_error.cpp   dllmain.cpp
      runtime_internal.h
    tests/
      host/
        test_loader.c             the loader on the committed code objects
        test_unbundle.c           the bundle reader, with the two measured traps
        test_kernarg.c            the packer against the committed metadata
        test_pm4.c                the PM4 stream against the golden dwords
        test_descriptor.c         the kernel descriptor fields against the measured values
        bc250hsa_mock.c           a mock device for layer 2, over host memory
        test_hip_mock.cpp         layer 2 against the mock: register, launch, stream, event
        hip_threads_client.h      the multithreaded client, shared with samples/threads.hip
        test_hip_threads.cpp      layer 2 under four threads, and its negative control
      data/
        PROVENANCE.txt            the clang revision and the exact build command of each file
        m16_kernels.gfx1013.co    one code object with vadd, reduce256 and writeGridSize
        m16_kernels.fatbin        the same object inside a clang offload bundle
        m16_kernels.metadata.txt  the metadata note, as a reference for the test
        pm4_vadd.golden.txt       the expected dispatch stream, dword by dword, with comments
    tools/
      hipprobe.c                  the step-1 tool
      run-lab.ps1                 the step-1 trial of section 6
```

The three test code objects are about 24 KB in all. They are our own build output, not a third-party
blob, and the tests need them to run in a build gate with no AMDGPU compiler present. `PROVENANCE.txt`
names the clang revision, the source of each kernel and the exact command, and `build.ps1 -Rebuild`
rebuilds them when the portable AMDGPU clang is available, then compares the result with the committed
file.

### 5.2 Which compiler

DECIDED, and this is the same answer for both layers.

- The product code is built with MSVC: `cl.exe` and `link.exe` from the installed Visual Studio
  toolset, with the headers and the import libraries from the SDK NuGet packages under `-Kits`, exactly
  as `tools/win/monfence/build.ps1` and `tools/win/kmtprobe/build.ps1` do. Flags `/W4 /WX /O2 /MT` for
  the library and the tool, `/MD` for the DLL, `/std:c11` for layer 1 and `/std:c++17` for layer 2.
  Reason: layer 1 and layer 2 call D3DKMT and ship beside the driver, every other user-mode component
  of this repository is built this way, and the release build must stay reproducible with one toolchain.
- The portable AMDGPU clang 22 is used for device-side artifacts only: the test code objects, the HIP
  sample programs, the ROCm device library bitcode, and `llvm-dlltool` for `amdhip64.lib`. It is never
  needed to build the product code, and `build.ps1` skips those steps with a clear message when it is
  absent.
- No installer and nothing on drive C:. Build output goes to the directory that `-Out` names, which is
  outside the repository and outside drive C:, and the compiler temporary directory is redirected, as
  the existing build scripts do.

### 5.3 What each build script does

`compute/hip/build.ps1`, parameters `-Root`, `-Kits`, `-Out`, `-KitVersion`, `-Rebuild`, `-CheckDoc`,
`-SkipTests`:

1. Resolve the kits and the MSVC toolset, create the output directory and redirect `TEMP`.
2. `-CheckDoc`: compare the fenced header block of `docs/design/m16-hip-route-b.md` with
   `include/bc250hsa.h` byte for byte and fail on a difference.
3. Restate-and-compare gates, in the style of `monfence/build.ps1`: the `RELEASE_MEM` constant dwords
   against the Mesa fork's progress write, the `SET_SH_REG` offsets of `pm4_regs.h` against
   `third_party/linux-amdgpu/gc_10_1_0_offset.h`, and the private blob magic values against
   `driver/contract/bc250_umd_submit.h`. A copy that drifts is a build failure.
4. Build `bc250hsa.lib`, then the five host tests, then `hipprobe.exe`.
5. Run every host test and `hipprobe.exe --selftest`, which runs the loader, the packer and the PM4
   builder with no device and no GPU.
6. Parse `tools/run-lab.ps1`, because it runs under Windows PowerShell 5.1 on the lab.
7. Print the size and the SHA256 of every artifact.

`compute/hip/build-runtime.ps1` does the same for layer 2: it builds `amdhip64.dll` and
`amdhip64.lib`, checks that the exported names equal `amdhip64.def` and that the def file holds exactly
the 55 names of sections 4.1, 4.9 and 8.7, builds `test_hip_mock.exe` against `bc250hsa_mock.c`, runs it, and, when
the AMDGPU clang is present, compiles and links the HIP sample and checks its import table. It also
builds and runs the two multithreaded tests of section 5.4, the negative control among them, and the
clang-built `hipthreads.exe` against the mock build of the DLL.

### 5.4 The host tests, and what each one would catch

| Test | Input | It fails when |
|---|---|---|
| `test_loader` | the committed code object | a segment is not copied, `.bss` is not zeroed, or a relocation type is accepted wrongly. It also fails when a descriptor address is wrong (the measured 0xC80 and 0x1E00) or an ABI version check is wrong |
| `test_unbundle` | the committed fat binary, and three hand-made bad ones | the host entry of size 0 is chosen, or the repeated offset confuses the reader. It also fails when the host triple is matched, or a `CCOB` bundle is not refused by name |
| `test_kernarg` | the committed metadata and a kernel start of known numbers | an explicit argument lands at the wrong offset, or a hidden field is written from a fixed structure instead of the list. It also fails when a hidden kind is not zeroed, or the hostcall counter does not rise for a kernel that asks for it |
| `test_descriptor` | the three committed descriptors | `RSRC1` or `RSRC3` is modified, or `LDS_SIZE` is not written for `reduce256`. It also fails when the user SGPR count is not read from `RSRC2`, or a reserved field is not checked |
| `test_pm4` | a dispatch of known numbers | the packet order changes, a register offset changes, or the shader-type bit is lost. It also fails when the entry address shift is wrong, the fence dwords change, or the padding is wrong |
| `test_hip_mock` | layer 2 over the mock device | registration does not find a kernel from its host stub, or the packer writes the wrong arguments. It also fails when stream order is lost across an event wait. It fails as well when the per-thread error state leaks between threads, or when a second module in one process registers twice |
| `test_hip_threads` | layer 2 under four threads, over the mock device | a thread that waits for the device stops the other threads. It counts the operations the others finish inside one long wait. It also compares the wall time of four threads with the time a serialised runtime takes. It fails as well when a stream or an event that another thread destroyed inside a wait leaks, or is freed twice |
| `test_hip_threads_control` | the same test over a runtime compiled with `BC250_HIP_WAIT_UNDER_LOCK=1` | the negative control. It fails when the other threads finish work although the lock is held over the wait. Such a run would measure something else than it says. It also refuses to call itself a control unless the runtime it links really waits with the lock held |

The mock device implements `bc250hsa.h` over host memory with a synthetic GPU address base, and it
links the real loader, the real packer and the real PM4 builder. Only the device, the submission and
the fence are mocked. A mock submission records the dword stream and retires the fence at once, so a
test asserts on the stream.

### 5.5 Branches and the order of work

| Branch | Layer | Base | Contents |
|---|---|---|---|
| `m16/hip-dispatch` | 1 | `origin/main` | this design document, the frozen header, then `bc250hsa/`, `tests/host/` (the five layer-1 tests and the mock), `tools/`, `build.ps1`, `README.md`, `include/hip/hip_runtime.h` |
| `m16/hip-runtime` | 2 | the first commit of `m16/hip-dispatch`, which is the commit that adds `compute/hip/include/bc250hsa.h` | `runtime/`, `amdhip64.def`, `tests/host/test_hip_mock.cpp`, `build-runtime.ps1` |

The first commit of `m16/hip-dispatch` holds the design document and the header and nothing else, so
`m16/hip-runtime` starts from a tree that holds the contract and no implementation. The two branches
touch disjoint files after that commit, apart from `include/hip/hip_runtime.h`, which layer 1 commits
once as the measured artifact of the spike and layer 2 extends in step 3. `tests/data/` and the mock
belong to layer 1, because layer 1 owns the loader.

Both builders may land their branch into main on its own, because neither branch changes a driver
component and both carry their own build gate. Layer 2 does not wait for a lab trial: it ships against
the mock, and the first run on hardware follows step 2 of the route document.

---

## 6. The step-1 lab trial

One trial, one unit, under three minutes of lab time. It proves the whole chain from HIP source to
hardware with no AMD user-mode component: clang, our loader, the kernel argument buffer, a PM4 dispatch
on node 0, a fence, and the expected bytes in memory.

### 6.1 Before the trial

- The deployed kernel driver already accepts user-mode submissions, which a working Vulkan run on the
  same install proves. No new kernel driver build is needed for this trial.
- Check the compute unit mode (40) and the GPU clock through the normal preflight of the lab harness,
  and check `Tctl` below 87 C before the first run.
- No held SSH session before the trial runs. Open the sampler only after the tool prints that it
  started.
- Copy `hipprobe.exe`, `m16_kernels.gfx1013.co` and `run-lab.ps1` to the lab scratch directory
  through the normal target tool. Total about 300 KB.

### 6.2 The runs, with their bounds

`run-lab.ps1` performs these in one session and writes one JSON record per run:

| Run | Command | Bound |
|---|---|---|
| 0 | `hipprobe --selftest` | host only, under 2 s |
| 1 | `hipprobe --info` | opens the device, prints `bc250hsa_props`, closes, under 3 s |
| 2 | `hipprobe vadd --n 1048576 --wait-slice 1000 --wait-total 10000` | under 15 s |
| 3 | `hipprobe reduce256 --n 65536 --wait-slice 1000 --wait-total 10000` | under 15 s |
| 4 | `hipprobe writeGridSize --grid 7,3,2 --block 64,2,1 --wait-slice 1000 --wait-total 10000` | under 15 s |
| 5 | `bc250kmd_cli log` and a second `hipprobe --info` | under 10 s |

Every wait is bounded: the tool passes 1000 ms slices and a 10000 ms total, instead of the library's
120 s default, because each kernel is microseconds of work and the trial must stay short. The tool
refuses to start a run when the device already reports a loss, and it never resubmits after a timeout.
The whole session, with the copy and the log pull, is about 90 seconds. The hard stop is 170 seconds.

### 6.3 Pass criteria

All of these, from step 1 of the route document:

1. `vadd`: every one of the 1048576 elements equals the sum of its two inputs. The tool reports
   `mismatches: 0` and the index and the values of the first three mismatches when there are any.
2. The fence retires one time per dispatch: the value the tool read equals the value it was promised,
   and `waits_fast` or `waits` rose by exactly one.
3. `reduce256` gives the reference reduction of its input. This is the measurement that proves the
   computed `LDS_SIZE`, which is decision 3 of section 2. A wrong value here, while `vadd` gives the right
   numbers, is the local memory path and nothing else.
4. `writeGridSize` writes the grid numbers that the dispatch asked for. This proves the hidden argument
   block.
5. The kernel driver log ring holds no "umd submit fence N not run" line, no memory fault and no
   timeout.
6. No TDR: no bugcheck, no device loss reported by `bc250hsa_query_fault()`, and the second
   `hipprobe --info` of run 5 still opens the device.
7. `hostcall_buffer_requests` is 0 for these three kernels, which is the expected value and the first
   data point for kill criterion K4.

### 6.4 Evidence to pull

Into `evidence/m16/step1-<date>/`, with a short README that names the artifacts, the tool build hash
and the kernel driver version:

- the JSON record of every run, with the counters of `bc250hsa_counters_read()`,
- the indirect buffer dwords of the last dispatch, from `bc250hsa_last_ib()`, always, not only on a
  failure, because the golden test in the repository compares against the same stream,
- the kernel driver log ring before and after the session,
- `bc250hsa_props_read()` output, with the clock and the compute unit count of the moment,
- the `bc250hsa_fault` block when any run ends in a loss,
- the temperature before and after.

On success, two facts rows follow: the first gfx1013 dispatch of a clang-built code object through our
own user-mode path, and the measured `LDS_SIZE` behaviour of a PM4 dispatch.

### 6.5 Stop rules

- K1. If the dispatch work does not reach these criteria within 10 working days, stop route B and
  review route A2. The trial itself is cheap. The criterion is about the work, not about one run.
- K5. On a hardware hang: stop at once, do not resubmit, do not start a second trial. Pull the kernel
  driver log ring, and the crash dump when a bugcheck occurred. This chip has no working GPU reset
  (fact M53), and the kernel driver's own recovery is a soft one that only kills the hung job's waves
  when hang recovery is enabled. Without it the result is a bugcheck. Review the queue design before
  another trial, as the route document requires.
- Thermal: the standing 87 C rules of the lab apply unchanged. The trial is far too short to heat the
  part, and that is one more reason to keep it short.

---

## 7. What stays open for the lab

Each item names the trial that answers it. None of them blocks step 1.

1. Does `D3DKMTLock2` work on a device-local allocation on this part? It decides whether `hipMalloc`
   can stay in VRAM with a host mapping, or whether a copy kernel comes first. One probe inside the
   step-2 trial.
2. Does the command processor accept `SET_SH_REG_INDEX` with index 3 here? If it refuses, drop packets
   7 and 8 (`BC250HSA_DISPATCH_NO_CU_MASK`). The queue descriptor already holds the same compute unit
   mask. The step-1 trial answers it, because the tool can repeat run 2 with the flag.
3. Is `CONTEXT_CONTROL` needed before a compute dispatch on node 0 after the kernel driver's own ring
   frame? The design emits it, following libdrm. A later trial may drop it and measure.
4. A kernel that spills needs a scratch ring, a waves and wave size encoding with a granularity of 1024
   bytes on this part, and a private segment buffer whose stride and swizzle bits are right. Our Vulkan
   path does not answer this, because RADV's compute ABI puts a 2-dword scratch pointer in `s[0:1]`
   instead of the 4-dword HSA private segment buffer. Until a trial with a deliberately spilling kernel
   measures it, `uses_dynamic_stack` is refused with `BC250HSA_EUNSUPPORTED`.
5. Does live llama.cpp code ask for `hidden_hostcall_buffer`, which is device-side `printf`? The
   counter answers it in the first step-3 run. A non-zero count costs about three weeks for a host call
   service (kill criterion K4).
6. Does the 8 GiB device-local segment ceiling that the Vulkan path meets also bind a HIP allocator? A
   memory map question, and a stop criterion of M16 if it does.
7. What must `multiProcessorCount` and `maxThreadsPerMultiProcessor` report for llama.cpp to pick good
   block sizes? The compute unit count is known (40 after the regression of 2026-10-05), so this is a
   reporting question and not a discovery.
8. Does a second dispatch in flight behave as the one-indirect-buffer rule says? The step-2 trial
   submits two dispatches back to back and measures where the second one waits.
9. Event timing. Build 1 reports host timestamps. A GPU timestamp through a second `RELEASE_MEM` with
   a 64-bit clock value is the better answer and needs one measurement of its cost.

Four questions stay open offline, and none of them needs the lab. Does `R_AMDGPU_ABS64` ever appear
in a `-fgpu-rdc` code object? A two-file probe answers it in one hour, and llama.cpp does not use
`-fgpu-rdc`. Does a compressed offload bundle ever reach us? The reader refuses one by name until a
measurement says otherwise. Who writes the real `prepare-builtins` tool of the device library build?
What does the hipBLAS shim cost? Step 3 of the route document measures that with a call counter in
every entry point.

---

## 8. The off-GPU cost of one dispatch: batching and the barrier

Build 1 submits one kernel dispatch as one indirect buffer and one `D3DKMTSubmitCommand`. That is the
simplest thing that can be right, and it was the right first step, but it makes the cost of a dispatch
a fixed cost of the operating system: one user-to-kernel transition, one ring slot, one fence, one
completion write, for a kernel that may run for two microseconds. A decode step of a language model is
hundreds of such kernels per token, so this is the number that decides whether the route is usable.

This section is a later addition to the design (interface minor version 1.1, section 8.1 of
`bc250hsa.h`). It changes no interface that build 1 established: every call keeps its meaning, and a
caller that does not know the new policy exists has the behaviour of build 1.

### 8.1 What one dispatch costs, and what batching removes

| Per dispatch, build 1 | Who pays | Removed by a batch of N |
|---|---|---|
| one `D3DKMTSubmitCommand` | the kernel driver, the scheduler, the ring | all but one of the N |
| one ring slot and its fence value | the device | all but one of the N |
| one `CONTEXT_CONTROL` and one full `ACQUIRE_MEM` at the head | the GPU front end and its caches | all but one of the N |
| one `RELEASE_MEM` completion write | the GPU and the memory it writes | all but one of the N |
| the state writes and `DISPATCH_DIRECT` | the GPU | nothing: this is the work itself |
| the kernel argument buffer | the host | nothing |

What a batch does not remove is the dispatch itself and the dependency between two dispatches. Section
8.5 is about making that dependency cost what it has to cost, and no more.

### 8.2 The mechanism lives in layer 1

Batching is in `bc250hsa`, not in the HIP runtime, for one reason that is worth more than the lines it
saves: layer 1 owns the wait. `bc250hsa_wait` submits an open buffer as soon as a caller asks for a
fence value that was promised but not yet sent to the device. Every path by which a program can observe
the device therefore flushes by construction, and layer 2 needed two insertions in all, not an audit of
every entry point. A forgotten flush point is not a slow program: it is a wait that cannot end.

The kernel driver takes exactly one indirect buffer per submission (`single_ib` in `umd_blob.h`, refused
otherwise), so a batch is not several buffers in one submission. It is one larger buffer with several
dispatches inside it.

### 8.3 The fence, and the ring slot

One submission carries one fence value, so the dispatches of one batch share it. That has two
consequences, and both are handled where the value is produced:

- Two numbers, not one. `fence_last_submitted` is what the device was given. `fence_last_assigned`
  includes the value an open buffer has already promised to its dispatches. The public
  `bc250hsa_fence_last_submitted` reports the promised value, because that is what a stream or an event
  of layer 2 has to compare against.
- The ring slot of an open buffer is held. Its slot fence is set to the promised value when the buffer
  opens, and the device fence can never reach a value that was never submitted, so the slot cannot be
  handed out again while the buffer is still being filled.

When a submission fails, the dispatches in it already carry a value that nothing can ever deliver. The
device is therefore marked lost by name at that point, as section 3.8 requires, and the slot fence is
cleared.

### 8.4 The flush points, and the caps

A buffer is submitted at the first of these:

| Flush point | Why |
|---|---|
| a wait for a value above `fence_last_submitted` | the value cannot retire until the device has the work |
| an event record | an event answers a question about time, so it must cover the work asked for by then |
| a copy through the host mapping, a map, an unmap | the bytes the caller reads or writes are the bytes the open work produces |
| a free | the range may be named by a dispatch of the open buffer |
| a code object load | the instruction cache invalidate of the new object must not be behind older work |
| the device closing | nothing may be left unsubmitted |
| the dispatch count cap (default 32, hard maximum 256) | a bound on how far the host runs ahead |
| the dword cap (one ring slot, less the completion write) | the buffer must fit the slot |
| the hold time cap (default 1000 us) | see the honest note below |

There is a sixth one, and it is not in layer 1 at all: the kernel argument pool of layer 2
(`kKernargPoolMax` in `runtime/hip_launch.cpp`, 64 buffers). A kernel argument buffer may not be
written again until the dispatch that reads it has retired, so dispatch 65 of a run waits for the
oldest buffer, and a wait for a value an open buffer has promised is the first flush point of the
table. MEASURED on unit A (`evidence/m16/perf-2026-10-09`): at a dispatch cap of 256 with the
time cap effectively off, 2000 dispatches became exactly 32 submissions and 1000 chained dispatches
exactly 16, which is 2000/64 and 1000/64 rounded up, and the wait count of each arm equals its
submission count. **That pool, and not the dword cap, is what makes a dispatch cap above 64
unreachable today.** The dword cap is nowhere near it: one dispatch and its barrier is 72 dwords
without the state cache of section 8.8 and 23 with it, so a 65536-byte ring slot holds 227 or 711
of them. The default cap of 32 is below the pool and the pool therefore never binds. A cap above 64
would need the pool raised with it, which is 4096 bytes of host-visible memory per entry and a
decision nothing yet needs.

A batch trades the cost of a submission against the time to the first instruction: while the host
fills a buffer, the device has nothing of that buffer to run. For a program that runs ahead of the
device, which is what a decode loop does, that is free. For a program that sends one kernel and waits
for it, it is not, and that program keeps the behaviour of build 1 anyway, because its wait flushes.
The dispatch cap and the hold time cap bound the trade in between, and the `chain` measurement of
hipbench is end to end on purpose, so an arm that holds work back too long is visible there and
not only in the submission count.

The library runs no thread of its own, so the hold time cap is checked when the next call arrives and
not by a timer. This is stated plainly because it would be easy to present it as a guarantee: the
guarantee is the one above it. Nothing a program can observe stays behind an open buffer, because every
observation flushes it. The time cap only keeps a program that dispatches slowly from holding its own
work in the buffer for longer than it meant to.

### 8.5 The barrier between two dispatches of one buffer

Every dispatch of build 1 has a full `ACQUIRE_MEM` at its head: every cache level invalidated, L2
written back. At the head of an indirect buffer that is right, because the host has written kernel arguments and
possibly code since the last submission. Between two dispatches of the same buffer, where the only new
writer is the GPU itself, it asks the hardware for work nothing needs.

What a compute-to-compute buffer dependency needs on GFX10 is taken from Mesa, not invented
(`ref/mesa` at `05e6c9622e1`):

- `src/amd/common/ac_barrier.c:89-103` builds the cache operation of a dependency: the scalar and
  vector level-0 caches from their own flags, and, below GFX12, L1 whenever either of them is
  invalidated. `src/amd/vulkan/radv_cmd_buffer.c:8133-8147` is the destination side that asks for
  both for a storage buffer read, because ACO reads a storage buffer through the scalar unit.
- `src/amd/common/ac_barrier.c:289-291` is the wait itself, `CS_PARTIAL_FLUSH` under
  `AC_BARRIER_SYNC_CS`, which build 1 already writes after every dispatch.
  `src/amd/vulkan/radv_cmd_buffer.c:7879-7881` is where a dependency on earlier compute work asks
  for it.
- The L2 part is a separate decision: `src/amd/vulkan/radv_cmd_buffer.c:7910-7914`
  (`can_skip_buffer_l2_flushes`), with `:7969-7982` and `:8094` as its use. A buffer dependency on
  GFX10 can skip the L2 write-back and invalidate when L2 is coherent for the writers involved.
- `src/amd/common/ac_gpu_info.c:1236` is the condition under which that skip is unsafe
  (`tcc_rb_non_coherent`).

The light barrier is therefore `GL1_INV | GLV_INV | GLK_INV`, written in `pm4_regs.h` beside the full
value, with the bits it drops named: `GL2_INV`, `GL2_WB`, `GLM_INV`, `GLM_WB`, `GLI_INV`. The
instruction invalidate is dropped because a code object load is a flush point, so no buffer can hold a
dispatch of code that was written after the buffer opened. `test_pm4.c` checks the exact dwords of both
values and asserts the dropped bits as its negative control.

Deviation from Linux, stated as repo rule 7 requires: radv decides the L2 part per barrier from the
resources involved, and we decide it once per buffer from a policy switch. Whether gfx1013 reports a
power-of-two L2 block count, which is what makes the skip safe for a buffer dependency, is not measured
on this part yet. That is why the default is the full acquire and why the light one is a switch: the lab
arm of section 8.7 is what may change the default, and nothing else.

### 8.6 The switches, and the defaults

| Switch | Values | Default |
|---|---|---|
| `BC250_HIP_BATCH` | `0`, `1` | `1`, several dispatches in one indirect buffer |
| `BC250_HIP_BATCH_MAX` | 1 to 256 dispatches | 32 when batching is on |
| `BC250_HIP_BATCH_HOLD_US` | microseconds | 1000 |
| `BC250_HIP_BARRIER` | `full`, `light` | `light` |
| `BC250_HIP_PM4_STATE_CACHE` | `0`, `1` | `1`, write only the state that changed |

The first two defaults were the conservative value until the hardware had spoken. It has
(`evidence/m16/perf-2026-10-09`, facts M853 to M857): batching with the light barrier is exact over
two chains of 1000 dependent kernels, 2.2 times faster than build 1 on the `launch` line and 3.8
times faster on the `chain` line, and it cuts submissions per dispatch from 1.000 to 0.032. The
defaults are therefore the measured arm P3 of that session. `BC250_HIP_BATCH=0` with
`BC250_HIP_BARRIER=full` is exactly build 1, and `hipbench --batch 0 --barrier full` is the control
arm the build gate runs on every build.

Layer 1 reads no environment variable: `hip_device.cpp` reads these five and calls
`bc250hsa_batch_policy_set`, so the policy stays the caller's (header rule 6), and a program that
embeds layer 1 directly sets the same structure itself. The state cache is not part of the policy
structure, because it is a property of one indirect buffer and not of the device: it is the
`BC250HSA_DISPATCH_FULL_STATE` flag of a dispatch, and `BC250_HIP_PM4_STATE_CACHE=0` is what sets
it on every dispatch of a process.

### 8.6a What the measurement cost, and the one line it did not explain

Two numbers of that session are worth carrying in the design, because both are easy to read wrongly.

- **The gain is host time, not GPU time.** The off-GPU cost of a dispatch fell from 14.6-16.5 us
  to 3.8 us on the chain line. For a 7-billion-parameter model that is not the first bottleneck:
  it is about 6.2 ms of one processor core a token, which batching cuts to 1.5 ms. It decides the
  result for a small model, a long kernel chain, or a build that wants the processor for its own
  host side.
- **One line got slower, and it is not in this code path.** A 4 KB device-to-host `hipMemcpy` is
  19.2-19.4 us with batching off and 28.8-28.9 us with it on, while the same arms' 1 MiB and
  64 MiB device-to-host copies do not split with the policy at all: all six arms are within
  0.8 % of each other on the 1 MiB line and within 0.1 % on the 64 MiB line, and the batched and
  the unbatched arms are mixed through both of those spreads. The host path of a
  copy is one function per direction and it does not look at the policy: one `bc250hsa_flush`
  (which returns at once with no buffer open), one lookup and one `memcpy`. The host-to-device
  line of the same arms is 0.264-0.333 us in every arm, which bounds that whole shared path well
  under one microsecond, and that path does not move with the policy. `test_hip_batch` section 5
  asserts the flush, submission and wait counts of a copy are equal under both policies. So the
  difference is a read rate of the write-combining mapping (203 MiB/s against 135 MiB/s at 4 KB),
  not a fixed host cost, and what sets that rate is not established. The arm that decides it is in
  `scratch/m16-hip/lab/perf-README.md`: the same copy line with no kernel phase in front of it.

### 8.7 What is measured, and what the lab must answer

`samples/hipbench.hip` is the measurement: the dispatch rate of an empty kernel, a dependent chain of tiny
kernels in the shape of a decode step, host copies at 4 KB, 1 MiB and 64 MiB, the cost of synchronising
an already retired stream, and an event round. It reads the submission counters of the driver through
`bc250hipGetCounters`, so it prints submissions per dispatch beside its timings. No HIP entry point
answers that question, which is why those two calls exist.

The host tests prove the harness and the mechanism, and they cannot prove the gain: the mock backend
runs no instruction. The lab answered it on 2026-10-09 in six arms of 3.4 to 3.8 s each
(`evidence/m16/perf-2026-10-09`, facts M853 to M857), and section 8.6 states the defaults that
follow. What is left for the next slot is in `scratch/m16-hip/lab/perf-README.md`: the state cache
of section 8.8 against the switch that turns it off, a cap of 64 under it, and the copy line of
section 8.6a with no kernel phase in front of it.

### 8.8 The compute state a dispatch repeats

A `SET_SH_REG` write is persistent register state. `DISPATCH_DIRECT` does not clear it, and this
build programs nothing at the ring frame, so inside one indirect buffer the second dispatch of the
same kernel does not have to say the same thing again. Build 1 said it anyway, 64 dwords per
dispatch, because build 1 had one dispatch per buffer and there was nothing to repeat.

`bc250hsa_pm4_ib_append` now takes a `bc250hsa_pm4_state` cache (`bc250hsa/internal.h`, not public)
and writes only what differs from the dispatch before it in the same buffer. What is in the cache:
the program address, the two resource registers, RSRC3, the workgroup size and the user data run.
What is left out of every dispatch but the first: the three start registers, the shader checksum,
the six request-control registers, the coherency start delay, the two compute-unit masks, the
scratch ring size and the resource limits, none of which this build ever varies.

Three rules, and each one is a test in `tests/host/test_pm4.c` section 2c:

1. **The first dispatch of every buffer is complete.** Another context's buffer runs between two of
   ours, so nothing may be assumed across a submission. `submit.c` clears the cache when it opens
   a buffer, and `bc250hsa_pm4_build_batch` starts a fresh one per call.
2. **A dispatch that overflowed the buffer did not write its dwords, so it does not touch the
   cache.** `submit.c` appends it again into the next buffer with `first` set.
3. **A field that changed is written.** This is the negative control, one changed field at a time:
   the block, the kernel argument pointer, the local memory a dispatch asks for, and a second
   kernel. A cache that skipped any of them would dispatch a kernel with the previous dispatch's
   value, which is the whole risk of the mechanism.

MEASURED on the development machine, by the pure builder with no device in it: one dispatch and its
barrier is **72 dwords** without the cache and **23** with it, for the shape a real dispatch has (the
same kernel, the same block, a new kernel argument buffer each time, which is what layer 2 hands
out). A 65536-byte command ring slot holds 16368 usable dwords, so it holds 227 such dispatches
without the cache and 711 with it. Neither number is the ceiling that binds: the kernel argument
pool of section 8.4 closes a buffer at 64. What the cache buys is therefore not a deeper cap. It is
3.1 times fewer dwords for the host to write and for the command processor to read on every
dispatch after the first, inside the cap we already use.

`BC250HSA_DISPATCH_FULL_STATE` turns it off, dispatch by dispatch, and the stream is then dword for
dword the one the builder wrote before this section existed. That flag is the control arm, on the
development machine and on the lab.

---

## 9. What this design does not do

It does not design the hipBLAS shim. It does not change any driver component. It does not add a
hardware queue display driver interface, and it does not need one. It does not plan Strata. It
evaluates no PAL component, because the owner excluded PAL for every purpose. It makes no claim that
any of this ran on hardware: every statement marked MEASURED was measured on the development machine,
and the first hardware statement will come from the trial of section 6.
