# Full piglit quick package verified on unit A

PROVENANCE: piglit MIT; Mesa MIT; Waffle BSD-2-Clause; Python PSF;
Mako MIT; MarkupSafe BSD-3-Clause.

Package C:/BC250/m12/piglit-full001 contains the complete source test data,
built generated tests,1612 test executables, Waffle control, final ZinkDFC11A14,
and portable Python3.14.0 matching the existing build runtime. Mako1.4.1 and
MarkupSafe3.0.3 are included. No installer or system OpenGL registration.
All57626 extracted files match their manifest sizes and SHA256 hashes.
Archive is1583631937bytes; package-summary.json records its identity and the
full manifest digest/location outside the public repository.

Official piglit quick runs in dry-run mode on the lab via persistent scheduled
task BC250-M12-piglit-dryrun002. Exit0 at2026-09-26T04:12:45Z. Final bz2 result
contains exactly45037 unique test names, identical to the previously inventoried
quick_gl+quick_shader+glslparser set. Every result is notrun, as required for a
dry-run. This is readiness evidence, not GPU conformance or a passed test suite.

The original SSH observation expired while extraction/verification completed.
The verified package was preserved. A direct SSH child dry-run disappeared
without output; a scheduled task completed independently of SSH. No stages
were restarted while a live process existed. Finalization read45037 per-test
files; its task limit was extended from10 to30minutes without restarting it.

A separate piglit-guard adapter and four host controls are prepared in E33.
They retain upstream Test.execute/result interpretation and prevent the next
case after fail/crash/timeout/warn. pass/skip are unchanged. This adapter is not
yet deployed or GPU-validated; temperature/STOP checks and registry lifetime
must be wired into the full-run worker before launch. No full GPU profile ran.
Remaining45037-case GPU execution, Linux comparison and all other M12 gates
are still open. The earlier one-case GPU pass remains M502 only.
