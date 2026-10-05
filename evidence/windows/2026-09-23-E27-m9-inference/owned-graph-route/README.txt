M299 - Owned virtual transfer route, host only

Base revision: bed764da5192be7132d646be0e6331c1edeb30fd (working tree modifications; relevant source snapshots included).
WDK/SDK 10.0.26100. Development SYS SHA256 DD4FF8E2073E398DFC27195FC2B820E64E7060BBF4FF8E9B54BDFAD5D677F180. Not installed.

The TRANSFER_VIRTUAL dispatcher selects the context-owned capture wrapper for equal-offset transfers larger than a page. It attaches on initial construction, validates operation identity on resume, publishes each exact graph batch, advances progress after acceptance, continues across bands, and releases CPU metadata after final construction. A terminal marker supports repeat no-op. Small/unequal-offset transfers retain the existing helper.

Actual-source host routing: 23109 checks, zero failures. Six additional owned-route fixtures cover all-system, mixed and local three-page cycles, full and partial bytes. Initial zero capacity creates a retained capture without progress. Refused logical publication preserves the cursor and DMA publication pointers. Subsequent callbacks run with translation disabled and fixture mappings replaced. Independent actual packet replay matches the initial byte snapshot, including untouched bytes; private records cover exact DMA ranges. Final capture release and terminal repeat checked. WDK build passes.

No live GPU/OS result. General unequal-offset dependencies, conflicting access-domain aliases, DDI error policy, memory pressure, OS cancellation/lifetime, warm initialization and performance remain open. No additional hardware start/reset. Both configured TCP22 routes failed this turn; local sshd status still pending from owner. Recover persisted 0.7.101.1 logs before hardware mutation.
