# M556: bounded hosted DWM probe008

Source: bc250-win 5c90f67, Mesa base 05e6c962 with the recorded E34 patches.
Unit A, KMD 0.7.152.1. Exact DLL and router hashes are in manifest.json.

DWM PID5892 loaded UMD5A48852B and hosted ICD3508416F in all retained module
samples. Its DDI log records Present calls, GPU-side render waits with S_OK and
cpu_render_wait=0, rendering fence values4/5/6, and Present signals1/2/3.
These are process-local callback witnesses, not an independent kernel execution
trace or a complete accounting of CPU copies.

A private half-scale screenshot captured during the same bounded interval shows
an intact desktop, taskbar and overlapping application windows with no obvious
corruption on visual inspection. It is not a pixel oracle and does not establish
that every visible surface was freshly composed by this device. The screenshot
and thread dump remain private; their hashes are retained separately. The dump
has not yet been analysed for this result.

The main runner restored baseline UMD8279AC7F and ICD9C40083C and restarted DWM
from5892 to1528. Both the runner and independent watchdog report baselines
verified. No OS reboot or KMD change was performed. The earlier CPU DWM was9648.

G0 remains open: instrument steady full-frame CPU copying, establish independent
GPU execution/completion attribution, and add a controlled visual oracle. Fix
the Fable017 sampler-view bounds and NULL-sampler findings before another probe.
No baseline promotion or performance claim follows from this run.

Privacy: only account and machine identification lines were redacted from the
public run transcript; other retained logs/JSON are copied unchanged. Private
raw originals remain under scratch/g0-hosted/dwm008/result. The harness samples
six times at five-second intervals with a separate sixty-second watchdog.
