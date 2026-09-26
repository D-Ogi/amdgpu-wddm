# tools/firmware

Fetches, verifies and pushes the eight AMD PSP firmware files the KMD loads at runtime from
`C:\BC250\firmware\` on the target (`BC250_PSP_FIRMWARE_DIR`, `driver/kmd/psp.c`). Windows-native: everything
runs here on the development PC with the Python standard library, no pip install, no Linux machine needed
anywhere - "linux-firmware" below is only the name of the upstream project the files come from.

```
python tools/firmware/fetch_firmware.py fetch [--out DIR]     download all eight files, skip if already correct
python tools/firmware/fetch_firmware.py verify [DIR]          check a directory against the manifest
python tools/firmware/fetch_firmware.py push [DIR]            verify, then copy to the lab target
```

`cyan_skillfish2.json` is the manifest: source repository, pinned commit, download URL templates, the
target's `remote_dir`, and each file's name, size and SHA-256. `fetch` and `verify` default to
`<BC250_ROOT>/ref/linux-firmware__WARN-AMD-blobs-never-commit/amdgpu` (`BC250_ROOT` is the workspace root,
by default the parent directory of this repository, same convention as `tools/win/target.py`); `push` copies
from there to the manifest's `remote_dir` through `tools/win/target.py`, comparing remote SHA-256 first so
identical files are not rewritten. `fetch` tries the URL templates in manifest order; the kernel.org
endpoint is the canonical source and the GitLab mirror is the fallback.

PROVENANCE: linux-firmware, amdgpu/cyan_skillfish2_*.bin, redistributable per LICENSE.amdgpu (WHENCE)

The firmware blobs are never committed to this repository (`*.bin` is repository-wide gitignored); the
manifest carries only names, sizes and hashes, never the files themselves.
