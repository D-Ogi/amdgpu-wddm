# M590: state re-emission with batching retained

Diagnostic UMDFC7EB75A/ICD3508416F on unit A restores the production Draw.cpp
(no added per-draw flush/wait) and forces zink_draw_vbo's BATCH_CHANGED=true
specialization. It re-emits graphics pipeline/dynamic/vertex/index/descriptor
state and descriptor references, without ending the batch. This is diagnostic.

Control110 passes56 state images/229376 pixels/3584 draws plus8 graphics cases.
DWM020 runs185.825s,3774 loss-free matched DMA pairs attributed to DWM5112,
matching submission/completion IDs, no preemption or unmatched/pending/duplicate
pairs. Static8000-pixel composition ROIs and all sampled cyan-shape checks pass.
The count is comparable to ordinary batching (M585), far below M589's41943.
No owner visual verdict was received; sparse samples do not exclude transient
or residual shrink artifacts. G0/BD-043 remain open, no performance acceptance.

Together with019 this motivates isolating individual state/reference updates.
A source hypothesis is stale index-buffer binding after DISCARD replaces its
backing object: ordinary draw caching compares resource identity, while flush
and BATCH_CHANGED force a new index bind. This is not yet a measured cause.
Next: WARP/CPU/GPU control with repeated index DISCARD within a batch, fully
initialized storage and differing index offsets, then a narrow fix if reproduced.

Baseline ICD93B1D1FD and CPU UMD8279AC7F verified restored; CPU DWM9192,
all020 tasks removed. Raw ETW/images/logs private; hashes retained. No promotion.

PROVENANCE: Mesa (https://gitlab.freedesktop.org/mesa/mesa), MIT.
