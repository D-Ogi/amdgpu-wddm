# vkace: the compute queue family of the BC-250 Vulkan driver

bc250kmd runs COMPUTE work on WDDM node 2, a MEC compute queue (`docs/design/compute-node.md`). When the caps show
node 2, RADV shows a second queue family: compute and transfer, one queue, no sparse binding, no presentation.
vkace checks that family on a real device before an application uses it.

One compute shader (`shaders/stage.comp`) does all the GPU work: `data[i]` gets rounds of a mixing function over
`i ^ seed` (mode 0) or over `data[i] ^ seed` (mode 1). The same function in `vkace.c` is the CPU oracle, and every
check compares words with it. A chain of mode 1 stages depends on the order of its stages, so a broken cross-queue
wait gives wrong words, not only a hang.

| Step | Name | What it checks |
|---|---|---|
| 1 | families | Family 0 has graphics, compute and transfer. One compute-only family has one queue, compute and transfer only, timestamps, and no presentation. |
| 2 | same | The same dispatch on both families gives the oracle words in both buffers. Then a read-modify-write stage on the compute family. |
| 3 | chain | Graphics, then compute, then graphics, then the CPU, on one timeline semaphore, eight times. In half of the runs the compute stage is submitted first, so its wait comes before the signal. |
| 4 | overlap | One long dispatch on each queue, alone and together, with timestamps. It gives the GPU times, the wall time and the overlap of the two dispatches. Sampled words of the run together are checked. |
| 5 | threads | Two threads, one per queue, walk one timeline in turns. Each stage reads the result of the stage before it. Both threads submit all their work before the GPU completes it. |
| 6 | absent | The negative control: no compute-only family, and the graphics family still gives the oracle words. |

With no `--step`, steps 1 to 5 run. On the lab, use `--icd`: an elevated process, which every SSH session is, has
the Vulkan loader ignore `VK_DRIVER_FILES`. The `icd module` lines name the driver that ran, with its SHA-256.

Every line is flushed when it is written. A watchdog ends the process at the deadline. A GPU wait that times out
ends the process without device teardown.

Exit codes: 0 all checks pass, 1 a check failed, 2 usage, 3 Vulkan or system error, 4 deadline, 5 no compute-only
family (only with `--any-device`).

    pwsh -File build.ps1 -Kits <BC250_ROOT>\toolchain\nuget      -> <BC250_ROOT>\scratch\build\vkace\vkace.exe
    vkace.exe [--icd DLL | --manifest JSON] [--step N] [--any-device] [--deadline S] [--fence-ms MS]

The build runs steps 1 to 5 with `--any-device` on the first Vulkan device of the build PC as the positive control
of the client. Exit 3 or 5 skips that self-test.
