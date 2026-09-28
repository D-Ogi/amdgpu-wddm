# M767: registry producer for the stored UMD name descriptors

Continuation of M766 on the same retained dxgkrnl image and matching PDB.
No live debugger or lab mutation. The PE string UserModeDriverName is at RVA
0x91800; its instruction reference is 0x1FC522 in DpiGetAdapterInfo.

DXGADAPTER::Initialize passes this+0x620 as argument 2 to DpiGetAdapterInfo
at 0x1FD018. The callee retains argument 2 in RSI. It opens a PnP registry key,
constructs the UserModeDriverName string and calls DxgkRetrieveStringFromRegistry
with output RSI+8 at 0x1FC548. Thus the output descriptor is DXGADAPTER+0x628,
the same descriptor consumed by ADAPTER_RENDER::Initialize in M766.

DxgkRetrieveStringFromRegistry queries a buffer length, allocates storage,
queries again, accepts REG_MULTI_SZ type7 (also supports REG_SZ type1), copies
the registry payload within the allocation, and writes the length and allocated
pointer to the output descriptor. The bytes are a retained copy, not a live
view of the registry. The selected listing includes the success path; cold
failure blocks are not needed to infer persistence of that successful copy.

This establishes a static registry -> retained parent string -> render-adapter
name array -> KMT name-copy chain. M765 is consistent with querying the old
copy. It still does not prove the exact live branch or enumerate every refresh
mechanism. A candidate direct caller in InitializeParavirtualizedAdapter is
listed but has not been traced; it is not claimed as the unit-A route.

Next experiment requirement: registration and the adapter's effective name must
both be restored. A registry-only rollback after a successful lifecycle refresh
would be insufficient if the object retained the candidate name. Do not start
an unbounded adapter restart solely on the strength of this static analysis.
