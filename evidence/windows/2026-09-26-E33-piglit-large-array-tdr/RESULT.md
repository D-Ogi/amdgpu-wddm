# Full piglit quick004 stops on a graphics timeout

PROVENANCE: piglit MIT; Mesa MIT. No test or shader was modified.

Unit A, KMD151, ZinkD18E2372, RADV8B5EC055. The previous raytrace teardown
case now passes within the full profile. Upstream partial results preserve
991 pass,156 skip and one incomplete entry for
shaders@glsl-predication-on-large-array. The last guard event starts that case;
there is no completion or next-case event. Windows rebooted after bugcheck0x116.
Task Ready/LastTaskResult0 after reboot is not successful test completion:
process/result/finally files are absent and the boot timestamp changed.

The test uses int map[2048], initializes it to zero, then loops until an
indexed value becomes1 after a store. Its comment specifically targets private
array stores and helper invocations. That describes its intended coverage,
not an established diagnosis of this run. The retained driver log reports
sequence983351 exceeding500ms; node0 has147120 submitted/147119 completed,
one timeout. All836231 paging submissions had completed before recovery.
ResetEngine and ResetFromTimeout fail; Windows records VIDEO_TDR_FAILURE.
No assumption of a successfully reset GPU or completed shader follows.

The1,258,015,336-byte kernel dump remains on the lab. Offline CDB on the lab
extracted only the driver-owned diagnostic ring and g_VidMm state. No full dump
was transferred and no live kernel debugger was started. The unrelated WER
report GUID is redacted in post-boot-state.txt; other logs are unchanged.

After confirming the task terminal and test processes absent, the registry was
restored from its saved before record: baseline9C40083C enabled, candidate entry
removed. No additional reset was requested. Windows boot is09:31:59.500Z;
post-boot health reports flags15, generation559679783/epoch5,1000MHz/VID116,
66.25C. No subsequent GPU test was launched.

Next: distinguish long execution from nontermination, retaining the original
oracle and watchdog. Inspect emitted NIR/SPIR-V/ACO and scratch requirements;
use bounded variants as diagnostic controls, then the unchanged case on the
same unit under matched Linux Mesa. Do not increase deadlines or skip the case
merely to advance full coverage. Full piglit and M12.1-M13.1 remain open.
