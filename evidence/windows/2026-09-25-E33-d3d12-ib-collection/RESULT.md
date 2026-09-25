# D3D12 Asteroids and complete IB collection

PROVENANCE: Mesa MIT; Diligent Apache-2.0; DXVK zlib; vkd3d-proton LGPL-2.1
standalone runtime, no vkd3d code copied into the driver. Correction to earlier
E33-system-dxgi and E33-present-wait prose: DXVK is zlib, not MIT; the original
source package retains its upstream LICENSE. Immutable old evidence is unchanged.

Unit A KMD151, vkd3d-proton472989aa, DXVK3.1.1. Run001 with MesaA6D11B64
fails at the16-fragment collection limit: used15 plus3, cs_count5, native submit
STATUS_INVALID_PARAMETER before that work can reach the GPU. The application
subsequently exits0xc0000005 after20448ms. This is not a successful D3D12 run.

Candidate8B5EC055 counts all initial/continue preambles, command streams and
postambles in order, then collects them without truncation. Up to16 fragments
still use stack storage; larger lists use queue-owned memory with geometric
growth, reused on later submits and freed with the queue. Count/size overflow
checks and the hardware G_3F3_IB_SIZE limit remain. Several command streams still
become one packed IB; this is not an implementation of arbitrarily large chains.
The complete78-file Mesa port patch replays exactly on05e6c962.

Run002:18fragments,2011168bytes are packed successfully. Asteroids D3D12
completes660frames in30012ms, exit0,1080x720 capture. All six required modules
are witnessed (Diligent D3D12, vkd3d d3d12/core, DXVK dxgi, system Vulkan loader,
expected ICD). Both CSVs have660ordered rows, positive finite values and correct
tick/frequency conversion. Preview visibly contains the asteroid field, skybox
and Diligent D3D12 title. Full capture SHA25637D1251D... is in the validation
record; full PPM stays in scratch/m12/asteroids-d3d12-151-002-collected.

This capture run is not performance acceptance. GPU query intervals do not mean
active ALU time. Image and performance comparison against matched Linux/Proton
remains pending, as do full CTS and the other M12 requirements. No reset or reboot;
baseline system ICD9C40083C restored by each wrapper. Candidate not promoted.
