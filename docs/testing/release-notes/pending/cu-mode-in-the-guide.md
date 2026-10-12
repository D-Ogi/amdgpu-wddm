# Pending release-notes lines: the installation guide names the number of graphics cores (BD-077)

This file is not a release. It holds the tester-facing lines for the next tester release notes. The release
step copies the lines into the sections of the same name in `docs/testing/release-notes/<version>.md` and
deletes this file. The headings here are the four sections that `build-release.ps1` requires, so that the
lines move across without a new section name.

## Fixed

- The installation guide now says how many graphics cores an install gives you. The GPU of the BC-250 has 40
  compute units, and an install uses 24 of them, the number the board's firmware switches on. The guide said nothing
  about this, and the switch `-CuMode` was only one name in a list. "What the installer changes" now holds the
  number, the switch `install.cmd -CuMode 40` for all 40 cores, the same choice on the Graphics page of
  amdgpu-wddm Control, and the rule that the first start with 40 cores needs the confirmation of the small blue
  window after logon. The driver itself, the installer and every setting stay as they were.

- The same section said that the maximum GPU clock of a new install is 1500 MHz. The release default has been
  2000 MHz since version 0.7.216.100-tester.27. The guide now says 2000 MHz.

## Known issues

- The number of graphics cores of an install does not change in this version. An install still gives 24 cores,
  and the game results that we publish are measured with all 40. Use `install.cmd -CuMode 40` or the Graphics
  page of the control application to get them.
