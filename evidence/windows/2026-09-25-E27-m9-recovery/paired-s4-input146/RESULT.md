# M468 - Paired cursor controls identify CPU rendering stalls after S4

See PAIRED-ETW-ANALYSIS.md for exact QPC-aligned controls. Same146 SYS and
original DWM704 survived real S4 (OS EffectiveState5). Logical cursor moves
and short cursor-only renders remain refresh-paced. Later full Present calls
reach757ms: first DxgKrnl flip occurs756.687ms into one measured757.086ms
call, followed by a66us queue packet. Twelve llvmpipe workers execute shaders
while the main thread waits. The known sampled DLL address resolves to
shade_quads at the JIT shader call. This measured long call is not a long KMD
flip or fence wait. Rendering remains on the CPU; no hardware D3D claim.

DWM-only restart704->6076 retains OS boot03:57:55.500 and installed146.
Long render/Present work persists, and owner reports only slight improvement.
Unmarked screen-content differences remain a limitation; this experiment does
not prove that every extra shader cost is caused by S4 itself. Raw traces,
process metadata and the screen capture remain private under scratch/m9.

A bounded post-S4 CPU control reads64MiB in5.590-5.800ms with full sum checks.
During a five-second12-thread integer workload, Windows reports107-109%
processor performance and100%maximum frequency during loaded samples. This
contradicts a CPU stuck permanently at the observed idle43% state; it does not
measure graphics-allocation cache attributes. Temperature66.5C before,76C after.
The helper completes normally; its shell Process.ExitCode field is blank, so
use its complete output and successful data checks, not a claimed numeric exit.

Next hypothesis: CPU-read shared graphics backing defaults to write-combined
when Cached is not requested. Check the local WDDM contract, primary exclusion
and a direct Lock2 allocation measurement before attributing the slowdown.
No cache-policy fix has yet been deployed. M9 remains incomplete.
