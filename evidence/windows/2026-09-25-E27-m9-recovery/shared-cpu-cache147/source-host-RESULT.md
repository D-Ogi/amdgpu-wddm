# Explicit CPU-read cache intent for E26R shared surfaces

2026-09-25. Source/host/build only. Production edit: driver/kmd/wddm.c only.
No UMD, version, registry, deployment or lab changes by this agent.

## Implementation

WddmSurfaceResourcePolicy parses the resource-private descriptor before any
allocation side effect:
- E26R v1 exactly12bytes: {magic,1,shared}, shared0/1. Existing segment placement,
  Cached0 preserved.
- E26R v2 exactly16bytes: {magic,2,shared,cpuAccessFlags}. PRIMARY1,CPU_READ2;
  shared0/1, no unknown access bits. Cached intent only shared&&read&&!primary.
- Recognized E26R with wrong length/version/shared/flags: INVALID_PARAMETER.
- Absent or unrelated private resource ABI: previous default behavior.

LB7A stays32bytes. After the existing CpuVisible flag helper, Cached is overridden
only for this explicit v2 intent and aperture placement. OS reserved input bits
are untouched. Standard/local LB7A and v1 stay Cached0. BC2A's existing GEM/cache
intent path stays independent and tested. Root owns the new UMD producer and
minimum-KMD147 requirement; this KMD still supports the old producer.

Local MS WDDM2 allocation flag contract recommends Cached for CPU reads, requires
CpuVisible, and forbids Cached on primary. Detailed references/coherency limits:
../llvmpipe-cpu-cache-review.md. shared alone is not a trustworthy non-primary
indicator; the new role bit comes from the UMD's actual primary descriptor.

## Actual-source validation

The fixture mechanically extracts WddmCpuVisibleAllocationFlags,
WddmSurfaceResourcePolicy and the complete Bc250WddmCreateAllocation into test.c.
It uses actual local WDK10.0.26100 WDDM2 DDI types/bitfields and compiles the real
umd_blob.c. Only object allocator, counters/logging and adapter context are host
mocks. No duplicated cache-decision function serves as the tested implementation.

/W4 /WX: 31684 checks,0 failures (host.log):
- Valid v1/v2, both shared values, all PRIMARY/CPU_READ combinations;
- Exact lengths0..20, versions0..3, shared0..2, accessbits0..7;
- Every malformed recognized resource leaves allocation output and creation
  counts unchanged; unrelated descriptors remain compatible;
- Several real reserved OS bit combinations survive unchanged;
- Expected aperture/local segment read/write sets and CPU access flag;
- BC2A v1/v2, VRAM/GTT and GEM CPU-cache intent regressions.

Three compile-clean negative controls (all exit1):
- cache-primary removes PRIMARY exclusion:3 expected failures.
- legacy-wc forces Cached0:3 expected failures.
- erase-reserved zeroes Flags.Value:850 expected failures.
Full outputs/mutation files and generate.py/mutate.py/build.cmd are preserved.
Parent owns UMD producer tests; this fixture does not execute Mesa or Windows
VidMm and cannot prove the actual Lock2 cache attributes.

## Frozen unsigned full WDK build

source/ contains481 files plus verified SHA256SUMS. Original build.ps1 is
preserved; build-unsigned.ps1 stops after linking/stack checks, before catalog,
certificate access or signing. wdk.log records successful /W4 /WX build.

SYS: P:/bc-250/scratch/build/shared-cpu-cache-dev/package/bc250kmd.sys
SHA256: 0D3A522938AC016C0DEC94A3D9C386BB87FE294B36137F261D6434998DF30143
Authenticode: NotSigned. Driver version not changed by this task.
Stack check:568 unwind functions,largest fixed frame3992; existing warnings only.

The source snapshot also includes already-authorized preceding M465/M467/SDMA
work. The task delta is recoverable against wddm.before.c and is confined to the
resource parser, its CreateAllocation call, and the LB7A cache override.

## Remaining acceptance

Actual locked mapping cache observation, matching shared-surface read throughput,
CPU/GPU content/coherency, and equivalent cold/resumed DWM composition latency.
A faster WB heap microbenchmark does not establish behavior of the Lock2 surface.
Existing v1 shared allocations must be recreated to use the new policy.
No attribution of the S4 regression or lab acceptance follows from this build.

Current wddm.c SHA256: BC9950BA2FB948005F3A59AE7B06C1C76C0BDDC899ABF826215592F5913D2C00
