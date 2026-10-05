# M591: index DISCARD stale binding reproduced and fixed

Unit A, unchanged EXE7492F85A1649AF89B12C5866FE1C5202292E053716A9FF61E543651468107E1F.
WARP111 and CPU112 pass64 state images/262144 pixels/4096 draws plus8 graphics
cases. Ordinary GPU113 UMD67E4C9F5 passes earlier cases then fails the new
indexed-list16-discard pass0:4032/4096 mismatches. Baselines restored each time.

Candidate114 UMD49AFB21F49962AD5D6844CE627D34A7312BC4EF02BE54F80D2E39ABAEF733EB7
with hosted ICD3508416F passes all64 images with hashes identical to WARP/CPU.
The narrow change clears ctx->index_buffer after invalidate_buffer changes the
backing object of that bound resource. Exact source pre/post hashes and patch
are in the experiment directory. Broad BATCH_CHANGED forcing and added per-draw
flush/wait are absent; both source files match the preserved production versions.

This establishes the regression and correction for the tested index-DISCARD
sequence. It does not establish complete desktop correctness or resolve the
residual exposed-area artifact. Sharing/Present and DWM validation are next.

GPU runs113/114 restore baseline ICD93B1D1FD and CPU UMD8279AC7F; CPU DWM9192
unchanged. No promotion. Raw logs remain private, verified hashes retained.
