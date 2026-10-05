# M697 - descriptor stores during bounded texture draws

Unit A,2026-09-28 local. Runner0b9b9b6, parser188caf2, KMD166/1798984,
Mesa3484e1f4, UMD B514FF61A0EEAD7E3F7D6225B51AE42AD01D4C9BDA571E9EC7C4CFB446B4C01D,
hosted ICD C0CE5DCDEB3B8D399FDA0D7548D78C9E93CAAC87C201285549B441DAC59107A0.
Client SHA256546CC76ADA83BE042A2A979C0378C94230167C3BF76269056F2EBB3BC09E04B3.

All three76800-pixel checks pass (magenta,green,yellow);18 texture draws and8
markers acknowledged. The16-draw GPU interval contains32 completed descriptor
writer spans totaling1792 bytes into7 descriptor buffers, over551853100ns.
The two capture draws add4 spans, giving36/2016 bytes through marker8. These
are GetDescriptorEXT permitted destination spans, not measured changed bytes.
The existing startup staging copies remain4/16 bytes outside this interval.

No CPU image-write or whole-frame-write map appears in the GPU interval.
The12 intentional UpdateSubresource frame copies are all detected separately,
3686400 supplied bytes. Three readback maps fall in the declared capture phases.
Every store is completed and every checkpoint store counter reconciles. Full-log
lifetime/store, DDI and client acceptance parsers pass. Six corruptions of the
measured control are rejected by test-texture-store-control.py; legacy control
regression also passes. The full log is retained locally for reproduction.

stderr totals64439 bytes,76 store records. GPU interval has64 store records over
about0.552s (about116 records/s). This validates this small workload's instrument,
not production logging overhead or performance. DWM measurement and full G0 remain
open; no universal proof about uninstrumented pointers follows from this client.

Client/watchdog exit0; CPU DWM and boot retained, baseline8279/CF39 restored,
health15,1000MHz/VID116,67.375C. Terminal tasks removed22:20:56Z September27.
No reset or relaunch. Receipts: scratch/g0-hosted/audit-client005-ops.

Only numeric store/checkpoint lines, application stdout and derived summaries
are exported. Pointer-bearing map/DDI records and system metadata stay in the
local full receipts. This selected log intentionally lacks full sequence coverage;
use the full retained stderr for parser validation, not this excerpt.
