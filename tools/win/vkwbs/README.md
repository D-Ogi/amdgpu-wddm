# vkwbs

A test of the timeline semaphore wait-before-signal contract of a Vulkan ICD (BD-200). One C file over core
Vulkan 1.2 and 1.3, no window. It runs from an SSH session 0 shell.

The test submits a batch that waits for value 1 of a timeline semaphore and signals value 2. Then the host
signals value 1 with `vkSignalSemaphore` and waits for value 2. An ICD that obeys the contract returns from the
submit at once and completes the batch after the host signal. If the submit blocks the application thread on the wait, the
host signal never comes, and the test stops on its watchdog.

The release of the wait must come from the host. A second submit on the same queue that signals value 1 is
not a valid release: queue submission order gives no forward-progress guarantee for it, and a conforming
driver also times out on it. Earlier versions of this test used that release, so their results do not prove
the defect or the fix.

## Build

```
pwsh tools\win\vkwbs\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
pwsh tools\win\vkwbs\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Arch x64
```

Output: `<BC250_ROOT>\scratch\build\vkwbs\vk_wbs_hang_64.exe` and `vk_wbs_hang_32.exe`. The compiler is
clang-cl from `<BC250_ROOT>\toolchain\llvm-23.1.4-win64` (`-ClangToolchain` sets another one). The build has
no PDB and uses `/Brepro`, so two builds of one source give the same bytes. The script prints the SHA-256 of
each program.

## Run

```
vk_wbs_hang_64.exe [--icd <absolute path of the ICD DLL>] [results file]
```

- With `--icd`, the program loads the DLL directly through `vk_icdGetInstanceProcAddr`, without the Vulkan
  loader. Use this for the hosted ICD, which no loader manifest names, and for a candidate DLL.
- Without `--icd`, the program uses `vulkan-1.dll` and the ICD that the loader selects.
- The results file gets one line for each case and the last `PROBE` line.

Every Vulkan call has a `step:` line before it, and the output is not buffered. Thus after a crash or a hang,
the last `step:` line names the call. The program examines each function pointer before it uses it. A NULL
pointer gives `PROBE FAIL <name> NULL` and exit 2. The line `icd module: <first 8 hex of the SHA-256> <path>`
names the ICD file that ran. A kit compares this line with the file it expects.

| Case | What it does |
|---|---|
| 1 | A GPU signal of value 1, then a CPU wait for value 1. |
| 2 | `vkQueueSubmit` with wait(1) and signal(2), host signal(1), CPU wait for value 2. |
| 3 | The same as case 2 through `vkQueueSubmit2`. SKIP when the device has no `vkQueueSubmit2`. |

| Exit | Last line | Meaning |
|---|---|---|
| 0 | `PROBE PASS 3/3` | All cases pass. |
| 1 | `PROBE FAIL <n> failure(s)` | A wait returned an error or a wrong counter value. |
| 2 | `PROBE FAIL ...` | Test error or missing function. This is not a driver result. |
| 3 | `PROBE HANG ...` | A case wait timed out (10 s) or the watchdog of a case fired (20 s). |

A pass takes approximately 2 s. The longest run is approximately 25 s, when one case stops on its watchdog.
