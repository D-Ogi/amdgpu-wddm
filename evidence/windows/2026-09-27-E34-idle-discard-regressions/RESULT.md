# M584: post-DISCARD-fix regressions

Exact UMD67E4C9F5 and hosted ICD3508416F from M582 were tested:

-101 passes8 textured images (t0/s0 and t1/s0) and the preceding8 graphics cases.
-102 passes1000 cross-process exchanges across10 generations,2702400 checked
 pixels per direction with distinct producer/consumer processes.
-103 was incorrectly launched through SSH/session0. Swapchain creation returns
 887a0022, exit5. It does not reach rendering and is not a regression pass.
-104 repeats the existing interactive-task method from093:120 native hosted
 flip Presents pass, final green readback0/76800 mismatches, device removal0.

Every runner restored baseline ICD93B1D1FD and UMD8279AC7F with hash checks.
CPU DWM4400 remained unchanged.104 was observed terminal and its task removed.
Full diagnostics and KMD logs remain private and hash-bound; no counter-based
claim of global GPU completion or performance is made here.

This validates bounded regressions, not desktop image correctness. DWM015
is prepared but has not run. BD-043 and G0 remain open. Unit A,2026-09-27;
source parent628d890 (M582). No candidate promotion.
