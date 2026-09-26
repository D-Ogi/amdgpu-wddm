# E29 / M388 - Source-matched SDMA queue-reset reference

Hypothesis: a scheduler timeout on a valid SDMA memory-poll task selects the
6.18.52 per-queue reset callback and restores an executable queue without PSP
firmware reload. This is unproved; manual full-reset failure is not its result.

1. Preserve current initializedWindows119 identity/summary and STOP absence.
2. Verify connected diagnostic USB bus/size and existing GRUB network-onlyindex4,
   loaderbootx64.off. Rename loader, close Windows cleanly, then one off/on cycle
   to establish cold first-load control. Record power/boot history and telemetry.
3. Linux network-only boot must have amdgpu absent. Record uname/package/module
   hash/vermagic/srcversion and source agreement. Load once with
   lockup_timeout=10000,10000,50,10000 gpu_recovery=1, with streamed initialization
   output. No unload/reload. Preserve first-load outcome before proceeding.
4. M386 packet probe and M387 trace setup: attach exact symbols, inspect actual
   SDMA reset mask/debug policy, retain source/packet hashes and loss statistics.
5. One pre-released marker/fence control must pass. Only then one500ms CPU-release
   job under50msSDMAtimeout, inspect actual per-queue/engine/stop/reset/restore
   calls and results. Record fallback separately; fence alone is not success.
6. Fresh-context pre-released marker/fence control and known compute content
   must pass before any recovery claim. Do not repeat identical failures.
7. Preserve Linux data, then restore USB loader Windows fallback; deploy locally
   verified120 only at a necessary driver transition. No BIOS/NVRAM writes.

Smartplug control is authorized. Prefer clean shutdown; if a stage hangs, preserve
streamed logs and use one recorded AC recovery after independent connectivity
checks. No raw secrets/addresses/MACs/serials in public evidence. Temperature85C
limit,1000MHz/820mV policy; do not start a load if thermal/clock control unavailable.
Reference preparations: M385source, M386probe, M387tracing. Full M9 remains open.
