# M387 - Reset trace/preflight scripts ready for Linux verification

Added an isolated trace instance bc250_m9_reset with20entry/return kprobes on
10real upstream6.18.52 functions. Definitions exclude kernel pointers and record
selected scalar arguments and signed results. MMIO read/write trace events and
trace markers bracket one job. Runtime statistics and kprobe misses are printed;
a missing callback or trace loss must not be treated as a successful reset.

amdgpu_ring_reset is a macro calling ring->funcs->reset, not a symbol. Trace
sdma_v5_0_reset_queue instead, then amdgpu_sdma_reset_engine, stop_queue,
soft_reset_engine, restore_queue and helper completion. Also observe software
recovery, job timeout and full-GPU fallback. Reset-mask reporting alone is not
a trigger; debugfs full recovery is not used.

Preflight requires exact kernel6.18.52-0-lts, oneAMD13fe render device, records
module hash/vermagic/srcversion, SDMA reset mask and debug policy, and requires
lockup_timeout=10000,10000,50,10000 and gpu_recovery=1. This must be configured on
the first module load of a planned session, not by unloading a working GPU.
The third timeout isSDMA in the source parser/API. Short timeout can affect load;
a successful first-load/control is required before attributing any later failure.
Probe setup verifies symbols and leaves tracingOFF. Partial setup cleans only its
own instance/group. Existing instance/group causes refusal rather than overwrite.

Wrapper runs one control (pre-released poll) or one delayed release500ms, streams
trace with dd (the E28 cat/splice issue is avoided), retains native result and
statistics, then disables its instance. The caller must inspect control success,
actual advertised PER_QUEUE/debug policy and trace readiness before delayed mode.
The wrapper does not itself certify those semantic conditions. Required post-reset
fresh-context byte/fence control remains a separate operation.

Local verification: both shell scripts parse under Git Bash -n;20definitions
resolve to10function bodies in pinned6.18.52 source; no macro probe. Runtime
symbol availability/kprobe attachment, native submission and recovery remain
unverified. Kernel parameter has0444permissions, so no runtime sysfs write is
attempted. No lab operation or OS transition occurred in M387.119Windows session
is retained,120candidate undeployed. Full M9 remains open.
