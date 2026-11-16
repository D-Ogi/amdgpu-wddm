# Offline SPI SMU tools

These tools read stock files and write JSON reports only.
They never save a modified firmware image or access hardware.
PSPTool provides parsing and signature checking as an installed dependency.
Each report records dependency versions and hashes of the parser source files.

Run the stock checks for each image:

```powershell
python tools/spi-smu/offline.py <stock-image> --report <new-report.json>
python -m unittest discover -s tools/spi-smu -v
```

The report includes directory entries, header hashes, load addresses, flags, and authenticated ranges.
It checks stock signatures before testing four mutations per entry in memory.
It extracts each SMU header, body and signature through the parser, reassembles those components,
preserves opaque gaps, compares the full image and reparses the result. This is the same-size plaintext
SMU rebuild path. It does not reconstruct or check every enclosing firmware container.
Ranges use file offsets and exclude their end position.

Each directory keeps `entries`, the original list of parsed files, and adds `raw_entries`.
The raw list retains records that the parser could not turn into files.
It records the raw location, parser-normalized offset, declared size, parsed range, and coverage status.
`missing` means no parsed file. `partial` identifies an unparsed tail. `complete` means the extents match.
These statuses do not establish authentication or complete interpretation of a file.
Inline soft-fuse records and directory references have status `special`.
BIOS directory records, unresolved slot-relative offsets, and invalid extents have status `unsupported`.
Their metadata remains visible, but ordinary PSP file-range rules do not apply to them.
Directory discovery still depends on PSPTool and is not exhaustive container validation.

To review a future patch recipe:

```powershell
python tools/spi-smu/plan_patch.py <stock-image> <recipe.json> --report <new-report.json>
```

A recipe contains `stage`, `entry_type`, `stock_sha256`, and `edits`.
The stage is S2, S3, or S4. The entry type is 8 or 18.
Each edit contains an absolute `offset` and equal-length hex strings named `before` and `after`.
The planner checks the full stock hash, expected bytes, overlap, bounds, and signature rejection.
It preserves the signature and rejects changes outside the selected authenticated body.
It produces hashes and changed ranges for review. It never marks a recipe ready for release.
Do not use synthetic verifier mutations as S2 candidates.
They have no established inert runtime role.

## Limits

PSPTool reports a boot-loader header parse error on both inspected stock images.
The reports do not prove entry selection, runtime ownership, ABI compatibility, or silicon trust policy.
The tool does not change keys, checksums, signatures, or directory layout.
No assembler or disassembler ran in these checks.
The earlier Ghidra log remains a historical input, not a fresh disassembly.

