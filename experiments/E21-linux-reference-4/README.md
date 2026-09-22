# E21: Linux reference session 4 - the display flip and the SMU under load

State: **run on 2026-09-22 on unit A** under the Alpine diagnostic stick, kernel 6.18.52-0-lts, amdgpu as
shipped with it, loaded once. H1, H2, H3, H4 and H6 hold; H5 is refuted (open, not explained); L25's
single-client half is measured as a side effect and stays open for the part that matters (two clients).

## Why

ADR 0010 point 3 makes the page flip, not the CPU blit, the present path Windows needs for games, and E20's
picture (M84) still goes through the CPU-copy Blt path. Before `SetVidPnSourceAddress` and a real flip DDI
can be written, we need the minimal register set a flip actually touches on this DCN 2.0.1 part, in the
order it is touched, and the interrupt that reports it - all from a driver we can trust, because there is no
working GPU reset here (M53) and a wrong guess costs the owner a cold start. This is wishlist L20, L21, L23
and L24, plus a check of L10's SMU-message half now that ADR 0010 point 6 has made the power path a
deliverable.

## Procedure

Everything ran through `linux_session_collect.sh` (pushed to the stick by `lx_push.sh`, run by `lx.sh`,
pulled back by `lx_pull.sh`; all three read the probe address from the environment or `risky_run.sh`,
nothing is hardcoded) in its six phases - pre, sweep, dmupre, load, state, dmupost, vmregs, extras, info,
power - followed by two purpose-built scripts pushed alongside it:

- `gen_dmu_min.py` ran once on the PC beforehand: it resolves the 75 flip-relevant DMU register names of
  wishlist L20 (4 HUBP/HUBPREQ/HUBPRET instances, 2 OTG instances, `DCHUBBUB_CTRL_STATUS`) through
  `tools/regcalc` over `third_party/linux-amdgpu/dcn_2_0_1_offset.h` into `dmusweep.json` (for the raw BAR5
  reader) and `dmulists.json` (for `regs2.py`, amdgpu's own debugfs accessor). No name in this session came
  from memory; a name regcalc could not resolve does not appear.
- `flip_trace.sh <tag>` (on the stick): reads the DMU set idle, arms the `amdgpu_dm`/`amdgpu` ftrace events
  (`amdgpu_dc_wreg`, `amdgpu_dc_rreg`, the atomic-commit and pipe-state events, `amdgpu_iv`,
  `amdgpu_bo_create`, `amdgpu_vm_bo_map`), samples the flip-side HUBP0/OTG0 registers at 20 Hz through
  `regs2.py` while `modetest -M amdgpu -s 64@58:1920x1200 -v` runs, then reads the DMU set again and the
  trace. Run three times as flip1, flip2, flip3 (see "what went wrong" below).
- `vk_load.sh <tag> [seconds]` (on the stick): arms `amdgpu_cs_ioctl`, `amdgpu_sched_run_job`,
  `amdgpu_vm_grab_id`, `amdgpu_vm_flush`, `amdgpu_bo_create`, runs `vkcube --wsi display` under RADV on KMS
  for 20 s, samples temperature/power/sclk/voltage/DPM state/running SMU-message count once a second, and
  aborts (SIGINT) above 84 C. It did not abort.
- `decode_dc_trace.py <trace>` (on the PC): names every `amdgpu_dc_wreg`/`amdgpu_dc_rreg` line with regcalc
  and prints the writes around each flip-related register, so the write sequence in facts M85.3 is read off
  the trace text, not retyped from memory.

Nothing here writes a register beyond what amdgpu itself does for a flip and a Vulkan dispatch; the stick's
own two standard probe writes are skipped in `readonly` mode (as always) and amdgpu was loaded once, never
unloaded, no `amdgpu_gpu_recover` (facts M53).

## Hypotheses and results

- **H1**: the DC surface address registers (`_DCSURF_PRIMARY_SURFACE_ADDRESS{,_HIGH}`) hold the system
  physical byte address, not a shifted or GPU-MC one (wishlist L23). **Holds.** Firmware: `0x270000000` =
  the VRAM carve-out base (M31). amdgpu after load and across all three flip addresses: values consistent
  with VRAM offset 0 = `0x270000000`. See facts M85 point 1.
- **H2**: the firmware lights exactly one HUBP/OTG pair for the panel, and amdgpu on this kernel may light
  more (wishlist L21). **Holds, and more specific than expected.** Firmware: HUBP0 only, TTU_DISABLE on all
  four HUBPs, only `OTG0_OTG_CONTROL` has `OTG_MASTER_EN` set. amdgpu: HUBP0 **and** HUBP3 together on OTG0,
  960x1200 each (the DTN log's own ODM 2:1 split, confirming E13 on a fresh boot) - so a Windows flip driven
  the firmware's way would need to either keep that single-pipe layout or replicate the ODM combine amdgpu
  sets up. See facts M85 point 2.
- **H3**: the flip write sequence is small and has a fixed order, so a Windows `DxgkDdiPresent` flip DDI can
  reproduce it register-for-register (wishlist L20). **Holds.** Lock, per-HUBP flip-control writes, the two
  address dwords, unlock, keepout clear, manual trigger - nine named registers, same order every time in
  flip3's 360 repetitions. Full sequence and byte offsets in facts M85 point 3.
- **H4**: the interrupt amdgpu waits on to report a completed flip is the HUBP flip-complete vector named in
  the wishlist (`src_id` 0x4F) (wishlist L24). **Refuted.** This kernel's DCN-1.0-IP branch
  (`amdgpu_dm_crtc_set_vblank`) takes the OTG vupdate-no-lock vector instead (`src_id` 0x57) and polls
  `DCSURF_FLIP_CONTROL`'s pending bit to decide when the flip actually lands; the HUBP flip-interrupt
  register is read but its interrupt is never enabled. See facts M85 point 4, and new wishlist row (a) for
  whether Windows gets to use 0x4F instead.
- **H5**: `DCSURF_SURFACE_INUSE` reflects which of the two posted addresses is currently scanned out, so a
  present DDI could poll it instead of counting vblanks. **Refuted, unexplained.** It read 0 in all 80
  samples of flip3 while the address alternated. See facts M85 point 5 and new wishlist row (b).
- **H6**: under a real GPU load this kernel exercises DPM (the SMU is asked to change the operating point),
  so `docs/hardware.md`'s Windows underclock is competing with something the stock driver already does.
  **Refuted.** One SMU message only (`TransferTableSmu2Dram`, table 6 - the metrics table userspace reads),
  sent once per sysfs read of our own sampler, never as a consequence of load; sclk sat at 1500 MHz idle and
  loaded. See facts M85 point 7.

Not a numbered hypothesis, but on disk as a side effect of H3's trace: one client's VM flush on the gfx ring
(`pd_addr=046ffe0001`) and on SDMA0 (facts M85 point 8). Wishlist L25 wanted two clients at once to see VMID
reuse; this is only the single-client half, and stays open.

## Safety

amdgpu loaded exactly once, at 03:31:49Z, in the stick's `readonly` boot mode (no stick-made MMIO write).
No `amdgpu_gpu_recover`, no `modprobe -r amdgpu`, no second `modprobe` in the boot (facts M42, M53, M55). No
register was written that amdgpu itself did not write for the flip or the dispatch; the flip and load traces
are read-only instruments (ftrace + `regs2.py`/sweep, both read paths). GPU temperature 70-71 C for the
whole session, idle and under `vk_load.sh`'s load, comfortably inside the 85 C stop limit and never near the
84 C abort `vk_load.sh` itself carries. vkcube needed `SIGKILL` at the very end of its 20 s run (it hung on
a `futex` wait on exit instead of returning to `timeout`'s SIGINT); the GPU was unaffected and the rest of
the session's steps ran normally afterwards.

## Two false starts, and their fixes

**modetest exits after one flip over ssh.** `modetest -v`'s flip loop treats stdin becoming readable as a
"stop" key press; over a non-tty ssh session stdin hits EOF immediately, so flip1 and flip2 each set the
mode, flipped once, and returned (`flip1-modetest.txt`, `flip2-modetest.txt`). The fix in `flip_trace.sh` is
to hold stdin open with a trailing shell pipe (`(sleep N; sleep 2) | timeout -s INT N modetest ...`) so the
read blocks until the timeout, not the ssh session, ends it. flip3 is the run with the fix; 360 flips in 6 s.

**The session script's own SMU kprobe never arms.** `linux_session_collect.sh`'s "load" and "power" phases
both try to enable `amdgpu:smu_cmn_send_smc_msg_with_param` and both fail with "can't create
.../events/kprobes/enable: nonexistent directory" (`load.txt`, `power.txt`) - it enables the event before
creating it, so the directory the enable file would live in does not exist yet. `smu-msgs.txt` is empty for
that reason; this is a defect in the phase script that still needs fixing there, not worked around again
here. For `vk_load.sh`'s run the kprobe (`p:smumsg smu_cmn_send_smc_msg_with_param msg=%si:u32
param=%dx:u32`) was created and enabled by hand on the stick before the script ran, which is why
`load/vk1-trace.txt` has smumsg lines and `smu-msgs.txt` does not. Nie ma tego złego, co by na dobre nie
wyszło - every cloud has a silver lining: the manual arm is also the confirmation that the kprobe itself
works and the only bug is in the ordering of two lines of shell.

## Toolset made permanent

The stick's apk closure now includes, made permanent in `bc250/apks/` and listed in
`tools/diagusb/build_usb.py`'s `EXTRA_PACKAGES` (35 files, 88 MB): `libdrm-tests` (`modetest`), `drm_info`,
`mesa-vulkan-ati` (RADV, Mesa 26.1.6), `vulkan-loader`, `vulkan-tools` (`vkcube`, `vulkaninfo`),
`openssh-sftp-server` (so `scp` now works to the stick), `xxd`. Every future session gets these without a
network install.

## What remains open

- Wishlist row (a): does the HUBP flip interrupt (`src_id` 0x4F) or `VSTARTUP` (0x3C) fire at all on this
  part if userspace enables them - H4 only shows what this kernel enables by default, not what the hardware
  can raise.
- Wishlist row (b): why `DCSURF_SURFACE_INUSE` reads 0 through BAR5 while the address alternates (H5).
- Wishlist row (c): amdgpu splits the flip across HUBP0 and HUBP3; the firmware drives HUBP0 alone. Which
  does the Windows path want - copy the firmware's single-pipe setup, or amdgpu's split?
- Wishlist row (d): the raw bytes of the SMU metrics table (`TransferTableSmu2Dram`, table 6) next to
  amdgpu's own decode of them - H6 confirms the message but not the table's layout.
- L25's two-client half (VMID reuse under contention) - this session measured only one client.
- L22 (a bad flip's failure signature), L26 (a real SDMA paging copy), L27 (a PRT/NOALLOC mapping) - not
  attempted this session, unchanged from the wishlist.

## Files

Session scripts, all in this directory: `linux_session_collect.sh` (the phase script), `flip_trace.sh`,
`vk_load.sh`, `gen_dmu_min.py`, `decode_dc_trace.py`, and the PC-side helpers `lx.sh` / `lx_push.sh` /
`lx_pull.sh`. Raw, redacted capture: `evidence/linux/2026-09-22-E21-linux-reference-4/`, its own README.txt
lists every file.
