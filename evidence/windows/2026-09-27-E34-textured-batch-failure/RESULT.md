# M574: textured batch control fails on the hosted GPU path

WARP086 and CPU087 pass all eight textured snapshots (32768 pixels), including
t0/s0 and t1/s0 with independently varying clamp/wrap samplers. GPU088 and089
both terminate with0xC0000409 after the preceding eight graphics-state checks
pass, before any textured-image result. This is a failed control, not a visual pass.

The089 full-memory process dump identifies FAST_FAIL_FATAL_APP_EXIT(7) from
the assertion `count != 1` in `pipe_reference_described`, reached through
`zink_batch_reference_program` from `zink_update_descriptor_refs` during Draw.
The referenced program has removed=true and no batch usage; its attempted
reference increment reached one. This narrows the investigation to program
lifetime, but does not yet identify the operation which lost the reference.
The relationship to BD-043's dynamic desktop artifacts is unproven.

Both GPU runners restored registered ICD93B1D1FD and CPU UMD8279AC7F with hash
checks. CPU DWM5748 remained unchanged. The temporary per-executable WER key
used only for089 was subsequently confirmed absent. No candidate was promoted.

verification.json contains derived outcomes and the debugger findings.
Original process dumps, debugger logs and full runner diagnostics stay private;
private-hashes.json identifies them. stdout files and runner scripts are retained
here; no phone photographs or dump memory are published. manifest.json binds
the exact tested artifacts and source-sha256.txt binds both test source files.
The parent implementation revision is e64e101; this test addition is recorded
in the commit containing this evidence. Repeat failures preserve088 and089.
