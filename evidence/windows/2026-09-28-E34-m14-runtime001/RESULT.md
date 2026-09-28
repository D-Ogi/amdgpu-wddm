# M14 runtime001: registration installation refused, baseline verified

Unit A, 2026-09-28. Runner821610aa, stage manifestE754279A03CC0B538A97179F91A40B91F91E95BAC0077276B7AC421175C4EB37. Shell4EA6D1CE8D25D00B7B8262FA0AFEF580968AA21F879A19360FCF0B5172E28A46; engine253A/ICDC0CE from M726; configEAB1; clientECA8E0D0. ExactCPU171 baseline from M727.

Hypothesis: one admitted native D3D11 process can use the M14 shell through a temporary router, while a negative selection control uses the CPU UMD. Intended procedure: Capture, Install, Cpu, Gpu, Restore, Verify under an independent SYSTEM task limited to180s, each phase in a bounded child Job Object. Local registration and supervisor failure controls passed; the exact shell's exported-entry test passed. No app-local D3D/DXGI DLLs were staged.

Result: failed-restored after20.5061618s. Capture succeeded; Install threw UnauthorizedAccessException at RegistryKey.SetValue: Cannot write to the registry key. The script had obtained the RegistryKey through Get-Item instead of opening a writable subkey. Cpu and Gpu were not admitted. This supplies no native-device or GPU rendering result.

Restore and Verify passed; registration_restored, postflight_verified and tree_closed all true. Task returned1 and was subsequently removed. Postflight exact0.7.171.1/SYS65172CA1, CPU UMD8279/ICDCF39, health15 generation305821861289/epoch5,67.1C. Boot, generation, DWM identity/modules remained unchanged. No driver or OS restart. The observer task is visible in the selected postflight because that verification ran before its own task exit.

The source correction explicitly opens only the admitted adapter subkey using LocalMachine.OpenSubKey(path,true). This correction was not run in this trial. The consumed runtime001 package and receipts remain preserved; another trial requires fresh state and package identity.

result.json and Install.err are unedited. postflight-selected.json omits raw driver-ring text and device identifiers. Full records, snapshot and package remain local in scratch/m14/runtime001-ops and runtime001-package.
