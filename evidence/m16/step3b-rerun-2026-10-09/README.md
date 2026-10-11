# M16 step 3 repeat, 2026-10-09

This record covers unit A from 22:36 to 22:45 UTC on 2026-10-09.
It adds the later trial to the [first attempt](../step3b-2026-10-09/README.md).
The evidence was exported offline on 2026-10-11.
The facts are [M858-M861](../../../docs/facts/icd.md#m858).
The [step page](../../../docs/testing/m16-hip-step3.md) separates these historical results from current acceptance.

## Artifacts and method

The runtime source was `6ae2ec97` on `m16/hip-step3`.
The tested runtime was `amdhip64.dll`, 271360 bytes, SHA-256 prefix `6021F358`.
The full build digest recorded with the trial was
`6021F3586903DC7C291BE8E13520ADE47532FD991303AF4512D7B6E1E3EE883A`.
The corrected threads client was `CDADBA9E`, vector control `D5B0F9CC`, BLAS shim `DB972FFB`, and GEMM control `4C5E454F`.
The default ggml HIP image was `1102A1DD`, the no-assembly image `B45DCA6D`, and the Vulkan backend `2E45226E`.
The [verifier before](raw/artifact-hashes-before.txt) and [after](raw/artifact-hashes-after.txt) each print 39 matching rows.
These rows contain short digest prefixes. They do not independently reproduce the full build digest above.

The installed KMD was `181C3E31`, with system Vulkan ICD `4E3F393F`.
The two [state snapshots](raw/state-diff.txt) match across 24 lines after removal of their timestamp line.
That comparison covers the recorded files and settings, not all machine state.
No installation or restart was part of the trial.

Every arm has a retained wrapper console, command, logged environment assignments, exit code and 170 s bound.
The wrapper checked the child every 3 s. Its short wall times include that observation delay.
Logged assignments do not provide an exhaustive inherited environment.
The [derived summary](summary.json) contains all 13 arms and the thread measurements.

## Results and their limits

The vector control and all three thread runs passed.
The default, batch-off, and batch-off/full-barrier runs each completed 13 other-thread operations inside one thread's wait.
Their parallel-to-serialised ratios were 1.029, 0.992 and 0.986, below the client's corrected 1.1 bound.
Each run reports six lifetime-test rounds, zero API failures and zero failed verdicts.
This measures progress during a host wait. It does not establish concurrent execution of GPU dispatches.
The raw client's phrase "host cost" is not a measurement of processor-active time.

The [GEMM control](raw/m16-hip3b--rr-3b-a-gemm-20261009T224250Z.out.txt) reports 532 checks and zero failures.
The default llama.cpp image still failed before benchmark results or generated text.
Its [diagnostic log](raw/m16-hip3b--rr-3b-b-hip-log-20261009T224044Z.err.txt) names an unreadable metadata note for `k_set_rows`.
The runtime then reports `hipErrorInvalidImage`.
The [no-assembly log](raw/m16-hip3b-noasm--rr-3b-d-noasm-log-20261009T224132Z.err.txt) instead names a scratch requirement in `mul_mat_q`.
The runtime reports `hipErrorNotSupported` there.
The same application arms also failed with runtime logging off.
These observations distinguish the refusals. They do not establish the final loader cause or validate scratch support.

The [Vulkan control](raw/m16-vk--rr-3b-b-vk-20261009T224326Z.out.txt) measured pp512 at 2629.57 tokens/s and tg128 at 301.64 tokens/s.
It used TinyLlama 1.1B Q4_0, four threads, full GPU offload and one repetition.
The [text control](raw/m16-vk--vk-64.txt) matches the first attempt's SHA-256:
`00D338AEC0070BEF86CC8D7D113CBB2D874B8082892E0EDF08DC307D3D55100E`.
The [HIP text file](raw/m16-hip3b--hip-64.txt) contains only a three-byte UTF-8 byte-order mark.
Therefore this trial supplies no HIP throughput figure or HIP/Vulkan text comparison.

## Driver observations

The last node-0 summary in the before snapshot has 1343416 submitted and 1332129 completed packets.
The last summary in the final snapshot has 1347336 submitted and 1335856 completed packets.
The deltas are 3920 submitted and 3727 completed. They are not equal.
These counters cover the adapter, not only the trial's processes.
Neither snapshot establishes that every submitted packet completed during the interval.

The retained summaries report zero timeouts, refusals and soft recoveries, with no TDR recorded by those callbacks.
The log headers report wrap loss, so these snapshots are not a complete event history.
No separate Windows event-log export accompanies this record.
The wrapper's clock and thermal readings are samples, not continuous proof of thermal compliance.

## Export and verification

[EXPORT.json](EXPORT.json) identifies each source file by digest and records its exported digest, size and redactions.
The export retains 52 files, including the wrapper consoles missing from the first attempt's public record.
It omits collection-console output, the preliminary directory listing and plug samples because no claim here depends on them.
Redactions replace device-interface instance lines and local compiler source-root paths.
All other bytes, including empty stderr files and the HIP text byte-order mark, are unchanged.
The source files remain untouched.

Run `python verify.py` from this directory to check the digests and the derived counters.
Existing evidence directories were not edited to fill gaps in the earlier session.
