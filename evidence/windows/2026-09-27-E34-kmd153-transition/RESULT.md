# M603: diagnostic KMD153 transition and GPU regression

The M602 binary0C0DA4E1 (version0.7.153.1, sourcec3499f1) loads during an
in-session PnP transition from152. Windows boot and CPU DWM4596 are retained.
Independent post-check verifies the exact SYS hash, device problem0, full WDDM,
healthflags15, guard0 and native1000MHz/820mV. Temperature is66.1C at final check.
This is a diagnostic deployment, not promotion or complete G0 acceptance.

The task from28a0617 ends1: its immediate log read after enable sees no adapter
interface. PowerShell5.1 surfaces stderr from the child as NativeCommandError,
so the wrapper stops collecting before readiness. Independent queries then prove
153 active; no installation was repeated. The raw failure remains in the private
receipt. The initial collector has11 samples, no reader timeout; the later log
retains the256-line head and a wrapped tail, with missing intervening sequences.
No GDI surface record is observed, but this does not prove no GDI requests occurred.

Hosted GPU control122 uses clean UMD5C74BF98, hosted ICD3508416F and control7492F85A,
the same artifacts as M599/118. Both submission patterns pass:4096 draws,
64 images,262144 state pixels, exact hash matches with WARP111; eight preceding
graphics cases check32768 pixels. Node0 changes0/0 to2147/2147 submitted/completed;
paging275/275 to783/783, both with zero reported timeouts/refusals. Native DMA
counters change from2 transfers/8 fills/92160000 bytes to20/27/130850816.
Global live allocation count is33 before and after. The final log reports no TDR.
These counters support this bounded regression, not arbitrary paging/lifecycle.

The runner verifies CPU UMD8279AC7F and registered ICD93B1D1FD restoration,
DWM4596 unchanged. Final independent check at04:51:27Z verifies those hashes,
no active test processes and the transition task removed. No GPU desktop trial
was run on153 and there is no owner visual verdict for this transition.

Follow-up script fixes preserve an installer process handle/code independently
of stderr, wait for the adapter interface and allow the bounded collector to
continue despite worker failure. The first readiness helper itself failed to
observe a code with Start-Process on PowerShell5.1; pinning the process handle
before WaitForExit passes on the same active153 at its first attempt. This helper
is read-only. The revised installation/rollback flow has not been rerun.

Raw receipts, diagnostic logs and hardware-instance identifiers remain private;
only selected counters, image checks and hashes are published. Remaining work:
CDD/DWM allocation contract and GPU engine Blt, exact-artifact desktop/lifecycle
validation, full M13/G0 acceptance. The preserved152 rollback package remains.
