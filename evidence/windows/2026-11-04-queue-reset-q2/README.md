# Stage 2 queue reset, lab trials Q1 and Q2 on unit A (2026-11-04)

The trials of [queue-reset.md](../../../docs/design/queue-reset.md) section 8 on unit A. Windows 11 22631. The
tester.37 package is installed, and its KMD is swapped for a lab-flavour build of branch `t37/f2-queue-reset`.
Parameters `LabQueueReset` is 1, `HangRecoveryMode` is 1 (the default), and `TdrDelay` is 10 s.

## Images

| Run | KMD | Source |
|---|---|---|
| Q1, Q2 r1 | `8FEEA181...` lab flavour | `d52e842c`: stage 2 of the design, no fallback |
| Q2 r2 to r4 | `4FB59574...` lab flavour | `44e844bc`: the whole HQD witness and the fallback of section 5.3 (`78745544`) |

## Q1: control, node 0

`bc250hang --lab --long` (a spin dispatch on the DIRECT queue). Stage 1 drains it with one kill of VMID 10
(`SOFT RECOVERED, aborted fence 1779`). Event 4101. Stage 2 is not reached, as predicted. A later
`bc250hang --lab --short` completes (`RESULT=SHORT-COMPLETED-DEVICE-ALIVE`).

## Q2: node 2, vkd3d-proton `test_execute_indirect_multi_dispatch_root_descriptors`

The case of arm A7 (kit `a7-mesh-r1`, client `d3d12.exe` 212A7B80) that hung node 2 on 2026-11-02.

### r1 (`q2-r1-glog-excerpt.txt`, the log ring of the dump)

- Stage 1: 97 kills of VMID 11 in 10.4 ms, the ring does not drain.
- Stage 2: the HQD reads active 1, rptr 0, wptr 0. The KIQ answers `UNMAP_QUEUES RESET_QUEUES` (its ring test
  passes). Then `CP_HQD_ACTIVE` stays 1 for the whole 100 ms bound: failure at step 3 (`-62`).
- `ResetFromTimeout` (S1) refuses with busy mask `0x2A` (`CPC_STATUS 0xA0000001`).
- Result: bugcheck 0x116 (`0xC0000001`, 3), dump `boot-20261104T161520Z`.

### r2, r3, r4 (`q2-r2-r4-kmdlog.txt`)

The same case three times in one boot, with the fallback in the image.

| Run | VMID | Stage-1 kills | HQD after the unmap | Fallback poll | Stage 2 | Event |
|---|---|---|---|---|---|---|
| r2 | 8 | 105 | active, 100 ms | 5 us, inactive | `QUEUE_RESET`, 165531 us | 4101 |
| r3 | 9 | 100 | active, 100 ms | 3 us, inactive | `QUEUE_RESET`, 165284 us | 4101 |
| r4 | 10 | 101 | active, 100 ms | 3 us, inactive | `QUEUE_RESET`, 164272 us | 4101 |

Each run: the wave proof reads 0 waves (complete capture), the VMID is released, and `ResetEngine` of node 2
reports `SOFT RECOVERED` with the node reopened. After each run a new D3D12 device completes a short dispatch.
The test itself reports 16 failures (values 0): the work of the hung dispatch is lost, as for every engine reset.

The HQD witness of r2, before the unmap and after the 100 ms poll (the same in both):

```
ring base 0x546000 (PQ_BASE wants 0x5460)
active 0x1 vmid 0x800 persist 0xBE05301 base 0x0:00005460 ctl 0xD030890A
rptr 0x0 wptr 0x0:00000000 db 0x0
ib 0x2:00601000 rptr 0x56 ctl 0x0 dequeue req 0x0 status 0x0
```

- `PQ_BASE` equals the ring base shifted by 8, so the queue select names the HQD of our compute ring.
- The CP is inside an indirect buffer at `0x2_00601000`, IB read pointer `0x56`.
- After the KIQ unmap, `CP_HQD_DEQUEUE_REQUEST` and `CP_HQD_DEQUEUE_STATUS` read 0, and the HQD stays active.
- `CP_HQD_DEQUEUE_REQUEST` = 1 (DRAIN_PIPE, the handshake of `kgd_hqd_destroy`) makes it inactive in 3 to 5 us.

The persisted hang record after r4: Attempts 13, Recovered 4 (Q1 and r2 to r4), LastVerdict 11, LastPath 1.

## Not covered

- Node 0 (GFX): no Windows client makes a GFX CP-side wait (Q3), and the 3DMark fault of Q4 was fixed with the
  residency hold of tester.36. The GFX half stays behind `LabQueueReset`.
- The desktop: since the 0x116 of r1 the boot-loop guard kept DWM on the CPU route, so r2 to r4 do not show the
  GPU desktop through a stage-2 reset.
- Why the KIQ unmap leaves the HQD active on this part. amdgpu's `gfx_v10_0_reset_kcq` has no direct dequeue,
  and M944 measured it on a wave spin, not on a CP wait inside an indirect buffer.
