# Provenance of the M15.11 encoder MFT sources

All code in this directory is written for this project. Nothing is copied from another codebase.

Normative reference used while writing it: ITU-T Rec. H.264 / ISO-IEC 14496-10, the published standard.
The variable-length code tables of clause 9.2 (coeff_token, total_zeros, run_before), the mapped
Exp-Golomb coded_block_pattern table of clause 9.1.2 and the normative inverse transform, dequantisation
and prediction formulas of clause 8 are facts of the standard, not expressions owned by any
implementation. They are entered here from the standard's tables and checked structurally at build time
by `tests/mfthost.cpp --selftest` (prefix-freeness, Kraft sum, round-trip of every table entry) and
functionally by decoding our own output with the Windows inbox H.264 decoder MFT.

Projects read for understanding but NOT copied from:

- `ref/bc250-encoding-decoding-fix__WARN-GPL-read-only-no-code-import` - GPL-3.0-only. Read for its
  per-stage profile of a compute H.264 encoder on this exact silicon. No code, no shader and no
  table from it is here.
- x264 - GPL-2.0-or-later. Not read for code. Not imported.

Windows contracts come from the SDK headers under the workspace's
`toolchain/nuget/microsoft.windows.sdk.cpp` and from
learn.microsoft.com; every constant used is cited by header and line in the source comments.
