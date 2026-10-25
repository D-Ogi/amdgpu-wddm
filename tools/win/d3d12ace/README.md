# d3d12ace: D3D12 compute and copy queues on the compute node

bc250kmd runs COMPUTE work on WDDM node 2, a MEC compute queue (`docs/design/compute-node.md`). On the D3D12 route
the hosted Vulkan driver then shows a compute queue family, the D3D12 engine puts each D3D12 COMPUTE and COPY queue
on it, and the shell opens the context of that queue on node 2 (`driver/umd/d3d12/README.md`, "Queues and fences").
When the kernel driver's compute queue setting keeps node 2 closed, the caps show no compute node, the family is
hidden and all queues run on node 0. In a lab-flavour ICD, `RADV_BC250_ACE_QUEUES=0` does the same. d3d12ace
checks both cases through the D3D12 runtime of System32, before an application uses the queues.

One compute shader (`stage.hlsl`) does all the GPU work. It has the function of `tools/win/vkace`: `data[i]` gets
rounds of a mixing function over `i ^ seed` (mode 0) or over `data[i] ^ seed` (mode 1). The same function in
`d3d12ace.cpp` is the CPU oracle, and every check compares words with it. A chain of mode 1 stages depends on the
order of its stages, so a broken cross-queue wait gives wrong words, not only a hang.

| Step | Name | What it checks |
|---|---|---|
| 1 | queues | A DIRECT, a COMPUTE and a COPY queue, their timestamp frequencies, and the nodes of the adapter by name. With `--expect-node 2`, the adapter has node 2. |
| 2 | same | The same dispatch on the DIRECT and the COMPUTE queue gives the oracle words on both. Then direct, compute (read-modify-write), copy and direct (readback), each behind a fence value of the stage before it. |
| 3 | chain | Three stages on one fence, then the CPU. The orders are direct-compute-direct (8 runs), compute-direct-compute (8 runs) and compute-copy-direct (4 runs). In half of the runs the middle stage is submitted first, so its Wait comes before the Signal that it waits for. |
| 4 | overlap | One long dispatch on each queue, alone and together: GPU times, wall time, and the overlap when both queues give a clock calibration. Sampled words of the run together are checked. |
| 5 | threads | `--threads` threads (default 4) walk one fence in turns. Each thread creates its own queue: thread 0 a DIRECT queue, the others a COMPUTE queue. Each stage reads the result of the stage before it, and each thread also runs a private chain. All threads record at the same time and then submit all their work. |
| 6 | node | The packets of this process per node (`D3DKMTQueryStatistics`), in three windows. The windows are 16 dispatches on the COMPUTE queue only, 16 copies on the COPY queue only and 16 dispatches on the DIRECT queue only. |

`--expect-node` sets the node checks of steps 1 and 6 and of the end of the run:

- `2` (the default on a BC-250): the compute and the copy work ran on node 2, the direct work on node 0 and not on
  node 2.
- `0` (node 2 closed, or `RADV_BC250_ACE_QUEUES=0` in a lab-flavour ICD): the compute and the copy work ran on
  node 0, none on node 2, and the whole run put no packet on node 2.
- `any` (the default on another adapter): the packets are reported and not checked.

The `module` lines name the D3D12 runtime and the driver modules in the process, with their SHA-256. Every line is
flushed when it is written. A watchdog ends the process at the deadline. A GPU wait that times out ends the process
without device teardown. The client opens no window.

Exit codes: 0 all checks pass, 1 a check failed, 2 usage, 3 system or D3D12 error before the device exists,
4 deadline, 5 no hardware adapter (only with `--any-device`).

    pwsh -File build.ps1 -Kits <BC250_ROOT>\toolchain\nuget      -> <BC250_ROOT>\scratch\build\d3d12ace\d3d12ace.exe
    d3d12ace.exe [--step N] [--expect-node 2|0|any] [--threads T] [--any-device] [--deadline S] [--fence-ms MS]

The build runs steps 1 to 6 with `--any-device --expect-node any` on the first hardware adapter of the build PC as
the positive control of the client. Exit 5 skips that self-test.
