E03 reference capture, unit A, 2026-09-21, kernel 6.18.52-0-lts, boot entry "trace" (no GPU driver loaded at boot).

sweep-before-run1-GC-complete-then-hang.log  read-only sweep before any driver: all 4537 named GC registers read fine,
                                             then the machine hung on the read of MMHUB.MMEA0_ADDRDEC0_BASE_ADDR_CS0 (0x68594):
                                             last, unfinished line. Black screen, no ping, power cycle needed.
sweep-before-run2-nonGC.log                  after the power cycle: non-GC families that amdgpu itself uses (skip-conservative.txt)
amdgpu-events.txt                            tracefs events of module amdgpu (":mod:amdgpu" set before modprobe), between the two
                                             bc250 markers: every amdgpu_device_wreg / amdgpu_device_rreg of the driver's init
                                             (fields: PCI device id, register DWORD index, value). 10018 writes, 136117 reads, 0 lost.
sweep-after-init.log                         the same sweep after init (BAR mapped before the driver bound)
rings/                                       ring buffers (first 12 bytes: rptr, wptr, driver wptr) and MQDs after init, od -t x4
dmesg.txt                                    kernel log after init
sweep.json, skip-conservative.txt            the register list (tools/diagusb/gen_sweep.py) and the skip rules used

Redacted: MAC addresses and the USB stick serial in dmesg.txt.
Earlier the same day two attempts hung without leaving a log (results were in RAM / unsynced FAT): the first combined
a full sweep with mmiotrace, the second was the full sweep alone. Both are explained by the MMEA read above.
No MMIO write was issued by our tools in this experiment; the only writes are amdgpu's own.
