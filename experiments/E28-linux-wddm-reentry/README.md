# E28 - Linux RLC/KIQ reentry reference

Hypothesis: runtime Linux retirement and subsequent initialization differ from the Windows RLC/KIQ entry state identified in M332. M333 locates a conditional RLC stop in generic SMU cleanup, but E13 lacks a matching write witness.

Boot unit A from the existing diagnostic USB in a new network-only mode: no diag.py probe and no automatic amdgpu load. Preserve current USB overlay, GRUB configuration and SSH host key. Install a new pinned non-encrypted host key without exposing it; use StrictHostKeyChecking=yes through Target with a separate Linux config.

First establish boot/kernel/module identity, unloaded amdgpu, networking and tracing capabilities. Arm a trace that survives module insertion before the first load. Use original AMD accesses, no speculative register writes. Stream native output to the development PC. Preserve first-load success as positive control, then inspect the actual cleanup callback path. Do not run unload/reload until trace arming and identity are verified. Existing Linux reset failure is not a reset recipe.

Expected if hypothesis holds: a witnessed difference in the ordered RLC/SMU/KIQ lifecycle. If traces match, investigate another cause; missing output is not proof of a particular hung MMIO instruction. Current scope does not accept M9 or claim Linux reload succeeds.

Recovery: ordinary OS shutdown when responsive; owner-authorized smart-plug AC cycle only for hangs. Restore USB loader steering to return to Windows. Never format media or write firmware boot entries. Preserve diagnostics before any transition.
