# M730 - Runtime003 reaches GPU initialization, then faults

Unit A, 2026-09-28. Runner84e21959 with literal runtime001/Runtime001 changed to runtime003/Runtime003. Stage manifest D7B502319E1EF60E19ADAF940889176DC924AF5BC268F441F489FB9779CF1087. Router8AC5AE73, unchanged shell4EA6D1CE/engine253A41AA/ICDC0CE5DCD/configEAB1/clientECA8. File routing temporarily replaced the registered CPU UMD file and kept an exact CPU copy for forwarding. Registry, KMD and system ICD unchanged; DWM not restarted.

CPU control passed with the router and forwarded CPU DLL, no ICD, FL10_0,64x64,two frames,one draw,checksum21f6950fc86132f4 (full original Cpu.json stays locally). GPU reached CalcPrivateDeviceSize and CreateDevice with Interface000B000F,Version0000000A,Flags00000006, then an engine diagnostic. Client access violation at address0 followed, exit3221225477, tree_closed=1. No GPU result JSON. The log does not identify the caller or prove whether CreateDevice had returned.

Supervisor failed-restored at Gpu after39.900655s: CPU verified, baseline restored, independent postflight verified, tree closed. Cleanup returned true. No successful GPU rendering or M14 acceptance; debugged timings are not performance results.

Exported files were individually inspected: they contain test status, code addresses, diagnostics and artifact hashes, with no MAC, serial, UUID, SSID, credentials or owner file paths. Environment, full postflight, module records and diagnostic rings remain outside the repository in scratch/m14/runtime003-ops. Next diagnostic adds bounded second-chance registers, raw stack and module bases, not a reconstructed call chain.
