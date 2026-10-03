# Tester release package

Builds the folder and zip that external testers run on their own ASRock BC-250 (Windows 11 x64). The tester guide
is `docs/testing/INSTALL.md`; it is copied into the package as `INSTALL.md`.

| File | Purpose |
|---|---|
| `new-release-cert.ps1` | Creates the release test-signing certificate once, in `<BC250_ROOT>\secrets\release` (never in the repo or the package). Separate from the lab certificate. |
| `release-sources.json` | The registered lab artifacts that make up the release, with SHA256. The build refuses any other byte. |
| `build-release.ps1` | Copies the sources, re-signs the KMD (.sys signature, new catalog via Inf2Cat), writes `manifest.json`, zips. Gates: source hashes, signer, no key material, scripts parse under PowerShell 5.1. |
| `installer\` | What the tester runs: `install.cmd`, `uninstall.cmd`, `verify.cmd` (package root) and the PowerShell 5.1 scripts. |
| `test-parse51.ps1` | Gate: every script parses under Windows PowerShell 5.1. |
| `test-dryrun.ps1` | Host test on a PC without a BC-250: install and uninstall dry runs refuse cleanly and change nothing; `-DryRunIgnoreBoard` walks every phase. Never run the real install on a development PC. |

```
pwsh -File tools\release\new-release-cert.ps1                 # once
pwsh -File tools\release\build-release.ps1 [-ControlApp <dir> -ControlAppExe <exe>]
pwsh -File tools\release\test-dryrun.ps1 -Package <BC250_ROOT>\scratch\release\out\amdgpu-wddm-tester-<version>
```

v0 ships the registered binaries as they run on the lab (KMD 0.7.197.1 re-signed, code unchanged); a rebuild from
the exact commits with `/Brepro` is planned for v1. Install paths differ from the lab's: everything under
`%ProgramFiles%\amdgpu-wddm`, except the firmware, which the KMD reads from the compiled-in `C:\BC250\firmware`.

PROVENANCE: linux-firmware `amdgpu/cyan_skillfish2_*.bin` at 2b8daaf611fbade74f26a5b58ec1defe6a02f5e0, redistributable per `LICENSE.amdgpu` (shipped with the files in the package, never committed).
