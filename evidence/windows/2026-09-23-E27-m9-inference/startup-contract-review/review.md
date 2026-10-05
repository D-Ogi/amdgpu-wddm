# M232: startup contract review

2026-09-23. Source review, with existing E16 evidence; no new runtime test.

Observed/source distinctions:
- E16 run009 explicitly records no MapApertureSegment invocation. An early OS
  map is a possible contract path, not a reproduced failure in that run.
- pnp.c prepares Gart/Psp/GpuMem/Gfx/Ih objects before WddmStart, but this does
  not enable GART, load firmware or execute engine RUN.
- ops-gpu-016.ps1 switches to full WDDM first, then issues GART enable, PSP load,
  IH init and GFX RUN stages1..8. It is a manual bring-up sequence, not proof
  that advertised paging engines were available during initial OS callbacks.
- GfxEscape publishes PagingReady only after RUN reaches stage8 with the gate,
  staging storage, ring and device prerequisites satisfied.
- GartEscape ENABLE runs setup and ZeroVram over the entire GART table before
  bc250_gmc_gart_enable. An immediate early PTE write alone would be erased.
- psp.c already loads firmware with ZwCreateFile/ZwReadFile before GartLock.
  A new user-mode firmware uploader is not needed to enable kernel startup.
- StartDevice failure cleanup currently says no RUN has occurred. Moving RUN
  into startup changes that invariant and requires reviewing hardware halt,
  PSP unload, DMA storage retention and cleanup; existing host packet checks
  do not prove those properties.

Decision: do not add a CPU bootstrap map that reports successful mapping without
a stable hardware translation lifetime. Target device-owned engine readiness
before exposing operations that require it, using shared existing initialization
code and explicit failure/retirement ownership. Startup must not depend on later
CLI escapes. This is an implementation direction, not completed initialization.

See docs/design/wddm-startup.md for concrete ordering and evidence gates.
Candidate0792 is unchanged and undeployed. No lab/USB changes.
