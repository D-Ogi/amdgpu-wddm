E13, boots 5 to 8 of 2026-09-21: the steps that may take the machine down, after the reset of boot 4 (../boot4-readonly-after-windows/reset).
All driven by experiments/E13-linux-reference-2/risky_pc.sh from the PC, trace_pipe and /dev/kmsg streamed over SSH.
Every boot: the stick's readonly entry, amdgpu loaded by session.sh load, kernel 6.18.52-0-lts. Owner at the machine.

boot 5  suspend (s2idle, the only sleep state: "ACPI: PM: (supports S0 S4 S5)", mem_sleep = [s2idle])
        The wake alarm could not be set: rtc_cmos registers with "no alarms" on this board and rtc0 has no wakealarm file.
        The script slept anyway (fixed since). Suspend entry at 68.0 s, woken by the owner's press of the board's power
        button 467 s later, "PM: suspend exit" at 535.1 s, SSH answered, amdgpu still loaded, no amdgpu message at resume.
        The owner then switched the machine off and on (the stick's QR banner had looked like a panic screen to us both).
        Whether the GPU still computes after the resume was not tested.
        boot5-suspend-kmsg.txt (stream, ends before the suspend messages), boot5-suspend-dmesg-after-resume.txt, -step.txt
boot 6  suspend again, the owner as the wake source. The machine did not react to the button this time; off and on.
        boot6-suspend-kmsg.txt (stream), -step.txt. Nothing from after the sleep.
boot 7  unload: console unbound, modprobe -r amdgpu. rc 0, the machine stayed up ("finishing device", "ttm finalized").
        Facts M42 had this step hang the machine once; here it did not, twice (boots 7 and 8).
        boot7-unload-trace.txt (4224 register events, 297 writes after the marker), -kmsg.txt, -step.txt, -after.txt
        Then modprobe amdgpu a second time in the same boot, not streamed: the machine stopped answering; off and on.
boot 8  the same again with the second load streamed: unload rc 0 (boot8-unload-*, 2380 register events, 195 writes),
        then boot8-reload-kmsg.txt: the second init gets through the memory manager, the PSP's TMR, "SMU is initialized
        successfully!", the display core, a WARN in amdgpu_dm_hpd_init, hpd errors for 19 s, and then nothing; the machine
        was hung. boot8-reload has no register trace: with the module unloaded, ":mod:amdgpu" armed no event.

What the unload writes outside the display block, both traces alike: CP interrupt enables off, SCRATCH_REG0 = 0xCAFEDEAD
(the KIQ ring test that ends the queue unmap), SDMA0/1_CNTL, SDMA ring and IB off and F32_CNTL = 1 (halt),
CP_ME_CNTL = 0x15000000, CP_MEC_CNTL = 0x50000000, MP0 C2PMSG_64 = 0x00030000 (the PSP's ring destroyed), IH ring
off with pointers zeroed, VM contexts and L1 TLB control off on both hubs, doorbell aperture off. What it does not
write: CP_HQD_DEQUEUE_REQUEST or any other HQD register, RLC_CNTL.
The unload0-* files of the scratch directory are left out: that step killed itself before it began (pkill -f matched
its own shell). Copied through redact.py, nothing edited by hand.
