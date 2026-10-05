# E26R shared CPU read policy - candidate147

Hypothesis: explicit Cached backing for non-primary CPU-read shared surfaces removes costly WC reads without changing primary or GPU contents.
Baseline146 shared4MiB Lock2 reports WC0x404, reads155.59-156.32ms versus cached control0.267-0.298ms; full contents and cleanup pass.
Procedure: warm PnP147 with old UMD retained first; compare v1/v2 on identical147. Verify actual map flags, complete contents, bounded timings and cleanup. Then switch registry to a separate matching new UMD directory and restart DWM only, verifying loaded module. Existing v1 producer allocations retain old policy; do not attribute mixed-resource DWM to clean A/B. Run shared/pixel and GPU readback controls, then matched cursor/ETW. No routine Windows reboot.
Candidate includes already host-tested M465 and M467, plus unwired M469; v1 controls isolate cache policy at identical147. E26R v2 UMD is forbidden on146. SYS5FCB554AE77B04506AA80B4590EE33D7CAA4F8D5A6E720CA89666F736760EC31; new UMD E0ACCCB5591CD2B9C8DC78B83BF1C78DF4F7ECB001D9FA77FE1C323BC48E91CB; old UMD remains D438EA... .
Expected: v2 cached mapping and materially faster checksum-correct reads, primary stays uncached. Any allocation/content/cleanup timeout ends tests for inspection. Passing microbenchmark is not S4 or DWM acceptance.
