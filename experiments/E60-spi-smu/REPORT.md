# SPI SMU work report

## Update: installed P3.00, 2026-11-16

The target decision below is superseded: the experiment uses the installed P3.00 backup,
SHA256 09fb260f498bbce5d2e67729d9b6d35d4a75ee833be52f32d22bc84cd2693103.
No hardware was accessed for this update. The private input and detailed reports remain outside this repo.

The S1 tool now extracts header, body and signature components through PSPTool, reassembles both SMU
entries, retains opaque image bytes and reparses the result. Full-image equality passes on the installed
backup. This tests the selected plaintext-entry rebuild path, not every enclosing firmware container.
Nineteen tests pass on that backup and nineteen on the existing P5 regression input. The controls reject
changed component bytes, component gaps/overlaps, missing tails, duplicate entries and encoded entries.

Entry selection, actual runtime mapping, complete ABI/free-space proof and an inert S2 change remain open.
The older report below is retained as historical context; its span-only S1 and undecided target describe
the earlier tool revision. The earlier reconciliation was committed as 4bdb56e6e.

## Done

- Added read-only image inspection and in-memory patch planning in tools/spi-smu/.
- Added nine unit tests for bounds, identity, expected bytes, overlap, reconstruction, and byte differences.
- Inspected both P3.00 and P5.00 stock images with the installed PSPTool.
- Recorded exact dependency versions, parser source hashes, image hashes, entries, flags, and authenticated ranges.
- Checked all four stock SMU signatures with their recorded keys.
- Checked sixteen negative signature controls in memory. All failed verification as expected.
- Rejoined image slices at SMU span boundaries; this checks span arithmetic only. Both matched stock byte for byte.
- Added a recipe planner that checks one entry and emits a review manifest without saving firmware.
- Preserved original inputs. No lab access, firmware write, SPI write, or push occurred.

Reports: [P3](p3-offline-reconciled.json), [P5](p5-offline-reconciled.json).
These describe offline files. They do not establish hardware behavior or approve candidates.
The tool README gives commands and limits.

## Needs the owner

Select the target BIOS and the intended trial entry after selection analysis.
The plan names P5.00. The review reports P3.00 on unit A.
Authorize each later candidate write, restoration write, and experimental message by identity.

The initial implementation is in commit acdea3d4b. Reconciliation changes are uncommitted.

## Needs the lab

Identify the installed image, flash part, protection state, and programmer configuration.
Obtain matching programmer backups and preserve them in two locations.
Establish stock controls, independent thermal monitoring, and physical recovery.
Capture entry selection or fallback evidence if static analysis cannot establish it.
Run S2, S3, and S4 only after their separate gates pass.
Restore stock and repeat controls after each trial.

## Open offline work

PSPTool cannot parse the boot-loader header in either image.
Enclosing integrity checks and active-entry selection remain unresolved.
Exact-image ABI analysis, free-space ownership, and an inert S2 byte remain unresolved.
The REA firmware inspection call required approval that this session cannot grant.
No fresh disassembly or assembly ran. No S2, S3, or S4 candidate was approved.
The historical handler log uses callx8 and retw.n, unlike the source plan's callx4 sketch.
Its binary has 134 bytes. Subtracting the 32-byte literal pool leaves 102 bytes.
This arithmetic does not establish ABI correctness or code ownership.

The VCN sequence still needs named registers, masks, and primary-source semantics.
The default GC register lookup does not resolve the proposed UVD status names.
That lookup does not establish their absence from the VCN IP headers.
The repository bans UVD/VCN window access. No proposed read overrides that ban.

## Validation

Thirteen tests passed with the P5 image, including four integration tests. Both stock-image checks completed.
The reports record S1 equality and the signature control results.
No hardware facts were added.

