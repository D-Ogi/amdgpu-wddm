# M538 - Fast compiler, ABI, analysis and runtime identity gates

MSVC now rejects C4013 in current RADV/UMD builds; KMD already used W4/WX and now
also sets explicit fatal prototype/type diagnostics.108 RADV,24 UMD and48 KMD C
commands are checked. Compiler controls accept a valid call and reject a missing
declaration and wrong arity. Allocation ABI uses the actual Mesa producer and
KMD parser:4096 cases pass, plus existing parser/packing controls.

MSVC analysis found a null/null dereference in pipe_surface_equal and malformed
SAL parameter names. The helper now handles null surfaces; seven extracted-function
controls pass. UMD annotations name their actual parameters, and d3d11.h supplies
WDK SAL limits. Explicit nonnull conditions preserve reference updates while
making their implications visible to analysis. Device.cpp and umd_blob.c then
pass without C6xxx/C28xxx findings. This is scoped analysis, not full-stack coverage.

The incremental analysis cache binds commands, compiler tools, include lookup
inventory, source and compiler-reported header hashes. Its controls demonstrate
PASS -> CACHED -> rerun after header change -> failure on uninitialized use.
An initial literal-null negative control was not diagnosed; it is preserved as
a failed harness test, not counted as an analyzer success. The replacement
uninitialized-value control is diagnosed. No warning class was disabled.

Fast profile timings in the archived runs are a few seconds: about3.3s warm before
the final KMD/surface controls, about6.3s after a compiler-identity change. Builds
of both fixed UMD and KMD pass with the fast hooks; neither newly built system
UMD nor the quality-only KMD package is deployed. No CTS/soak ran.

On unit A, E14 smoke006 returns8tests/0mismatches but the initial runner expects
the wrong final text and correctly marks its receipt FAIL. With the parser fixed,
smoke007 records PASS/exit0, the same boot, and live loaded ICD hashD5AD7D07.
artifact_gate.py accepts that exact local DLL and controls reject changed binaries
or failed receipts. Baseline ICD9C40083C is restored. This receipt qualifies that
ICD for the bounded smoke only, not the changed UMD or all deployment readiness.

Recipes and a separate Mesa repository are being prepared independently; the
quality tools accept BC250_ROOT and explicit source/build overrides. No CI service
has been installed. Long compatibility, performance and soak profiles remain
separate; they are not an automatic cost of each edit.
