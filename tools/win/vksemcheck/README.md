# vksemcheck

A two-process check of `VK_KHR_external_semaphore_win32` on a timeline semaphore. One C file over core
Vulkan 1.2, no shaders of its own, no window. It runs from an SSH session 0 shell and from the interactive
session.

Release 0.7.216.100-tester.27 changes how the 64-bit system ICD shares a semaphore through a Windows handle
(mesa `amdgpu-wddm/b29-system-win32-semaphore` 2fbc4913, BD-105):

- An import keeps its own duplicate of the NT handle. The specification says that an import of a Windows
  handle does not transfer the ownership of the handle. The earlier build kept the application's handle and
  closed it when the semaphore was destroyed.
- An export applies the access, the inheritance and the name of `VkExportSemaphoreWin32HandleInfoKHR`.
- A named import maps the `Global\` and `Local\` namespaces to their `BaseNamedObjects` directories.
- The default export access is `GENERIC_ALL`. Only a real sync object of the kernel driver can say whether
  `D3DKMTShareObjects` accepts that right.

The host gate of that branch drove these paths against a KMT mock. This client drives them on the device.

## Build

```
pwsh tools\win\vksemcheck\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
pwsh tools\win\vksemcheck\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Arch x64
```

Output: `<BC250_ROOT>\scratch\build\vksemcheck\vksemcheck-x64.exe` and `vksemcheck-x86.exe`. The build
compiles with `/W4 /WX`, keeps the previous executables under `retained\` by their hash, checks the image
architecture, and runs `--help` and a usage check. Then it runs the whole check with `--any-device` on the
first Vulkan device of the build PC. That run is the positive control of the client: a driver of another
vendor must pass every check. Exit 3 (no device with timeline semaphores and the extension) skips the run.
At the end the build prints the SHA-256 of each artifact. A lab runner pins these values.

## What it does

The parent process exports. A child process (the same executable with `--child`) imports. The two then walk
one timeline in both directions. The parent writes the odd values and the child writes the even values.
Each side waits for the value of the other side before it writes its own value, so every value that arrives
has crossed the process boundary:

| value | written by | how |
|---|---|---|
| 1 | parent | a queue submission |
| 2 | child | a queue submission |
| 3 | parent | a host signal |
| 4 | child | a host signal |
| 5, 6 | parent, child | a parent host signal releases a child queue wait, and that submission signals 6 |
| 7, 8 | child, parent | a child host signal releases a parent queue wait, and that submission signals 8 |

Each queue submission holds one `vkCmdFillBuffer` with a marker, so a signal comes from a real GPU
submission and not from an empty batch. The walk stops at the first wait that times out (`--wait-ms`,
default 10000), so a broken pair costs one wait and not eight.

Two cells:

| cell | export | import | extra checks |
|---|---|---|---|
| unnamed | no export attributes, so the default access of the driver | an inheritable duplicate of the handle | a probe semaphore imports the handle and is destroyed. The handle must then still be open. The semaphore of the walk imports it, and the child closes the handle before the walk |
| named | access `0x00120003`, inheritable, a `Local\` name | the name | an import of a name that nobody exported must fail |

The requested access `0x00120003` is `READ_CONTROL | SYNCHRONIZE | D3DDDI_SYNC_OBJECT_WAIT |
D3DDDI_SYNC_OBJECT_MODIFY_STATE`. It is not the earlier default (`0x001F0003`) and it is not `GENERIC_ALL`,
so the granted access of the exported handle shows whether the driver applied the request. The client reads
the granted access, the object type and the inherit flag of every exported handle from the kernel
(`NtQueryObject`, `GetHandleInformation`) and prints them.

`Local\` maps to `\BaseNamedObjects` in session 0 and to `\Sessions\<n>\BaseNamedObjects` in an
interactive session. For that reason the lab arm runs the 64-bit program in both sessions.

## The 32-bit control

The 32-bit ICD of tester.27 is the earlier build, so `vksemcheck-x86.exe` on the lab is a negative control.
The checks that this change is about must fail there. The named cell can stop the process at its first
semaphore, because the earlier build calls a callback that its sync type does not have. The lab arm runs
each cell of the control as its own process, so that a crash of one cell does not hide the other.

## Output and exit codes

Every line is `PASS`, `FAIL` or `INFO`, with the milliseconds since the parent started. The lines of the
child have the prefix `[child] `. The parent prints the log of the child after the child ends. The last line
is `vksemcheck PASS|FAIL <bits>-bit: <n> checks, <n> failed`. That count is for the parent only: count the
`FAIL` lines for the total.

| exit | meaning |
|---|---|
| 0 | every check of both processes passed |
| 1 | a check failed |
| 2 | usage |
| 3 | no usable device, or a Vulkan or system error before the first cell |
