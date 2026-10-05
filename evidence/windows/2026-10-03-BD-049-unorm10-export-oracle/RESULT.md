# BD-049 10-bit UNORM export oracle, host run

Date: 2026-10-03. Machine: the development PC. No lab unit and no GPU take part: the test links the built RADV
objects and evaluates the compiler output on the CPU.

## What ran

`tools/build/build-radv-unorm10-export-test.ps1` built and ran
`src/amd/vulkan/tests/radv_unorm10_export_test.c` against the RADV tree of the registered ICD's source line
(mesa-wddm `amdgpu-wddm/bd049-unorm10-export-f9dc`, head `16354ed2c68fd002627b0e35e813422e72e87a7c`). The runner
then ran the same executable a second time with the rounding switched off. That second run is the negative
control: it must fail.

The test checks three things for each input:

1. the PS epilog key that `radv_generate_ps_epilog_key` produces (format, blend, write mask and RB+ cases);
2. the NIR that `ac_nir_lower_ps_late` lowers, evaluated with `nir_eval_const_opcode`;
3. the ACO PS epilog, evaluated instruction by instruction.

## Result

| Run | Exit | Inputs (NIR / ACO) | Checks | Failures (key / NIR / ACO) |
|---|---|---|---|---|
| test | 0 | 1312733 / 1312733 | 57760299 | 0 / 0 / 0 |
| negative control, no rounding | 1 | 1312733 / 1312733 | 57760296 | 0 / 5131683 / 3655129 |

The test run shows no failure in 57 760 299 checks. The negative control fails 8 786 812 of them, so the test
can see the defect it is there to catch. Its first failure is the value from the defect report: 0.6 exports as
`0x38cc` and stores as 613, while D3D stores 614.

## Files

| File | What it holds |
|---|---|
| `record.json` | the runner's record: source head, input hashes, executable hash, both summaries and the first failures |
| `test.txt` | the test run's output |
| `negative-control.txt` | the negative control's output, 43 lines, with its summary line last |
| `test/epilog-marked.txt`, `test/epilog-blended.txt` | the ACO epilogs the test run evaluated |
| `negative-control/epilog-marked.txt`, `negative-control/epilog-blended.txt` | the same epilogs without the rounding |
| `build.log` | the exact `cl`, `lib` and `link` command lines |

## Scope

This run proves the arithmetic of the fix on the host. It does not show the lab GPU storing 614: that needs a
lab Present ladder run, which BD-049 still lists as pending.
