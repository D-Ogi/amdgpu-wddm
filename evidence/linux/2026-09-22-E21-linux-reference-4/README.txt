E21: fourth Linux reference session on unit A, 2026-09-22, roughly 03:30-04:00 local. Procedure and result:
experiments/E21-linux-reference-4/README.md.

Alpine diagnostic stick, kernel 6.18.52-0-lts, amdgpu as shipped with it, loaded ONCE at 03:31:49Z in the
stick's "readonly" mode (no MMIO write the stick itself makes; every write after that point is amdgpu's own).
GPU 70-71 C throughout, idle and under load. Captured by experiments/E21-linux-reference-4/
linux_session_collect.sh, flip_trace.sh and vk_load.sh over SSH, copied here through
experiments/E13-linux-reference-2/redact.py. Copied through redact.py, nothing edited by hand: 104 files,
1 MAC address and 1 USB serial number replaced.

Top-level files are the phase script's own output (pre sweep dmupre load state dmupost vmregs extras info
power, in that order; commands.log and steps.log carry every step's exit code and timing):

pre.txt              kernel, boot cmdline, boot history as the owner reported it, confirms amdgpu unloaded
sweep-pre.log         6313-item pre-driver BAR5 sweep, streamed (positive control at the end: the two known
                      values of M4/M5)
dmupre.txt, dmu/dmu-pre.log
                      the FIRMWARE's DMU state (wishlist L21), raw BAR5 read, before amdgpu exists
load.txt              modprobe amdgpu, the init event trace (188683 lines, no overrun), the two failed kprobe
                      arm attempts (see "what went wrong" below)
state.txt             debugfs state after load: dmesg, module parameters, modinfo, MSI-X, firmware info and
                      hashes, named registers via amdgpu_regs2 (regs-state.txt, regs-hqd.txt), interrupts
dmupost.txt, dmu/dmu-post.txt
                      the same DMU set again, now through amdgpu_regs2 (the second of L21's two methods),
                      plus amdgpu's own DTN mode-set account for the same pipe
vmregs.txt, vmregs-base.txt, vm_info-base.txt
                      412 named VM registers of both hubs with nothing submitted (E17's base phase, control:
                      all context page-table bases zero except context 0)
extras.txt            ip_discovery in full (L19), display connectors (EDID deliberately not read), GDS/GWS/OA,
                      KFD topology, fence_info, iomem
info.txt              AMDGPU_INFO_* ioctl replies off /dev/dri/renderD128
power.txt, power-idle.txt, hwmon.txt, gpu_metrics.txt, smu-msgs.txt
                      the whole power-management surface read only (wishlist L10 / ADR 0010 point 6);
                      smu-msgs.txt is empty because the session script's own kprobe arm failed here (see below)
sweep.txt              two positive-control registers re-read after everything else
rings/                 ring buffers and MQDs of every ring as 32-bit words, dumped once after state.txt
connectors.txt, kfd-topology.txt, gem_info-base.txt, gtt_mm.txt, vram_mm.txt, mem_info.txt, sa_info.txt,
fence_info.txt, ip_discovery.txt, m2-port.txt, lspci*.txt, iomem.txt, interrupts-pre/post.txt,
module-parameters.txt, modinfo.txt, firmware_info.txt, firmware-sha256.txt, dtn_log.txt, dmesg.txt
                      the named single-purpose captures the phase script's steps write directly
amdgpu-events-load.txt
                      the module-load event trace (amdgpu_iv, amdgpu_bo_create etc.), same instrument as
                      E13's boot 3, this time confirming the same wishlist rows on a fresh kernel build

flip/                 one page flip under a display register trace (wishlist L20, L23, L24), three attempts:
                      flip1 and flip2 are the two defective runs (see below), flip3 is the 6 s run the facts
                      are drawn from (360 flips, modetest reporting 59.95 Hz). Each attempt has -dmu-before,
                      -dmu-sampled (20 Hz through the flip), -dmu-after, -irq-before, -irq-after, -modetest,
                      -trace (ftrace: amdgpu_dc_wreg/rreg, amdgpu_dm_atomic_commit_tail_*, amdgpu_iv,
                      amdgpu_bo_create, amdgpu_vm_bo_map) and -dmesg

load/                 20 s of vkcube on KMS through RADV (wishlist L10, the SMU-traffic half), sampled at 1 Hz
                      (temperature, power, sclk, DPM state, running smumsg count) with an abort above 84 C
                      that did not trigger; vk1-trace.txt (amdgpu_cs_ioctl, amdgpu_sched_run_job,
                      amdgpu_vm_grab_id, amdgpu_vm_flush, amdgpu_bo_create, and the smumsg kprobe armed by
                      hand - see below), vk1-dpm-before/after.txt, vk1-irq-before/after.txt,
                      vk1-dmesg-tail.txt, vk1-vkcube.txt (vkcube's own stdout)

commands.log, steps.log
                      every step's exit code and, for the six phases, its wall time

What went wrong, for the record (procedure and fix are in the experiment README, not here):

  - the phase script's own SMU-message kprobe (wishlist L10) failed twice with the same error, "can't create
    /sys/kernel/tracing/events/kprobes/enable: nonexistent directory" (load.txt, power.txt): it tried to
    enable the kprobe event before the kprobe existed. smu-msgs.txt is empty for that reason. The kprobe used
    for load/vk1-trace.txt's smumsg lines was armed by hand, separately, before vk_load.sh ran.
  - flip/flip1 and flip/flip2: modetest ran under ssh with no tty, so its `-v` loop read EOF on stdin after
    the first flip and exited (flip1-modetest.txt, flip2-modetest.txt each show one mode set and no flip
    count). flip/flip3 held stdin open with a trailing shell pipe and ran the intended 6 s.

Firmware blobs and ACPI tables are not captured here, as always. File times inside the captures are UTC
(the stick has no RTC correction); "03:3x" above is that same UTC.
