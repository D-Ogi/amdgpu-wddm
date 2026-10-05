# fwinfo - the headers of AMD amdgpu firmware containers

`amdgpu/cyan_skillfish2_*.bin` is a header followed by a payload. The driver reads four
numbers out of that header - where the payload starts, how long it is, and for the MEC
where the jump table sits inside it - and hands the result to the PSP. If one of those
numbers is wrong, the PSP is fed something that is not microcode, and on this part that
looks exactly like "the PSP refused the command for reasons of its own".

This tool reads the same numbers here, on the development PC, before any of it reaches
the hardware. It also answers the question that comes up every time somebody drops a new
firmware file into `C:\BC250\firmware\`: is this the same microcode that amdgpu loaded on
unit A?

The blobs themselves are not in this repository (`.gitignore`: `*.bin`). They live under
`<BC250_ROOT>\ref\linux-firmware\amdgpu\` (`BC250_ROOT` is the workspace root, by default the
parent directory of this repository). Versions are facts about public files and are
printed here; payload bytes never are.

## Usage

```
python tools/fwinfo/fwinfo.py dump <file...> [--json] [--kind gfx|rlc|sdma|psp|ta]
python tools/fwinfo/fwinfo.py check <file...> [--kind ...] [--strict]
python tools/fwinfo/fwinfo.py versions <dir>
python tools/fwinfo/fwinfo.py compare <dir> <amdgpu_firmware_info.txt>
python -m unittest discover -s tools/fwinfo
```

`dump` prints every field of the header, `--json` for a machine. `check` validates the
header against the file and exits non-zero when a rule is broken. `versions` is one line
per `.bin` file in a directory. `compare` matches those versions against a Linux
`amdgpu_firmware_info` debugfs dump (the ones in `evidence/linux/`) and exits non-zero on
a disagreement.

The container does not say which IP it belongs to; the file name does, the same way
amdgpu asks for a file by name and then calls the matching `amdgpu_ucode_print_*_hdr()`.
`--kind` overrides that for a file with an unusual name. A file whose name says nothing
gets its common header read and a note saying so.

The struct layouts are one table at the top of `fwinfo.py` (`STRUCTS`): base struct, field
name, field width, nested exactly as the kernel nests them. There is no offset written
down anywhere - a field's offset is the sum of the widths in front of it. Adding a header
version is one entry in `STRUCTS` and one in `KIND_STRUCTS`.

## What `check` validates

| Rule | Severity |
| --- | --- |
| the file is at least a `common_firmware_header` (32 bytes) | error |
| `size_bytes` equals the file size | error |
| `header_size_bytes` is between 32 and the file size | error |
| `header_size_bytes` equals the size of the kernel struct for the declared version | error |
| `ucode_size_bytes` is not zero | error |
| `ucode_array_offset_bytes` is at or after the end of the header | error |
| `ucode_array_offset_bytes + ucode_size_bytes` is within the file | error |
| `jt_offset + jt_size` (dwords) is within the payload, where the type has a jump table | error |
| the other (offset, size) pairs - the rlc reg lists, the PSP/TA descriptors - are within the file | error |
| the `psp_fw_bin[]` / `ta_fw_bin[]` entries of a v2.0 header are within the file | error |
| `crc32` matches the payload | note, `--strict` makes it an error |

A zero `jt_size` means "no jump table" and is not an error: the rlc of this part carries
`jt_offset 0, jt_size 0`. The units come from `amdgpu_ucode.c`
(`amdgpu_ucode_init_single_fw`: `jt_size * 4`, `ucode_array_offset_bytes + jt_offset * 4`),
so both fields are in dwords relative to the start of the payload.

## What `check` does not validate

- **Nothing about the payload itself.** The bytes are microcode for a processor we do not
  emulate. A file with a perfect header and random payload passes.
- **No signature check.** These files are signed; the PSP verifies them and we cannot.
  `check` passing says nothing about whether the PSP will accept the image.
- **`ip_version_major/minor`** is printed, never checked. Nothing tells us from the file
  alone which IP version the running part wants.
- **Which file is which.** `check` believes the file name about the kind. A
  `cyan_skillfish2_rlc.bin` that actually holds an sdma header fails on the struct size,
  but two files of the same kind swapped for each other pass.

## The crc32 field

`struct common_firmware_header` calls it "crc32 checksum of the payload". The kernel never
verifies it: the only place the field is read is `amdgpu_ucode_print_common_hdr()`
(`amdgpu_ucode.c:50`), a `DRM_DEBUG` print. The only header validation the kernel does is
`amdgpu_ucode_validate()` (`amdgpu_ucode.c:513-522`), and that is `fw->size ==
hdr->size_bytes` and nothing else. So the kernel source does not say which bytes the field
covers or which CRC variant it is, and a mismatch cannot break anything upstream. That is
why `check` reports it as a note rather than an error.

The covered range was found here instead, by trying thirteen 32-bit CRC parameter sets and
four other checksums over six candidate ranges of each of the eight files. Exactly one
combination matches, and it matches all eight:

> `crc32` is a plain CRC-32/ISO-HDLC (the zlib one: polynomial `0x04C11DB7` reflected,
> init `0xFFFFFFFF`, reflected in and out, final xor `0xFFFFFFFF`) over
> `file[32:]` - every byte after the common header, to the end of the file.

Note what that is not: it is not the payload alone. It also covers the version-specific
tail of the header (`ucode_feature_version`, `jt_offset`, `jt_size`, the rlc reg-list
fields), which is why the boundary is at 32 and not at `header_size_bytes`. The eight
files carry three different header sizes (44, 48, 104), so the three candidates cannot be
confused with each other here. Since all eight agree, a mismatch on a file in
`C:\BC250\firmware\` means the file was edited or is corrupt - worth acting on, even
though the kernel would not notice.

## PSP and TA containers

There is no `cyan_skillfish*_sos.bin`, `_asd.bin` or `_ta.bin`: this APU has no PSP
firmware file of its own, its PSP firmware comes from the platform. The
`psp_firmware_header_v1_0..v1_3/v2_0/v2_1` and `ta_firmware_header_v1_0/v2_0` layouts are
in the table anyway, copied field for field from the kernel header, and are exercised by
synthetic containers in `test_fwinfo.py`. They have never been run against a real AMD PSP
file. If one ever turns up here, read its `dump` output with that in mind.

## Findings on the BC-250 files

`ref/linux-firmware__WARN-AMD-blobs-never-commit/amdgpu/`, eight files, 2026-09-21.

```
$ python tools/fwinfo/fwinfo.py versions $env:BC250_ROOT\ref\linux-firmware\amdgpu
file                       struct                     hdr  ucode_version  feature  ucode/file
cyan_skillfish2_ce.bin     gfx_firmware_header_v1_0   1.0  0x00000025     32       263040/263296
cyan_skillfish2_me.bin     gfx_firmware_header_v1_0   1.0  0x00000063     32       263168/263424
cyan_skillfish2_mec.bin    gfx_firmware_header_v1_0   1.0  0x00000090     32       268336/268592
cyan_skillfish2_mec2.bin   gfx_firmware_header_v1_0   1.0  0x00000090     32       268336/268592
cyan_skillfish2_pfp.bin    gfx_firmware_header_v1_0   1.0  0x00000094     32       263168/263424
cyan_skillfish2_rlc.bin    rlc_firmware_header_v2_0   2.0  0x0000000D     0        25088/25344
cyan_skillfish2_sdma.bin   sdma_firmware_header_v1_0  1.0  0x00000034     50       33536/33792
cyan_skillfish2_sdma1.bin  sdma_firmware_header_v1_0  1.0  0x00000034     50       33536/33792
```

```
$ python tools/fwinfo/fwinfo.py check $env:BC250_ROOT\ref\linux-firmware\amdgpu\cyan_skillfish2_*.bin
cyan_skillfish2_ce.bin: OK (gfx_firmware_header_v1_0)
cyan_skillfish2_me.bin: OK (gfx_firmware_header_v1_0)
cyan_skillfish2_mec.bin: OK (gfx_firmware_header_v1_0)
cyan_skillfish2_mec2.bin: OK (gfx_firmware_header_v1_0)
cyan_skillfish2_pfp.bin: OK (gfx_firmware_header_v1_0)
cyan_skillfish2_rlc.bin: OK (rlc_firmware_header_v2_0)
cyan_skillfish2_sdma.bin: OK (sdma_firmware_header_v1_0)
cyan_skillfish2_sdma1.bin: OK (sdma_firmware_header_v1_0)
8 file(s), 0 with errors
```

```
$ python tools/fwinfo/fwinfo.py compare $env:BC250_ROOT\ref\linux-firmware\amdgpu \
      evidence/linux/2026-09-21-E01-diagusb-run-001/amdgpu_firmware_info.txt
file                       label  ucode_version  feature  vs the dump
cyan_skillfish2_ce.bin     CE     0x00000025     32       match
cyan_skillfish2_me.bin     ME     0x00000063     32       match
cyan_skillfish2_mec.bin    MEC    0x00000090     32       match
cyan_skillfish2_mec2.bin   MEC2   0x00000090     32       match
cyan_skillfish2_pfp.bin    PFP    0x00000094     32       match
cyan_skillfish2_rlc.bin    RLC    0x0000000D     0        match
cyan_skillfish2_sdma.bin   SDMA0  0x00000034     50       match
cyan_skillfish2_sdma1.bin  SDMA1  0x00000034     50       match
8 file(s), 0 disagreeing with amdgpu_firmware_info.txt
```

What that says:

1. All eight headers are well formed, and all eight `crc32` fields match (no notes). Three
   header versions between them: gfx v1.0, rlc v2.0, sdma v1.0.
2. Every version matches what amdgpu reported on unit A. The files we would feed the PSP
   are the files amdgpu fed it, byte for byte as far as the headers can tell.
3. `cyan_skillfish2_mec.bin` and `cyan_skillfish2_mec2.bin` are the same file twice, byte
   for byte (same SHA-256, same `crc32` `0x3BB301C1`). MEC1 and MEC2 run the same
   microcode, which is why the driver loads the same image under two firmware types.
   `cyan_skillfish2_sdma.bin` and `cyan_skillfish2_sdma1.bin` are not: same versions and
   same sizes, different payloads and different `crc32`. The two SDMA engines get
   different microcode, and swapping the two files would pass every check in here.
4. The payload starts at byte 256 in all eight files, while the headers end at 44, 48 or
   104 - so each file carries 212, 208 or 152 bytes of padding in between. Padding, not
   data: `ucode_array_offset_bytes` is what the driver must use, not `header_size_bytes`.
5. The MEC jump table is `jt_offset 66860, jt_size 224` dwords, and
   `66860 + 224 == 268336/4` exactly - the jump table sits at the very end of the payload.
   The split `bc250_fw_locate()` makes for `CP_MEC1` and `CP_MEC1_JT` therefore covers the
   payload exactly once, with nothing left over and nothing counted twice.
6. The rlc has no jump table and no reg lists: every field after `ucode_version` is zero,
   and `RLC feature version: 0` in the dump agrees. The whole v2.0 tail is dead weight on
   this part, which matches the driver loading `RLC_G` as one image.

## Provenance

The structure definitions and the meaning of every field follow
`drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h` and `amdgpu_ucode.c` of the Linux kernel
(MIT, Copyright Advanced Micro Devices, Inc.), through `ref/linux-src` and the slice
already imported into `driver/shim/include/amdgpu_ucode.h`.
