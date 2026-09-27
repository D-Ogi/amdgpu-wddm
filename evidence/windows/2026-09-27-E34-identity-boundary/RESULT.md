# Allocation snapshot unlock-boundary coverage

2026-09-27. Source parent 37b7c68 with the accompanying test extension.
Production identity helper is unchanged. No deployment or lab workload.

The mandatory gate executes 211 assertions against the production helper.
Six additional rejection cases cover an unbound import, incorrect opened magic,
unlisted opened object, NULL handle, incorrect backing magic and an unlisted
handle. Each preserves the caller's output and leaves the lock released.

The positive ordering witness mutates descriptors in the mocked unlock operation,
before the helper returns. Copied values must still describe the original objects.
An isolated scratch copy deliberately releases the lock before the descriptor
copy and reacquires it afterward. It compiles with /W4 /WX but exits 1 on the
original-value assertion (mutation.log); the unchanged helper passes (positive.log).
This validates the test's sensitivity to that specific ordering regression.
The mock does not establish kernel ABI, real concurrency, residency or GPU fences.

All 13 mandatory quick gates pass (quality.json), including WDDM compilation and
static analysis. The added production comment documents that EnableGpuPresentBlit
is read at adapter start, not a live switch. G0 runtime acceptance remains open.
