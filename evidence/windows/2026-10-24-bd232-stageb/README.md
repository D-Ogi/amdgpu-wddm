# BD-232 Stage B target result

Date: 2026-10-24. Target: BC-250, Windows 11 build 22631.
Status: MEASURED. Both diagnostic arms passed the frozen analyzer.
This record covers a manually paired shader, not general 16-sample support.

## Artifact identities

The kit archive SHA256 is `2320222152faec251a61e47282f91421c8a98746f3e72ba4761592eddb577802`.
The kit manifest SHA256 is `52b14b82a50b4c2876a7ac16a57d50f4d5260ff8c3d2a50672c52b0744f93d86`.
The diagnostic source commit is `9c5cb1abbc78543d9de3c5ebfd183e5784168e7c`.
Its parent is the perspective fix `d97c4a069a0f6e503cf00b63c0bf61ab227ec739`.
The source patch base is `f8101281935998608b8be2b6608ee4261b8ac1da`.
Clang 23.1.4 built the x64-v3 lab variant with `RADV_S16F8_PROBE=1`.

| File | SHA256 |
|---|---|
| Diagnostic ICD | FA3637B03A50075224AABE02A03D0DE991DEBCF79FF0F239AE1F12BF190DEEC5 |
| Engine | F7AB0CF8C178BBEB6C147570E6627441E9EE09AC21B207ECB57468F1E02B23E3 |
| Client | 6EA29A7985C7522B6823CB9D07E5CAE094D9114291CD0F729F038B69F093A8B3 |
| Analyzer | 5e7efd548f26427a46d28921211edf154c318ab5d4ad3218683467d64adb4fb0 |

Both target receipts show the expected loaded engine and ICD hashes.
The source-parent control in the kit was not part of these two runs.
The kit includes that control with the perspective fix already applied.

## Results

The lab ran each arm serially, without elevation, in a new output directory.
The runner enforced a 170-second process limit.
Neither arm timed out. Both child exit codes were zero.

| Arm | Mode | Logical records | Physical launches | Analyzer errors |
|---|---|---:|---:|---:|
| native8 | diagnostic, off, native, 8 | 26112 | 26112 | 0 |
| paired | diagnostic, exposed16, paired, 8 | 52224 | 49260 | 0 |

Native8 passed 26112 own-offset audits.
Paired passed all coverage, count, guard, interpolation and body checks.
The paired receipt contains 138 driver markers and a zero-mask marker.
Each logical sample ID has 3264 records in the paired result.
The unchanged numerical tolerance is 1e-5.
Full per-field errors and per-arm counters are in each `analysis.json`.

The paired comparison uses the frozen native16 RTX 4090 capture.
The reference uses the same client, engine, geometry and masks.
It is a native16 reference, not evidence of AMD scheduling behavior.

## Reproduction and provenance

The three compressed raw buffers decompress to their original hashes.
The metadata files and analyzer are byte-identical to the source artifacts.
Only module paths in runner receipts were reduced to basenames.
No measurement or module hash was changed.
`provenance.json` records the original receipt hashes and the kit build identities.
`kit-manifest.json` lists the complete kit, including files not copied into this record.

The stored analyzer reproduced all three original analysis objects exactly after export.
The command output paths below must not exist before analysis.

```powershell
python analyze.py native8/capture --output native8-recheck.json
python analyze.py native16-reference/capture --output reference-recheck.json
python analyze.py paired/capture --reference native16-reference/capture --reference-proof native16-reference/analysis.json --output paired-recheck.json
```

The capture commands used these options in the complete kit:

```powershell
.\run-arm.ps1 -OutputDirectory NEW_NATIVE8_DIR -Icd diagnostic -Mode off -NativeSamples 8 -Program native
.\run-arm.ps1 -OutputDirectory NEW_PAIRED_DIR -Icd diagnostic -Mode exposed16 -NativeSamples 8 -Program paired
```

This record omits executable binaries and the original diagnostic log streams.
Their hashes remain in the kit manifest and runner receipts.
Raw buffers contain test records, not process or physical-memory dumps.

## Scope limits

This pass does not establish an automatic compiler transformation.
It does not establish general shaders, arbitrary pre-raster stages or interpolant capacity.
Helper lanes, discard, derivatives, centroid and subgroup behavior remain separate tests.
Ordering-sensitive UAV operations and ROV operations remain separate tests.
Pixel-frequency interactions, ForcedSampleCount and programmable sample positions remain separate tests.
The complete no-output 1/4/8/16 contract remains separate.
Production capability limits do not change through this diagnostic result.
No HLK certification or performance claim follows from this pass.
