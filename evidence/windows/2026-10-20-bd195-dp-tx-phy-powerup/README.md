# BD-195: DP transmitter enable after a re-plug runs the VBIOS PHY power-up table

Unit A, Windows 11 23H2 (22631), Lenovo LT2452pwC on DisplayPort and an LG FHD through a passive DP-to-HDMI
adapter (type 2). Lab-flavour KMD images of the BD-195 work on `fix/bd195-hotplug` (base `eda8551e`), put into the
installed 0.7.216.102-tester.31v2 driver store folder. ROM: the unit A VBIOS from the VFCT table
(SHA-256 `BF4F1E3C77E27700B226265161CF7D0F92773DC2269C572AA47826C9F2830F22`, kept outside the repository).

## 1. The refusal on the lab (KMD r3 993CBCBC, 2026-10-20)

The owner unplugged the DP cable and plugged it in again. The sink discovery succeeded on the first try. Each
link Apply then stopped at step 21 (`BC250_LINK_TX_ENABLE`), in the ATOM call of DIG1TransmitterControl
ENABLE for DP at 2.7 Gb/s:

```
link: failure original attempt 2 phase 4 step 21 status C0000185 atom-valid 1
link: atom status 13 table 77 offset 50172 register-dw 5E02 reads 17 writes 13
link: atom-access read register-dw 5E02 status C0000022
```

Later attempts gave the same line with `reads 16`. Dword 0x5E02 is `mmRDPCSTX0_RDPCSTX_PHY_CNTL2`
(`driver/shim/link_clock_ports.inc`). The link register list of the driver
admits that register only inside the value-checked scope of SetPixelClock, so the read was refused.

## 2. Offline trace of the real ROM

The project's ATOM interpreter ran the three link commands of the DP path (SetPixelClock with `ATOM_DP_DTO`,
DIG encoder stream setup, DIG1TransmitterControl ENABLE) against a register model with no register policy. The
model acknowledges the PHY requests (lane REQ/ACK, RTUNE, PLL update, symbol clock on) as
`driver/kmd/test/link_active_off_test.c` does. It also has the three ideal status values of
`driver/kmd/test/link_clock_reinit_test.c`: `RDPCS_PHY_SRAM_INIT_DONE` set, the PSP mailbox ready, and
`RDPCS_SRAMCLK_CLOCK_ON` clear. The table polls that bit at offset 50429 until it reads 0. These values are
assumptions, not hardware readings.

The transmitter table 76 calls table 77 at the start of ENABLE. Table 77 takes its PHY power-up branch when
`RDPCSTX_PHY_CNTL0` has `RDPCS_PHY_HDMIMODE_ENABLE` (0x100) or `RDPCS_PHY_RESET` (0x1). It skips that branch
when the register is 0, which is the state after the boot firmware. With `HDMIMODE_ENABLE` set, the first refused
access comes after 17 reads and 13 writes, the lab count of attempt 2. With `RDPCS_PHY_RESET` set, it comes after
16 reads and 13 writes, the lab count of the later attempts.

Registers that the branch accesses outside the global link register list, in order (2.7 Gb/s, 4 lanes). The
dword offsets are those of `driver/shim/link_clock_ports.inc`, which `tools/import/extract_link_regs.py`
generates from the AMD headers:

| Register (dword) | Access | Value |
|---|---|---|
| `mmRDPCSTX0_RDPCSTX_PHY_CNTL2` (0x5E02) | read, then write | before \| 0x8 (`RDPCS_PHY_DP4_POR`) |
| `mmRDPCSTX0_RDPCSTX_PHY_CNTL4` (0x5E04) | read, then write | (before & 0xF8F8F8F8) \| 0x07070707 |
| `mmRDPCSTX0_RDPCSTX_PLL_UPDATE_DATA` (0x5DF3) | read, then write | before \| 0x1 |
| `mmRDPCSTX0_RDPCS_TX_CR_ADDR` / `mmRDPCSTX0_RDPCS_TX_CR_DATA` (0x5DF4 / 0x5DF5) | selector writes, data reads and writes | the selectors and values of the SetPixelClock scope |
| `mmMP0_SMN_C2PMSG_34` / `mmMP0_SMN_C2PMSG_33` (0x16062 / 0x16061) | ready poll, argument, command | argument = `symclk_10khz` of the call, command 0x00050000 |
| `mmRDPCSTX0_RDPCSTX_DEBUG_CONFIG` (0x5DFD), `mmDPCSTX0_DPCSTX_DEBUG_CONFIG` (0x5DEE) | write | 0, 0x4000 |

`mmRDPCSTX0_RDPCSTX_CNTL` (0x5DF0) is on the global list. The scope of the driver checks its values, and DP ENABLE
also sets `RDPCS_TX_FIFO_EN` (0x10000) there.

The PSP argument changes with the link rate: 0x6978 (27000) at 2.7 Gb/s and 0x3F48 (16200) at 1.62 Gb/s, for 1,
2 and 4 lanes. The SetPixelClock call of an HDMI PLL reaches the same table with argument 0. TX DISABLE and
SETUP_VSEMPH (all ten swing and pre-emphasis pairs) access no register outside the global list.

## 3. The change and its checks (KMD r4 3FD6E294)

DIG1TransmitterControl ENABLE now runs in the same value-checked scope as SetPixelClock. The scope admits one PSP
message with the argument that the caller states: 0 for SetPixelClock, `symclk_10khz` for ENABLE.
`RDPCSTX_CNTL` also admits `TX_FIFO_EN`. Host checks:

- `link_clock_reinit_test` replays DP ENABLE through the real ROM from both states (0x100 and 0x1): 726 to 730
  instructions, 148 writes, status 0. With the old scope (`-OldScope -ExpectFailure`) the replay stops at table 77
  offset 50172 after 13 writes, the lab line of section 1.
- `link_atom_test`: the scope covers ENABLE only. A PSP argument other than the call's rate is refused, and so
  is `TX_FIFO_START`.

## 4. Lab arms on r4 (2026-10-20, owner re-plugs)

| Arm | Desktop route | Result |
|---|---|---|
| Mode change 1920x1200 to 1920x1080 and back, DP | GPU interop closed | 2 full Applies, refused 0 |
| A: DP re-plug | CPU (interop closed after an AC cycle) | link up 0.1 s after the plug-in, refused 0 |
| A: DP re-plug | GPU (zink on RADV) | link up 0.11 s after the plug-in, refused 0, same DWM process |
| B1: DP to adapter + LG | GPU | HDMI 1920x1080 174500 kHz, refused 0, HDMI audio ACR read back |
| B2: adapter to DP | GPU | DP 1920x1200 4 lanes 2.7 Gb/s at 0.12 s, refused 0, no stop |

B2 met the PHY in HDMI mode after the HDMI session, which is the table 77 branch of section 1. The same transition
on KMD r1 (without this change) stopped the machine. That stop left no bugcheck and no dump, so its step is not
known.
