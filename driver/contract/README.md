# driver/contract - the KMD/UMD caps contract

What `bc250kmd` tells a user-mode driver about the GPU, and a host test that checks Mesa agrees.

On Linux, RADV learns what the hardware is by calling the amdgpu `INFO` ioctl a dozen times. On
Windows there is no ioctl. A WDDM user-mode driver calls `D3DKMTQueryAdapterInfo` with
`KMTQAITYPE_UMDRIVERPRIVATE`, dxgkrnl turns that into `DXGKQAITYPE_UMDRIVERPRIVATE` on
`DxgkDdiQueryAdapterInfo`, and whatever bytes the miniport writes arrive verbatim in user mode.
dxgkrnl neither inspects nor versions them. The shape of those bytes is this directory.

Nothing here is wired into the driver yet. It is the contract and its test, built so that the
shape can be argued about before anything depends on it.

## Files

| File | What it is |
|---|---|
| `bc250_umd_private.h` | The blob. Versioned, fixed-width, **version 3, 1472 bytes**, compile-time size and offset asserts. Compiles both as user-mode C and under the miniport's `/kernel` flags |
| `bc250_umd_private_fields.h` | The member names of the three imported UAPI structures, as X-macro lists. Names only |
| `third_party/amdgpu_drm.h` | Linux UAPI, MIT, tag v6.18, imported byte for byte. See `third_party/PROVENANCE.md` |
| `uapi-shim/drm.h` | Ours. The seven names `amdgpu_drm.h` needs from `drm.h`, so it compiles under MSVC unmodified |
| `test/run.ps1` | Builds and runs everything. `pwsh driver\contract\test\run.ps1` |
| `test/bc250_caps_unitA.c` | Unit A's blob, decoded from the raw ioctl words, plus the 32-row cross-check table |
| `test/bc250_caps_mesa.c` | The winsys side: translates the blob and calls Mesa's real `ac_*` functions |
| `test/bc250_caps_stubs.c` | Twelve symbols `ac_gpu_info.o` references but never reaches on our path |
| `test/bc250_caps_test.c` | `main()`: the wishlist, the layout comparison, the cross-checks, the assertions, the controls. Also `--radeon-info`, which dumps the derived caps through Mesa's own `ac_print_gpu_info()` |
| `test/compare_radv_info.py` | Diffs that dump against what RADV derived on unit A. The only check here that compares against the device |

The test needs a Mesa checkout at `P:\BC-250\ref\mesa` (shallow sparse clone; `src/amd/common`,
`src/amd/registers`, `src/amd/addrlib`, `src/util`, `include`). It writes only under
`P:\BC-250\scratch\contract`. No hardware, no lab, no driver load.

## The contract in one paragraph

The blob is the Linux UAPI structures by value, in the kernel's own order, plus the handful of
things the ioctl does not carry (`gb_addr_config`, the firmware versions, the DRM version the
winsys would have read off the device node). It is deliberately not a new information model. The
KMD fills the same fields from the same registers amdgpu reads, so a disagreement with a Linux run
on the same unit is a diff, not an argument; and the winsys can hand `&blob->device` straight to
`ac_identify_chip()`. `magic`, `version` and `size` are the first three words and never move.

## Where the numbers come from

The blob's `DEV_INFO`, `MEMORY`, `HW_IP_INFO` and `FW_VERSION` content is **decoded from the raw
ioctl replies**, not assembled from facts. `test/bc250_caps_unitA.c` holds the little-endian u32
words exactly as
`evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-windows/info.txt` recorded them
(amdgpu 6.18.52, read-only queries), `memcpy`s them into the structures of
`third_party/amdgpu_drm.h`, and zero-pads the trailing words the dumper cut. Not one member of
those four replies is typed out by hand.

The numbers that used to be the input are now the **cross-check table** (part 2b of the test): 32
predictions from `gca_config.bin`, `ip-discovery.bin`, dmesg and fact M5 about what the kernel's
reply should contain. All 32 pass. A failure there means `docs/facts.md` is wrong, or the dump is
not from this unit.

This reversal is not cosmetic. Decoding the dump **refuted nine things the previous version of
this contract asserted** - see the next section.

## The result

Two independent results, because the second is the one that can catch what the first cannot.

**`165 checks, 0 failures`** - Mesa's unmodified `ac_gpu_info.c` accepts the blob and derives:

```
family              CHIP_GFX1013 (Cyan Skillfish)
gfx_level           GFX10 (gfx10.1);  GFX IP 10.1.3
queues              gfx 1, compute 0, sdma 2
shader array        max_se 2, max_sa_per_se 2, cu_per_sh 10, num_cu 24, 6 good CUs per SA
render backends     16 of 16;  TCC 16 of 16
caches              tcp 16 KiB, l1 128 KiB, l2 4 MiB
tiling              gb_addr_config 0x00100044, num_tile_pipes 16
clocks              gpu 2000 MHz, memory 0 MHz, bus 1024 bit, vram_type UNKNOWN
memory              vram 8 GiB (all visible), gart 3926228 KiB, not dedicated (APU)
VA                  max 0x0000800000000000, high 0xFFFF800000000000..0xFFFFFFFFFFBF0000
misc                clock_crystal 100000 kHz, pc_lines 1024, max_gflops 6144
firmware feature    me 32, pfp 32, mec 32
max_submitted_ibs   gfx 192, compute 125, sdma 49, vcn_jpeg 16
```

**`177 matched, 0 mismatched`** - and the same `struct radeon_info`, compared field by field
against the one RADV derived on unit A itself. See the next section.

## Comparing against the device

`test/compare_radv_info.py` diffs the caps this contract derives against the caps RADV derives on
the real GPU. Both sides are printed by the **same function**, Mesa's `ac_print_gpu_info()`, so it
is a diff of one `struct radeon_info` against another and not a diff of two formats:

| Side | How |
|---|---|
| the device | `RADV_DEBUG=info vulkaninfo --summary` on unit A. `radv_physical_device.c:2903` calls `ac_print_gpu_info()` when that flag is set. Captured as `evidence/linux/2026-09-21-E14-vulkan-compute-reference/radv-info.txt`, Mesa 26.1.6, kernel 6.18.52, drm 3.64.0 |
| this contract | `bc250_caps_test.exe --radeon-info`, which runs the blob through the real `ac_*` path and then calls the same `ac_print_gpu_info()` |

`test/run.ps1` runs it automatically when the evidence file is present. Standalone:

```
python driver\contract\test\compare_radv_info.py            # add --verbose for the matches
```

**Why it exists.** A reversed conclusion about `gb_addr_config` survived two written reports and a
133-check test, because every check agreed with the blob and the blob agreed with the mistake.
Nothing inside the test could have caught it. This compares against the device.

It earned that on its first run, finding four things no internal check could:

| Found | What it was |
|---|---|
| `pcie_gen`, `pcie_num_lanes`, `pcie_bandwidth` all 0 | measured in the blob, but the winsys never copied them into `radeon_info`. They are assigned inline in `ac_query_gpu_info()` at `ac_gpu_info.c:1662-1663`, not in any `ac_fill_*` function, so calling only the `ac_fill_*` set missed them |
| `max_gflops` 0 | same cause, `ac_gpu_info.c:1659` |
| `has_vm_always_valid` 0 | `ac_gpu_info.c:1621`. Not a device property at all: for a non-virtio device `ac_drm_query_has_vm_always_valid()` is `info->has_vm_always_valid = true` and nothing else (`ac_linux_drm.c:1120-1129`) |
| a whole missing epilogue | those were symptoms. `ac_query_gpu_info()` has ~60 lines of assignments *between* the `ac_fill_*` calls (`:1629-1691`, `:1808-1815`) that the winsys was not reproducing at all: `max_scratch_waves`, `scratch_wavesize_granularity`, `instr_prefetch_distance`, `spi_cu_en`, `se_tile_repeat` and more. All are now reproduced line for line with citations |

The remaining differences are classified, never silently excused. Three buckets, each with a
written reason per field, and the script exits non-zero unless everything falls into them:

- **5 expected** - structural, no WDDM equivalent: `dev_filename`, `marketing_name` (comes from
  libdrm's `amdgpu.ids` data file, not from any ioctl - a WDDM driver must supply the string
  itself), `kernel_has_modifiers` (DRM/KMS only), `has_timeline_syncobj` (a winsys synchronisation
  backend decision), `max_alignment` (set only in `ac_surface.c:1064` from addrlib, which this
  test does not link).
- **8 Mesa version skew** - the lab ran 26.1.6, `ref\mesa` is a newer main checkout. Each name was
  checked against `ref\mesa`'s `ac_gpu_info.c`: absent from one version's source, so a version
  difference rather than a branch taken differently. Worth noting for later:
  `has_smem_with_null_prt_bug` is new, and when it is 1 `ac_gpu_info.c:1817-1827` makes
  `ac_query_gpu_info()` issue an extra `amdgpu_sw_info_address_prt_wa_control_bit` query and
  **fail the whole probe** if it errors. A future Mesa bump turns that into a blob requirement.
- **5 more expected** - `max_submitted_ibs` for the five IP types this part does not have. See
  below; this one was open for a while and is now closed by a source reading.

### The `max_submitted_ibs` question, and how it closed

For the five video IPs the raw `AMDGPU_INFO_MAX_IBS` reply says 49 and RADV on the device says 1:

```
                  GFX  COMPUTE  SDMA  UVD  VCE  UVD_ENC  VCN_DEC  VCN_ENC  JPEG  VPE
E13b4 info.txt    192   125      49    49   49   49       49       49       16    49
E14 radv-info     192   125      49     1    1    1        1        1       16    49
```

The first guess was that the two captures came from different boots, and the fix would have been
another Linux session. It would have been wasted: the kernel side is static. `amdgpu_kms.c:1325-1333`
answers this query by looping `amdgpu_ring_max_ibs(type)` over every `AMDGPU_HW_IP_*` into a local
array, with no per-boot state and no check for whether the IP exists. **49 is what the ioctl
returns on any boot**, so re-capturing could not have changed it.

That leaves the 1 coming from Mesa 26.1.6's own derivation for IPs with no queues. *(Mesa 26.1.6
side, source not checked - only `main` is checked out in `ref\mesa`.)*

The blob keeps the kernel's values, because the kernel's values are what a WDDM KMD would have to
supply. Every IP that actually exists agrees to the digit.

## What the raw ioctl dump refuted

Nine claims from the assembled version, and what the kernel actually returned. Seven are in
`DEV_INFO`, two in `HW_IP_INFO`. The 43 other `DEV_INFO` fields matched exactly.

| Field | Asserted | Measured | Consequence |
|---|---|---|---|
| `gpu_counter_freq` | 0 (on the wishlist) | `0x000186A0` = 100 MHz | Was making `ac_gpu_info.c:1231` print *"clock crystal frequency is 0, timestamps will be wrong"* and substitute 1. Every GPU timestamp was wrong; now correct |
| `pa_sc_tile_steering_override` | 0, assumed *"0 on everything but Navi10"* | `0x00122000` | Non-zero and copied straight into `radeon_info`, so the assumption was not harmless. Reaches addrlib |
| `high_va_max` | `0xFFFFFFFFFFBFE000`, derived from mainline v6.18's `AMDGPU_VA_RESERVED_TOP` = trap `0x2000` + seq64 `0x200000` + CSA `0x200000` = `0x402000` | `0xFFFFFFFFFFBF0000` | Implies `0x410000` on the 6.18.52 kernel that answered. The derivation is gone; the kernel is the authority on its own reserved range |
| `max_memory_clock` / `min_memory_clock` | 450000, from `pp_dpm_mclk` | **0** | Not a dumper artefact: the same dump's `SENSOR_GFX_MCLK` reads `0x1C2` = 450 MHz, so the memory is running. amdgpu simply reports 0 in `DEV_INFO` here, so Linux RADV computes memory bandwidth from 0. Parity means carrying the 0 |
| `ids_flags` | `0x01` (FUSION only) | `0x11` = FUSION \| GANG_SUBMIT | Settles both wishlist bits at once. PREEMPTION (`0x02`) clear, so `has_kernelq_reg_shadowing` is false; TMZ (`0x04`) clear, confirming E03 dmesg from the other side |
| `ce_ram_size` | 0, on the theory that gfx10 has no CE RAM | `0x10000` = 64 KiB | Mesa does not read it, so no behavioural change - but the theory was wrong |
| `hw_ip[COMPUTE].available_rings` | `0xFF`, eight rings, from `debugfs-rings.txt` | `0x0F`, four | The debugfs file list and the ioctl answer different questions: amdgpu advertises a ring only once its scheduler is ready. No downstream effect, because Mesa drops compute on GFX1013 anyway (point 2) |
| `hw_ip[GFX].hw_ip_version_minor` | 1 | 0 | Harmless: `ip_discovery_version` is non-zero, so `ac_gpu_info.c:509-513` takes major/minor/rev from there (10.1.3) and ignores these two fields entirely |

Three former wishlist rows are now measured (`gpu_counter_freq`, `pa_sc_tile_steering_override`,
both unknown `ids_flags` bits), and `tcc_disabled_mask` = 0, `pcie_gen` = 4 and `pcie_num_lanes`
= 16 are confirmed rather than assumed.

Two confirmations worth stating plainly:

- **The `cu_bitmap` derivation was right.** Fact M5's measured
  `CC_GC_SHADER_ARRAY_CONFIG = 0xFFF80000`, run through `gfx_v10_0_get_wgp_active_bitmap_per_sh()`
  and `gfx_v10_0_get_cu_active_bitmap_per_sh()`, predicted `0x3F` in each of the four shader
  arrays and `cu_ao_mask = 0x3F3F3F3F`. The ioctl reply is byte-identical.
- **The gfx level is stated, not inferred.** `HW_IP_INFO` for GFX and COMPUTE both return
  `ip_discovery_version = 0x000A0103`, and DMA returns `0x00050001`. Mesa reads 10.1.3 and 5.0.1
  straight out of those fields. It is no longer an inference from the IP discovery table.
- **All VRAM is CPU-visible, measured three ways.** `MEMORY` reports
  `cpu_accessible_vram.total_heap_size == vram.total_heap_size == 0x200000000`, and `VRAM_GTT`
  says the same twice more. `info->all_vram_visible` is measured, not assumed.

## Five things the test found that are worth knowing before M7

**1. Mesa has no `CHIP_CYAN_SKILLFISH`.** Cyan Skillfish is `CHIP_GFX1013` in
`enum radeon_family`, inside `FAMILY_NV`, reached because `external_rev` 132 falls in
`AMDGPU_GFX1013_RANGE` (`addrlib/src/amdgpu_asic_addr.h:94`). And `enum amd_gfx_level` has no
10.1.3 rung: GC 10.1.3 lands on `GFX10`, with the `.3` surviving only in
`info->ip[AMD_IP_GFX].ver_rev`.

**2. RADV will not use the compute queues.** `ac_gpu_info.c:501-504` drops `AMD_IP_COMPUTE`
outright when the family is `FAMILY_NV` and `external_rev` is in the GFX1013 range, with the
comment *"GFX1013 is known to have broken compute queue"*. `num_compute_queues` comes out 0 no
matter what the blob says. The eight compute rings `bc250kmd` brings up in M5/M6 are still worth
having - they are how the ring tests and fences are exercised - but on the RADV path everything
will go through the one gfx ring until somebody patches Mesa.

**3. `gb_addr_config` must be the golden constant, and a Windows KMD must not read the register.**

Two values exist for `GB_ADDR_CONFIG` on this part, and which one a driver uses decides whether
its surfaces match Linux:

- `0x00000044` - what an `RREG32` returns. Fact M46. `E13b4/state.txt` records it, read through
  debugfs `amdgpu_regs2`, which is a raw register read that never reaches user mode.
- `0x00100044` - what **user mode actually gets**. Fact M50. `E14/radv-info.txt:186`, Mesa 26.1.6
  on unit A: `GB_ADDR_CONFIG: 0x00100044`.

The two differ in bit 20, the high bit of `NUM_SHADER_ENGINES` (`gc_10_1_0_sh_mask.h`, shift 0x13,
mask 0x00180000): the register reads back 0 shader engines where the kernel's config says 2.

Mesa does ask the kernel for this register - `ac_linux_drm.c:730` calls
`ac_drm_read_mm_registers(dev, 0x263e, ...)`, which is `AMDGPU_INFO_READ_MMR_REG`. **But that
ioctl does not perform an `RREG32` for it.** The path is `amdgpu_info` ->
`amdgpu_asic_read_register` -> `nv_read_register` -> `nv_get_register_value`, and `nv.c:381-382`
reads:

```c
if (reg_offset == SOC15_REG_OFFSET(GC, 0, mmGB_ADDR_CONFIG))
        return adev->gfx.config.gb_addr_config;
```

an explicit special case that returns the cached value and never touches the hardware. It is
reached because `nv.c:354` lists `mmGB_ADDR_CONFIG` in `nv_allowed_read_registers` with
`grbm_indexed` unset, so `nv_read_register` (`nv.c:402-404`) passes `indexed = false` and
`nv_get_register_value` takes the `else` branch. The cached value is
`CYAN_SKILLFISH_GB_ADDR_CONFIG_GOLDEN` (`gfx_v10_0.c:3675`), assigned at `gfx_v10_0.c:4619` in the
`IP_VERSION(10,1,3)` arm of `gfx_v10_0_gpu_early_init()` without reading the register.
`E01/gca_config.bin` dword 22 shows the same constant from the debugfs side.

So the kernel and user mode agree on `0x00100044`; only a raw register read disagrees. The blob
carries the constant, `BC250_UMD_F_GOLDEN_GB_ADDR` is set, and `BC250_MUTATE_GB_ADDR_REGISTER`
substitutes the register value to show what the mistake looks like.

> **For the Windows KMD: do not fill this field from a register read.** Use the constant, exactly
> as `nv.c` does. A driver that reads the hardware here hands addrlib `0x00000044` and diverges
> from Linux in surface layout, with nothing in `radeon_info` to warn it - see the limitation
> below.

*How this entry came to be written twice.* An earlier revision argued the opposite, reasoning that
`READ_MMR_REG` implies a real register read. It does not on this family. The measurement
(`radv-info.txt:186`) settled it, and the source reading above explains the measurement, which is
the only reason the question is closed rather than merely decided. The rule it cost us: **when a
measurement and a source reading disagree, the measurement wins until the source reading explains
it.** It is also why `compare_radv_info.py` now exists - a comparison against the device would
have caught the reversal on its first run, and nothing internal to the test could.

*Limitation, stated rather than glossed.* `ac_gpu_info.c` decodes only `NUM_PIPES` and
`PIPE_INTERLEAVE_SIZE` out of this register, and both are identical between the two values. The
control therefore proves the value travels intact through `radeon_info`, not that either value
tiles correctly. The difference bites in addrlib, which this test does not link.

**4. Two of the three UAPI structures are NOT layout-compatible with Mesa's Windows copies.**
Mesa built for Windows does not include the kernel header; `ac_linux_drm.h:16-321` declares its
own. Measured by the test, not assumed:

| Structure | Kernel | Mesa (Windows) | Verdict |
|---|---|---|---|
| `drm_amdgpu_info_device` | 448 B, 65 members | 448 B, 65 members | identical name, order and width, member for member. Pass by pointer |
| `drm_amdgpu_heap_info` | 32 B, 4 members | 8 B, 1 member | `drm_amdgpu_memory_info` is 96 B here, 24 B there. **Must be translated** |
| `drm_amdgpu_info_hw_ip` | 40 B, 8 members | 24 B, 6 members | no `capabilities_flags`, no `userq_num_slots`; every later offset shifts. **Must be translated** |

A winsys that `memcpy`s the blob into Mesa's types gets the device right and the memory and IP
information wrong, silently. `test/bc250_caps_mesa.c` is the worked reference for the translation,
and the test compares all 65 device offsets on both sides every run, so the day either side moves
a field it says so.

**5. `num_sqc_per_wgp` and the two SQC cache sizes are 0, and that is the parity value.** The GC
discovery table on unit A is version 1.1 (`evidence/linux/2026-09-21-E01-recon-binaries/ip-discovery.bin`).
`amdgpu_discovery.c` fills those three only from version 1.2 upward, so on Linux they arrive at
RADV as 0 on this machine. Zero is measured-by-absence, not a placeholder. The same goes for
`tcp_cache_size`, `gl1c_cache_size`, `gl2c_cache_size` and `mall_size`: `ac_gpu_info.c:602` takes
the `else` branch below GFX11 and substitutes its own constants regardless.

## Measured, derived, assumed

- **The ioctl replies are the input.** `DEV_INFO` (93 words), `MEMORY` (23), `HW_IP_INFO` and
  `HW_IP_COUNT` per IP, and `FW_VERSION` per firmware type, all decoded from
  `E13/boot4-readonly-after-windows/info.txt` through the structures of
  `third_party/amdgpu_drm.h`. Nothing in these four is a typed-in fact.
- **The independent captures are assertions.** 32 cross-check rows in part 2b:
  `E01/gca_config.bin` (amdgpu's own gfx config, 36 dwords, decoded against
  `amdgpu_debugfs_gca_config_read()`), `E01/ip-discovery.bin` (GC table v1.1),
  `E01/gpu-sysfs.txt`, `E01/lspci-gpu-vvv.txt`, `E03/dmesg.txt`, `E13/boot3-readonly/state.txt`,
  and fact M5. Each row names its source well enough to go and look. All 32 agree with the bytes
  the kernel returned.
- **Still hand-written, and tagged in the source**: `tiling.gb_addr_config` (measured at the
  user-mode boundary in `radv-info.txt`, the rest [K] because Mesa never fills them on this
  family) and `kernel.*` (the DRM version and `address32_hi`, neither of which is an
  `AMDGPU_INFO` query - both measured from `radv-info.txt`). Tags are [M] measured, [D] derived
  with both sides cited, [K] a kernel or Mesa constant, [W] wishlist, now unused.
- **Firmware, measured twice.** All seven `FW_VERSION` replies match `E13/boot3-readonly`'s
  `amdgpu_firmware_info` debugfs text from the previous boot, to the digit. A raw ioctl and a
  debugfs dump on two different boots returning the same numbers is the strongest agreement in the
  file.
- **One field deliberately not cross-checked**: `gtt.total_heap_size`. amdgpu sizes the GTT from
  free system RAM at init, so it moves between boots - `0xEFA35000` on boot4, `4020453376` on
  boot3, `4020506624` on E01. All three are right. The contract is pinned to the boot every other
  byte came from.

## Wishlist: empty

Every field of the blob is measured on unit A, and `BC250_UMD_F_UNMEASURED` is **clear**. It was
eight rows two revisions ago.

Closed by the E13b4 raw ioctl dump (`info.txt`): `gpu_counter_freq`, `tcc_disabled_mask`,
`pa_sc_tile_steering_override`, both unknown `ids_flags` bits, `pcie_gen`, `pcie_num_lanes`,
`ce_ram_size`, `high_va_max`, `max_memory_clock`, and `max_submitted_ibs[]` - the last of which
was in the committed dump all along, at query `0x22` after `MEMORY`. An earlier revision of this
file asked for it as a wishlist item because it was read from a stale copy of `info.txt`. Read the
evidence copy in the repo, not a scratch copy.

Closed by the E14 `RADV_DEBUG=info` dump (`radv-info.txt`):

| Field | Measured | Note |
|---|---|---|
| `kernel.drm_minor` | `drm = 3.64.0` | We had claimed 3.63 from the v6.18 tag. `ac_gpu_info.c:1493` requires major 3 and `:1496` refuses below 3.54, so both would have passed - which is exactly why it needed measuring rather than asserting |
| `kernel.address32_hi` | `0xffff8000` | Still not an ioctl: `ac_drm` computes it in user space from the VA range, so a Linux session cannot read it out. But RADV prints the result, so the number our winsys must arrive at is now known rather than guessed. It is the high word of `high_va_offset`, which is where that computation lands |
| `tiling.gb_addr_config` | `0x00100044` | Point 3 above. This one decided a question two source readings had answered both ways |

If a field ever goes back on this list, set the flag again: a release UMD should refuse a blob
that admits to guessing.

**Queries explicitly not worth a boot.** `UQ_FW_AREAS` (`0x24`) is gated on
`info->gfx_level >= GFX11` at `ac_gpu_info.c:1769`, `:1783` and `:1795`; we are GFX10, so it is
never called - consistent with the measured `userq_ip_mask` = 0, and with the dump's
`UQ_FW_AREAS = ERROR 95`. `VCE_CLOCK_TABLE` (`0x1A`), `GPUVM_FAULT` (`0x23`) and
`NUM_VRAM_CPU_PAGE_FAULTS` (`0x1E`) are not on `ac_query_gpu_info()`'s path at all.

**Worth re-running after any Mesa bump**, as a check on the output rather than a missing input:
`RADV_DEBUG=info vulkaninfo --summary` on unit A, then `compare_radv_info.py`. A new Mesa can add
a field that `ac_query_gpu_info()` hard-fails on - `has_smem_with_null_prt_bug` is already on that
trajectory.

**Measured but not carried.** `GDS_CONFIG` came back with `gds_total_size` 64 KiB,
`gws_per_compute_partition` 64 and `oa_per_compute_partition` 16. `ac_query_gpu_info()` does not
query GDS - Mesa hardcodes those for gfx10+ - so the blob has no field for it. Recorded here in
case a v2 needs one. `SENSOR` (SCLK 1500 MHz, MCLK 450 MHz, 69 C, 58 W average / 65 W input,
VDDGFX 906 mV, VDDNB 1193 mV) is instantaneous telemetry, not a capability, and belongs nowhere
near this contract.

## How the test is built, and why it is not a fork of Mesa

`ac_gpu_info.c` is compiled straight out of `P:\BC-250\ref\mesa`, unmodified. It needs:

- `/std:c11`, for `_Alignas` in Mesa's `src/util/u_atomic.h:374`;
- two headers meson would generate, generated here by Mesa's own scripts into `$Out\gen`
  (`makeregheader.py` with the same argument list as `src/amd/common/meson.build:76-82`, and
  `u_format_table.py --enums`);
- twelve link stubs (`test/bc250_caps_stubs.c`), because the object file references every symbol
  the whole file needs, not just the ones the functions we call need. Ten of them abort with a
  message naming the call site and why it is unreachable. A stub that returns 0 is a bug waiting
  to be believed.

The three option readers (`os_get_option`, `debug_get_option`, `debug_get_bool_option`) are the
exception and are handled differently, because one of them *is* on our path:
`ac_fill_feature_info()` reads `AMD_IMAGE_OPCODES` at `ac_gpu_info.c:1193`. They return the
caller's own default and record that they were asked; the test prints the list and asserts it is
exactly `AMD_IMAGE_OPCODES`. Deliberately not `getenv()`: a contract test whose answer depends on
the operator's shell is not a contract test.

The test calls `ac_identify_chip()` and the `ac_fill_*` set directly, in the order
`ac_query_gpu_info()` calls them (`ac_gpu_info.c:1463-1643`), rather than calling
`ac_query_gpu_info()` itself, which needs a live DRM device node. The order matters:
`ac_fill_hw_ip_info()` must run before `ac_identify_chip()`, which picks the gfx level from
`info->ip[AMD_IP_GFX].ver_major/ver_minor` at `ac_gpu_info.c:779-805`. Two steps are not
reproduced and are named in the source rather than quietly dropped: `ac_drm_query_pci_bus_info()`
and `ac_fill_video_info()`.

## The controls

A test that cannot fail proves nothing. Seven deliberate corruptions, each in its own process so
that one crash cannot hide the other six:

| Mutation | Result |
|---|---|
| `family := FAMILY_NV3` | **Mesa dies.** It identifies a Navi 3x, then trips its own `assert(0)` at `ac_gpu_info.c:1373`: that gfx11 family has no `pc_lines` entry in the switch our gfx10.1 IP version routes it to. A wrong family is not politely refused |
| `external_rev := 0x90` | Refused, `AC_QUERY_GPU_INFO_UNIMPLEMENTED_HW`, *"unknown (family_id, chip_external_rev): (143, 144)"* |
| GC IP version `:= 10.3.0` | `gfx_level` becomes `GFX10_3` |
| `cu_bitmap := 0` | `num_cu` becomes 0 and Mesa **survives**, because on gfx10.1 `num_se` comes from `max_se` rather than from the bitmap. From GFX10_3 on, `ac_gpu_info.c:1286` derives `num_se` from this bitmap and an empty one divides by zero at `:1311`. Either way it is a blob no KMD should emit |
| `cu_bitmap := 0x3FF` | `num_cu` becomes 40 |
| `vram := 4 GiB` | `vram_size_kb` halves - which also proves the hand-written heap translation is live |
| `gb_addr_config := 0x00000044` | the raw register value reaches `info->gb_addr_config` instead of the constant the `READ_MMR_REG` ioctl really returns - the mistake a Windows KMD makes if it reads the hardware here. See point 3 for what this proves and what it does not |

Child exit codes deliberately skip 3: MSVC's `abort()` exits with 3, and reading that as a result
rather than as a death is the kind of quiet wrong answer this whole directory exists to prevent.
The harness also disables the abort message box and Windows Error Reporting - a control is meant
to be able to kill the process, and nothing on this PC gets to open a window.

## Not done

- Nothing is wired into `driver/kmd`, confirmed again by the ADR 0012/0013 review that added
  version 3 (`docs/design/umd-contract-stage-d.md`): `DxgkDdiQueryAdapterInfo` still refuses
  `DXGKQAITYPE_UMDRIVERPRIVATE` outright (`wddm.c`'s `default:` arm), `DxgkDdiCreateAllocation`
  parses a different, GDI-shaped private struct of its own (`BC250_WDDM_ALLOCATION_PRIVATE`, magic
  `"LB7A"`, not this file's `"BC2A"`) and refuses anything else, `DxgkDdiCreateContext` never hands
  a context a `bc250_umd_context_private` to fill in (`DmaBufferPrivateDataSize = 0`), and
  `DxgkDdiSubmitCommandVirtual` reads the raw DMA buffer directly rather than a
  `bc250_umd_submit_private`. All of that is expected for stages A-C, which have no user-mode
  driver to hand private data to, and all of it is M8 work, not something this contract update
  does. No `DxgkDdiQueryAdapterInfo` handler writes this blob yet.
- `addrlib` is not exercised. The test establishes that Mesa recognises the chip and derives sane
  limits, not that surfaces tile the same way as on Linux. That needs `ac_surface` and a
  comparison against a Linux capture, and it is where `gb_addr_config` bit 20 actually matters.
  The device comparison already names the first thing that work has to reproduce: `max_alignment`
  = 65536, which comes from `AddrGetMaxAlignments()` and is 0 here.
- `marketing_name` is empty. On Linux it comes from libdrm's `amdgpu.ids` data file rather than
  from the kernel, so a WDDM driver has to supply the string. A blob field for it is a v3 change
  and nothing needs it yet.
- The next layer up - BO allocation, VA mapping, contexts and submission - is `README-winsys.md`
  and `bc250_umd_submit.h`, and is likewise a contract rather than an implementation.
- The blob carries no per-process or per-adapter state (GPU VA layout, doorbell assignment,
  paging queue). That is a separate contract and belongs with the M7 memory-manager work.
