# HIP device contract and lab validation

The new device headers let TinyLlama complete on unit A.
Its 16-token greedy output matches the CPU reference byte for byte.
The Gemma 12B control fails during allocation, before any model dispatch.
This record does not claim complete HIP compatibility or a passing dense-model control.

## Cause and change

The old header returned AQL work-item extents for `gridDim`, which must count blocks.
The runtime's AQL packet met the contract. The header needed the conversion.
Rebuild each consumer that embeds device code. Replacing the runtime DLL cannot change that code.

The retained journal names batch 27, dispatch 15, and `soft_max_f32<true, 512, 512, float>`.
Its GFX sequence is 11695, OS fence 1842 and VMID 11.
The first GPUVM read fault precedes the watchdog timeout by 12,314 ms.
The [GuardLog excerpt](incident-guard-excerpt.txt) preserves that ordering.
No retained wave program counter identifies a particular instruction.

The launch grid is `(512,32,1)` and its block is `(512,1,1)`.
The AQL work-item grid is therefore `(262144,32,1)`.
At block `(0,1,0)`, the old row calculation predicts the recorded fault address:

```text
input base                     0x4027F84000
old stride                     262144 * 512 * 4 = 512 MiB
predicted address              0x4047F84000
first recorded fault page      0x4047F84000
correct stride                 512 * 512 * 4 = 1 MiB
```

The final code reads hidden block counts at kernel-argument offsets 160 and 164.
The [compiled excerpt](fixed-softmax-geometry-excerpt.txt) shows those loads.
The old compiled geometry control reports 11,130 incorrect records across three cases.
The new control reports zero mismatches on the same cases.
The final expanded control adds 1D and 2D cases and checks logical tails.

## Final candidate results

All runs use the journal KMD, enabled runtime and UMA 8192 MiB.
The GPU clock ceiling is 2000 MHz, idle clock 500 MHz and TdrDelay 10 seconds.
The desktop remains on the GPU route.
Each trial has a 170-second host bound and a shorter supervisor deadline.
The maximum sampled temperature in the final trials is 64.8 C.

| Control | Result |
|---|---|
| vadd | Two passes, 65,536 values each, zero mismatches |
| Geometry | Five cases, 11,410 physical threads, 1,436 logical-tail threads, zero mismatches or damaged canaries |
| Live builtins | 64 threads across two waves, zero failures |
| hipbench | Completed, zero API failures |
| HIPBLAS | 532 checks, zero failures. Deliberate numeric corruption detected |
| Half/block | 108 CPU checks and 117 GPU checks, zero failures. Blocks of 32, 64 and 256 threads |
| TinyLlama Q4_0 | pp512 1868.00 t/s. tg128 122.31 t/s. One repetition |
| TinyLlama greedy text | 16 tokens, identical bytes to the CPU control |
| Gemma 12B, ub512 | Model allocation refused. No benchmark result |
| Gemma 12B, ub128 | Same allocation refusal. No benchmark result |

The [trial summaries](trials.json) include deadlines, cleanup results and sampled temperatures.
Original output and health records are beside this document.
The deliberate old-header failure is a negative control, not a passing workload.
Each final arm's separate health gate also found no GPU fault, fence timeout or Windows recovery event.

The [text comparison](text-comparison.json) records the exact output hash.
The prompt is `The capital of France is`, with temperature 0 and seed 1234.
This is a correctness control for that output, not a general model-quality assessment.
Benchmark rates are single-run observations, not a matched performance comparison.

## Other header corrections

The review covers implemented mappings against ROCm 6.2 headers and the LLVM AMDGPU ABI.
It fixes byte permutation, atomic scopes, floating-point atomic exchange, masked ballots and cooperative geometry.
It also fixes half NaN behavior, direct binary64 conversions and packed bfloat alignment.
Reachable unsupported device printing and sleeping now fail during compilation or linking.

The complete runtime builder passes its host gates, including 41 compiled builtin witnesses.
It reproduces enabled runtime SHA-256 `B0FEB14BD34797DFBB866B504C13859AD0BD15935C62D3126AF019CA3311F106`.
The device controls and all 139 objects of the final llama.cpp backend use the same nine header inputs.
The backend's existing unsupported-specialization trap remains in place. Only its diagnostic print is omitted.

See [the device contract](../../../compute/hip/DEVICE-CONTRACT.md) for supported behavior, commands and limitations.
The [artifact hashes](artifact-sha256.json) and [header hashes](header-sha256.json) identify the tested inputs.
Runtime and KMD sources descend from journal commit `1186a757`, based on train commit `f725e7ec`.
The device-header change is committed with this record.

## Limits and remaining work

Both Gemma controls request a 6,960,120,064-byte model buffer.
`D3DKMTCreateAllocation2` returns `0xC000000D`. The runtime reports an unknown allocation error.
Reducing the microbatch does not reduce that buffer.
These runs do not prove a physical-memory shortage or identify the Windows branch that refused it.
They do not establish the dense-model gate of BD-114 through HIP.

Code-object metadata has no header revision marker, so a generic old-header detector is unavailable.
The known bad backend is refused by the candidate staging check. Artifact hashes remain necessary.
This is not a general runtime rejection of every stale code object.
The loader's missing required-workgroup-shape check remains outside this fix.
None of the scanned shipped objects declares that attribute.

The journal changes shared KMD submission bookkeeping as well as HIP recording.
The final release still needs its graphics regression suite on the exact installed package.
Full dumps and machine metadata remain outside the repository.
The [collection record](collection.json) states which records were selected.
