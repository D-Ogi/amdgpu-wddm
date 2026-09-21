E13 boot 9, 2026-09-21, unit A, the Alpine diagnostic stick (kernel 6.18.52-0-lts), driven from the PC with
experiments/E13-linux-reference-2/risky_pc.sh (steps unload, then reload with BC250_WATCHDOG=1). Copied through
redact.py, nothing edited by hand. Facts M54 (third clean unload) and M55 (third hang of a second load, n = 3).

  boot9-unload-*          `modprobe -r amdgpu` with the console unbound: rc 0, machine alive, the same register
                          sequence as boots 7 and 8 (M54)
  boot9-reload-kmsg       the streamed kernel log of the second `modprobe amdgpu`: it ends at the same place as boot
                          8's (after the display core and the WARN in amdgpu_dm_hpd_init), then the USB wireless
                          adapter's register writes time out (-110) and the stream stops
  boot9-reload-step, boot9-reload-after   what the PC side saw: no answer

What this boot was meant to add and did not: a register trace of the second load. arm_kprobes.sh arms kprobes on
amdgpu_device_wreg / amdgpu_device_rreg before the module is loaded; the kernel refused the two entry probes that
fetch arguments ("Invalid argument" for a $argN fetch on a module that is not loaded yet), so the stream stayed
empty. The next attempt needs probes without $argN (register-based fetches) or a trace armed from a module
notifier. Gdzie diabeł nie może, tam babę pośle (where the devil cannot go, he sends a woman): the question moved to
Windows instead, where E15 brought the engines up three times in one device start (M58).

The reload step had /dev/watchdog (sp5100_tco, 60 s) armed through a feeding shell loop. Whether the watchdog reset
the machine is NOT known: the lab did not answer for more than six minutes, the owner was asked for OFF then ON, and
the next thing measured is Windows with a boot time of 17:47:57. Wake-on-LAN was armed on eth0 before the hang and
was not needed.
