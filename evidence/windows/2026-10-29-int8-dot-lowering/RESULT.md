# INT8 dot-product lowering on gfx1013: lab measurement

2026-10-29, 14:57-15:00Z, unit A, driver 40.7.216.106 (tester.34), lab flavour package installed. The design is
[docs/design/int8-dot-lowering.md](../../../docs/design/int8-dot-lowering.md). Every lab step had a bound of 170 s.

"Before" is the installed Mesa. "After" is the same Mesa with the nine commits of the design page. Both sides are
lab-flavour builds of the release recipe (clang-cl 23.1.4, version 0.7.216.106-tester.34).

| Side | File | SHA-256 |
|---|---|---|
| Vulkan, before | installed `vulkan\vulkan_radeon.dll` | 4DCDFAC96A8855AB77932F271E84ADA32E84AE4A67AC34A9C10B03601DB2CEF4 |
| Vulkan, after | `vulkan_radeon.dll`, lab row | 5C987CE9B32042161BB87E2EA3D8011C35FBF467084B29228327B4FCA2D47CCF |
| D3D12 hosted ICD, before | installed `d3d12\amdgpu_wddm_radv_d3d12.dll` | D5D29702DFE6D1E91497B07254B9B38B7B872E397FFB0E47592C3453EBA56519 |
| D3D12 hosted ICD, after | `amdgpu_wddm_radv_d3d12.dll`, lab row | 2B519C17F22D4332E00A4E2E0E65EF5FE4B15F1828AFE946C37A40EB7027F15E |

The release-flavour rows of the same source are `vulkan_radeon.dll`
96F31868C0F3363115D6A0466C57D6E8E315EFDC651ADF582116E0227D183AE9 and the hosted ICD
09875F4AD7F5DD341ED48134836D984DF235E5774EE5B6860CBC2A1B2A071764. This run did not use them.

The Vulkan clients load the ICD that `--icd` names, without the Vulkan loader. The Vulkan side needs no change to
the installed driver. For the D3D12 side, the hosted ICD in the active DriverStore folder was replaced by the
after row, and the installed file was put back after the run with its hash checked.

Clients: vkdotcheck.exe C00447B41F3D77305CAA2D29620BD478E74ADCCA80B3D4AA8CF8331C51D6CB80, vkcorpusbench.exe
73AA30D1F53C8956B0F3AB78F57D83A1CD2760AC97E2B7E69D591B88BE3F71FA, fsr4host.exe
6DD48A6D42ADE029B3E9739A1132E35BF5FCD46973F22107C007D206659BF842. `verdict.txt` is the output of the verdict
script over these folders. `SHA256SUMS.txt` lists every file here.

## GPU exactness (`dot/`)

| Shader | Records | Mismatches before | Mismatches after | Output FNV-1a, before and after |
|---|---|---|---|---|
| run-time operands, plain and `_sat` | 1051648 | 0 | 0 | 7e2bc410075dd03b |
| constant-weight layer | 311296 | 0 | 0 | 1c4788c65ce68699 |
| uniform data, scalar path | 8192 | 0 | 0 | 292c8534d3f5c0d6 |
| dxil-spirv form | 262144 | 0 | 0 | 263e40f9465482b5 |

- Both sides report 0 accelerated dot-product properties and have no `v_dot` instruction.
- The negative control, the after row with the wrong sign extension in the reference, fails with 9625073
  mismatches and exit code 1.
- The RESULT line counts each instruction over two listings, "ACO IR" and "Assembly". The lab-flavour driver has
  no LLVM disassembler, so its "Assembly" listing is the ACO program after the lowering to hardware instructions.

Hardware-instruction listing (`dot/ir-*/<shader>.assembly.txt`):

| Shader, instruction | Before | After |
|---|---|---|
| constant-weight layer, `v_mad_i32_i24` | 0 | 37 |
| dxil-spirv form, `v_mad_i32_i24` | 0 | 31 |
| scalar path, `s_bfe_i32` | 2 | 6 |
| scalar path, `s_mul_i32` | 25 | 35 |

The offline compiler harness gives the same four after counts for gfx1013.

## FSR 4.1.2 INT8 corpus (`corpus/`)

14 compute shaders of the FSR 4.1.2 upscaler, translated by dxil-spirv: one of each network pass and two
post-pass variants. Each has seeded inputs and the grid of its entry guard. Each side ran 2 exactness
dispatches and 30 timed dispatches per shader. Times are GPU timestamps.

- 14 of 14 shaders are deterministic on both sides.
- Every written buffer and image has the same SHA-256 before and after.
- VGPRs and spilled VGPRs equal the offline rows of `offline/corpus-offline.csv` for every shader.

| Shader | Family | Grid | VGPRs | Spilled VGPRs | Median ms | Ratio | Minimum ms | Ratio |
|---|---|---|---|---|---|---|---|---|
| blob_0815 | pass1 | 15 x 540 | 256 > 168 | 0 > 0 | 2.3803 > 0.7773 | 0.327 | 1.8442 > 0.7769 | 0.421 |
| blob_0060 | pass2 | 30 x 1080 | 256 > 168 | 40 > 0 | 9.7487 > 6.1293 | 0.629 | 9.7420 > 5.2316 | 0.537 |
| blob_0036 | pass3 | 8 x 270 | 56 > 56 | 0 > 0 | 0.7390 > 0.6892 | 0.933 | 0.7387 > 0.6890 | 0.933 |
| blob_0760 | pass4 | 30 x 1080 | 256 > 168 | 0 > 0 | 9.9234 > 5.5744 | 0.562 | 9.9164 > 5.2987 | 0.534 |
| blob_0811 | pass5 | 8 x 270 | 256 > 168 | 0 > 0 | 1.0774 > 0.4569 | 0.424 | 1.0413 > 0.4306 | 0.414 |
| blob_0759 | pass6 | 8 x 270 | 84 > 84 | 0 > 0 | 2.1306 > 1.1717 | 0.550 | 1.2710 > 1.0514 | 0.827 |
| blob_0824 | pass7 | 8 x 270 | 64 > 64 | 0 > 0 | 2.4945 > 2.4296 | 0.974 | 2.3562 > 2.1492 | 0.912 |
| blob_0798 | pass8 | 4 x 135 | 64 > 64 | 0 > 0 | 0.5875 > 0.6792 | 1.156 | 0.5872 > 0.6790 | 1.156 |
| blob_0842 | pass9 | 8 x 270 | 84 > 72 | 0 > 0 | 2.8390 > 3.2937 | 1.160 | 2.6515 > 2.5307 | 0.954 |
| blob_1010 | pass10 | 8 x 270 | 256 > 168 | 27 > 0 | 1.0389 > 0.5319 | 0.512 | 1.0048 > 0.5070 | 0.505 |
| blob_0412 | pass11 | 30 x 1080 | 56 > 56 | 0 > 0 | 10.4095 > 9.6910 | 0.931 | 10.4063 > 9.6882 | 0.931 |
| blob_0765 | pass12 | 30 x 1080 | 256 > 168 | 0 > 0 | 4.0271 > 2.9004 | 0.720 | 4.0246 > 2.6124 | 0.649 |
| blob_0068 | post-pass | 120 x 68 | 128 > 100 | 0 > 0 | 6.6388 > 5.5788 | 0.840 | 6.6338 > 5.5746 | 0.840 |
| blob_0192 | post-pass | 120 x 68 | 128 > 84 | 0 > 0 | 6.6322 > 5.5646 | 0.839 | 6.6212 > 5.5591 | 0.840 |

The sum of the medians is 60.667 ms before and 45.468 ms after (0.749).

pass9 has a wide spread: its slowest dispatch is 1.34x its fastest before and 1.68x after. pass8 has a narrow
spread on both sides: 0.5872 to 0.5886 ms before and 0.6790 to 0.6803 ms after.

`offline/pass7-8-9-opcode-diff.txt` gives the offline hardware-instruction counts of pass7, pass8 and pass9. pass7
and pass8 have the same changes: 336 fewer `v_add_u32` and 168 more `v_add3_u32`. The lab statistics of the two
also agree to within one instruction. pass7 runs 2160 workgroups and pass8 runs 540. That the small grid causes the
pass8 loss is an inference from these counts. The run does not measure it.

## FSR 4.1.2 RDNA 2 model through D3D12 (`fsr4-*/`)

Three runs of 300 frames for each geometry and each set. Each value is the median GPU time of frames 150 to 299
of one run, in ms. "After-cold" is the first set after the ICD change. Its first run compiled the pipelines again
(5.56 s on the first frame at 1200p).

| Geometry | Before | After-cold | After |
|---|---|---|---|
| 1706x960 to 2560x1440 | 16.2450, 16.8824, 16.2247 | 11.9352, 11.9357, 11.9370 | 12.5392, 11.9344, 13.9727 |
| 1280x800 to 1920x1200 | 10.9625, 10.5793, 10.9776 | 11.8279, 9.6616, 8.6302 | 7.7883, 7.7932, 7.8017 |

- At 1440p all nine runs give the output FNV-1a d7ddba1725c13731, and each set has `warm_runs_identical=1`.
- The lowest 1440p run median goes from 16.2247 ms to 11.9344 ms (0.736).
- At 1200p the output differs between runs on both sides, so this run compares only its times.
