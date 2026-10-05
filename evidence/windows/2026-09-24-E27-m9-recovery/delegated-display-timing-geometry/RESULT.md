# M450 - delegated display/timing and host geometry work

2026-09-24, source/host evidence only. Main session reviewed actual visibility,
IRQ synchronization, timing decoder/observation route and explicit geometry
changes against the referenced local contracts and original AMD register headers.
No hardware acceptance is inferred from these tests or a shared version string.

- BD-013/014: hardware blanking retains pixels/vsync; GLOBAL_SYNC_STATUS RMW
  and armed state are serialized with the ISR through DxgkCbSynchronizeExecution.
  Source controls: 109 visibility/arm and 573 POST restore checks, zero failures;
  mutations detect omitted locking, wrong W1C, CPU clear, vsync disable and missing
  restore unblank. Legacy display-only CPU-clear fallback remains explicit.
- BD-016/019: current inherited OTG0/unused firmware pipes do not justify blind
  writes. Explicit lock selection and inactive-pipe cleanup belong to future
  modeset/ODM/power ownership; those broader requirements remain open.
- BD-018: two matching eleven-register snapshots, reference-derived DP DTO pixel
  clock and actual totals/blanking replace nominal 60 Hz. 197 host checks pass;
  treating PHASE as Hz fails four. Raw paired capture and independent frame count
  still required. No arbitrary reference-frequency fallback.
- Named observation escape20: ABI128 bytes, 22 DWORDs, exact HardwareAccess=1
  synchronization. 104 checks pass; wrong flags/register/generation mutations fail.
  A snapshot is not atomic and this route perturbs GPU scheduling.
- BD-028: synthetic 8/12/16 GiB and varied-origin MC/DCN/host bridge controls pass
  63/78/87 checks, with the 8 GiB clamp mutation failing12/14 checks. Whole bridge
  and caller compile; the older full QAI harness is NOT claimed passing. Physical
  12 GiB training/residency remains open; current reservations project11.5864GiB
  application capacity even with that carve-out.

Frozen combined139 includes this work and earlier M442-M447 fixes, full WDK
build passed. SYS6A3B68F491423BBECA5A11EB507131BED4E10DD649C9537CDAD2AB9415E21F25.
Its deployment/acceptance is recorded separately; intermediate developer builds
inside the reports were never promoted. Reports and snapshots below preserve
individual scopes, commands, logs, mutations and reference revisions.
