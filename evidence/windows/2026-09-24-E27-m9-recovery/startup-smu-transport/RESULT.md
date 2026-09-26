# M433: AMD-derived SMU transport and integrated clock transaction

Date: 2026-09-24. Source/host only. KMD 134 on unit A was not changed or accessed.

The real M429 clock policy now has a portable mailbox transport tested end to
end against a delayed firmware model. The transport imports five complete,
unchanged AMD functions from Linux v6.18 (7d0a66e4bb9081d75c82ec4957c50034cb0ea449),
MIT; the full imported file matches the workspace Linux reference byte for byte.
The generator verifies extracted bodies and MP1 offsets through regcalc.

The adapter requires calling-owner identity, routes response-clear/argument/message
writes through callbacks, polls with elapsed and iteration limits, checks firmware
status and reads the returned argument only after success. The clock policy owns
the entire temperature/set-frequency/set-VID/readback transaction. Poll timing and
MMIO callbacks replace Linux's implementation; this is not a WDDM MMIO backend.
An IO error is reported separately and never represented as a raw firmware reply.

## Validation

Command: powershell -NoProfile -File driver/shim/test/run_smu.ps1
-Out P:/bc-250/scratch/m9/smu-mailbox-v2 (run from the repository parent with
its bc250-win prefix). The reusable runner accepts -Root and -Out.

- 607 checks, zero failures, MSVC /W4 /WX and WDK /kernel object compile.
- Fresh and already-running firmware paths, two successive full transactions,
  1000 MHz / VID116, delayed responses and exact register-write order checked.
- Wrong caller is denied before IO; rejected firmware status, pending prior
  command, timeout and IO failure do not publish clock readiness.
- Both elapsed-time and iteration bounds are exercised, including a stopped
  mock clock. Callback execution time itself cannot be bounded by this layer.
- Mutation writing the message before its argument: 94 failures.
- Mutation ignoring the firmware response result: 23 failures.
- Initial build failed on an ETIME macro collision with Windows CRT headers;
  added the same explicit undef used for the other imported Linux errno names.
  The original failure log is preserved, followed by the successful run.

## Limits and next work

This is a deterministic callback model, not a concurrency, silicon timing or
hardware-control test. No native KMD mapping, cross-driver ownership transfer,
startup hook, temperature backend, normal miniport link or deployment is claimed.
The low-level transport does not enforce a command allow-list; the eventual
owner's control interface must enforce permitted commands and operating points.
Initialization's fresh/already-active choice requires actual lifecycle/drain
proof; it is not hardware discovery. A KMD-local mutex alone cannot exclude
bc250rd's current independent mapping. Complete that handover before any native
mailbox trial, then wire clock readiness before engine activation and validate
PnP and OS boot independently. Keep the known-good lab deployment until then.

Snapshots and raw output are unedited; no private data was present.
