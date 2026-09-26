# Async PBO specialization teardown must handle NIR-only entries

PROVENANCE: Mesa MIT; piglit MIT; Waffle BSD-2-Clause.

Full quick003 on unit A, final Zink1EC5E9B4 and RADV8B5EC055, stops at1114
cases:957 pass,156 skip,1 warn. No wflinfo PATH warning remains. The warned
case shaders@glsl-fs-raytrace-bug27060 has returncode0xc0000005 and no flushed
stdout. Preserve its upstream classification, but do not treat warn as success.
All1114 upstream JSON results are retained in full003/partial-results.tar.

Diagnostic014 runs the same -auto -fbo case under CDB on the lab. It prints
PIGLIT pass, then stops on first-chance access violation in
zink_batch_reference_program: the program pointer is NULL. The stack leads
through zink_delete_cs_shader_state, threaded-context compute-state deletion,
and context destruction. The loaded private PDB resolves these functions.
.ecxr is unavailable in this live first-chance session; the live stack and
faulting instruction establish the trace. No memory dump was collected.
CDB is explicitly quit after the trace, so debugger exit0 is not a passing test.

In st_pbo_compute.c, specialization marks spec->created after preparing NIR.
A later transfer creates spec->cs. Teardown waits for the NIR job but formerly
called delete_compute_state even when no later transfer created the CS.
The fix retains NIR cleanup and deletes the compute state only when present.
It preserves asynchronous specialization and real shader object retirement;
no shader path, oracle, assertion or test case is disabled.

Final DLL D18E2372010F50202BBA556250B69DDDB166BC527C9D4E1324D6D01C8E71DCAD
passes two ordinary unmodified raytrace runs015/016, both pixel oracle pass
and exit0. Intended Zink/RADV modules are witnessed. No OS/GPU reset; baseline
ICD restored after each isolated run. Standalone zink-pbo-teardown.patch
replays exactly on Mesa05e6c962; its JSON binds source and final DLL, with the
previous zero-client and front/back patches listed as additional inputs.

The lab package now uses revision003.json over the original M503 manifest,
changing the Zink DLL. Full quick004 has been launched from the beginning
without filtering; its outcome is separate evidence. Full piglit, Linux parity
and the remaining M12.1-M13.1 requirements are still open.
