# Pending release-notes lines: Direct3D 10.0 programs (BD-081)

This file is not a release. It holds the tester-facing lines for the next tester release notes. No driver file and
no default changes: the change is a correction of what the earlier notes and the router documentation said, a
measurement and one new test program. The release step copies the lines into
`docs/testing/release-notes/<version>-tester.N.md` and deletes this file.

## Corrections

- Direct3D 10.0 programs use the GPU driver like Direct3D 11 programs. The notes of 0.7.213.102-tester.17 said that
  they still get the processor driver and feature level 10_0. That was wrong on Windows 11: Windows opens our driver for a Direct3D 10.0 program in the
  same way as for a Direct3D 11 program, so the setting `AppRouter` `Mode` (default `gpu-default`) and the lists
  `Allow` and `Deny` decide for both. This applies to 64-bit and to 32-bit programs.

## Known issues

- No Direct3D 10.0 game has run on an installed package yet. The measurement is one test program on a computer with
  another GPU. The check on the lab computer is open.

## For testers

- `d3d10probe.exe` (in the source tree, `tools/win/d3d10probe`) draws one triangle through Direct3D 10.0, checks
  every pixel and says which driver drew it. `--trace-entry` also shows how Windows opened the driver.
