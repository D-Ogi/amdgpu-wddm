# BD-215 first-exception collector

This tool records the first access violation of one x86 process. It is a lab diagnostic for
BD-215, the x86 access violation of the BD-201 rows. It is not part of the driver package.

The collector is a Win32 user-mode debugger. It does not use a driver, Mesa, an MSR, a kernel
debugger or a context write.

## Files

| File | Contents |
|---|---|
| `collector.cpp` | The x64 collector, `bd215collector.exe` |
| `fault-x86.c` | The x86 test process, `bd215fault32.exe`. It makes intentional faults that its own handler stops. |
| `build.ps1` | The build of both programs with clang-cl 23.1.4 and lld-link |
| `selftest.py` | The host test of the collector |
| `client-record.patch` | The patch for the BD-201 scratch client. The client is not in this repository. |
| `identity-fixture.c`, `identity-test.c` | The host test program for the identity record of the patched client |
| `client-host-test.py` | The host test of the identity record |
| `Invoke-Bd215Row.ps1` | The lab wrapper for one hosted x86 row of the BD-201 kit |

## Build

```powershell
pwsh -NoProfile -File build.ps1 -Kits <BC250_ROOT>\toolchain\nuget [-Llvm <LLVM directory>] [-ClientDir <patched client directory>]
```

The output goes to `<BC250_ROOT>\scratch\build\bd215-collector` (`-Out` changes it). The script
stops when clang-cl 23.1.4 is not at the given path. It does not use cl.exe.

The identity test needs `bd201scratch_modules.h` of the patched client. Give its directory with
`-ClientDir`. Without `-ClientDir`, the script does not build the identity test.

## Host tests

```powershell
python -B selftest.py [<build directory>]
python -B client-host-test.py [<build directory>]
```

Both scripts read the programs from the build directory and write their results there.
`client-host-test.py` needs the Python package `pefile`.

`selftest.py` runs eight cases:

- A far JMP to selector 0. This is an architectural #GP, and the access violation has the address -1.
- A null DS, then a valid indirect `ff 24 bd` jump. The collector must record the jump table.
- An ordinary read access violation that the `--module` argument selects.
- An access violation in a module that `--module` does not name. The collector must not record it.
- A process that ends with no fault.
- A started process that is still alive at the bound. The collector must stop it.
- An attached process that is alive at the bound. The collector must detach and leave it alive.
- An attached process that faults. The collector must record the fault and detach.

In each recorded case, the test compares the context blobs with the custom dump streams byte for
byte. It also checks the selector queries, the module SHA-256 and the memory regions. The test
process gets a minimal environment, so its full dump does not contain other credentials of the
host.

`client-host-test.py` loads a fixture DLL in an x86 and an x64 process. It checks the base, size
and SHA-256 that the identity record gives for that DLL.

## The BD-201 client patch

`client-record.patch` changes the BD-201 scratch client (`tools/win/bd201scratch` in its own tree):

- The client writes one `bd201scratch: identity {JSON}` line before it creates the device.
- The client writes one more identity line after its first submission, also when the run is
  correct.
- `run.ps1` gets the `-Collector` parameter. With it, the row starts the client through the
  collector.

An empty module list before the device is correct when no driver is loaded yet. The identity line
gives the CPU at the time of the line. That CPU is not the CPU of a fault.

## Lab use

Apply the patch to the client, then build and stage the kit. Copy these items to one directory
on the lab unit:

- `bd215collector.exe` from the build directory, as `bin\bd215collector.exe`
- `Invoke-Bd215Row.ps1`
- `lab-tools\` with `cpu-boot-record.ps1`, `app-crashes.ps1` and `state-census.ps1` of the lab kit.
  These three scripts are not in this repository.

Run the wrapper in an elevated Windows PowerShell 5.1:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File <dir>\Invoke-Bd215Row.ps1 -Lab -Kit <kit dir> -Out <new empty dir> -ExpectSha256 <SHA-256 of the hosted x86 RADV DLL> -Arm wait -KmdCli '<path of bc250kmd_cli.exe>'
```

The wrapper refuses a kit with an old `run.ps1`. It refuses an output directory that is not
empty. It does not change the system.

The wrapper records the CPU boot state, the state census and the CPU and DPM policy before the
row. It runs the row with zero client retries in session 0. After the row, it records the
application crashes and the CPU boot state again. The module hashes add CPU and disk load, so a
collector run is not a performance measurement.

The collector can also run alone:

```powershell
bd215collector.exe --out <new dir> --seconds 60 --module amdgpu_wddm -- <x86 program> <arguments>
bd215collector.exe --out <new dir> --seconds 60 --module amdgpu_wddm --pid <x86 process ID>
```

The `--module` value is a part of a module file name. Case is not important.

## What the collector records

The collector records the first first-chance access violation that agrees with one of these
conditions:

- The address argument of the exception is all ones.
- The faulting module name contains the `--module` value.

The collector records one exception only. Then it continues the exception with
`DBG_EXCEPTION_NOT_HANDLED`, so the handler of the process and WER run as usual. A debugger changes
the timing, so a run with the collector is not identical to a run without it.

| File | Contents |
|---|---|
| `first-exception.json` | The raw debug event, the native and WoW64 contexts and six selector queries. The bytes at the instruction, the jump table and the target. The memory regions, the module SHA-256 and the mitigation policies. |
| `first-exception.dmp` | A dump with full memory, memory information and thread information (`0x1802`), and the custom streams `0xB2150001`, `0xB2150002` and `0xB2150003` |
| `capture-result.json` | The dump result, error codes and custom stream IDs |
| `run-result.json` | The collector result and, separately, the exit code of the process |
| `watchdog-timeout.json` | Written when the hard bound expires. Do not use a partial dump. |

The collector writes the JSON record before the dump, so a dump failure does not delete the record.
It creates each output file with `CREATE_NEW` and never overwrites a file.

| Exit code | Meaning |
|---|---|
| 10 | No matching exception before the end of the process or the bound. See `timed_out`. |
| 11 | One complete record and one dump |
| 20 | A collector error, a capture error or the end of the hard bound |

The `--seconds` bound is 1 to 180. A separate watchdog gives one more second for cleanup. At the
bound, the collector stops a process that it started. It detaches from a process that `--pid`
selected, and that process continues. Kill-on-exit is off, so an end of the collector does not stop
an attached process. With `-Collector`, the BD-201 row gives the exit code of the client, not of the
collector. A capture error or a timeout makes the row fail.

## Limits

- A failed read or query stays in the JSON with its API name and error code.
- The jump table decode applies only to an instruction that starts with `ff 24 bd`. The slot
  address is EDI times 4, plus the immediate, plus the queried DS descriptor base. The hidden
  segment cache of the processor is not visible.
- The module SHA-256 is the hash of the file at the path of the event, not of the relocated image
  in memory.
- User mode cannot find the CPU of the original fault. The record gives the ideal processor and
  the group affinity, each with its own label.
- These items are not available: the hardware vector and error code, CR2, CR3, the page tables,
  the TLB and the hidden segment caches.
- The native context is the context of the stopped thread. It is not necessarily the RIP of the
  kernel trap.

Contracts:
[ContinueDebugEvent](https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-continuedebugevent),
[DebugSetProcessKillOnExit](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-debugsetprocesskillonexit),
[Wow64GetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/wow64apiset/nf-wow64apiset-wow64getthreadcontext),
[MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump).
