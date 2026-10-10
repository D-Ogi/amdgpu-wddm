# GFX10 image instructions that only AMD's ISA XML describes

AMD documentation, transcribed 2026-10-10. **Nothing on this page is a measurement on unit A.** When the gfx1013
sweep runs one of these instructions on the silicon, its result goes to
[What the gfx1013 silicon really does](gfx1013-isa.md) with a fact and evidence, and its row here gets a link.

## Which sources name them

Eighteen MIMG opcodes of the GFX10 generation have a name and a description in one AMD source only:

| source | what it says about the 18 opcodes |
|---|---|
| AMD machine-readable GPU ISA, package of 2026-08-06, `amdgpu_isa_rdna1.xml` and `amdgpu_isa_rdna2.xml` | Names and describes all 18. The two files give the same text for each of them |
| The same package, `amdgpu_isa_rdna3.xml` | Gives six of the slots (0x42, 0x43, 0x4a, 0x4b, 0x62, 0x63) to other instructions |
| "RDNA" ISA reference guide (25 September 2020, both editions) and "RDNA 2" ISA reference guide (30 November 2020), the public PDF documents | Not in their MIMG opcode tables and not in their text. The RDNA 2 table goes from 97 `IMAGE_GATHER4H` to 128 `IMAGE_MSAA_LOAD` |
| LLVM 22.1.8, AMDGPU assembler and disassembler | Knows none of the 18 names for any target. It refuses the words for gfx1013 |
| AnyPS5, a PS5 shader decoder | Decodes all 18 with the same names ([PS5 decoder leads](research/gfx1013-isa-leads-ps5-decoders.md)) |

The XML package is MIT-licensed, and the licence is in each file. Get it from
<https://gpuopen.com/download/machine-readable-isa/latest/>. The PDF documents are on
<https://gpuopen.com/amd-gpu-architecture-programming-documentation/>. The copies we read have SHA-256 `4c0de2f4`
(RDNA, 268 pages), `8a4ed11e` (RDNA, the older amd.com edition, 297 pages) and `22aec03f` (RDNA 2), first 8
digits.

An entry in AMD's XML does not prove that a chip executes the instruction. The XML describes the ISA of a
family. Desktop drivers and compilers do not emit these opcodes, so no public test of them on a desktop part
exists. PS5 code is the reason to ask: a PS5 decoder carries all 18, so Sony's compiler can emit them.

## The instructions

All quotations are AMD's text from the RDNA1 file, word for word.

| opcode | name | AMD's description |
|---|---|---|
| 0x42 (66) | `image_load_by2` | "Load 2 horizontal elements from the largest miplevel in an image surface and store the result into a vector register. Perform the format conversion specified by the resource descriptor. Illegal for formats with more than 2 components. No sampling is performed." |
| 0x43 (67) | `image_load_by4` | The same with 4 elements. "Illegal for formats with more than 1 component." |
| 0x4a (74) | `image_load_mip_by2` | As `image_load_by2`, but "from a user-specified miplevel" |
| 0x4b (75) | `image_load_mip_by4` | As `image_load_by4`, but "from a user-specified miplevel" |
| 0x52 (82) | `image_store_by2` | "Store 2 horizontal elements from a vector register to the largest miplevel in an image surface. The texel data is converted using the format conversion specified by the resource descriptor prior to storage. Illegal for formats with more than 2 components." |
| 0x53 (83) | `image_store_by4` | The same with 4 elements. "Illegal for formats with more than 1 component." |
| 0x5a (90) | `image_store_mip_by2` | As `image_store_by2`, but "to a user-specified miplevel" |
| 0x5b (91) | `image_store_mip_by4` | As `image_store_by4`, but "to a user-specified miplevel" |
| 0x62 (98) | `image_gather4h_pck` | "Gather all components of 4 texels from a 4x1 row vector on an image surface. Store the result into vector registers. The DMASK selects how many channels to write." |
| 0x63 (99) | `image_gather8h_pck` | The same for "8 texels from a 8x1 row vector" |
| 0x70 (112) | `image_load_pck2` | "Load 2 horizontal elements from the largest miplevel in an image surface and store the result into a vector register. 8- and 16-bit components are zero-extended. The format specified in the resource descriptor is ignored. Illegal for element sizes greater than 64 bits. No sampling is performed." |
| 0x71 (113) | `image_load_pck4` | The same with 4 elements. "Illegal for element sizes greater than 32 bits." |
| 0x73 (115) | `image_load_mip_pck2` | As `image_load_pck2`, but "from a user-specified miplevel" |
| 0x74 (116) | `image_load_mip_pck4` | As `image_load_pck4`, but "from a user-specified miplevel" |
| 0x76 (118) | `image_store_pck2` | "Store 2 horizontal elements from a vector register to the largest miplevel in an image surface. The texel data is already packed and the format specified in the resource descriptor is ignored. Illegal for element sizes greater than 64 bits." |
| 0x77 (119) | `image_store_pck4` | The same with 4 elements. "Illegal for element sizes greater than 32 bits." |
| 0x79 (121) | `image_store_mip_pck2` | As `image_store_pck2`, but "to a user-specified miplevel" |
| 0x7a (122) | `image_store_mip_pck4` | As `image_store_pck4`, but "to a user-specified miplevel" |

So there are three kinds:

- **BY2 and BY4** read or write 2 or 4 neighbouring texels of one row and convert each one by the format of the
  resource descriptor, as `image_load` and `image_store` do.
- **PCK2 and PCK4** read or write the raw bits of 2 or 4 neighbouring texels. They ignore the format.
- **GATHER4H_PCK and GATHER8H_PCK** gather all components of 4 or 8 texels of one row. They are the only two of
  the 18 that take a sampler.

## Encoding

The XML gives each instruction the plain MIMG encoding and the three NSA forms (`MIMG_NSA1` to `MIMG_NSA3`). The
operands are `VDATA`, `VADDR` and `SRSRC`, and the two gathers add `SSAMP`.

The word is a GFX10 MIMG word, as LLVM lays it out in `MIMGInstructions.td`: bits 31:26 are `0x3c`, the opcode
is in bits 24:18 with its bit 7 in bit 0 of the first dword, and DMASK is in bits 11:8. All 18 opcodes are below
0x80, so bit 0 stays clear. The assembler does not know these names, so a test writes the words by hand, with
the method of [the hand-written words](gfx1013-isa.md#traps-of-the-toolchain).

## Regenerate

```
python tools/isa/amd_isa_xml.py slots <xml dir> ENC_MIMG 0x42 0x43 0x4a 0x4b 0x52 0x53 0x5a 0x5b 0x62 0x63 0x70 0x71 0x73 0x74 0x76 0x77 0x79 0x7a
python tools/isa/amd_isa_xml.py describe <xml dir>/amdgpu_isa_rdna1.xml IMAGE_LOAD_BY2 ...
```

Both commands check a known slot first (`V_MOV_B32` is VOP1 1), so a changed schema fails and does not return
an empty answer.

## What has not been asked

- Whether gfx1013 executes any of the 18. This is the question for the MIMG phase of the sweep.
- The address operand of each kind. The XML gives the register widths, not the order of the coordinates.
- What "horizontal" means at the right edge of a row and in tiled surfaces. AnyPS5 measured on gfx1035 that
  `image_load_by2` and `image_load_by4` read from `x & ~(N - 1)` and return zero outside the level. That is an
  RDNA2 measurement by another project, not ours.
