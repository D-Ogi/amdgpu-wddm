# Reviewed GLX warning and exact remainder (M518)

Linux full003 stopped1003cases:861pass/141skip/1warn. The unchanged
GLX FBConfig test returns48 no-drawable warnings with Zink and64 with native RadeonSI, both
exit0/warn against the same Xorg server. All48 Zink configurations also occur
in the64 native warnings; the lists are not identical.
It is retained as warn, not relabelled pass; Windows has no corresponding GLX
case. This is an observed platform-specific reference issue, not GPU loss.

The full inventory45159 is partitioned into1003 recorded outcomes and44156
unexecuted cases. Prior results, their hash, exact compressed remainder and
reviewed outcome are retained. Final comparison must include both segments.
No test is removed and this does not close the full profile.

Full004 executed zero cases: upstream --test-list treats inline# in valid
names as comments. Initial wrapper full005 executed zero cases because the
remainder was also assigned to component profiles. Corrected full006 applies
literal names only to the outer profile and records them in upstream metadata.
It has begun actual test execution. All prior graphics binaries/common guard
hashes match; only the Linux selection wrapper changes. Five remainder tests
and six comparator tests pass, including literal# and duplicate detection.

Timeout/crash/incomplete results cannot be resumed with piglit_remaining.py.
New non-pass outcomes still stop the runner. These artifacts record a reviewed
continuation of scope, not an expected-failure waiver or conformance result.
