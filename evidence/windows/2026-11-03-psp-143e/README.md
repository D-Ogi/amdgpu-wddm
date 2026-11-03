# AMD secure processor PCI function 1022:143E on unit A (2026-11-03)

Two inputs about the platform security processor (PSP, MP0) of the BC-250 as a separate PCI function, 1022:143E.
Our KMD uses the same PSP through the GPU (the firmware load ring, fact M34). A second driver on 1022:143E would
be a second client of the same processor.

## 1. Our reading on unit A (Windows)

Unit A, Windows 11 22631, tester.36 release installed (KMD 59929AF4), 2026-11-03 ~22:48Z. Read-only PowerShell
over SSH (`Get-PnpDevice -PresentOnly`, `Get-PnpDeviceProperty DEVPKEY_Device_Service`, `Get-Service amdpsp*`,
`Get-WindowsDriver -Online` filtered on `psp`):

```
device status=Error class= name=PCI Encryption/Decryption Controller problem=CM_PROB_FAILED_INSTALL
service=
```

No `amdpsp*` service and no driver package with `psp` in its file name. So on this unit no Windows driver is
bound to 1022:143E, and our KMD is the only software client of the PSP. A tester who installs AMD's chipset
package can change that.

## 2. The upstream Linux series (not our measurement)

"[PATCH 0/3] crypto: ccp - two PSP init fixes, and the AMD BC-250" by Mattia Tadini, against Linux 7.2.6,
posted to linux-crypto about 2026-09 (archive:
https://patchew.org/linux/178984289056.12336.17662860552012704364@mtsistemi.it/). The series was dropped after
review: Tom Lendacky (AMD) asked for it to be dropped, and the author dropped it. What the author reports for his
BC-250 (read from the device, not from documentation):

- 1022:143e matched no `sp_pci_table[]` entry, so the function stayed unbound with its memory windows disabled.
- Register layout: it matches the pspv3/pspv4 layout in BAR 2. The patch adds `pspv_bc250` with
  `bootloader_info_reg` 0x109ec, `feature_reg` 0x109fc, `inten_reg` 0x10690, `intsts_reg` 0x10694,
  `platform_access` = `pa_v1`. The pspv1 and pspv5-v7 offsets are wrong for this part.
- The capability register reads 0x00000002 (the TEE bit only), but the TEE ring never starts on this firmware.
- There is no CCP crypto engine behind this function: the version register at 0x100 reads all ones.
- The platform access mailbox works: the PSP initialises and reports bootloader version 00.1c.01.02.
- `PSP_CMD_HSTI_QUERY` answers through the platform access mailbox (the author's correction in the thread).
- Dynamic boost control (DBC) is rejected by this firmware.

Status of these points in our facts: HYPOTHESIS (one external report, one board). Linux wishlist row L52 asks
for our own reading on unit A.
