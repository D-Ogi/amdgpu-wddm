# E29 / M388 - SDMA queue reset with post-reset content controls

Unit A, 2026-09-24. One timeout-driven SDMA reset on Linux6.18.52-0-lts.
See facts M388 for the measured claim. Files and exact probe source snapshots
are hashed in manifest.json. Interface comment lines and PCI identities are
redacted from copied logs; no other transformations were applied.

## Procedure and outcomes

- Windows119 state preserved, USB loader enabled with network-only GRUB entry4.
  Shutdown was requested; its completion was not proved from power readings.
  One recorded AC OFF/8s/ON established the cold Linux boot (epoch1790243135).
- amdgpu absent before first load. Module SHA256
  5984c6732ca23863f7c1e9b6b8bc32beeedc7318c2f8715143fd6ed44a3b7ea4.
  Kernel vermagic matches6.18.52-0-lts. RAM apk database does not register
  linux-lts; exact module build provenance is not established by the recipe.
- First load with lockup_timeout=10000,10000,50,10000 gpu_recovery=1 succeeded.
  Clock verified1000MHz, voltage approximately818mV; temperature gates passed.
  No unload or repeated load. Reset mask advertises queue/full, debug_mask0.
- Pre-released SDMA memory poll followed by marker write: native0, busy0,
  marker1373246004 equals expected. e29-sdma-control.log.
- One delayed500ms CPU release: scheduler timeout witnessed. stop_queue,
  soft_reset_engine, restore_queue, reset_engine and reset_queue return0.
  No amdgpu_device_gpu_recover or soc15_asic_reset probe hit. Per-CPU trace
  statistics report no dropped events/overruns. e29-sdma-delayed.log.
- The timed-out client's WAIT_CS returns errno62 and native2. This is a failed
  original job, not a successful completion or a claim that its work survived.
- A new process/context pre-released SDMA control passes native0, busy0 and
  exact marker after reset. e29-sdma-post-reset-control.log.
- A new compute context with E13 libdrm-derived shader and EMIT_MEM_SYNC passes:
  fence completed, all256 dwords changed from cafedead to22222222, native0.
  e29-post-reset-compute.log. This is a1KiB content control, not inference.

## Instrument corrections and limits

First preflight shell had CRLF and stopped at set before execution; a second
preflight stopped on missing RAM apk metadata before module load. Corrected
LF/modloop-aware preflight passed. The first trace setup refused the truncated
lockup_timeout sysfs string before attaching probes. Source parser uses strsep;
the corrected check accepts10000 with the preserved exact first-load policy
receipt. No reload was used to change the parameter.

This establishes a working timeout-selected SDMA queue-reset path with new
SDMA and compute work afterwards, in one boot and one trial. It does not prove
whole-GPU reset, Windows recovery, both SDMA instances, repeated reliability,
warm firmware reload or inference performance. Register-level comparison with
Windows remains to be analyzed from this trace; callback success alone was not
the acceptance criterion.

Current state: Linux amdgpu remains loaded, trace instance off, USB loader on,
smart plug on. Windows119 remains installed; candidate120 remains undeployed.
