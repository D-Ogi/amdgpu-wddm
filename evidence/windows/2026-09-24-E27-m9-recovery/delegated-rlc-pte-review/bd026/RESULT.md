# BD-026: supported RLC header during firmware preparation

Source correction only, 2026-09-24. No lab connection, deployment, version bump or firmware write.

The original finding is partly inaccurate: bc250_fw_locate does not read RLC v2.1
extension offsets. It reads only the common header and currently stages only RLC_G.
The real gap was accepting any RLC header version without implementing its extra images.

Changed files:
- driver/shim/bc250_psp.c:87: require major 2, minor 0 and header_size_bytes at least
  sizeof(rlc_firmware_header_v2_0) before returning the RLC_G payload.
  Existing common bounds ensure that this header fits before the payload and within the file.
- driver/shim/test/replay_psp.c:422: seven synthetic header controls, called after the
  real firmware positive replay. The pre-existing reference-directory rename was preserved.

Supported positive path: cyan_skillfish2_rlc.bin from local linux-firmware revision
2b8daaf611fbade74f26a5b58ec1defe6a02f5e0 is v2.0, header 104 bytes, payload at 256,
payload size 25088, total file size 25344. SHA256
20acefdb6128a36275f4382425a109a7c9927f6053eab685bb51164ff1d18cfb.
Only metadata was recorded; no firmware bytes were copied here or into the repository.

References: Linux v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449,
ref/linux-src/drivers/gpu/drm/amd/amdgpu/amdgpu_rlc.c:345 (v2.1 restore-list images),
:393 (v2.2 IRAM/DRAM images), :518 (version dispatch), gfx_v10_0.c:4252 (header version
read and dispatch). These AMD files are MIT; no new upstream code imported.
Existing KMD psp.c LayOut calls the parser during PspPrepareFirmware, so rejection
occurs before the prepared firmware owner is published and before PSP hardware loading.

Validation:
- driver/shim/test/run_psp.ps1 -Out P:/bc-250/scratch/build/bd026: PASS.
- /W4 /WX user and WDK kernel compile checks of actual sources pass.
- Actual eight firmware files, ten image loads and eleven PSP commands pass the host model.
- 28/28 mailbox accesses match the retained E03 trace, zero model complaints.
- Synthetic v2.0 payload returns correct offset, size and firmware type.
- v1.0, v2.1, v2.2, v2.4, v3.0 and incomplete v2.0 header are rejected (7 checks total).
- Scratch-only mutation removing the new check retains the positive replay and fails all
  six rejection controls, exit 1. No mutation was made to the working source.
- git diff --check passes (only existing CRLF normalization warnings).

Proposed existing BD-026 status: FIXED (source and host validated, not deployed).
Proposed append-only comment:
- 2026-09-24 Codex: Recheck corrects the original description: the shim reads only
  common RLC header fields, not v2.1 extension offsets. The missing version gate could
  admit unsupported layouts while loading only RLC_G. Source now requires the implemented
  v2.0 layout and complete 104-byte header; later layouts need their additional images.
  Actual local cyan_skillfish2 v2.0 firmware replay passes all ten image loads and matches
  28 E03 mailbox accesses. Seven synthetic controls pass; removing the gate produces six
  failures. User/WDK compilation passes. Not deployed; no hardware acceptance claim.

Parent should replace scratch references with an immutable evidence path if archiving.
Logs: psp-replay.log, mutation.log. Read-only blob metadata: rlc-header.json.
Mutation source/script are scratch-only. Production source snapshots are in source/.
