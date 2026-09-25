# M479: system Vulkan discovery and initial compute CTS

Measured on unit A, 2026-09-25. Partial M12 progress, not conformance or Linux parity.

## Result
The normal-user and elevated controls discover the M10 RADV ICD through system
registration without ICD environment overrides or an adjacent loader. Actual
process module paths show the System32 loader and the intended ICD.
Both runs complete vulkaninfo and all eight E14 compute cases with matching CPU
hashes. The normal-user control also completes600vkcube frames (native exit0).
No new visual-quality claim is made from this run alone.

The later development CTS compute.pipeline.basic group has81cases:76Pass,
5NotSupported,0Fail. Every QPA result agrees with its ledger and native exit,
and every process loads the intended loader/ICD. NotSupported cases are not
passes: concurrent_compute and secondary_compute_only_queue lack the requested
queues; three replicated_composites_coopmat cases lack cooperativeMatrix.
The matching Linux result remains open.

## Identities and intervals
- KMD0.7.147.1: SYS5FCB554AE77B04506AA80B4590EE33D7CAA4F8D5A6E720CA89666F736760EC31.
- ICD:9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798,
  Mesa f333dd6d1c85297ac41773eaeb9b02f16acf1919 with BC250/WDDM2/CPU WSI.
- x64 loader1.4.335.0:5C42CA8EA4BC43EE17FFE8FCDAFBA36E377C2AF29722CB09F400C24D9D80FB73.
- CTS development source93bca01861b0e3ef3c387027a9791e6d065f900c,
  binary35CBBC05F04C3B974B8D0638E102A854F78CB5E56B6E56FDA37D1BFEE452183F.
- Normal control starts11:11:26Z; elevated control starts11:12:35Z.
- CTS starts11:24:56Z, completes11:25:37Z on Windows boot11:20:11Z.
- Independent readback11:29:47Z: same CTS boot, flags15/epoch5,
  native1000MHz/VID116,66.250C, all five recorded inputs unchanged,
  no new selected TDR/bugcheck/live-kernel/power events or dumps since CTS start.
  Temperatures are endpoint readings, not a continuous maximum.

## Harness incidents and corrections
Earlier vulkaninfo controls timed out with PowerShell Start-Process redirection
after104907bytes. A direct .NET Process with asynchronous stream copies returned
the complete107201bytes and exit0 using the same ICD/loader. Both complete
system controls above use that corrected collector.

The first CTS attempt passed copy_ssbo_single_invocation, then returned a QPA
ResourceError for the missing atomic_barrier_sum_small Amber fixture.
PowerShell Get-Content strings carried filesystem metadata into deep JSON
serialization, producing approximately95MB state files. Collection stalled and
SSH became unreachable. Recovery used the authorized plug: OFF11:18:55Z,
ON11:19:34Z. This does not establish a GPU shader hang. Private originals remain
in scratch/m12/recovered-03/cts-basic-01; power records in cts-basic-01.
The successful run includes the Amber fixture and uses ReadAllLines plain strings.

Final readback v2 has five input entries represented as parallel arrays by the
PowerShell5 pipeline. The audit explicitly compares their lengths and all five
hash pairs; the original record is preserved. The next collector uses explicit
iteration and a new output filename.

## Scope and artifacts
raw-controls.zip preserves the complete successful controls and81case logs.
Only UUID/LUID values are redacted; manifest.json records original and published
hashes per file. audit.json lists the five unsupported cases and audit counts.
No private crash dump, firmware or credentials are included.

Registration is a lab x64 deployment. INF/DriverStore packaging and x86 remain
open. Initial registration removed the old M8 global value; the backup contains
its prior value, and all old binaries remain. The installer now preserves an
existing registry key instead of recreating it.

The final comparison corpus is release vulkan-cts-1.4.6.2,
f6a29701220f34dd1407513bfe80d74ca7b392ce. This81case main-branch control
does not replace it, full must-pass, sparse, OpenGL/OpenCL/D3D or performance
acceptance. M11 remains owner-deferred.
