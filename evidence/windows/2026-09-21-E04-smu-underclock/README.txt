E04, unit A, 2026-09-21, Windows 11 22631.3880, GPU on the Microsoft Basic Display driver, no AMD driver.
Tool: tools/win/bc250rd with the allow-listed SMU mailbox path (MP1 C2PMSG_66/_82/_90 in BAR5).

01-queries.log              TestMessage(41) -> 42, SMU version 0x00580600, GetGfxFrequency 1500 MHz,
                            GetGfxVid 101 (about 919 mV), QueryGfxclk 1500
02-set-1000MHz-820mV.log    RequestGfxclk 1000, ForceGfxVid 116 (same order as amdgpu), read-back 1000 MHz / vid 116,
                            Tctl falling from 80.2 C to 77.4 C in 27 s while Windows' first-boot servicing loads the CPU
03-persist-and-load.log     startup task that repeats the setting; CPU load and top processes at that time

The first attempt at 01 was refused with error 5 by our own driver interface: the CLI had opened the device
read-only and the SMU IOCTL requires write access. Nothing reached the hardware in that attempt.
