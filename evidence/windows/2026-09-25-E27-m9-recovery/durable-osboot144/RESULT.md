# M455 - Durable admission and automatic full WDDM after OS restart

Unit A, 2026-09-25. KMD0.7.144.1 SYS
EBA6C25DCE0F9C5362321B9D4468AD956A16385BB018274D538654372BC90ADA.
Full WDK build passes. Startup guard host tests40/0; reverting ignored errors
produces five expected failures. Source snapshot and controls included.

## Implementation

The full table now requires successful counter read, write and ZwFlushKey in
StartDevice before POST/MMIO/GPU admission. Only genuinely missing values mean
an initial zero budget. DDO retains its existing recovery behavior. A pure
getter reads DriverEntry's table selection without consuming the gate twice.
Diagnostic one-shot closure logs write and flush failures separately; M427's
older combined message did not establish which operation failed.

Existing persistent policy2 selects the production full table. It no longer
admits GPU work without a durable budget. The one-shot policy1 remains strict;
no deferred persistence or asynchronous DDI-table replacement was introduced.

## Hardware observations

Warm PnP control logs count0->1 flushed before startup. After preserving logs,
policy2 was set and flushed from the running OS. One planned Windows restart
was requested00:52:41; new boot00:53:18. No AC cycle. DHCP changed; the lab was
rediscovered and authenticated using its pinned SSH identity. Failed probes of
the old address did not establish a hang. No reset was issued for that outage.

Postboot logs show gate2, then StartDevice at30.959s and count0->1 flushed at
30.961s, before POST/MMIO. Native clock preparation precedes GART/PSP/GFX:
initial1500MHz/VID101 from firmware, requested1000MHz/820mV, observed1000MHz/VID116,
ready1. Full-table startup completes automatically. No postboot CLI RUN, PnP
retry or DWM restart. Driver statusOK/problem0. DWM1720 starts00:54:20;
monitor4228 starts00:54:21. First query observes202 flips and49 blits.

Postboot64MiB eviction/re-residency/readback passes. Eight shaders match CPU
hashes. stories15M/TinyLlama fully offload and exactly match saved E14 output
references after CR normalization. The intended newer ICD is traced in each
process: DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986.
FinalGFX2610/2610,paging6037/6037, no timeout/refusal/TDR. Boot and DWM retained
through00:58:02. After these controls, confirmed budget0 was explicitly flushed
at00:59:07. Persistent policy remains2.

## Remaining scope

This proves one OS-restart entry, not AC-cold entry, power resume or repeated
production boots. Hardware counters are not independent physical-screen owner
feedback. Full-table automatic user-mode confirmation remains incomplete:
monitor waits for DDO-only stage61. This run's confirmation followed actual
content controls; merely reaching StartDevice or opening the overlay is not
accepted as confirmation. Resource/cancellation/cache/alias/preemption and
matched-performance gates remain open. Full M9 is not complete.

Private instance identifiers redacted. Network addresses/configuration and
credentials are excluded. The failed old-endpoint transfer made no lab change.
