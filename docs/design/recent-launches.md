# Recent launches (format version 1)

Status: implemented in the D3D12 shell and the D3D11 (DXVK) shell on branch `gui/p1-recent-launch`; host gate
G-RG passes on the development PC. Not yet run on unit A (lab trials L2 and L2-game of the GUI plan).

The control application shows the applications that the user recently started on this driver. This document is
the contract between the writers (the two shells) and the reader (the control application). The record says that
a program **was launched** and created a Direct3D device. It does not say that the program is a game, that someone
played it, or for how long. User-visible text says "launched", not "played".

## Who writes, and when

| Writer | File | Point |
|---|---|---|
| D3D12 shell `amdgpu_wddm_d3d12.dll` | `driver/umd/d3d12/adapter.cpp` | adapter `CreateDevice`, after the engine device exists and the call returns `S_OK` |
| D3D11 GPU shell `amdgpu_wddm_d3d11.dll` | `driver/umd/dxvk/ddi-adapter.cpp` | adapter `CreateDevice`, after `create_render_device` succeeded |

Both call `note_outer_device` of `driver/umd/recent-launch/recent-launch.h`. The **outer** device is the one the
Direct3D runtime asked the shell for. The shells are the only layer that sees it: the engines below them (the
vkd3d-proton and DXVK forks) and the hosted Vulkan ICD below those create their own Vulkan instances and devices
for every outer device, and a failure there still fails the outer call (for example
`driver/umd/dxvk/engine-session.cpp` creates the Vulkan device and can then refuse the engine device). A note at
the shell's success return therefore counts each outer device once and never counts a failed one.

Not written (by design in version 1):

- the Vulkan ICDs, system or hosted. Bit 4 of `Apis` is reserved for a later ICD writer, which would have to skip
  hosted and internal instances;
- the CPU D3D11 UMD and the hosted zink UMD. D3D11 applications that the `AppRouter` policy does not send to the
  GPU shell (the default) are therefore not in the list;
- processes whose executable is below the system Windows directory (`GetSystemWindowsDirectoryW`, compared as a
  directory: `C:\Windows\x.exe` is below it, `C:\Windows2\x.exe`, `C:\Windows.old\x.exe` and
  `C:\WindowsApps\x.exe` are not). This keeps DWM, Explorer, Task Manager and other Windows parts out;
- this project's own programs, matched on the file name without case: `amdgpu_wddm_*.exe`, `bc250*.exe`,
  `vulkaninfo*.exe` (capability dumps, test clients, the KMD CLI, the bug report's vulkaninfo);
- AppContainer processes, and processes that run as LocalSystem, LocalService or NetworkService. Session 0 is
  recorded (lab trials start their clients there);
- an executable path that is not valid UTF-16 (a lone surrogate): it has no UTF-8 form.

Cost on the creating thread: one atomic exchange and one `CreateThread` (0.06 ms median, 0.16 ms worst of 100 on the
development PC). The rest runs on one worker thread at below-normal priority, at most once per shell per process,
never in `DllMain`, never on a submit or Present path, and with no network, icon or metadata access. The worker
holds a reference on the shell DLL until it ends, so the runtime may unload the shell meanwhile. Every failure only
loses the entry; the device never waits for the worker or sees its result. The worker ends with one debugger line
`amdgpu-wddm recent-launch api=<n> outcome=<name> us=<duration>`.

## Switch

`HKEY_CURRENT_USER\Software\amdgpu-wddm\Control`, value `RecordRecentLaunches`. The control application owns it.

| State | Meaning |
|---|---|
| key or value absent | on (owner decision D3: the one exception to "unchecked = nothing written") |
| `REG_DWORD` 1 | on |
| `REG_DWORD` 0 | off: nothing is written |
| any other type, size or number, or a read error other than "not found" | no record. This is never read as on. |

The writer reads the switch once before it takes the lock and again with the lock held.

## Store

Per user and local, in the user's own profile:

| File | Purpose |
|---|---|
| `%LOCALAPPDATA%\amdgpu-wddm\recent-launches.txt` | the list |
| `%LOCALAPPDATA%\amdgpu-wddm\recent-launches.lock` | the lock (empty file, byte 0 is locked). **Never delete it.** |
| `%LOCALAPPDATA%\amdgpu-wddm\recent-launches.tmp` | the writer's next list before the rename; ignore it |

A writer creates `%LOCALAPPDATA%\amdgpu-wddm` when it is missing (the D3D12 engine's shader cache uses the same
directory). Nothing is uploaded automatically. A user can still include the list in a support report that they
preview and export themselves, so the data page must not say "nothing leaves the PC".

### Format

UTF-8 without a byte order mark, lines end with LF only:

```
amdgpu-wddm recent-launches 1
<start>\t<pid>\t<starts>\t<apis>\t<path>
...
end <count>
```

| Field | Content |
|---|---|
| `start` | creation time of the process of the last launch: a Windows FILETIME (UTC, 100 ns since 1601-01-01), exactly 16 upper-case hexadecimal digits |
| `pid` | process id of that process, decimal. `(start, pid)` identifies the process |
| `starts` | launches counted, decimal, saturating at 4294967295 |
| `apis` | Direct3D APIs of the last launch, decimal bit mask: 1 = D3D12, 2 = D3D11 (4 reserved for Vulkan) |
| `path` | normalized full path of the executable (below), the rest of the line |
| `count` | number of entry lines, decimal |

Entries are in launch order, the most recently launched first, at most 64. The order is the order of the notes,
not of the clock. A writer drops the last entry when a new path would make 65.

A list is valid only as a whole: the header line exactly as above, then entry lines with exactly five fields
(digits only in the number fields, a non-empty path without control characters, valid UTF-8), then the `end`
line, whose count matches, as the last bytes of the file. Anything else (a truncated, torn or edited file, more
than 64 entries, more than 8 MiB) is not a list. The reader then shows no entries and may say that the list cannot
be read; it never shows a part.

A header with another version number (`amdgpu-wddm recent-launches 2`) belongs to a newer writer. A version 1
writer leaves such a file unchanged; a version 1 reader shows no entries. A torn version 1 file is replaced by the
next writer.

### Path identity

The key of an entry is the executable's full path as the file system resolves it: `GetFinalPathNameByHandleW`
(`FILE_NAME_NORMALIZED`, `VOLUME_NAME_DOS`) of the module file, so symbolic links, junctions, 8.3 short names and
letter case resolve to one spelling; the `\\?\` prefix is removed (`\\?\UNC\srv\share` becomes `\\srv\share`).
When the file cannot be opened, the path from `GetFullPathNameW` is used. Paths can be longer than 260 characters.

Two entries are the same path when they compare equal ordinally ignoring case (`CompareStringOrdinal` with
`bIgnoreCase`; in .NET `StringComparison.OrdinalIgnoreCase`). `A\game.exe` and `B\game.exe` are two entries. The
reader derives the file name itself. Per-game settings are still keyed by file name
(`HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<file name>`), so two tiles with the same file name share one
profile; the application must say so and must not suggest separate settings.

## Writer protocol (one launch note)

1. Skip unless the path is outside the Windows directory, is not one of this project's own programs and has a
   UTF-8 form.
2. Read the switch. Off or unreadable: stop.
3. Create the store directory if it is missing.
4. Lock: open the lock file (`OPEN_ALWAYS`, share read, write and delete) and take an exclusive `LockFileEx` lock
   on byte 0, waiting at most 1000 ms. Then: drop the entry (`busy`). No device or game thread waits for this.
5. Read the switch again. Off or unreadable: stop. This is what makes Off + Clear safe against writers in flight.
6. Read the list. A list that exists but cannot be opened or read: stop, never replace it. A torn version 1 list
   counts as empty. A list of a later version: stop.
7. Apply: the same process (equal `start` and `pid`) adds its API bit and keeps `starts` (one process through both
   shells is one launch); another process of the same path counts a start, sets `start`, `pid` and `apis` and
   moves the entry to the front; a new path goes to the front with `starts` 1. Keep at most 64. Nothing changed:
   stop without writing.
8. Write the new list to the temporary file and rename it over the list in one step (`FileRenameInfoEx` with
   replace and POSIX semantics, `MoveFileExW` with replace where those are not offered). A reader sees the old
   list or the new one.
9. Unlock and close. If the process ends while it holds the lock, the system releases it.

## Reader protocol (control application)

- Open `recent-launches.txt` for reading with sharing **read, write and delete**
  (`FileShare.ReadWrite | FileShare.Delete`). Without delete sharing a writer's rename fails while the file is open
  and that launch is lost.
- Read at most 8 MiB, close, then parse strictly as above. A missing file is an empty list.
- Do not take the lock to read and never write the list.
- Icons, titles and the file name come from the path, read off the UI thread with a time bound; never start the
  recorded program to get them.

## Clear and Off (control application)

"Clear the list" (this user):

1. If the user also turns the list off: write `RecordRecentLaunches` = 0 **first**.
2. Take the lock: open the lock file as the writer does and lock byte 0. .NET Framework `FileStream.Lock(0, 1)`
   does not wait; retry every 20 ms for up to 2 s on a background thread.
3. Delete `recent-launches.txt` and `recent-launches.tmp`. Never delete the lock file: a writer that opened the
   deleted file would lock a different file than the next one.
4. Unlock and close.

If the lock cannot be taken within 2 s, report that the list could not be cleared now and offer to try again.
Never delete the list without the lock: a writer that holds the lock could rename its list back afterwards.

Turning the list on again writes 1 or removes the value. Turning it off without Clear keeps the list as it is.

Removing the list of **all** users (the uninstall question of the GUI plan, C20) is a separate, explicitly named
action: the same Clear, run by an administrator, in each user profile.

```csharp
// Read (sketch)
using (var fs = new FileStream(listPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
{ /* read up to 8 MiB, parse strictly */ }

// Clear (sketch, background thread)
if (turnOff) WriteSwitch(0);
using (var lockFile = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite,
                                     FileShare.ReadWrite | FileShare.Delete))
{
    var deadline = DateTime.UtcNow.AddSeconds(2);
    while (true)
    {
        try { lockFile.Lock(0, 1); break; }
        catch (IOException) { if (DateTime.UtcNow > deadline) return ClearResult.Busy; Thread.Sleep(20); }
    }
    try { DeleteIfExists(listPath); DeleteIfExists(tempPath); }
    finally { lockFile.Unlock(0, 1); }
}
```

## Measured cost (development PC, 2026-10-04)

From `recent-launch-test.exe` (below), NVMe NTFS volume, Microsoft Defender real-time protection on:

| Part | Median | Worst |
|---|---|---|
| creating thread (once flag and `CreateThread`) | 0.06 ms | 0.16 ms of 100 |
| whole helper on the worker: inputs, both switch reads, lock, read, write, rename, against a full list of 64 long paths | 10.6 ms | 50 ms of 300 (95th percentile 16 ms) |
| of which the inputs (token, environment, module and Windows paths, process times) | 0.23 ms | |
| of which writing and renaming the list | 0.93 ms | |

Most of the worker's time is the first open of the list after a write (8.7 ms; a second open takes 0.04 ms), which
matches a real-time scan of a modified file. On unit A the comparison of device creation time with the switch on
and off is lab trial L2.

## Host gate G-RG

`tools/build/test-umd-recent-launch.ps1` builds and runs `driver/umd/recent-launch/recent-launch-test.cpp`; both
shell builds run it. It writes only below its output directory and under one test key
`HKCU\Software\amdgpu-wddm-test\recent-launch-<pid>`, which it removes; it only reads the real switch. The AppContainer
case creates the AppContainer profile `amdgpu-wddm.recent-launch-test` and deletes it again.

| Case | Test |
|---|---|
| format: round trip, every truncation rejected, count, version, limits | `test_format` |
| switch: absent key, absent value, 1, 0, 2, `REG_SZ`, `REG_QWORD`, a key denied to the reader | `test_switch` |
| duplicate basenames, letter case and `..` spellings, 8.3 names | `test_basenames` |
| Japanese, Korean and emoji paths, a path over 300 characters, a lone surrogate | `test_unicode_long` |
| Windows directory boundary, this process's own inputs | `test_windows_boundary` |
| own programs excluded by file name only (prefix and `.exe`, any case) | `test_own_tools` |
| one process through both shells counted once, also when both note at the same moment; pid reuse | `test_same_process` |
| pruning at 64, a launch with an earlier clock kept | `test_prune` |
| torn list replaced, newer version kept, unreadable list never replaced | `test_store_states` |
| Off + Clear while a writer is in flight (before and inside the lock), Clear alone in both orders | `test_off_clear_in_flight` |
| prune/read race: 600 writes against three readers, one holding the file open | `test_prune_read_race` |
| lock bound: busy after the wait, nothing written | `test_lock_bound` |
| 24 processes created at once, 12 of them for the same path | `test_processes` |
| an AppContainer child records nothing | `test_app_container` |
| `note_outer_device` end to end and the timing above | `test_note_and_timing` |

Failed outer creation: `driver/umd/dxvk/ddi-draw-test.cpp` checks that a `CreateDevice` without its engine modules
takes no note; `driver/umd/d3d12/adapter-test.cpp` points `LOCALAPPDATA` at its build directory and checks that its
failing `CreateDevice` calls leave no list.
