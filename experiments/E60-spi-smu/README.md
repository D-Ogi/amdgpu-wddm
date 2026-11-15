# E60: SPI SMU preparation

Status: not run. Offline inspection now covers P3 and P5. Candidate release remains gated.
The source plan and review have identities in [inputs.json](inputs.json).
The original files remain unchanged outside this worktree.

## Hypotheses

H1: The boot chain executes a modified SMU body from SPI.
H2: An accepted handler changes VCN power state without destabilizing the unit.
Successful boot alone does not establish H1. Handler completion alone does not establish H2.

## Target decision

No target is selected. The plan names P5.00. The review reports P3.00 on unit A.
Neither report establishes the installed image identity for this experiment.
The owner must select the installed-image route or a separate BIOS upgrade experiment.
Do not mix P5 SMU entries into P3. Do not substitute another image.
S0 requires the selected image and a later comparison with matching programmer reads.

## Review disposition

| Review item | Disposition |
|---|---|
| Target mismatch | Open. No candidate release before the target decision. |
| Entry selection | Open. Equal versions cannot distinguish the two entries. |
| P5 ABI | Open. Derive helpers, dispatch table, and completion semantics from the selected image. |
| Handler length | Input length is 134 bytes. A 32-byte literal pool leaves 102 bytes. |
| PCI transport | Do not describe the whole transport as rejected. The cited mailbox log needs evidence review. |
| Flash protection | Record status registers, protection bits, WP pin state, and expected values before any write. |
| Verifier control | Require acceptance of S0 before interpreting rejection of S2. |
| Marker | Fix queue, message, argument, result, and finite timeout before review. Invoke once. |
| Fallback | Require an independent entry-selection readout. Equal version values are insufficient. |
| Programmer identity | Record model, firmware, software, cable, clip, voltage, and isolation. |
| Thermal monitor | Select an independent sensor and tool. Check availability before any candidate trial. |
| Restoration | Require full readback equality and a stock boot with the same control readouts. |

The review's inclusive code range also spans 102 bytes. This arithmetic does not establish instruction boundaries.
The old 125-byte estimate is not an artifact specification.
The delivered handler has not passed an independent ABI or power-sequence review here.
Plaintext and compression flags do not prove that no enclosing authentication checks exist.

## Offline continuation gates

1. Select the target and record its full hash.
2. Pin the parser, verifier, disassembler, and assembler revisions.
3. Parse all directories and export both entries with headers.
4. Record lengths, load addresses, flags, authenticated ranges, keys, and enclosing checks.
5. Compare both bodies and establish entry selection separately from header interpretation.
6. Establish the exact ABI and code-area ownership, including stack and relocation conflicts.
7. Derive named registers with regcalc and review masks and sequencing against primary sources.
8. Preserve the UVD/VCN access ban in docs/hardware.md.
9. Stop if any gate remains open.

S1 must reproduce S0 byte for byte through the actual extraction and rebuild path.
A copy of S0 does not test that path.
S2 requires a justified inert change within an authenticated range and rejection by the controlled verifier.
Preserve its signature. Change only independently documented non-security checksums.
Prepare one entry per candidate. Do not patch both entries together.
S3 requires a harmless marker and disassembly at the actual load address.
S4 requires S3 execution evidence and a complete power-sequence review.
Do not prepare S2 through S4 while these gates remain open.

## Future run sheet

All fields below remain unset. This is not an executable lab procedure.

| Field | Required record |
|---|---|
| Unit and boot | Board revision, installed image hash, OS build, UEFI mode, cold or warm history |
| Recovery | Flash part, capacity, voltage, isolation, protection state, physical recovery owner |
| Backup | Two matching complete programmer reads, hashes, and two independent storage locations |
| Tools | Exact versions and reviewed commands for capture, programming, and restoration |
| Candidate | Full image hash, changed ranges, selected entry, and selection rationale |
| Transport | Single serialized owner, stale response handling, completion semantics, finite timeout |
| Marker | Queue, message, argument, unique return value, and stock negative control |
| Observation | Boot deadline, idle window, telemetry interval, and independent sensor tool |
| Consent | Exact candidate hash, stock restoration write, and named experimental invocation |

Do not start at or above 87 C. Stop at 97 C or at 92 C for 10 seconds.
Stop if monitoring fails, the fan stops, or the mailbox fails.
Do not retry a timeout. Do not send a reset message.
Do not use raw SMN writes into the VCN range or repeat the SRAM canary.
Keep the production allowlist unchanged. Do not load VCN firmware or submit decode work.

## Expected results and evidence

Signature-specific diagnostics support rejection. Silent boot failure establishes only boot failure.
A unique marker with a stock negative control supports modified execution.
Named, reviewed status transitions support a power-state measurement only.
Missing markers and unexplained fallback remain inconclusive.

Future captures belong in evidence/linux/YYYY-MM-DD-E60-spi-smu/ or the corresponding Windows directory.
Keep raw programmer logs, full readback hashes, boot logs, thermal traces, and transport records unchanged.
Preserve backups outside the source tree. Record tool commits and any private-data removal.
Require full stock restoration and a successful control boot after every trial.
Add measurements to the facts graph only when evidence exists.

## Result

No hardware session occurred. No candidate or firmware image was written.
The input manifest records file identities only. It does not establish hardware behavior.
No hardware facts changed.

