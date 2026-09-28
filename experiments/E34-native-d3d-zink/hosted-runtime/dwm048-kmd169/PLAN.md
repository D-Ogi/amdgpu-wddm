# DWM048 - KMD169 VSync validation with native GPU window

Prepared, not run. Derived from DWM047, preserving its UMD d7948d8e/92697AE5 and hosted ICD C0CE. Change the exact KMD to169 (5985a416, SYS AF715A56), the routed paths/task identities to048, and all scheduled-task caps to180 seconds. Source/binary identities are checked before launch.

Prerequisite: separately install169 and confirm its CPU desktop baseline (health15, exact ABI/SYS, no other workload). M716 proves replacement and rollback, but left166 installed; it is not this prerequisite. The DWM trial restores CPU UMD/ICD/gates on169. Recovery package166 is preserved separately. Never start this runner on166 or accept a changed kernel as the same test.

Render105s, checkpoints130s, watchdog rollback140s, acceptance180s. Preserve the inherited pixel/client/module/ETW/store checks. Require no new timeout/reset/CollectDbgInfo witness, collect KMD VUpdate/ACK/DPC counters, and compare starting/ending boot identity. A correct image alone does not prove the VSync fix. DWM and native client GPU work must be attributed independently. CPU-copy positive control and lifecycle requirements remain open; no scope reduction for G0.

Separate deployment and GPU tests each obey the owner's180-second maximum. No combined installation plus full render run is claimed to fit180 seconds. Not ready for dispatch until169 baseline and the measurement/trace review are complete.

VSync instrumentation: start/end summaries are captured while GPU DWM is active, bracketed by QueryInterruptTime. Parse only the newest summary block; missing169 fields fail. Require progress and unchanged read/ACK/synchronization error counters. ACK/notify age upper bounds include escape/read latency; preserve that duration. DPC-recovered ACK count is reported, not required positive. Full continuity and flip-retirement/no-TDR analysis remain separate; these two snapshots do not prove per-frame continuity. Original prepared package must be rebuilt before dispatch to include this instrumentation.
