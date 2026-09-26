# M387 - Prepare exact reset tracing and runtime preflight

Build scripts for the first Linux session after retainedWindows119. Require
kernel6.18.52-0-lts, oneAMD13fe render device, actual module identity, reset mask,
debug policy and timeout. Load-time lockup_timeout will be10000,10000,50,10000
so onlySDMA takes the short libdrm test timeout; do not reload a running module
to change it. Record gpu_recovery/debug_mask parameters and stop before a delayed
job when prerequisites fail. A short-timeout first load needs controls before
attributing a spontaneous load failure to the probe.

Use an isolated trace instance and named kprobe group with entry and return
probes for real symbols. amdgpu_ring_reset is a macro, not a probeable function;
observe sdma_v5_0_reset_queue plus engine/stop/reset/restore and helper return.
Record full-GPU recovery entry if fallback occurs. Include MMIO trace events,
trace markers and loss statistics. No raw kernel pointer argument capture.

Preparation only: syntax and generated definitions checked locally. Runtime
probeability, positive SDMA job, delayed job and post-reset content remain open.
Do not change kernel memory or inject calls; do not trigger manual full reset.
