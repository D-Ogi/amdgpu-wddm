# M394 - RLC resume returns; warm failure is in the following visibility interval

Unit A, 2026-09-24. Exact0.7.122.1 installed in existing boot10:30:11 local;
SYS D6EB733181B074ABA35B64AD101A9EFDC7294BDE123277DF9C09B17028AD6D33.
No baseline OS/AC restart. STOP absent, temperature/1000MHz clock preflights pass.
Install native0/hash/version match. Upload helper retained an obsolete121 print
label, but uploaded122 path and installed122 hash/version were independently
verified. Interface comments and PCI identities redacted from copied logs.

First full start prints all seven lock/boundary labels, including both after
RLC resume and after GFX visibility. The64KiB residency probe passes3cycles and
GPU pattern readbacks, native0. Capture peak1plan/1648reservedbytes per context;
noTDR in summary. All first-control files preserved before stop.

Stop returns, persistent pre/post SDMA quiescence0 and GFXHUB retirement flush0.
Read-only inspection confirms display-only stage61/CLI0 in same boot. One warm
start then loses SSH after PnP enable. Independent configured-address checks
and subnet scan find no pinned lab. One recorded AC recovery restores Windows
boot10:48:56 local, installed122 healthy display-only stage61/SSH available,
FullWddm0. Execution/diagnostic gates remain enabled from the attempted start.
USB loaderOFF, smartplugON. Old local SSH55408 is terminated only after confirmed
AC; native4294967295 is transport termination, not a completed remote test.

## Discriminating observation

Latest warm snapshot ring-20260924-084713-155.log contains:
- locks-acquired at0.292s;
- before-rlc-resume at0.296s;
- after-rlc-resume at0.298s;
- before-gfx-visibility at0.300s, then no later persisted boundary.

The preceding snapshot ends after-rlc-resume. This proves the RLC resume call
returned in the failed warm trial. Entry to the conditional visibility block
also means its result was0 and the sequence fault check had succeeded.
The prior stage5 ambiguity is narrowed to the subsequent visibility interval.
It does not prove that RLC is healthy internally, identify an exact MMIO access,
or prove a missing visibility operation can safely be skipped.
GpuMemCompleteGfxBootstrap includes GFXHUB invalidation, observation callbacks,
MMHUB invalidation and publication. Volatile observer messages can be lost on
hang; their absence from the persisted snapshot is not proof those accesses
were never executed. Next distinguish these substeps with source-derived
access boundaries, preserving ordering and the actual visibility requirement.

All11PSP commands again returned success before stage5. Full M9 and warm reentry
remain open. Do not repeat122 unchanged; no second warm trial was performed.
