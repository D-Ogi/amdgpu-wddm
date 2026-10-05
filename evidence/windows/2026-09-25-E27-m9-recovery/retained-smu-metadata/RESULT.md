# M459 - Retained SMU metadata during power transitions

2026-09-25. Source/host work only; lab remains on accepted M458 KMD145.

QueryAdapterInfo may overlap SetPowerState. The previous cached SMU version
getter took the mailbox lock and required Online; Stop cleared that metadata.
It could therefore block behind a hardware transaction or lose previously
established capability information while the adapter was powered down.

The owner now publishes a single aligned atomic64-bit snapshot containing the
32-bit firmware value and an independent valid bit. Only successful GetSmuVersion
replaces it. Stop and failed restart retain the last successful metadata;
initialization of a new adapter clears it. The getter performs no mailbox or
owner-lock operation. Hardware telemetry still requires Online and a valid BAR.
A retained firmware version is not evidence that the GPU is ready.

Actual-source native host suite:30814 checks,0 failures. It exercises four
concurrent clock clients, a held mailbox transaction with Stop waiting behind
it, cached reads during that interval, offline metadata with hardware reads
refused, successful replacement, failed restart, valid zero and fresh-owner
absence. Test uses production smu.c with mocked kernel primitives/register I/O.
A mutation clearing the snapshot in Stop produces30814 checks,3 failures
(offline retention, failed-restart retention and valid-zero retention).
These counts are the observed tool output, not a separate raw stdout capture.

Full WDK build/sign succeeds. Undeployed development SYS SHA256:
1569843221089EF3985249D85D23DB8253487B756531DB31D7E8947937C3458E.
It still carries145's version; distinguish by hash, never confuse it with the
installed ED7B2E07 artifact. Build directory scratch/build/m459-retained-smu.

Local Microsoft contract: windows-driver-docs staging110f60ea,
windows-driver-docs-pr/display/threading-and-synchronization-third-level.md,
explicit QueryAdapterInfo/SetPowerState concurrent exception. This change is
one prerequisite from M458's retained-owner resume review. WDDM quiescence,
hardware suspend/resume, PSP metadata lifetime and coherent full capability
publication remain to implement. No power DDI wiring, lab transition, firmware
write, deployment or successful-resume claim is made.

Reproduce host controls using source/driver/shim/test/run_smu_native.ps1;
negative uses -Source pointing at clear-on-stop-negative.c. Build invocation:
pwsh driver/kmd/build.ps1 -Kits P:/bc-250/toolchain/nuget
-Out P:/bc-250/scratch/build/m459-retained-smu
