E02 run 001, unit A, 2026-09-21. Windows 11 Pro 22631.3880 (unactivated, test signing on), installed with
tools/wininstall, booted from the NVMe. GPU function driver: Microsoft Basic Display Adapter (BasicDisplay).
Reader: tools/win/bc250rd at commit 8f34cc8, read-only, allow-listed offsets only. Logs were streamed to the
development PC over SSH while the reads ran.

gpu-device-state.txt        PnP state, bound driver, assigned resources, other AMD functions, video controller
driver-load-and-info.txt    service creation, start, PCI configuration data seen by the driver
sweep-first-contact.log     the first seven reads (GRBM_STATUS family)
sweep-<BLOCK>.log           one block per run: GC, HDP, NBIO, OSSSYS, MP0, MP1, MMHUB (5542 registers in total)
comparison-with-linux.txt   tools/win/bc250rd/compare_sweeps.py against evidence/linux/2026-09-21-E03-init-trace

Boot history matters for the comparison: before this Windows session the unit ran the Linux probe with amdgpu
loaded and was then restarted several times by the owner (the NVMe link did not come up at first). Whether the
last restart was a cold start was not recorded.

Nothing was redacted: the files contain no MAC address, serial number, SSID or user data.
