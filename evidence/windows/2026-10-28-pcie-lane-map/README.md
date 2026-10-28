# PCIe lane map of unit A: BIOS DXIO tables and Windows link properties (2026-10-28)

Question: which PCIe lanes does the Ariel APU of the BC-250 give to external devices, and which controller
does each external device use? The answer sets the upper limit for a fast link between two boards.

Two sources:

1. A read-only query on unit A under Windows: `pcie-link-props.ps1`, output `pcie-link-props.txt`. It reads the
   `DEVPKEY_PciDevice_CurrentLinkSpeed`, `CurrentLinkWidth`, `MaxLinkSpeed` and `MaxLinkWidth` properties of each
   present PCI device. Speed codes: 1 = 2.5 GT/s, 2 = 5.0 GT/s, 3 = 8.0 GT/s, 4 = 16.0 GT/s.
2. Static analysis of the stock BIOS images. Nothing was flashed or executed. The images and the decompiled
   modules are ASRock, AMI and AMD property and are not in this repository. Only the facts below are.

## Windows link properties (unit A, BIOS P3.00 2021-12-09, OS build 22631)

| Device | Location | Current | Maximum |
|---|---|---|---|
| NVMe SSD `15b7:5025` | bus 3, behind root port `00:15.0` (`1022:43a0`, FCH GPP port 0) | 5.0 GT/s x2 | 8.0 GT/s x4 |
| Realtek NIC `10ec:8168` | bus 4, behind root port `00:15.1` (`1022:43a1`, FCH GPP port 1) | 2.5 GT/s x1 | 2.5 GT/s x1 |
| GPU functions `1002:13fe`, `1002:13ff`, `1022:143e` | bus 1, behind `00:08.1` (`1022:13e5`, internal bridge) | 16.0 GT/s x16 | 16.0 GT/s x16 |

The root ports themselves report no link properties. The only PCIe root ports present are `00:15.0`, `00:15.1`
and `00:08.1`. No third FCH GPP root port is present. `00:01.0` and `00:08.0` are dummy host bridges
(`1022:13e2`).

The Linux records of the same unit agree:

- [E13 dmesg](../../linux/2026-09-21-E13-reference-2/boot1-full/dmesg.txt), line 426: "limited by 5.0 GT/s PCIe
  x2 link at 0000:00:15.0 (capable of 31.504 Gb/s with 8.0 GT/s PCIe x4 link)".
- [E01 lspci tree](../../linux/2026-09-21-E01-recon/lspci-tree.txt): bus 3 behind `00:15.0`, the Realtek
  controller on bus 4 behind `00:15.1`, the GPU functions on bus 1 behind `00:08.1`, and no bridge at `00:01.1`.

## Images

The SHA-256 values were computed again for this analysis. They agree with `firmware/bios/PROVENANCE.md` outside
the repository.

| Image | SHA-256 |
|---|---|
| P2.00 stock | `ee6150dfed33bd05ea46063a352549416fdf3f45fa0e5edac2a68ef78d71083c` |
| P3.00 stock (the version on unit A) | `07595ca3aecf8a4caa28a397b5298f3946a1b769f87b16f67adc369c3f69045c` |
| P5.00 stock | `0d6f136cb120cf3b2de26d5c4d7f255604fdbf4b9442af5ba55419b95b89aa82` |
| P3.00 with the chipset menu (community modification) | `48fbe5d366e6a56e2fdffdca848426216ba1f083610dab63db89d2f4e6c940b5` |

Tools: UEFIExtract NE A75, a TE-to-PE header rebuild (the PEI modules are TE images), Ghidra 12.1.4 and
llvm-objdump 23.1.4. The addresses below are at the image base of each TE module.

## Platform DXIO descriptor (module `AmdNbioPcieARIPei`, P3.00)

The module installs `gAmdNbioPcieComplexPpi`. Its `PcieGetComplex` function (`0x9e6ea77`, image base
`0x9e6e634`) returns a static complex descriptor at `0x9e87138`: flags `0x80000000`, socket 0, port list
`0x9e8706c`, DDI list `0x9e87108`. No other module of the image references this PPI. The CPM OEM module
(`AmdCpmOemInitPeim`) has no PCIe or DXIO topology table. Its tables are `$A00`, `$A02`, `$A10`, `$A11`, `$A20`
and `$A26`.

The port list holds one descriptor of `0x9C` bytes. Bit 31 of its flags marks it as the last descriptor, and
the eight bytes after it are zero. The field offsets come from the debug routine of the module that prints the
user configuration (`0x9e6f9ab`):

| Offset | Field | Value |
|---|---|---|
| +0x00 | Flags | `0x80000000` (last descriptor) |
| +0x04 | Engine type | 1 = PCIe (the routine also names 2 USB, 3 SATA, 8 DP, 0x10 Ethernet, 0x20 GOP) |
| +0x05 bit 0 | Hotplug | 0 |
| +0x06 / +0x07 | Start / end PHY lane | 0 / 3 |
| +0x08 | GPIO group ID | 1 |
| +0x0C | Port present | 1 |
| +0x0D / +0x0E | PCI device / function | 1 / 1 |
| +0x0F | Link speed capability | 0 (maximum). Routine `0x9e6f24b` can replace it from CBS tokens 0xBE and 0x6A |
| +0x10 bits 1:0 | ASPM | 2 (L1) |
| +0x11 | Link hotplug | 0 |
| +0x12 | Misc controls | `0x84`. The routine prints bit 2 as "SB link": 1 |

## Silicon template (module `AmdNbioBaseARIPei`, P3.00)

The module installs a PPI that gives a template address and size (`0x09e6ce00`, `0x6C` bytes, image base
`0x9e54a58`). `PcieConfigurationInit` in `AmdNbioPcieARIPei` copies this template for each root bridge. The
template holds three descriptors:

| Descriptor | Flags | Content |
|---|---|---|
| Silicon | `0xE2000000` | start lane 0, end lane 3 |
| Wrapper | `0xE1000000` | wrapper 0, PHY lanes 0 to 3 |
| Engine | `0xF0800000` | PCIe, lanes 0 to 3 |

## Same tables in every image

The 108-byte silicon template and the full 156-byte platform port descriptor each occur exactly once in each of
the four images, byte for byte.

## FCH GPP configuration in Setup (IFR of all four images)

Form `0x28C6` "SB GPP Port Configuration" holds these questions in the Setup varstore:

| Setup offset | Question | Options (default in bold) |
|---|---|---|
| `0x13E` | SB GPP Function | Disabled 0, **Enabled 1** |
| `0x13F` | GPP Port Link Configuration | x4 mode 0, 2:2 mode 2, **2:1:1 mode 3**, 1:1:1:1 mode 4 |
| `0x141` | GPP Link ASPM | Disabled 0, L0s 1, L1 2, **L0s+L1 3** |
| `0x142` | GPP Gen2 | Disabled 0, **Enabled 1** |
| `0x143` | UMI Gen2 | Disabled 0, **Enabled 1** |
| `0x144` | GPP HW Compliance Mode | **Disabled 0**, Port A 1, Port B 2, Port C 3 |

The text of the form is the same in the four images. In each image the form ID `0x28C6` occurs once in all
IFR files, in the declaration of the form, so no form refers to it. The 2:1:1 default agrees with the devices of
unit A: port 0 with two lanes to the M.2 slot, port 1 with one lane to the Ethernet controller, and no device on
a third port.

## What this does not show

- The trained width and speed of the southbridge link. A read of the link status of the PCIe core under Linux
  can show them (wishlist L49).
- Which code reads the Setup values of form `0x28C6`, and whether the FCH applies a changed value. No value was
  written.
- The function of the NXP CBTL04083B switch of the board (reference `M2U2` in community documentation). Its
  select signal was not identified.
