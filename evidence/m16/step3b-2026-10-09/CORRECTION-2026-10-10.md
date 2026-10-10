# Correction, 2026-10-10: which witnesses this directory keeps, and which it does not

`docs/01-evidence-rules.md:56` says: "Never edit afterwards. A correction is a new file." This is
that file. `README.md` and every raw file of this directory stay as the session wrote them.

## Why this file exists

`README.md` states per-arm exit codes, wall times, Tctl ranges, plug watts, the thread policy of
each part-3A run, the STOP-flag read and the overlay messages. Those values were printed by the arm
wrapper to the session's ssh console, and the record keeps only each program's own output. They are
therefore author-recorded, not reconstructible from the files here. The local verification of the
same session, `scratch/m16-hip/lab/VERIFY-2026-10-09-3b.md` section 3, already said so. The
exported record did not carry that qualification, which the independent audit of 2026-10-10 names
as finding HIP-F4.

## What cannot be recomputed from this directory

- The per-arm exit codes (0 and 0xC0000409) and the per-arm wall times (4.1, 9.5, 3.1, 13.1, 27.7,
  23, 9.5 s and 27.2 / 24.2 s).
- The per-arm Tctl minima and maxima. The ring snapshots kept here hold DPM readings of 59.1 to
  64.6 C, including the 64.6 C downshift at ring timestamps 12635.095 to 12635.742, and the ring
  headers report wrap loss. Those readings support sub-87-C operation and the 64.6 C observation,
  not a per-arm minimum or maximum and not continuous compliance.
- Which environment each of the three part-3A `hipthreads` runs had. The program does not echo its
  policy, and the three files are named `pre-3a-threads`, `-build1` and `-build1b`.
- The plug telemetry (82.8 to 89.5 W, 0.589 to 0.628 A, 239.2 to 240.1 V, four samples). No raw
  DPS values, query time or measurement age exist in this directory or under
  `scratch/smartplug/`.
- The STOP-flag read, the two overlay messages, the absence of a held ssh session, and the 31.0 s
  of the model push.

## What this directory does witness

- Arm (a): `3b-a-gemm-20261009T193922Z.out.txt` ends with `expected dispatches 43` and
  `532 checks, 0 failed`, with a negative-control matrix whose 33 elements differ.
- The llama.cpp arms stop with `hipErrorNotSupported` at `ggml_cuda_kernel_launch`, and the three
  error streams are byte-identical.
- The Vulkan reference of the same session, `vk-64.txt`, 251 bytes, SHA-256
  00D338AEC0070BEF86CC8D7D113CBB2D874B8082892E0EDF08DC307D3D55100E.
- `artifact-hashes-after.txt` holds 42 `ok` rows and no mismatch, which verifies the retained
  verifier's own run. It prints eight hash characters, so it is not a fresh digest of the 42 files.
- The counter deltas of the session: 11029 submitted and 10806 completed over the interval, with
  the gap growing by 223. 810304 is the last submitted fence value, not the completed count, which
  is 803338.

## What later sessions must keep

The arm wrapper's own console lines (`arm <name> env ...`, `arm <name> exit <code> after <n> s`,
`arm <name> Tctl during ...`, the clock lines) and the plug samples belong in the exported raw
directory beside each program's stdout and stderr. The committed wrappers under `compute/hip/lab/`
write those lines into a per-arm JSON report for exactly this reason. No raw file of this
directory is manufactured to fill the gap.
