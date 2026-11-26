# Provenance of tools/efi/tpm

The code in this directory is project code under the licence in its SPDX lines, with these inputs:

- libtpms (BSD-3-Clause), https://github.com/stefanberger/libtpms commit 165c303, is a build input of
  `engine/build.ps1` and is not in this repository. Its TPM 2 sources are compiled without changes into
  `tpm_engine.lib`.
- OpenSSL 3.5.9 (Apache-2.0), https://www.openssl.org, tarball SHA-256
  603F5602E2EEF00D77FBD429D34DCD5822BB301757A1BC9CDB24C670F1EB859A, is a build input of `engine/build.ps1` and is not
  in this repository. Its libcrypto is compiled without changes into `tpm_engine.lib`, with the target in
  `engine/openssl/bc250-uefi.conf`.
- The libtpms build uses the POSIX stand-ins of `driver/tpm/core/posix` (see `driver/tpm/PROVENANCE.md`).
- edk2 CryptoPkg (BSD-2-Clause-Patent, https://github.com/tianocore/edk2) was read as a reference for the OpenSSL
  configuration of a UEFI target (the `-U_WIN32` set, the `UEFI-x86_64` Configure target). No edk2 code is copied.
