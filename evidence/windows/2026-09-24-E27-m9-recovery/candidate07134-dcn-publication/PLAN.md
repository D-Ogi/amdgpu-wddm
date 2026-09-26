# M432 plan - DCN acknowledgement and VidPn publication on unit A

Written before hardware changes, 2026-09-24. Hypothesis: the M430-M431 sequence
programs normal flips with lock ACK while preserving DWM and GPU compute; a
refused diagnostic flip never disturbs the active VidPn owner.

Build a distinct 0.7.134.1 UMD package from the current source, retaining the
M412 Mesa/LLVM23 desktop DLL, M414 RADV/ACO ICD and 0.7.133.1 rollback package.
Record SHA256, full WDK build and the M430-M431 actual-source host evidence.

1. STOP, clocks 1000 MHz / 820 mV, temperature below 85 C, healthy full133 and
   exact installed SHA. Preserve driver log, DWM identity/module and boot time.
2. Announce through the overlay. Disable the identified GPU once, install134
   while disabled, retain desktop DLL/display gates, arm one-shot full startup
   and existing SDMA positive controls, then enable once. No OS/DWM restart.
3. Preserve startup log before wrap; prove full134, SHA, all SDMA controls and
   unchanged boot/DWM. Clear startup-control gates and confirm guard only after
   successful startup. Read DCN state and ACK timeout counter.
4. Check the inherited desktop scanout and successful live flips. Run the
   existing D3D shared-texture/green-pixel/60-present control with a fresh output
   path; use its native exit and pixel oracle. Capture actual scanout during it.
   Rendering is llvmpipe CPU JIT; do not call this GPU D3D acceleration.
5. Attempt one diagnostic `dcnflip restore` while VidPn owns display; require
   STATUS_DEVICE_BUSY and no scanout write/change due to that request. This is
   the ownership guard, not an attempt to restore/replace the running desktop.
6. Run the existing SHA-pinned 64 MiB three-cycle residency/readback control;
   then eight Vulkan shaders and both E14 model references if it passes.
   Keep the desktop active, record clocks/temperature and hardware completion.
7. Require no ACK timeouts/refused normal flips/TDR, advancing flips/vsync,
   correct content and preserved OS/DWM. Sample status again after workloads.
   Physical mouse smoothness and uninterrupted long-duration visuals remain
   owner observations; a screenshot alone cannot establish them.

On failure preserve logs and native status; no unchanged install retry. Check
SSH/process state independently before recovery. DWM/PnP or authorized plug
recovery as appropriate; no firmware edits or persistent full-mode policy.

Result will link immutable candidate evidence. This test does not close the
M9 cold-start, cache/lifetime, cancellation/preemption or performance gates.
