# Provenance of driver/tpm

The code in this directory is project code under the licence in its SPDX lines, with these exceptions and inputs:

- `core/posix/signals.c`: the bodies of the power and command signal functions of libtpms
  `src/tpm2/TPMCmd/Simulator/src/TPMCmdp.c` (BSD-3-Clause, https://github.com/stefanberger/libtpms, commit 165c303),
  without the TCP server.
- libtpms (BSD-3-Clause), https://github.com/stefanberger/libtpms commit 165c303, is a build input of `core/build.ps1`
  and is not in this repository. It is linked into `tpmcored.exe` without changes.
- OpenSSL 3.5.9 (Apache-2.0), https://www.openssl.org, built as a static `libcrypto.a`, is a build input of
  `core/build.ps1` and is not in this repository.
- The CRB register map and bit values in `crb.h` follow edk2 `MdePkg/Include/IndustryStandard/TpmPtp.h`
  (BSD-2-Clause-Patent, https://github.com/tianocore/edk2, master as fetched 2026-11-13, SHA-256 CF35F84D). No edk2 code
  is copied.

## Recorded deviations

- `core/build.ps1` builds with llvm-mingw clang, not clang-cl: libtpms needs POSIX-like headers. The stand-ins in
  `core/posix/` cover the rest. `posix/regex.h` never matches, so runtime profiles other than the default are not
  available.
