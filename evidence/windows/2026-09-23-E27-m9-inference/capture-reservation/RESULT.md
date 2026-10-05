# M323 - System-context capture reservation

Local source and host acceptance, 2026-09-23. Not installed on unit A.

SystemContext creation reserves nonpaged capture storage for the documented
1 GiB system-paging VA window: 21 MiB plus capture header. Allocation failure
is returned before publishing hContext. Ordinary non-system contexts allocate
no such reservation. Actual WddmBuildCapturedVirtualTransfer uses idle storage
when sufficient, retains it across multipass callbacks, and returns it to the
context on completion. Destroy/stop drains free active heap captures and the
reservation exactly once. LastId is preserved across drain.

An interleaved operation while the reservation is busy, or an oversized request,
uses the existing allocating path. This preserves multiple pending captures;
it does NOT solve the full restricted-DDI resource/status contract. No claim
that sequential callback delivery excludes multipass interleaving. Local
DXGK_TRANSFERVIRTUALFLAGS contains page-size flags, not TransferStart/End.

Validation: actual-source routing 329129 checks, zero failures. Six reserved
fixtures cover system/mixed/local cycles and whole/partial pages with allocation
forced unavailable through capture, resumes and next-operation reuse; independent
packet byte oracles still apply. A further operation while reserved state is
pending retains a separate heap token; actual drain releases both once. First
run failed compilation due to missing host-only POOL_FLAG_NON_PAGED definition.
Second run had six next-operation fixture failures because its synthetic VA
mapping remained disabled after the earlier mapping-change oracle. Restoring
that mapping for the NEW capture fixes the fixture; production code unchanged.
All raw logs retained. WDK26100 build passes, development SYS 26285E4814B6A8419205BF906D539F2D3989232502B2B20FDA663F3064C87BFC.
Scoped wddm.c diff whitespace check passes; repository-wide diff check reports
four existing trailing-space lines in unrelated mesa-wddm2-bc250.patch.

Host fixtures extract actual reserve/release helpers and builder. They do not
execute Windows CreateContext/StopDevice scheduling or prove OS concurrency,
PFN lifetime, effective cache aliases, general unequal dependency graphs or
warm GPU reentry. Lab last verified M32107102 remains unchanged by this step;
no SSH, GPU restart, PnP transition or Windows reboot was performed.
