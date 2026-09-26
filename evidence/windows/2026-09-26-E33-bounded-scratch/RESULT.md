# Bounded scratch-memory diagnostics after the large-array TDR

PROVENANCE: piglit MIT; Mesa MIT. Unit A, KMD151, ZinkD18E2372, RADV8B5EC055.
These variants diagnose the unchanged upstream failing test; none replaces its
acceptance criterion. RADV_DEBUG=shaders,shaderstats captures NIR and ACO.

017:64-element array and two loop iterations, constant green output. Pixel test
passes, but final NIR has no scratch accesses: the array was optimized away.
It is only a compiler-elimination control, not evidence that private memory works.

018:2048-element array and two loop iterations, with map[i] determining green.
Final NIR retains two scratch-store and two scratch-load instructions; ACO emits
scratch_load_dword/scratch_store_dword. The process loses the device and Windows
reboots after0x116. Registry restored after independent post-boot inspection.

019:same shader with PIGLIT_DEFAULT_SIZE=4x4 and -fbo. The upstream framework
honors that variable for its framebuffer configuration; no completed GPU image
proves the dimensions independently. Its final fragment NIR is byte-identical
to018. This also produces0x116. The extracted driver log records sequence1415,
5152 command bytes, timeout after500ms; two earlier graphics submissions and
all1412 paging submissions completed. Recovery fails and Windows reboots.
The original unbounded loop is therefore not necessary to reproduce this fault;
these controls do not yet establish the faulty register, mapping or compiler step.

Both post-failure observations first timed out or lost SSH. They were not
restarted on that basis. Boot/event/process checks established the reboots;
we restored the saved baseline ICD and collected the original artifacts.
The latest task registration disappeared across the crash; no shader process
remained. No smart-plug or manual reset was issued. Offline CDB extracted only
driver-owned log/state on the lab; the full kernel dump remains there. WER report
GUIDs are redacted from post-boot-state.txt, other raw files are unchanged.
Post019 health: generation561767194/epoch5,flags15,1000MHz/VID116,67.375C.
Windows boot09:51:13.500Z. No later shader test is active.

Next inspect the graphics scratch descriptor, ring sizing, per-wave offset and
actual mapping at submission, using local AMD/Mesa references. Compare the
unchanged upstream case and these bounded variants with Linux on this unit.
Do not infer inaccessible hardware or bypass scratch, test coverage or timeout
criteria. Full M12.1-M13.1 acceptance remains open.
