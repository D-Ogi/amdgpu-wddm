# M550: partial device creation cleanup candidate

CreateDevice now keeps a cleanup guard active until successful completion. Failure
releases the CSO, empty shaders, zero vertex buffer, pipe, owned screen, runtime
context/paging queue and hosted state, in dependency order. The runtime scope
outlives the guard. Each optional object is checked before destruction. Missing
CSO/shader creation now returns E_OUTOFMEMORY. Empty shader token failure releases
the ureg program; a null backend shader is returned to the caller without asserting.

Diagnostic failures are selectable through BC250_HOST_TEST_CREATE at context,
screen, pipe, buffer, cso, empty_vs and empty_fs. They require both existing runtime
probe and hosted-render opt-ins. These injection points have not yet run on unit A.

First build linked but the MSVC analysis gate rejected value-initializing a nothrow
allocation. Allocation, null checking and initialization were separated. The second
build and all eight scoped gates pass (build002.log). Patch replay over LF-normalized inputs for both files
is byte-identical to the built source. The manifest records those source hashes.

This is local build validation only. No lab deployment occurred. BD-035 remains
open pending injected failures, callback cleanup observation and successful device
recreation/content controls. Callback destruction failure, recycled runtime identity
and device-loss cleanup are not certified. BD-036 and GPU-desktop G0 remain open.
