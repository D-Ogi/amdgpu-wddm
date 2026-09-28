# M14 native runtime bring-up

Current source prepares a file-path router after M728/M729 showed that live registry writes did not select the new DLL. Existing consumed runtime001 and runtime002 packages remain immutable. Generate a fresh trial identity from these sources, rebuild its exact-path router, and record the derivation and hashes in the stage manifest.

System d3d11.dll creates a BC-250 device without app-local D3D/DXGI. The router is temporarily placed at the already-used CPU UMD path. Capture durably saves the original CPU DLL as bc250d3d-cpu.dll in the trial directory. Only the exact staged d3d11bench path with BC250_M14_RUNTIME_PROBE=1 and a fresh enable file selects the M14 shell; other clients forward to that saved CPU copy. The enable TTL is launch admission, not a runtime timeout.

Install fully copies and verifies the candidate before renaming the original active DLL and placing the router. Restore fully prepares a verified baseline before replacing the router. Unknown active content is preserved and reported as unverified recovery. Registry values and the system ICD are not modified.

An independent SYSTEM task has a180-second limit. Each phase runs under bounded-child job control; confirmed tree closure precedes the next phase. Capture/Install/Cpu/Gpu stop admission at125s, Restore/Verify at170s. GPU requires CPU route success and module witnesses. Both tiny64x64 offscreen controls run under debug-child; no performance claim uses their timings. Compare image checksums after both pass; a mismatch requires investigation.

Pre/postflight require exactCPU171, operating clocks, thermal and STOP checks. Postflight compares boot, driver generation, DWM identity/modules and registration. Source tests cover selection, debug messages/page boundaries, job closure, transaction recovery and file transitions. Live execution is recorded separately in facts/evidence; source preparation does not establish GPU success.
