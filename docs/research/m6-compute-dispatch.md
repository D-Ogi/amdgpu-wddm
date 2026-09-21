# gfx10 compute dispatch: libdrm reference transcription for BC-250 (Cyan Skillfish, GC 10.1.3)

Milestone M6 research note. Target: run a compute dispatch from a kernel-owned MEC compute
queue in VMID 0 that writes a known pattern to GART-mapped memory.

Everything below is read-only research. Nothing in `P:\BC-250\bc250-win` was touched.

## 0. Source provenance

### libdrm

| Item | Value |
| --- | --- |
| Repo | `https://gitlab.freedesktop.org/mesa/drm.git` |
| Tag | `libdrm-2.4.114` (annotated) |
| Tag object sha1 | `6d4be7f9ba89babe9f8e574f0fee8b1141d0ea14` |
| Commit (tag target, `libdrm-2.4.114^{}`) | `b9ca37b3134861048986b75896c0915cbf2e97f9` |
| Commit date | Thu Nov 3 09:33:36 2022 +0100 |
| Commit subject | `build: bump version to 2.4.114` |
| Local path | `P:\BC-250\ref\libdrm` (shallow clone, depth 1, detached HEAD) |

Naming note: the team brief referred to `shader_code_nv10.h`. That file does not exist at this
tag. As of the 2022 refactor the shader binaries live in **`tests/amdgpu/shader_code_gfx10.h`**
and the dispatch driver moved out of `basic_tests.c` into **`tests/amdgpu/shader_test_util.c`**.
Both are present at 2.4.114. Tags up to `libdrm-2.4.129` still exist upstream; 2.4.114 was chosen
because it is the first release carrying the clean `shader_code_gfx{9,10,11}.h` split, which makes
the gfx10 path easy to read in isolation.

### Linux kernel

`P:\BC-250\ref\linux-src`, mainline v6.18. Note for later sections: this checkout has **no
`drivers/gpu/drm/amd/amdkfd/`** and **no `mes_v10_1.c`**, and `v10_structs.h` lives under
`drivers/gpu/drm/amd/include/`, not under `amdgpu/`.

---

## 1. Which shader the test picks, and whether Cyan Skillfish qualifies

At 2.4.114 the selection is **not** by ASIC family. It is purely by the GFX IP major version
reported by the kernel.

`P:\BC-250\ref\libdrm\tests\amdgpu\shader_test_util.c:95-116`:

```c
r = amdgpu_query_hw_ip_info(device_handle, ip, 0, &info);
...
switch (info.hw_ip_version_major) {
case 9:  test_info.version = AMDGPU_TEST_GFX_V9;  break;
case 10: test_info.version = AMDGPU_TEST_GFX_V10; break;
case 11: test_info.version = AMDGPU_TEST_GFX_V11; break;
default: printf("SKIP ... unsupported gfx version %d\n", ...); return;
}
```

`hw_ip_version_major` comes straight from the IP block descriptor:
`P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\amdgpu\amdgpu_kms.c:556`
(`result->hw_ip_version_major = adev->ip_blocks[i].version->major;`), and for every gfx10 ASIC
that is the single `gfx_v10_0_ip_block` with `.major = 10`
(`P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\amdgpu\gfx_v10_0.c:10241-10247`).

**Conclusion: Cyan Skillfish (GC 10.1.3) selects `AMDGPU_TEST_GFX_V10`, i.e. the
`bufferclear_cs_shader_gfx10` path. There is no Navi10-only gate.**

Family checks do exist in the same file, but only for the *hang* shaders, which we do not want:

- `shader_test_util.c:176-186` (`amdgpu_dispatch_load_cs_shader_hang_slow`): switches on
  `AMDGPU_FAMILY_AI` / `AMDGPU_FAMILY_RV`, everything else falls to `memcpy_cs_hang_slow_nv`.
- `shader_test_util.c:877-884` (draw hang slow): checks `AMDGPU_FAMILY_NV` together with
  `gpu_info.chip_external_rev < 40`.

Neither is on the memset dispatch path (`shader_test_util.c:191-204` takes the `else` branch
whenever `test_priv->info->hang == 0`).

Sanity check on the register table: `shader_code.h:102-104` deliberately reuses the **gfx9**
register list for the gfx10 shader:

```c
// gfx10, cs_bufferclear
{{bufferclear_cs_shader_gfx10, sizeof(bufferclear_cs_shader_gfx10),
  bufferclear_cs_shader_registers_gfx9, ARRAY_SIZE(bufferclear_cs_shader_registers_gfx9)},
```

So `COMPUTE_PGM_RSRC1/RSRC2` and `COMPUTE_NUM_THREAD_X/Y/Z` values are shared between gfx9 and
gfx10. Only the ISA words and a couple of extra registers differ.

---

## 2. (b) Shader binary: location and size, not retyped

### The one we want

**`P:\BC-250\ref\libdrm\tests\amdgpu\shader_code_gfx10.h:27-31`** -
`static const uint32_t bufferclear_cs_shader_gfx10[]`, **9 dwords (36 bytes)**, 4 dwords per
source line over lines 28-30 plus one on line 30.

Loaded by `shader_test_util.c:200-202` with
`memcpy(cs_bo.ptr, shader_test_cs[version][cs_type].shader, ...shader_size)`.

### Companion / cross-check binaries

| Symbol | File:lines | Dwords | Use |
| --- | --- | --- | --- |
| `bufferclear_cs_shader_gfx10` | `shader_code_gfx10.h:27-31` | 9 | memset dispatch, gfx10. **This is the one to transcribe.** |
| `buffercopy_cs_shader_gfx10` | `shader_code_gfx10.h:33-36` | 8 | memcpy dispatch, gfx10 |
| `bufferclear_cs_shader_gfx9` | `shader_code_gfx9.h:27-32` | 13 | gfx9 equivalent, useful for decoding |
| `buffercopy_cs_shader_gfx9` | `shader_code_gfx9.h:42-46` | 11 | gfx9 memcpy |
| `bufferclear_cs_shader_registers_gfx9` | `shader_code_gfx9.h:34-40` | 5 x `{reg, value}` | the SH register table used by **both** gfx9 and gfx10 |
| `shader_bin` (minimal flat-store CS) | `basic_tests.c:332-337` | 15 | alternative, see section 4 |

`shader_code_gfx9.h:27-32` carries a **byte-swap caveat**: the gfx9/gfx10 arrays are plain host
dwords and require no swap. `basic_tests.c:332-337` wraps every word in `SWAP_32(...)`, so that
one is stored little-endian-per-word in a different order. Do not mix the two conventions.

### What `bufferclear_cs_shader_gfx10` actually does

Derived by decoding the 9 words against the GFX10.1 ISA encodings and cross-checking against the
gfx9 variant, which has the same structure with one extra `V_AND_B32`. Summary, safe to rely on:

1. `V_LSHL_ADD_U32 v4, s8, 6, v0` - global element index = `workgroup_id_x * 64 + tid_x`,
   into `v4`. (`s8` is the workgroup id, placed there because `COMPUTE_PGM_RSRC2.TGID_X_EN = 1`
   and `USER_SGPR = 8`.)
2. Four `V_MOV_B32 v0..v3, s4..s7` - the fill pattern, copied out of
   `COMPUTE_USER_DATA_4..7`.
3. `BUFFER_STORE_FORMAT_XYZW v[0:3], v4, s[0:3], 0 idxen` - the MUBUF store. `s[0:3]` is the
   buffer resource descriptor from `COMPUTE_USER_DATA_0..3`; `v4` is the record index; `idxen`
   is set (bit 13 of the first MUBUF dword), so the byte offset is `index * STRIDE` with STRIDE
   taken from the descriptor.
4. `S_ENDPGM`.

So: **the shader reads its arguments entirely from `COMPUTE_USER_DATA_0..7`**, which answers one
of the open questions in the brief. `USER_DATA_0..3` = a 128-bit buffer V#, `USER_DATA_4..7` =
four dwords of fill value. Register budget matches the declared `COMPUTE_PGM_RSRC1`:
VGPRS field 1 -> 8 VGPRs allocated, shader uses v0..v4; SGPRS field 1 -> 16 SGPRs, shader uses
s0..s8.

---

## 3. (a) The exact PM4 sequence, packet by packet

Entry point: `amdgpu_test_dispatch_memset()`, `shader_test_util.c:566-680`, reached from
`amdgpu_compute_dispatch_test` -> `amdgpu_test_dispatch_helper(device_handle, AMDGPU_HW_IP_COMPUTE)`
(`basic_tests.c:2481-2484`, `shader_test_util.c:853-856`, `shader_test_util.c:821-824`).

### 3.1 Packet header encoding

`shader_test_util.c:21-25`:

```c
#define PACKET_TYPE3    3
#define PACKET3(op, n)  ((PACKET_TYPE3 << 30) | (((op) & 0xFF) << 8) | ((n) & 0x3FFF) << 16)
#define PACKET3_COMPUTE(op, n) PACKET3(op, n) | (1 << 1)
```

`COUNT = n` means the packet is `n + 2` dwords total (header + n+1 body dwords).

**Bit 1 of the type-3 header is the shader-type bit.** `PACKET3_COMPUTE` sets it
unconditionally, for both the compute ring and the gfx ring. It tells the CP that the following
`SET_SH_REG` writes target the *compute* SH register space rather than the graphics one. This is
not optional. The kernel's own `nvd.h` does not define this macro, so we have to carry it
ourselves.

Opcodes used (`shader_test_util.c:12-19`, all matching
`P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\amdgpu\nvd.h`):

| Name | Value | Kernel citation |
| --- | --- | --- |
| `PACKET3_DISPATCH_DIRECT` | `0x15` | `nvd.h:61` |
| `PACKET3_SET_SH_REG` | `0x76` | `nvd.h:525` |
| `PACKET3_SET_SH_REG_INDEX` | `0x9B` | `nvd.h:576` (defined but never used by the kernel) |
| `PACKET3_SET_UCONFIG_REG` | `0x79` | uconfig START at `nvd.h:536` |
| `PACKET3_NOP` | `0x10` | `nvd.h:55` |
| `PACKET3_SET_SH_REG_START` | `0x00002c00` | `nvd.h:526` |
| `PACKET3_SET_UCONFIG_REG_START` | `0x0000c000` | `nvd.h:536` |

### 3.2 Register offset arithmetic (verified for Cyan Skillfish)

libdrm hardcodes the SH offsets. To confirm they are right for GC 10.1.3:

```
SH packet offset = (GC_BASE__INST0_SEG<n> + mmREG) - PACKET3_SET_SH_REG_START
```

- `GC_BASE__INST0_SEG0 = 0x00001260`, `GC_BASE__INST0_SEG1 = 0x0000A000`
  (`P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\include\cyan_skillfish_ip_offset.h:315-316`)
- all `mmCOMPUTE_*` have `BASE_IDX 0`
  (e.g. `mmCOMPUTE_PGM_LO_BASE_IDX 0`, `mmCOMPUTE_DISPATCH_INITIATOR_BASE_IDX 0`, in
  `P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\include\asic_reg\gc\gc_10_1_0_offset.h`)
- `mmCP_COHER_START_DELAY = 0x207b` with `BASE_IDX 1` (same header), so
  `0xA000 + 0x207b - 0xc000 = 0x7b`, matching libdrm.

Resulting map (all from `gc_10_1_0_offset.h`, `0x1260 + mmREG - 0x2c00`):

| Register | `mmREG` | SH packet offset |
| --- | --- | --- |
| `COMPUTE_DISPATCH_INITIATOR` | `0x1ba0` | `0x200` |
| `COMPUTE_START_X/Y/Z` | `0x1ba4/5/6` | `0x204/5/6` |
| `COMPUTE_NUM_THREAD_X/Y/Z` | `0x1ba7/8/9` | `0x207/8/9` |
| `COMPUTE_PGM_LO/HI` | `0x1bac/ad` | `0x20c/0x20d` |
| `COMPUTE_PGM_RSRC1/RSRC2` | `0x1bb2/b3` | `0x212/0x213` |
| `COMPUTE_RESOURCE_LIMITS` | `0x1bb5` | `0x215` |
| `COMPUTE_STATIC_THREAD_MGMT_SE0/SE1` | `0x1bb6/b7` | `0x216/0x217` |
| `COMPUTE_TMPRING_SIZE` | `0x1bb8` | `0x218` |
| `COMPUTE_STATIC_THREAD_MGMT_SE2/SE3` | `0x1bb9/ba` | `0x219/0x21a` |
| `COMPUTE_REQ_CTRL` | `0x1bc2` | `0x222` |
| `COMPUTE_USER_ACCUM_0..3` | `0x1bc4..c7` | `0x224..0x227` |
| `COMPUTE_PGM_RSRC3` | `0x1bc8` | `0x228` |
| `COMPUTE_SHADER_CHKSUM` | `0x1bca` | `0x22a` |
| `COMPUTE_USER_DATA_0..15` | `0x1be0..ef` | `0x240..0x24f` |

Every offset libdrm uses checks out.

### 3.3 The full sequence (compute ring, gfx10, CS_BUFFERCLEAR)

Command buffer is `memset` to 0 first (`shader_test_util.c:595`), which matters: several packets
advance the write pointer without writing (`i += 3`, `i += 6`) and rely on the zero fill.

`CONTEXT_CONTROL is skipped on compute`: `write_context_control()` at `shader_test_util.c:130-142`
emits its 3 dwords **only** when `ip == AMDGPU_HW_IP_GFX`. On a compute ring the function is a
no-op. There is no `CLEAR_STATE` anywhere on this path.

| # | Emitted by | Packet | Body | Dw | Cum |
| --- | --- | --- | --- | --- | --- |
| 1 | `:218-220` | `SET_SH_REG(count=3)` +compute | off `0x204`; `COMPUTE_START_X/Y/Z = 0,0,0` | 5 | 5 |
| 2 | `:223-225` | `SET_SH_REG(count=1)` +compute | off `0x218`; `COMPUTE_TMPRING_SIZE = 0` | 3 | 8 |
| 3 | `:240-242` | `SET_SH_REG(count=1)` +compute | off `0x22a`; `COMPUTE_SHADER_CHKSUM = 0` | 3 | 11 |
| 4 | `:244-246` | `SET_SH_REG(count=6)` +compute | off `0x222`; six zeros: `COMPUTE_REQ_CTRL`, `0x223` (reserved), `COMPUTE_USER_ACCUM_0..3` | 8 | 19 |
| 5 | `:248-250` | `SET_UCONFIG_REG(count=1)` **no compute bit** | off `0x7b`; `mmCP_COHER_START_DELAY = 0x20` | 3 | 22 |
| 6 | `:331-334` | `SET_SH_REG_INDEX(count=2)` +compute | off `0x30000216`; `STATIC_THREAD_MGMT_SE0 = 0xffffffff`, `SE1 = 0xffffffff` | 4 | 26 |
| 7 | `:336-339` | `SET_SH_REG_INDEX(count=2)` +compute | off `0x30000219`; `STATIC_THREAD_MGMT_SE2 = 0xffffffff`, `SE3 = 0xffffffff` | 4 | 30 |
| 8 | `:413-416` | `SET_SH_REG(count=2)` +compute | off `0x20c`; **`COMPUTE_PGM_LO = shader_mc_addr >> 8`**, **`COMPUTE_PGM_HI = shader_mc_addr >> 40`** | 4 | 34 |
| 9 | `:418-423` loop j=0 | `SET_SH_REG(count=1)` +compute | off `0x212`; `COMPUTE_PGM_RSRC1 = 0x000C0041` | 3 | 37 |
| 10 | loop j=1 | `SET_SH_REG(count=1)` +compute | off `0x213`; `COMPUTE_PGM_RSRC2 = 0x00000090` | 3 | 40 |
| 11 | loop j=2 | `SET_SH_REG(count=1)` +compute | off `0x207`; `COMPUTE_NUM_THREAD_X = 0x00000040` | 3 | 43 |
| 12 | loop j=3 | `SET_SH_REG(count=1)` +compute | off `0x208`; `COMPUTE_NUM_THREAD_Y = 0x00000001` | 3 | 46 |
| 13 | loop j=4 | `SET_SH_REG(count=1)` +compute | off `0x209`; `COMPUTE_NUM_THREAD_Z = 0x00000001` | 3 | 49 |
| 14 | `:426-428` | `SET_SH_REG(count=1)` +compute | off `0x228`; `COMPUTE_PGM_RSRC3 = 0` | 3 | 52 |
| 15 | `:431-436` | `SET_SH_REG(count=4)` +compute | off `0x240`; **`USER_DATA_0 = dst_mc_addr[31:0]`**, **`USER_DATA_1 = (dst_mc_addr >> 32) | 0x100000`**, `USER_DATA_2 = dst_size / 16`, `USER_DATA_3 = 0x1104BFAC` | 6 | 58 |
| 16 | `:439-444` | `SET_SH_REG(count=4)` +compute | off `0x244`; **`USER_DATA_4..7 = 0x22222222` x4** (the fill constant) | 6 | 64 |
| 17 | `:553-555` | `SET_SH_REG(count=1)` +compute | off `0x215`; `COMPUTE_RESOURCE_LIMITS = 0` | 3 | 67 |
| 18 | `:558-562` | `DISPATCH_DIRECT(count=3)` +compute | `DIM_X = 16`, `DIM_Y = 1`, `DIM_Z = 1`, **`DISPATCH_INITIATOR = 0x00000001`** | 5 | 72 |

`shader_test_util.c` line numbers above are within `amdgpu_dispatch_init_gfx9` (`:206-228`),
`amdgpu_dispatch_init_gfx10` (`:230-253`), `amdgpu_dispatch_write_cumask` (`:309-344`),
`amdgpu_dispatch_write2hw_gfx10` (`:403-463`) and `amdgpu_dispatch_write_dispatch_cmd`
(`:547-565`), called in that order from `amdgpu_test_dispatch_memset` at `:609-617`.

**Totals: 18 packets, exactly 72 dwords.** The trailing NOP pad at `shader_test_util.c:619-622`
(`while (i & 7) ptr_cmd[i++] = 0xffff1000;`) emits **nothing** here, because 72 is already a
multiple of 8. `0xffff1000` is a type-3 NOP with COUNT `0x3FFF`, the same value the kernel uses as
`gfx_v10_0_ring_funcs_compute.nop`
(`P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\amdgpu\gfx_v10_0.c:9890`).

### 3.4 Where each address and constant goes

| Thing | Packet | Encoding |
| --- | --- | --- |
| Shader program address | #8, `COMPUTE_PGM_LO/HI`, SH off `0x20c` | **`addr >> 8` split into lo = `addr >> 8`, hi = `addr >> 40`.** Source `shader_test_util.c:415-416`, `shader_addr = test_priv->shader_dispatch.cs_bo.mc_address` (`:409`). **Shader must be 256-byte aligned.** Hardware field widths confirm: `COMPUTE_PGM_LO__DATA_MASK 0xFFFFFFFF` (`gc_10_1_0_sh_mask.h:17125`), `COMPUTE_PGM_HI__DATA_MASK 0x000000FF` (`:17128`) -> 40 bits of `addr>>8` = a 48-bit byte address, matching `adev->gmc.mc_mask = 0xffffffffffffULL; /* 48 bit MC */` at `gmc_v10_0.c:864`. |
| Destination buffer address | #15, `COMPUTE_USER_DATA_0/1`, SH off `0x240` | **Raw, unshifted 64-bit MC address**, split lo/hi, with `0x100000` OR-ed into the high dword. `shader_test_util.c:433-434`. |
| Destination size | #15, `COMPUTE_USER_DATA_2` | `dst.size / 16` = number of 16-byte records. For `dst.size = 0x4000` that is `1024`. `shader_test_util.c:435`. |
| V# control word | #15, `COMPUTE_USER_DATA_3` | `0x1104BFAC` for gfx10 (`shader_test_util.c:436`). gfx9 uses `0x74FAC` (`:374`), gfx11 uses `0x1003DFAC` (`:503`). |
| Fill constant | #16, `COMPUTE_USER_DATA_4..7`, SH off `0x244` | `0x22222222` repeated four times. `shader_test_util.c:441-444`. Verified afterwards against `memset(cptr, 0x22, 16)` at `:662-667`. |
| Workgroup count | #18, `DISPATCH_DIRECT` dword 1 | `(dst.size / 16 + 0x40 - 1) / 0x40`. `shader_test_util.c:559`. For `0x4000` bytes: `(1024 + 63) / 64 = 16`. |
| Threads per group | #11..13, `COMPUTE_NUM_THREAD_X/Y/Z` | `64, 1, 1`. `shader_code_gfx9.h:37-39`. 16 groups x 64 threads x 16 bytes = `0x4000` bytes. |
| Dispatch initiator | #18, `DISPATCH_DIRECT` dword 4 | **`0x00000001`** = `COMPUTE_SHADER_EN` only. `shader_test_util.c:562`. Field positions at `gc_10_1_0_sh_mask.h:17058-17070` (`COMPUTE_SHADER_EN` bit 0 ... `CS_W32_EN` bit 0x0f). `CS_W32_EN = 0`, so this shader runs **wave64**. |

### 3.5 Decoding the two magic constants

Neither needs to be understood to copy it, but for review confidence:

**`COMPUTE_USER_DATA_1 |= 0x100000`** sets bit 20 of the second V# dword. In the gfx10 buffer
resource layout that dword is `BASE_ADDRESS_HI[15:0] | STRIDE[29:16] | ...`, so bit 20 is
`STRIDE = 0x10` = **16 bytes per record**, matching `USER_DATA_2 = size / 16` and the
`BUFFER_STORE_FORMAT_XYZW` (4 dwords) in the shader.

**`COMPUTE_USER_DATA_3 = 0x1104BFAC`** decodes as
`DST_SEL_X/Y/Z/W = X,Y,Z,W` (bits 2:0, 5:3, 8:6, 11:9 = 4,5,6,7),
`FORMAT[18:12] = 0x4B` (a 32_32_32_32 UINT buffer format),
`RESOURCE_LEVEL = 1` (bit 24, required on gfx10.1),
`OOB_SELECT = 1` (bits 29:28, structured out-of-bounds check against NUM_RECORDS),
`TYPE = 0` (bits 31:30, buffer).
Cross-check: the gfx9 value `0x00074FAC` decodes with the gfx9 field layout to the same
selects plus `NUM_FORMAT = 4` (UINT) and `DATA_FORMAT = 0xE` (32_32_32_32). Consistent.

**`COMPUTE_PGM_RSRC1 = 0x000C0041`**: `VGPRS = 1` (8 VGPRs), `SGPRS = 1` (16 SGPRs),
`FLOAT_MODE = 0xC0`, `PRIV = 0`, `DX10_CLAMP = 0`, `IEEE_MODE = 0`,
**`WGP_MODE = 0` (bit 29, CU mode), `MEM_ORDERED = 0` (bit 30), `FWD_PROGRESS = 0` (bit 31)**.
Field positions `gc_10_1_0_sh_mask.h:17142-17153`
(`COMPUTE_PGM_RSRC1__WGP_MODE__SHIFT 0x1d` at `:17151`).

**`COMPUTE_PGM_RSRC2 = 0x00000090`**: `SCRATCH_EN = 0`, **`USER_SGPR = 8`** (bits 5:1),
`TRAP_PRESENT = 0`, **`TGID_X_EN = 1`** (bit 7), `TGID_Y/Z_EN = 0`, `TG_SIZE_EN = 0`,
`TIDIG_COMP_CNT = 0`, `LDS_SIZE = 0`. Field positions `gc_10_1_0_sh_mask.h:17167-17177`
(`COMPUTE_PGM_RSRC2__USER_SGPR__SHIFT 0x1` at `:17168`).
`SCRATCH_EN = 0` is important for us: **no scratch/private aperture is needed**, which removes
the `SH_MEM_BASES` problem described in section 5.

### 3.6 `SET_SH_REG_INDEX` and the `0x30000216` offset dword

`shader_test_util.c:331-339`. The offset dword is `index[31:28] | reg_offset[15:0]`, so
`0x30000216` = index **3**, offset `0x216`. Index 3 on gfx10 tells the CP to route the
`COMPUTE_STATIC_THREAD_MGMT_SE*` write through the CU-mask path so it composes correctly with the
queue's CU reservation, rather than being a blind register write. gfx9 (`:318-326`) uses a plain
`SET_SH_REG` for the same registers.

The kernel defines the opcode (`nvd.h:576`) but never emits it, so there is no in-tree
documentation of the index semantics. Treat the value `3` as copied verbatim from libdrm.
**This is not strictly required for a first dispatch** since the MQD already sets all four
`compute_static_thread_mgmt_se*` to `0xffffffff` (see section 5), but emitting it matches the
known-good reference.

---

## 4. Alternative reference: the minimal flat-address dispatch

`basic_tests.c:2275-2470` (`amdgpu_sync_dependency_test`) contains a second, much simpler
dispatch that is worth having as a fallback because it does **not** use a buffer descriptor at
all. It writes a flat 64-bit address into `COMPUTE_USER_DATA_0/1` and the shader does a flat
store.

- Shader: `basic_tests.c:332-337`, 15 dwords, `SWAP_32`-wrapped. Its C source is quoted at
  `basic_tests.c:310-330`.
- `COMPUTE_PGM_LO/HI`: `basic_tests.c:2321-2324`, same `>> 8` / `>> 40` shift.
- `COMPUTE_PGM_RSRC1 = 0x002c0040`, `COMPUTE_PGM_RSRC2 = 0x00000010`, fully commented in the
  source at `basic_tests.c:2329-2359`. Note `USER_SGPR = 8`, `TGID_X_EN = 0`.
- `COMPUTE_TMPRING_SIZE = 0x00000100` (`:2367-2369`) - this one *does* set up a scratch ring.
- **`COMPUTE_USER_DATA_0/1` = the raw destination address, unshifted**
  (`basic_tests.c:2371-2374`).
- `COMPUTE_RESOURCE_LIMITS = 0` (`:2376-2378`), `COMPUTE_NUM_THREAD_X/Y/Z = 1,1,1` (`:2380-2384`).
- `DISPATCH_DIRECT 1,1,1` with **`DISPATCH_INITIATOR = 0x00000045`** (`:2388-2392`).
  `0x45` = bits 0, 2, 6 = `COMPUTE_SHADER_EN | FORCE_START_AT_000 | ORDER_MODE`.
- Result checked at `basic_tests.c:2464` (`ptr[DATA_OFFSET] == 99`).

Caveats: this one is submitted on **`AMDGPU_HW_IP_GFX`** (`basic_tests.c:2403`), uses plain
`PACKET3` headers without the compute shader-type bit, and prefixes `CONTEXT_CONTROL` +
`CLEAR_STATE` (`:2312-2317`) which only make sense on the gfx ring. Do not copy those two packets
onto a compute queue.

---

## 5. (c) + (d) What we must supply ourselves, in a kernel-only VMID 0 setup

### 5.1 IB versus direct ring submission

libdrm builds the 72 dwords into a **GTT command BO** and submits it as an IB
(`shader_test_util.c:590-594` allocates `cmd` in `AMDGPU_GEM_DOMAIN_GTT`;
`:630-640` sets `ib_info.ib_mc_address = cmd->mc_address` and calls `amdgpu_cs_submit`).

We do not have to. **The 72 dwords are ordinary PM4 and may be written directly into the compute
ring.** Nothing in the sequence is IB-only. Two things to preserve either way:

1. The ring's own `align_mask = 0xff` (`gfx_v10_0.c:9889`) applies to IBs, padded by
   `amdgpu_ring_generic_pad_ib` (`amdgpu_ring.c:160-164`) to a 256-dword multiple. If we submit
   directly to the ring, that constraint does not apply; the ring write pointer just has to land
   where the MQD expects.
2. If we do go the IB route, the kernel's compute IB packet is
   `gfx_v10_0_ring_emit_ib_compute()`, `gfx_v10_0.c:8677-8710`: a 4-dword
   `PACKET3(PACKET3_INDIRECT_BUFFER, 2)` with lo/hi address (`:8703-8708`) and a control dword
   built at `:8683` as `INDIRECT_BUFFER_VALID | ib->length_dw | (vmid << 24)`. Constants in
   `nvd.h:224-242`. `BUG_ON(ib->gpu_addr & 0x3)` at `:8702` - IB base must be dword aligned.
   The optional 3-dword GDS preamble at `:8695-8699` is only for
   `AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID` and is not needed.

**Recommendation: write the packets directly into the compute ring for the first attempt.** It
removes the IB address, the IB pool and the pad-to-256 rule from the list of things that can be
wrong, and leaves only the queue and the dispatch itself.

### 5.2 VMID 0 is what the kernel already uses, and shader fetch works there

- A kernel ring IB with no `amdgpu_vm` runs in **VMID 0**:
  `AMDGPU_JOB_GET_VMID(job)` is `((job) ? (job)->vmid : 0)`
  (`P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\amdgpu\amdgpu_job.h:42`), and
  `amdgpu_ib.c:139` reads `int vmid = AMDGPU_JOB_GET_VMID(job);` with `job == NULL` on the ring
  test path. `amdgpu_ib.c:221-227` skips `amdgpu_vm_flush()` entirely when `job == NULL`.
- The KIQ `MAP_QUEUES` packet hardcodes `VMID(0)` for compute queues:
  `gfx10_kiq_map_queues()`, `gfx_v10_0.c:3726-3764`, `VMID(0)` at `:3751`. The MQD also forces
  `cp_mqd_control` VMID to 0 (`gfx_v10_0.c:6961-6963`) and `mqd->cp_hqd_vmid = 0`
  (`gfx_v10_0.c:7001`). **So a KIQ-mapped kernel compute queue is a VMID 0 queue by construction.**
- VMID 0 = the GART aperture. `gfxhub_v2_0_init_gart_aperture_regs()`,
  `P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\amdgpu\gfxhub_v2_0.c:134-149`, programs GCVM
  context 0 from `adev->gmc.gart_start` / `gart_end` directly, and
  `gfxhub_v2_0_enable_system_domain()` (`:254-264`) gives context 0
  **`PAGE_TABLE_DEPTH = 0`**, i.e. a flat single-level GART page table. Contexts 1..15 get
  `PAGE_TABLE_DEPTH = adev->vm_manager.num_level` (`:283-330`).
- Cyan Skillfish uses **gfxhub v2_0**, not v2_1: it falls through the `default:` arm of
  `gmc_v10_0_set_gfxhub_funcs()`, `gmc_v10_0.c:608-625`.
- **Instruction fetch goes through the same VMID/UTCL2 path as data.** The GART PTEs carry an
  execute bit: `gmc_v10_0_gart_init()`, `gmc_v10_0.c:761-762`,
  `adev->gart.gart_pte_flags = AMDGPU_PTE_MTYPE_NV10(0ULL, MTYPE_UC) | AMDGPU_PTE_EXECUTABLE;`
  and `gfxhub_v2_0_setup_vmid_config()` arms `EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT` on the
  same GCVM contexts (`gfxhub_v2_0.c:306-307`). Corroborating evidence: the gfx9 EDC workaround
  places the shader *inside the IB buffer itself* and points `COMPUTE_PGM_LO` at
  `ib.gpu_addr + vgpr_offset` (`gfx_v9_0.c:4678-4701`) - one address space, one VMID, no separate
  shader aperture register.
- **GART base is not a fixed constant.** `amdgpu_gmc_gart_location()`,
  `amdgpu_gmc.c:293-332`, is called with `AMDGPU_GART_PLACEMENT_BEST_FIT` from
  `gmc_v10_0_vram_gtt_location()` (`gmc_v10_0.c:665-687`); best fit picks either 0 (below VRAM)
  or `max_mc_address - gart_size + 1` (above VRAM), then aligns down to 4 GB
  (`amdgpu_gmc.c:328-329`). GART size for 10.1.3 defaults to **512 MB**
  (`gmc_v10_0.c:725-736`, the `default:` arm). Our driver should read back
  `GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32/HI32` rather than assume.

### 5.3 `CONTEXT_CONTROL` / `CLEAR_STATE`: not needed, confirmed both ways

- libdrm: `write_context_control()` is a no-op for compute (`shader_test_util.c:135-139`).
- Kernel: `gfx_v10_0_ring_emit_cntxcntl()` (`gfx_v10_0.c:8813-8845`) is attached **only** to
  `gfx_v10_0_ring_funcs_gfx` at `gfx_v10_0.c:9873`. `gfx_v10_0_ring_funcs_compute`
  (`:9887-9925`) has no `.emit_cntxcntl`, no `.emit_switch_buffer`, no `.init_cond_exec`, no
  `.emit_frame_cntl`, no `.emit_gfx_shadow`. And `amdgpu_ib.c:244` guards the call with
  `if (job && ring->funcs->emit_cntxcntl)`, so it can never fire on a compute ring.
- `CLEAR_STATE` appears only in `gfx_v10_0_cp_gfx_start()` (`gfx_v10_0.c:6362-6439`), which
  operates on `adev->gfx.gfx_ring[0]`.

**Nothing to do here.**

### 5.4 What the MQD already gives us, and what we must still emit

`gfx_v10_0_compute_mqd_init()`, `gfx_v10_0.c:6905-7019`, sets **only these** compute state fields
(verified by grep: the only `mqd->compute_` matches in the file are lines 6913-6918):

```c
mqd->compute_pipelinestat_enable    = 0x00000001;   // :6913
mqd->compute_static_thread_mgmt_se0 = 0xffffffff;   // :6914
mqd->compute_static_thread_mgmt_se1 = 0xffffffff;   // :6915
mqd->compute_static_thread_mgmt_se2 = 0xffffffff;   // :6916
mqd->compute_static_thread_mgmt_se3 = 0xffffffff;   // :6917
mqd->compute_misc_reserved          = 0x00000003;   // :6918
```

Everything else in the MQD is `cp_hqd_*` / `cp_mqd_*` queue plumbing. **`gfx_v10_0.c` writes no
`mmCOMPUTE_*` register at all** - the only `COMPUTE`-named tokens in the whole file are
`mmGDS_COMPUTE_MAX_WAVE_ID` (`:8697`) and `mmSPI_COMPUTE_QUEUE_RESET` (`:3836`).

So the command stream is responsible for **all** of:
`COMPUTE_PGM_LO/HI`, `COMPUTE_PGM_RSRC1`, `COMPUTE_PGM_RSRC2`, `COMPUTE_PGM_RSRC3`,
`COMPUTE_NUM_THREAD_X/Y/Z`, `COMPUTE_START_X/Y/Z`, `COMPUTE_TMPRING_SIZE`,
`COMPUTE_RESOURCE_LIMITS`, `COMPUTE_USER_DATA_*`, `COMPUTE_REQ_CTRL`, `COMPUTE_USER_ACCUM_0..3`,
`COMPUTE_SHADER_CHKSUM`, and `COMPUTE_DISPATCH_INITIATOR` (which is the last dword of
`DISPATCH_DIRECT`, not a separate `SET_SH_REG`). Which is exactly what the libdrm sequence does.

MQD field names, `P:\BC-250\ref\linux-src\drivers\gpu\drm\amd\include\v10_structs.h`,
`struct v10_compute_mqd` starts at `:675`:

| Register | MQD field | Line |
| --- | --- | --- |
| `COMPUTE_DISPATCH_INITIATOR` | `compute_dispatch_initiator` | `:677` |
| `COMPUTE_NUM_THREAD_X/Y/Z` | `compute_num_thread_x/y/z` | `:684-686` |
| `COMPUTE_PGM_LO/HI` | `compute_pgm_lo` / `compute_pgm_hi` | `:689-690` |
| `COMPUTE_PGM_RSRC1` | `compute_pgm_rsrc1` | `:695` |
| `COMPUTE_PGM_RSRC2` | `compute_pgm_rsrc2` | `:696` |
| `COMPUTE_RESOURCE_LIMITS` | `compute_resource_limits` | `:698` |
| `COMPUTE_STATIC_THREAD_MGMT_SE0/SE1` | `compute_static_thread_mgmt_se0/se1` | `:699-700` |
| `COMPUTE_STATIC_THREAD_MGMT_SE2/SE3` | `compute_static_thread_mgmt_se2/se3` | `:702-703` |
| `COMPUTE_USER_DATA_0..15` | `compute_user_data_0..15` | `:741-756` |

There is **no `v10_3_compute_mqd` in this tree**; `gfx_v10_0.c:4974` uses
`sizeof(struct v10_compute_mqd)` for all gfx10 variants including 10.3.

### 5.5 `SH_MEM_BASES` and the VMID 0 quirk

`gfx_v10_0_constants_init()`, `gfx_v10_0.c:5338-5374`, writes `mmSH_MEM_CONFIG` for every VMID
but **deliberately skips `mmSH_MEM_BASES` for VMID 0** (`if (i != 0)` at `:5359`). In VMID 0 the
LDS / scratch / private aperture bases stay zero and every shader address is a flat GPUVM/GART
address.

For our shader this is fine, because `COMPUTE_PGM_RSRC2.SCRATCH_EN = 0` and
`COMPUTE_TMPRING_SIZE = 0`. **Do not copy the `basic_tests.c` variant's
`COMPUTE_TMPRING_SIZE = 0x100` into a VMID 0 dispatch** without also setting up
`COMPUTE_DISPATCH_SCRATCH_BASE_LO/HI` (`mm 0x1bb0/0x1bb1`), which nothing in either reference does.

`DEFAULT_SH_MEM_CONFIG` is at `gfx_v10_0.c:3668-3672`:
`SH_MEM_ADDRESS_MODE_64` + `SH_MEM_ALIGNMENT_MODE_UNALIGNED` + `INITIAL_INST_PREFETCH = 3`.

### 5.6 Cache flush and fence

libdrm relies on the kernel to wrap the IB. Doing this ourselves, two pieces matter:

- **Before the dispatch**: the kernel emits `gfx_v10_0_emit_mem_sync()`
  (`gfx_v10_0.c:9473-9494`) automatically for every IB, because `amdgpu_ib_get()` sets
  `AMDGPU_IB_FLAG_EMIT_MEM_SYNC` (`amdgpu_ib.c:80`) and `amdgpu_ib.c:211-212` acts on it. It is
  `PACKET3(PACKET3_ACQUIRE_MEM, 6)` with `COHER_SIZE = 0xffffffff`, `COHER_SIZE_HI = 0xffffff`,
  `POLL_INTERVAL = 0xA` and a `GCR_CNTL` of
  `GL2_INV | GL2_WB | GLM_INV | GLM_WB | GL1_INV | GLV_INV | GLK_INV | GLI_INV`. **We need this,
  8 dwords**, otherwise the shader may fetch stale instruction bytes through GL1/GL2.
- **After the dispatch**: the memset test relies on the CS fence. On a raw ring we need either
  `gfx_v10_0_ring_emit_fence()` (`gfx_v10_0.c:8712-8743`, a `RELEASE_MEM`, 8 dwords) or, at
  minimum, a `PACKET3_EVENT_WRITE` CS partial flush (`nvd.h:326`) followed by a
  `WRITE_DATA`/`RELEASE_MEM` marker we can poll, plus another `ACQUIRE_MEM` so GL2 is written
  back before the CPU reads the destination. `gfx_v10_0_ring_emit_vm_flush()`
  (`gfx_v10_0.c:8767-8778`) explicitly notes "compute doesn't have PFP" and skips
  `PACKET3_PFP_SYNC_ME` on non-gfx rings, so do not emit that.

A workable ring-level frame is therefore roughly:
`ACQUIRE_MEM (8)` + the 72 dispatch dwords + `EVENT_WRITE CS_PARTIAL_FLUSH (2)` +
`RELEASE_MEM fence (8)` + doorbell write.

### 5.7 Destination memory: VRAM in the test, GART for us

The libdrm test puts the destination in **VRAM** (`shader_test_util.c:605`,
`dst->heap = AMDGPU_GEM_DOMAIN_VRAM`) and the shader in **VRAM** (`:598`), with only the command
buffer in GTT (`:591`).

For our first BC-250 attempt we want GART-mapped system memory, which is what
`gfx_v10_0_ring_test_ib()` already uses: `amdgpu_device_wb_get()` +
`gpu_addr = adev->wb.gpu_addr + (index * 4)` (`gfx_v10_0.c:4083-4089`), where the writeback BO is
`amdgpu_bo_create_kernel(..., AMDGPU_GEM_DOMAIN_GTT, ...)` (`amdgpu_device.c:1598-1601`). That is
a GART MC address in `[gart_start, gart_end]`. Both shader and destination can live there.

Two constraints when we relocate the shader from VRAM to GART:

1. **256-byte alignment** for the shader, because of the `>> 8`. The kernel's IB suballocator
   already uses 256-byte alignment (`amdgpu_ib.c:345-347`), which is a convenient precedent.
2. GART PTEs on gfx10 are **`MTYPE_UC`** (`gmc_v10_0.c:761-762`). Uncached is fine, and actually
   helpful for a first bring-up: it reduces the number of ways a missing cache flush can hide the
   result.

### 5.8 Compute queue counts for GC 10.1.3

`gfx_v10_0_sw_init`, `gfx_v10_0.c:4761-4773`, puts 10.1.3 in the same arm as NV1x:
`mec.num_mec = 2`, `mec.num_pipe_per_mec = 4`, `mec.num_queue_per_pipe = 8` (64 queues of HW
surface). amdgpu enables 8 of them: `num_compute_rings = min(amdgpu_gfx_get_num_kcq(adev),
AMDGPU_MAX_COMPUTE_RINGS)` at `gfx_v10_0.c:7817-7818`, with
`amdgpu_gfx_get_num_kcq()` returning 8 by default (`amdgpu_gfx.c:1197-1206`). The multipipe
policy (`amdgpu_gfx_compute_queue_acquire()`, `amdgpu_gfx.c:203-237`) spreads those 8 as
**MEC1 pipes 0-3, queues 0-1**. This matches the eight compute queues we already have.

Doorbells: the only `mmCP_MEC_DOORBELL_RANGE_*` writes are in `gfx_v10_0_kiq_init_register()`
(`gfx_v10_0.c:7097-7103`), plus `CP_PQ_STATUS.DOORBELL_ENABLE = 1` at `:7124-7125`. KCQ doorbells
are never written by MMIO; they ride in the KIQ `MAP_QUEUES` packet
(`gfx_v10_0.c:3726-3764`, `DOORBELL_OFFSET(ring->doorbell_index)`).

---

## 6. (e) License status and attribution

| File | License header | Copyright |
| --- | --- | --- |
| `tests/amdgpu/shader_code_gfx10.h` | **MIT, lines 1-22** | Copyright 2022 Advanced Micro Devices, Inc. |
| `tests/amdgpu/shader_code_gfx9.h` | **MIT, lines 1-22** | Copyright 2022 Advanced Micro Devices, Inc. |
| `tests/amdgpu/shader_code.h` | **MIT, lines 1-22** | Copyright 2022 Advanced Micro Devices, Inc. |
| `tests/amdgpu/basic_tests.c` | **MIT, lines 1-23** | Copyright 2014 Advanced Micro Devices, Inc. |
| `tests/amdgpu/meson.build` | MIT, lines 1-19 | Copyright 2017-2018 Intel Corporation |
| **`tests/amdgpu/shader_test_util.c`** | **NO header. File starts at `#include <stdio.h>` on line 1.** | not stated in-file |

This is the one thing to flag to legal-minded reviewers. The files we take *content* from
(the 9 shader dwords and the 5-entry register table) both carry a clean MIT header. The file we
take the *packet sequence* from carries none; libdrm as a project is MIT, and this file was added
by AMD in the same 2022 series as `shader_code*.h`, so the intent is clearly MIT, but it is not
stated in the file. The repo root has no `COPYING` or `LICENSE` file at this tag, only
`README.rst`.

Practical attribution, assuming we copy the shader words and the register/packet layout:

- Add an MIT notice block to whichever BC-250 source file carries the transcription, reproducing
  the full 1-22 header text from `shader_code_gfx10.h` verbatim, since MIT requires the copyright
  notice and the permission notice to be included.
- Add a provenance comment naming the repo, tag `libdrm-2.4.114`, commit
  `b9ca37b3134861048986b75896c0915cbf2e97f9`, and the file:line ranges.
- Record in the repo's third-party notices that the PM4 sequence derives from
  `tests/amdgpu/shader_test_util.c` of the same tree, noting the missing per-file header and
  treating it as MIT under the project terms.
- Check this against the project's PolyForm NC license: MIT is permissive and inbound-compatible,
  so including MIT-licensed material in a PolyForm NC distribution is fine as long as the MIT
  notice travels with it. Worth one explicit confirmation from D-Ogi rather than assuming.

---

## 7. (f) Open questions and risks

### Top three

1. **The shader binary is Navi10-compiled and we cannot re-verify the ISA encoding from any
   source in `ref/`.** The decode in section 2 is my own bit-field arithmetic against the GFX10.1
   encodings, cross-checked against the structurally identical gfx9 shader. The mesa checkout at
   `P:\BC-250\ref\mesa` does **not** contain the generated `sid.h` register tables or
   `amdgfxregs.h`, so `V_008F0C_*` and the `IMG_FORMAT_*` enum could not be cited from a file.
   Risk: if `FORMAT = 0x4B` is not 32_32_32_32 UINT on 10.1.3, the `BUFFER_STORE_FORMAT_XYZW`
   writes the wrong width or nothing.
   *Mitigation*: we copy `0x1104BFAC` verbatim, so a decode error costs nothing at runtime.
   If the dispatch silently writes nothing, fall back to the `basic_tests.c:332-337` flat-store
   shader, which needs no descriptor at all.
   *Next step to close it*: build LLVM's `llvm-mc -arch=amdgcn -mcpu=gfx1013 -disassemble` over
   the 9 words, or fetch mesa's generated `amdgfxregs.h`.

2. **`COMPUTE_PGM_RSRC1` has `WGP_MODE = 0` and `MEM_ORDERED = 0`.** On GFX10.1 both are legal,
   but if anything else in our bring-up (golden settings, RLC, a CU mask we program) puts the SQ
   into WGP mode, a CU-mode shader launch can hang the pipe rather than fault. Cyan Skillfish
   has its own 40-entry golden settings table (`gfx_v10_0.c:3581-3617`, applied at `:3978-3983`)
   including `mmSQ_LDS_CLK_CTRL` and `mmGRBM_GFX_INDEX`, and its `GB_ADDR_CONFIG` is
   **hardcoded** rather than read back (`gfx_v10_0.c:3675`, `:4612-4620`, with a "TODO: pending on
   golden setting value" comment at `:3674`). That hardcoding is a standing bring-up risk on this
   chip generally.
   *Mitigation*: first dispatch should be a single workgroup (`DISPATCH_DIRECT 1,1,1`) with a
   watchdog, not the full 16 groups.

3. **Cache coherence between the dispatch and the CPU read.** The libdrm test gets its
   `ACQUIRE_MEM` for free from `amdgpu_ib_schedule` and its writeback from the `RELEASE_MEM`
   fence. On a raw ring we must emit both ourselves, and on the CPU side we also need the WC/UC
   mapping of the GART buffer handled correctly on Windows. A missing GL2 writeback looks exactly
   like "the dispatch did not run", which will cost debugging time.
   *Mitigation*: seed the destination with a distinct sentinel (the kernel uses `0xCAFEDEAD`,
   `gfx_v10_0.c:4088`) so "unchanged" and "wrong value" are distinguishable, and prove the ring
   itself first with `gfx_v10_0_ring_test_ring()`'s 3-dword `SET_UCONFIG_REG` scratch write
   (`gfx_v10_0.c:4033-4069`) before attempting a dispatch.

### Remaining open items

- **`SET_SH_REG_INDEX` index 3 semantics** are undocumented in both trees (section 3.6). If the
  CP rejects the packet, drop packets #6 and #7 entirely: the MQD already programs all four
  `compute_static_thread_mgmt_se*` to `0xffffffff` (`gfx_v10_0.c:6914-6917`).
- **`mmCP_COHER_START_DELAY = 0x20`** (packet #5) is a uconfig write with no `nvd.h` counterpart
  and no kernel equivalent. It is a performance knob for `ACQUIRE_MEM`, almost certainly optional.
  If it causes trouble on the compute queue, drop it; note it is emitted with a plain `PACKET3`
  header, not `PACKET3_COMPUTE`.
- **`COMPUTE_REQ_CTRL` (packet #4) clears 6 dwords starting at SH `0x222`.** SH `0x223`
  (`mm 0x1bc3`) has no name in `gc_10_1_0_offset.h`. Writing zero to an unnamed register is what
  libdrm does on every gfx10 part, so it is presumably harmless, but it is an unnamed write.
- **`DISPATCH_INITIATOR` difference**: libdrm's gfx10 compute path uses `0x1`,
  `basic_tests.c:2392` uses `0x45`. `0x45` adds `FORCE_START_AT_000` and one more bit. For a
  single-workgroup smoke test `0x45` may actually be the safer value since it forces the start
  index to zero. Worth trying both.
- **Cyan Skillfish 2 (0x13FE) loads MEC firmware via PSP, not direct**
  (`amdgpu_ucode.c:563-568`), and there is no cleaner shader for 10.1.3/10.1.4
  (`gfx_v10_0.c:4798-4814`), so `gfx10_kiq_set_resources()` sends address 0 there
  (`gfx_v10_0.c:3713`). Both already in scope for the PSP work, noted here only so the dispatch
  milestone is not blamed for a firmware-load failure.
- **CG and PG are both disabled for this chip** (`nv.c:871-875`, `cg_flags = 0; pg_flags = 0`),
  which removes a whole class of "the CU was gated" failure modes. Good news, recorded so nobody
  goes looking for it.

---

## 8. Suggested transcription order

1. Ring smoke test first: the 3-dword `SET_UCONFIG_REG` scratch write from
   `gfx_v10_0_ring_test_ring()` (`gfx_v10_0.c:4033-4069`) on the compute ring. Proves the queue,
   the doorbell and the wptr.
2. Then a `WRITE_DATA` to GART from the compute ring, mirroring
   `gfx_v10_0_ring_test_ib()`'s 5-dword IB (`gfx_v10_0.c:4091-4102`) but written straight to the
   ring. Proves GART addressing in VMID 0 and CPU-visible coherence, with no shader involved.
3. Only then the dispatch: `ACQUIRE_MEM` + the 72 dwords + fence, with `DISPATCH_DIRECT 1,1,1`
   and a 256-byte-aligned shader copy in GART.
4. Scale to 16 workgroups and compare the whole 0x4000 buffer, as `shader_test_util.c:659-667`
   does at three offsets (0, size/2, size-16).
