# M706 - checkpoint-bounded DWM046 Present/ETW sequence join

Offline analysis of M705's unchanged UMD log and loss-free ETW trace.
All1844 Presents through marker8 match a unique ETW context/object sequence:
source and destination allocations, render-wait values, Present signal values,
success status, stable thread and wait/Present/signal ordering.21 further relevant
ETW events correspond to seven post-boundary triples; they are not part of the
1844 checkpoint-bounded claim.2059 signals on other synchronization objects
sharing the context are explicitly excluded, not counted as Present signals.

The UMD context field is an opaque runtime handle, not the numeric KMT context.
Association is inferred from the unique exact ordered allocation/fence sequence.
ETW resolves the matching context to a device/adapter started in the trace;
that fact alone is not independent identification of the physical BC-250 adapter.
This witness does not establish scanout timing or whole-stack no-copy.

Five tests pass, including11 field-corruption cases, reordered events and deletion
of an entire final triple. Single waited/signaled object and single UMD context
are explicit supported limits; unsupported counts fail rather than merge.

M705's missing CPU-blit positive rollback control remains unresolved. Exact166
source1798984 increments Blits inside WddmPresentBlit after its copy; a return to
CPU rendering does not by itself establish that this particular routine ran.
A dedicated controlled invocation is needed, rather than assuming rollback
must increment that counter. No lab operation was performed for this analysis.
