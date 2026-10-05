# Provenance

One file, unmodified, from the Linux kernel, tag **v6.18**, commit
`7d0a66e4bb9081d75c82ec4957c50034cb0ea449`, taken 2026-09-21 from the local checkout
(`<BC250_ROOT>\ref\linux-src`, see `linux-src.PROVENANCE.txt` next to it). Same tag and same commit as
`driver/amdgpu-import/PROVENANCE.md`, on purpose: the caps blob and the imported IP-block code have
to describe the same hardware through the same structures, and a second tag would be a second
answer.

Taken with `git show`, so the bytes are the kernel's own, with LF line endings and no checkout
normalization:

```
git -C <linux checkout> show v6.18:include/uapi/drm/amdgpu_drm.h > driver/contract/third_party/amdgpu_drm.h
```

| File | Path in the kernel tree | Tag | Commit | License notice in the file |
|---|---|---|---|---|
| `amdgpu_drm.h` | `include/uapi/drm/` | v6.18 | `7d0a66e4bb90` | Full MIT permission notice. Copyright 2000 Precision Insight, Inc.; 2000 VA Linux Systems, Inc.; 2002 Tungsten Graphics, Inc.; 2014 Advanced Micro Devices, Inc. No SPDX tag in the file |

SHA-256 of the imported bytes, which is also the SHA-256 of `git show v6.18:include/uapi/drm/amdgpu_drm.h`:

```
ae82c452d94073cc5f483a001a42bf72b81ed4e915c1e52578ae5ba38fa589fa
```

52140 bytes, LF throughout, 0 CRLF. Checked after the import rather than assumed: a first attempt
through PowerShell's `Out-File` rewrote every line ending and produced a different digest.

## Why the whole file and not the three structures

`driver/contract/bc250_umd_private.h` needs `struct drm_amdgpu_info_device`,
`struct drm_amdgpu_memory_info` and `struct drm_amdgpu_info_hw_ip`. Copying those three out would
be retyping AMD's interface into ours, which is what `driver/README.md` forbids and what the whole
`driver/amdgpu-import/` arrangement exists to avoid. The header is 1521 lines, has exactly one
`#include`, and the unused parts cost nothing but bytes.

## What the header needs, and what supplies it

`amdgpu_drm.h` uses two things from `drm.h`: the `__u8`/`__u16`/`__u32`/`__u64`/`__s32` typedefs,
and `DRM_COMMAND_BASE` with `DRM_IO`/`DRM_IOW`/`DRM_IOWR`, which appear only in the
`DRM_IOCTL_AMDGPU_*` defines at `amdgpu_drm.h:62-90`. Nothing else, verified by grep rather than
assumed: there are no `__attribute__`s, no `__packed`, no `DECLARE_FLEX_ARRAY` and no other
`struct drm_*` from outside the file.

The kernel's real `drm.h` is not imported. It carries a BSD/GNU portability arm that reaches for
`<sys/types.h>` and `<sys/ioccom.h>`, neither of which exists under MSVC. Instead
`driver/contract/uapi-shim/drm.h` is **ours**, supplies those seven names and nothing else, and says
so in its first line. The ioctl macros there expand to an undefined token, so a translation unit
that tries to *use* a `DRM_IOCTL_AMDGPU_*` fails to compile - which is correct, because this driver
never issues a DRM ioctl.

Quote-include resolution puts the shim on the path without shadowing anything: the compiler looks
first in this directory, finds no `drm.h`, and falls through to `-I driver/contract/uapi-shim`.

## Proposal: where this should live when it is committed

This copy sits under `driver/contract/` because that directory was the assignment's boundary. It
belongs with the other kernel imports. The proposed move, for whoever commits it:

- `third_party/linux-uapi-drm/amdgpu_drm.h`, with the row above appended to that directory's own
  `PROVENANCE.md` (next to `third_party/linux-amdgpu/`, which already holds the register headers).
- `driver/contract/uapi-shim/drm.h` stays where it is: it is our code, not an import.
- The two `/I` paths in `driver/contract/test/run.ps1` follow.

Nothing else changes; `bc250_umd_private.h` includes it as `"amdgpu_drm.h"` either way.

## Licence

MIT, and MIT only. The blob header, the shim and the test are ours and carry the repository's
PolyForm-Noncommercial tag. A file that embeds `struct drm_amdgpu_info_device` by value does not
become a derived work of the kernel's GPL parts: `include/uapi/` is the userspace interface, this
header carries the MIT notice in its own text, and no GPL-2.0 header is reached from it.
