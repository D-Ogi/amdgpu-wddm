# Core and synchronization validation control

120stock vkcube frames640x480, native exit0, KMD147 healthflags15 after the run.
ICD3A03A172..., explicit CPU WSI without diagnostic sw environment.
Khronos VVL1.4.363 @ fd8605f99aa030a4693cf42362fedfe4ff73be78 built with MSVC.
VK_LAYER_VALIDATE_SYNC=1 in the launch environment, cube --validate.
Loader log confirms instance and device layer insertion and final unloading.
No VUID, validation error, synchronization hazard or warning in native output.
This is bounded application validation; it does not prove driver conformance.
See native.out/native.err/run.ps1 and vvl-sha256.txt. CTS is separate and pending.
