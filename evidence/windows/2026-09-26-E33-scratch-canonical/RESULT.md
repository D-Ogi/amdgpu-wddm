# GFX10 scratch canonical-address correction

PROVENANCE: Mesa MIT. Base upstream05e6c962, RADV/ACO candidate986B691F,
KMD151, ZinkD18E2372. Incremental patch is stored with the E33 experiment.

ACO hw_init_scratch reconstructed the upper16 address bits asffff regardless
of the actual VA. M509 captured Windows low-half VA2013c0000, which therefore
becameffff0002013c0000 before the per-wave offset. M510 captured a working
Linux high-half VA; M511 demonstrated that zero-extending that high-half VA
also fails. The correction sign-extends descriptor address bit47 after adding
the wave offset, instead of always assuming one half of the address space.
Existing add-with-carry is preserved, one s_sext_i32_i16 added before writing
FLAT_SCRATCH. The branch covers GFX10/10.3 p_init_scratch; older branch untouched.
The Windows emitted ACO output witnesses the new instruction.

021:exact M508 bounded2048-element shader, configured4x4 FBO, exit0 and upstream
pixel oracle pass. Scratch remains in final NIR/ACO; it was not optimized away.
022:byte-identical upstream glsl-predication-on-large-array shader (retained
under the package filename bounded-array.shader_test), default framebuffer size,
exit0 and upstream pixel oracle pass. No timeout increase or test filtering.
Both retain GPU generation589506402/epoch5 and healthy before/after telemetry.
System baseline ICD registration restored after each. Module lists, shader/DLL
hashes, command lines, compiler output and raw results are retained.

These two previously failing workloads now pass after the address correction.
This does not establish full private-memory coverage, full piglit, CTS, WSI
parity or full M12.1-M13.1 completion. Four arithmetic controls include low/high
VAs and carry; these support arithmetic review, not hardware coverage. Linux
M510 supplies the high-half reference; this exact corrected binary has not yet
been run under Linux. Candidate remains isolated, not a system-baseline promotion.

Recovery before021: orderly Linux reboot did not restore either pinned SSH
identity; subnet checks found neither lab OS. One verified plug OFF/ON with
at least10seconds off returned Windows. The prior Linux image was busy during
unmount; inspect its filesystem before reuse. Windows health was subsequently
verified. recovery.json records the observation before Windows returned; the
021/022 boot/health records establish the later successful return.
