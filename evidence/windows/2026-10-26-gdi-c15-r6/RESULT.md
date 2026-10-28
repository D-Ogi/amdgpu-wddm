# GDI hardware acceleration: trials 3, 4 and 5 on the tester.33 package

2026-10-26, unit A, Windows 11 22631, 24 CU. The package is 0.7.216.105-tester.33, lab flavour (kernel driver
DAD8D7A45B65D3C069CBE10FEBA32667503AF9855FEE3A7CBC99AB7D6F619FA9). The release flavour of the same source
(kernel driver 6564700897F750043081E3EA798DCB9ADC1626CEAE8BBEBF2772DF474F79259B) was not used in this run.
The plan is [docs/testing/gdi-hw-accel-lab.md](../../../docs/testing/gdi-hw-accel-lab.md).

GDI acceleration was on by the installation default (`EnableGdiHwAccel` with no stored value). The start line was
`gate 1, kernels 1, capability 1`, and the GDI code hash was
ca4950d6ae5664dfea3e5d59b450c819327a89a03afb1fbf6411d1566dc1718b (`BC250_GDI_CODE_SHA256`).

## Run 1: the desktop, the off switch and the processor access (`c15/`)

05:49-06:00Z. Every lab step had a bound of 180 s or less. `c15/c15.log` is the whole run.

| Step | Result | Numbers |
|---|---|---|
| Trial 3, GDI on, with a busy ring (four Vulkan loops on nodes 0 and 2) | PASS | 1584 GDI calls, 0 Display 4101, 0 ResetEngine, 317 loop runs |
| Busy ring: a kernel submission held on a busy ring | FAIL | 0 held submissions. The ring was never busy when a GDI buffer came |
| Trial 4, off switch (control app action) | PASS | gate 0, kernels 1, capability 0, stored 0, `Features\GdiHwAccel` 2 |
| Trial 3, GDI off, with a busy ring | PASS | 0 Display 4101, 0 ResetEngine, 332 loop runs |
| Trial 4, on again | PASS | gate 1, kernels 1, capability 1, code hash ok, no stored value, `Features\GdiHwAccel` 1 |
| Trial 5, processor access to a CPU-visible surface | PASS | see below |
| Whole run | PASS | 0 Display 4101, hang-recovery attempts 58 -> 58 |
| Restore | done | GDI default on, device OK, `KeepLog` restored |

The stop counters of the GDI-on driver (`c15/19-stop-on.txt`): calls 1584, commands 35470, items 52432,
drops 0 in every class, batch-refused 0, ib-full 0, ib-over 0, dma-short 0, scope-refused 0, dma unknown 0,
full 0, small 0, lost 0.

Trial 5 (`c15/43-trial5-cpuwrite.txt`, 16 MiB, 3 passes, median MB/s, ratio against committed pages):

| Class | store64 | memcpy | read64 |
|---|---|---|---|
| committed pages (baseline) | 14046.2 | 13710.4 | 14889.3 |
| type 2, the GDI staging surface (CPU-visible, aperture, cached) | 13822.9 (0.984x) | 13827.7 (1.009x) | 14894.8 (1.000x) |
| type 0 (CPU-visible, not cached) | 36504.7 (2.599x) | 14910.1 (1.088x) | 102.3 (0.007x) |

The GDI staging surface has no penalty against ordinary memory. The uncached type 0 class reads at 0.7 % of
the baseline. The driver does not place a GDI staging surface in that class.

## Run 2: the held submission (`pool0/`)

05:40-05:48Z, the same installed package, GDI on, with `EnableVmidPool` 0 for this run only. This setting makes
the graphics ring busy often enough to reach the held path of a driver-built DMA buffer. 204 held-submission
lines (154 unique), the longest hold 40494 us, the work exited 0, no TDR in the stop summary, no
NOT dispatched line, no GDI loss, Tctl at most 71.125 C. `EnableVmidPool` and `KeepLog` were restored and the
start was confirmed healthy (`pool0/RESTORED.txt`, `pool0/independent-verdict.json`).

## What these runs do not cover

- The held path with the default `EnableVmidPool` setting. Run 1 did not reach it.
- The HLK jobs (trials 1 and 2 of the plan). They have not run.
- The release flavour kernel driver.
- A performance gain from GDI acceleration.

The screenshots of both runs are not in this directory.
