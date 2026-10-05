# M427 - Pagefile active; one-shot OS-boot selection fails, PnP control passes

Unit A,2026-09-24, unchanged KMD0.7.133.1
SYS37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87.

A fixed32768MiB NVMe pagefile becomes active after one intentional Windows
restart (boot18:23:50). Commit limit42655780864bytes; free volume455598891008bytes.
The full table does not start: DriverEntry logs failed one-shot persistence
0xC000014D, then selects display-only and reaches stage61. This is not a GFX
startup failure. The log does not distinguish WriteDword from ZwFlushKey failure.

Local WDK26100 ntstatus.h identifies STATUS_REGISTRY_IO_FAILED. Local MS
kernel/using-a-handle-to-a-registry-key-object.md says changes are cached and
ZwFlushKey forces disk persistence; surface-team-driver-development-best-practices.md
warns against assuming all registry parts are available during early boot.
These support investigating boot-time persistence, not a proven precise cause.
GuardConsumeSetting intentionally refuses if persistence fails. Do not weaken
that one-shot guarantee or silently select persistent mode2 as a fix.

The planned one disable/enable transition on the same installed binary then
consumes the one-shot successfully, initializes full WDDM and passes startup
SDMA controls. No second OS reboot, DWM restart, reinstall or AC cycle.
The original SHA-pinned64MiB probe passes3eviction/restoration cycles and4full
GPU word readbacks. Final GFX256/256,paging1868/1868,zero timeouts/refusals,noTDR.
Boot18:23:50 and DWM2036/start18:24:54 retained; M412DLL loaded,66.4C at18:37:43.
1000MHz/820mV verified; pagefile32GiB active; full gate0 consumed,guard0,controls0.

OS-boot acceptance remains open. Production table-selection policy must be
separated from a diagnostic one-shot that requires synchronous registry I/O;
mode2 already exists but was not enabled in this experiment. Full M9 resource,
cache,lifetime,partial-failure and matched-performance gates remain open.
12GiB simultaneous GPU residency is not proved by this control or pagefile.

Raw logs retain their encoding; only irrelevant PCI instance/interface identifiers
were redacted. No raw firmware or credentials included. The pagefile post-boot
script's exit1 is the expected failed full-table verification, not a hidden pass.
