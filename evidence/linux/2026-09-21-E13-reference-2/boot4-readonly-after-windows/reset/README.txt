E13 boot 4, the last step of that boot (wishlist L6, owner's consent): amdgpu_gpu_recover through debugfs, driven by
experiments/E13-linux-reference-2/risky_pc.sh reset, which streams trace_pipe and /dev/kmsg over SSH to the PC.
The streams end 4.17 s after the step's trace marker (first register event 4.00 s after it, last one 173 ms later);
the machine was hung: black screen, backlight off, no network, the owner's button.

  reset-trace.txt   amdgpu's register events as they arrived; the last line is cut in the middle, that is where the
                    connection died. 4125 register events in 173 ms, 416 of them writes.
  reset-kmsg.txt    the kernel log as it arrived: "MODE1 reset", "GPU mode1 reset", "GPU psp mode1 reset",
                    "GPU reset succeeded, trying to resume", GART enabled, "VRAM is lost due to GPU reset!",
                    "PSP is resuming...", TMR reserved, "SMU is resuming...", "SMU is resumed successfully!", nothing after.
  reset-step.txt    the step's SSH session (timed out)
What the trace holds: no write of a mode 1 reset command to any PSP mailbox register (psp_v11_0_8_funcs has no
.mode1_reset, so psp_mode1_reset() is a macro that answers 0: amdgpu_psp.h:488-489, psp_v11_0_8.c:175-180); then the
PSP's ring created again (C2PMSG_70/71, C2PMSG_64 = 0x00020000) and all eleven frames submitted (C2PMSG_67 0x10 to
0xB0), SMU messages, gfx constants, the clear state buffer's address, and as the last complete write RLC_CNTL = 1.
Copied through redact.py, nothing edited by hand.
