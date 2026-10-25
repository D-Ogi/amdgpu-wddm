# The compute node: WDDM node 2 on a MEC compute queue

This note describes WDDM node 2. Node 2 runs user command buffers on one MEC compute queue, beside the gfx ring of
node 0. Phase A of the work is the kernel-mode driver part. The Vulkan and D3D12 user-mode drivers do not use node 2
yet. That is phase B.

Status: implemented in the KMD, host tests pass. The lab proof of the product path passed on unit A on
2026-10-25 (section 9).

## 1. Node layout

| Node | Engine type | Friendly name | Hardware | Gate |
|---|---|---|---|---|
| 0 | `DXGK_ENGINE_TYPE_3D` | BC-250 GFX | gfx ring 0 | always |
| 1 | `DXGK_ENGINE_TYPE_COPY` | BC-250 SDMA0 | SDMA0, the paging node | `EnablePagingNode` |
| 2 | `DXGK_ENGINE_TYPE_OTHER` | BC-250 Compute0 | MEC compute ring 1 | node 1 (lab flavour: also a selector) |

The engine type enum has no compute value, and an adapter has exactly one 3-D engine. A compute-only engine is
therefore `OTHER`, and `OTHER` requires a friendly name (`d3dkmdt.h`, `DXGK_ENGINE_TYPE` remarks).

Node 2 exists only when node 1 exists. Thus its ordinal is always 2, which is the ordinal that the RADV winsys
names (`RADV_DXGI_COMPUTE_NODE`).

Compute ring 0 is the high-priority queue. Node 2 uses compute ring 1, at normal priority.

## 2. The gate

In the release flavour, node 2 is always open. The driver reads no setting for it.

The lab flavour has a selector for control runs: `EnableComputeNode`, a `REG_DWORD` under the service `Parameters`
key. The driver reads it once, at `DriverEntry`, and latches it at `GfxStart`.

- Absent or not 0: node 2 is open. This is the default.
- 0: node 2 is closed. The adapter reports two nodes, as before this change.

The selector is not a catalog setting. Thus a lab arm can replace the KMD image only, and the installed catalog
stays that of the installed package. A catalog change needs a package install. The INF writes no value for it.

To stop applications from using node 2, use the user-mode driver: RADV does not show the compute queue family when
`RADV_BC250_ACE_QUEUES=0` is set.

Node 2 also needs every gate that its submit path rests on. These are `EnableGpuSubmit`, the paging node, the VMID
pool (`EnableVmidPool`) and the ring VM flush (`EnableRingVmFlush`). When one of them is closed, node 2 is closed.
The compute ring has no MMIO root-write path, so it needs the flush on the ring.

## 3. The job frame

A node 2 job is one IB. The frame on the compute ring has three parts:

1. The VM flush of the job's VMID: the page-table root pair and the TLB invalidation, on invalidation engine 4.
2. The IB packet with `INDIRECT_BUFFER_VALID` and the VMID in the control word.
3. A `RELEASE_MEM` fence to slot 11 of the gfx fence page, with the end-of-pipe interrupt.

This is the frame of upstream `gfx_v10_0_ring_emit_vm_flush`, `emit_ib_compute` and `emit_fence`. Engine 1 is the
gfx ring's, engine 0 is the paging stream's and engine 17 is the CPU path's. No two writers share a request and an
acknowledge register.

## 4. VMIDs over two rings

The two rings share one VMID table. A page-table root keeps one VMID on both rings. Both rings take their sequence
numbers from one counter, so a VMID's first and last sequence still name one job each.

Each ring's fence is in order on its own slot. The table keeps the last live sequence of each VMID for each ring.
A VMID is free only when its last job has retired on both rings. The claim of a VMID for a new root needs that
condition, whichever ring makes the claim.

A compute job always gets its VMID from the pool. It never runs at VMID 0. A VMID 0 diagnostic in flight on the gfx
ring holds compute jobs off as well.

A submission that the pool refuses waits on the retirement event. A retirement on either ring signals that event.
Thus `GfxSubmitBusy` counts a compute job, and `GfxComputeBusy` counts a gfx job. Without this, a hold loop could
stop waiting while the other ring holds every VMID.

## 5. Completion

Node 2 has its own completion queue, submit mutex and pending fence, all under the WDDM lock. The rules of node 0
apply:

- The queue is in submit order.
- A completion of fence N retires every fence up to N.
- A software completion that comes while a job is on the ring waits for that job.

The IH DPC reads slot 11 after every interrupt, the same rule as nodes 0 and 1. A poll timer reads slot 11 while
a job is at the head of the queue. Thus a lost interrupt cannot keep a finished job unreported.

Node 2 has no diagnostic watchdog of its own. The OS TDR detects a hang, and `DxgkDdiResetEngine` recovers it
(section 6).

## 6. Hang recovery and faults

`DxgkDdiResetEngine` on node 2 uses the soft recovery of node 0 (hang-recovery.md). The driver kills the waves of
the hung job's VMID and waits for the newest compute sequence to retire. A drained ring is reported as recovered.
A ring that does not drain is refused, and the OS then resets the adapter (0x116 on this part).

The wave kill (`SQ_CMD`) names a VMID, not a ring. Thus it also ends the waves of that VMID on the gfx ring. One
VMID is one page-table root, which is one process, and dxgkrnl removes that process's device after a TDR.

A fault of node 2 closes node 2 only. Node 2 does not witness the start (wddm-start-confirmation.md), so a compute
fault leaves the start health as it is.

A CP privilege or decode fault carries a ring id. The driver decodes it as `gfx_v10_0_handle_priv_fault` does: ME 0
is the gfx ring, and ME 1 and 2 are the MEC queues. A MEC fault closes node 2 when node 2 exists, otherwise node 0.

## 7. DPM

A compute job starts the DPM busy share, as a gfx job does (dpm.md). The share is one interval for both rings. It
ends when neither ring has a job in flight.

## 8. The UMD signal

The caps blob field `submittable_node_mask` gets bit 2 at query time while node 2 is open. The template holds bit
0 only. The COMPUTE `available_rings` field stays as before. The shipped Mesa revisions read it and then drop the
compute queue for GFX1013 by family, so they keep one queue family.

A UMD opens a node 2 context with a context blob whose IP is COMPUTE and whose node is 2. GFX runs on node 0 only,
and COMPUTE runs on node 2 only. A submit blob must name the IP of its node.

## 9. Open items

- Closed 2026-10-25: the lab proof of the product path. The lab KMD opened three nodes. One dispatch on node 2
  gave the same words as node 0. Node 0 and node 2 ran in parallel with fence dependencies each way. Two threads
  with one node each overlapped (24 submits on each node). The control run with the selector at 0 showed two
  nodes and no node 2. The run had no bugcheck, no TDR and no dump.
- Whether `GRBM_STATUS` shows a load on the MEC only. The DPM governor samples it.
- A MEC queue reset (`reset_kcq`). Without it, a compute hang that the wave kill does not drain ends in 0x116.
