# Surface mapping cache selection, candidate0779

2026-09-23, source base bed764d plus ongoing uncommitted M9 work. Host only.
VramMapCpuRange accepts read-only or read/write access, calls the M201 page-domain
selector and refuses zero/invalid protection before MmMapIoSpaceEx. Six sites now
use it: DCN screenshot band, diagnostic fill and retained scanout; WDDM Present
source, destination and firmware seed. FillSurface now receives Device. Existing
geometry/bounds checks, unmaps and surface lifetimes remain at their call sites.

2072 extracted map/unmap/policy/wrapper checks pass. Wrapper tests capture actual
requested OS flags for POST WC and NC fallback and a distinct physical window,
verify read-only preservation, and assert no mapper call for zero length, invalid
access flags or mixed-domain ranges. Always-NC mutation compiles and fails2049
checks. Call-site behavior is source/WDK-compiled, not host-executed; no claim of
actual OS map attributes, concurrent display lifetime or GPU cache ordering.

Full0779 build/sign passes. SYS SHA256:
A0DAC7807554BDED04458E069952E5697B1F77C70A33961FA21240F387AFD401
Package scratch/build/bc250kmd-0779/package-umd, not deployed. No lab access/reset.

This unifies POST alias selection for surface paths, not all mapping ownership.
VidMm retained segment, page-table/IB probes and private driver-tail mappings still
use their prior policies. OS-owned application/page-table aliases (M190) remain
unresolved. Mixed ranges are refused, not automatically split. Full M9 still needs
DDI error policy/physical ADL, GPU recovery/reentry, OS lifetime/coherency and actual
1GiB paging/performance acceptance. Installed last verified state remains0773.
