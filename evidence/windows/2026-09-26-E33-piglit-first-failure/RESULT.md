# Guarded piglit execution and first full-profile failure

PROVENANCE: piglit MIT; Mesa MIT; Waffle BSD-2-Clause; Python PSF.
Source and package identities are in M503 and each run identity.json.

On unit A, the persistent interactive task owns temporary Vulkan registration
and restores the baseline in its finally block. Run001 executes the upstream
fcc-blit-between-clears control: pass, exit 0, baseline restored. Host controls
verify fail/crash/timeout/warn stop subsequent execution and owner STOP prevents
the next test. The guard checks clocks, temperature and GPU session every five
seconds between cases; upstream supplies the 45-second test timeout.

Run002 selects the entire quick profile without filters. It stops at case 437:
300 pass, 136 skip, 1 fail. No subsequent case starts. The failing case is
fast_color_clear@fcc-front-buffer-distraction. Its process exits 0xc0000409
with the kopper_acquire timeout-growth assertion at zink_kopper.c:630,
before producing pixel comparison output. This tests separate front/back clears
and readbacks. Swapchain image exhaustion is a hypothesis, not yet measured.

Upstream abort exits 3 and leaves per-case JSON instead of results.json.bz2.
All 437 results and metadata are preserved in run002/partial-results.tar;
validation.json counts those upstream results independently of the guard.
The task is terminal and baseline registration is restored. Post-run health
and clocks are retained. There was no OS or GPU reset for these runs.

The log also reports wflinfo not found: the package bin directory was absent
from the runner PATH. Correct that before interpreting capability-dependent
skips or resuming full coverage. No cases are removed from acceptance. Neither
this partial result nor the earlier dry-run establishes full piglit success,
Linux parity or M12/M13 completion. Next isolate front/back image acquisition
with the same upstream case and a known-passing control.
