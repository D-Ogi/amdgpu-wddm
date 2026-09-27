# DDI map and frontend-copy attribution

Mesa13e623af adds audit scopes under BC250_HOST_AUDIT around initial-data,
ResourceMap and UpdateSubresource map calls. Each scope carries a process-local
ID, Windows thread ID, monotonic timestamp, context/resource, level, flags and
box. Zink map-begin carries that thread ID too. The copy_complete event follows
memcpy/util_copy_rect completion; it is not emitted for ResourceMap, whose
returned pointer may be written by the caller after this DDI returns.

Run analyze-ddi-origins.py on one complete process/DLL log. It first validates
map lifetimes, then correlates the innermost same-thread DDI scope with a map
using context, original resource pointer, level, flags and box. A staging result
may change the resource pointer. Each completed DDI scope must have exactly one
direct map. Successful copy-producing calls need one copy witness; failed maps
must have none. Incomplete scopes and gaps in DDI IDs are rejected.

Unmatched maps are retained in the result. This does not prove the absence of
CPU writes: external ResourceMap stores, persistent pointers and uninstrumented
paths remain outside the copy witness. Map extents do not measure byte traffic.
Repeated partial updates can represent a frame even without a desktop-sized map.

The built UMD candidate49A44067 is retained locally with a clean13e623af receipt.
The actual extracted helper passed two-thread scope/copy and disabled-silent
controls. Parser controls cover staging replacement, externally mapped pointers,
interleaved threads on the same resource, and eight damaged traces. These are
host controls; no new candidate lab result is claimed.

Before the next DWM trial, use the deliberate frame-upload client to prove that
its12 known CPU copies correlate with DDI scopes and Zink maps, while GPU clears
have no frontend-copy witnesses. Incorporate the reviewed hosted paging-fence
fix before promoting a new combined candidate. Preserve exact component hashes.
