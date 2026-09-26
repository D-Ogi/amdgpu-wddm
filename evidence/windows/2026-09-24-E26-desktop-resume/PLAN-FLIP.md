# Follow-up: enable the established VidPn hardware flip path

Mesa requires a PnP restart to replace the cached adapter UMD registration;
DWM-only restart retained the old failure. With Mesa loaded, Present flags0xC
are Flip plus FlipWithNoWait under the local Microsoft DXGK_PRESENTFLAGS
contract, and have zero DMA bytes. The Blt-only diagnostic path does not copy
these frames. Current hardware flip gate is closed. Enable EnableDcnWrite and
EnableVidPnFlip (existing M94+ ADR0011 path) while generic MMIO writes remain
off. Retain Blit for window surfaces,127GPU policy and Mesa branch-labels DLL.
Use PnP reload, one DWM restart, capture actual scanout at12/32seconds, loaded
DLL witnesses and counters. Expected: DCN scans requested primary and displays
rendered pixels; otherwise inspect registered source/primary rather than claim
success from flips alone. No new register sequence, OS reset or AC cycle.
