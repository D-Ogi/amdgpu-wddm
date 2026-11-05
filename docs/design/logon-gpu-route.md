# The logon screens on the GPU route

Status: implemented in the router (`driver/umd/router`), host gate PASS. Not yet measured on the lab: the lab
script is `tools/win/app-route/ops/logon-gpu-probe.ps1`.

## Goal

The vendor drivers load their D3D user-mode driver in every process. The sign-in screen, the lock screen and
the UAC prompt therefore draw on the GPU. Up to this change our router kept five images on the CPU UMD with no
exception: `LogonUI.exe`, `consent.exe`, `LockApp.exe`, `CredentialUIBroker.exe` and `winlogon.exe`. The reason
was a risk: a fault of the GPU UMD in such a process can stop the sign-in.

This change gives four of these images the application GPU UMD, the same as any other image. It adds gates
that keep them on the CPU UMD when the start of the driver is not known to be healthy, and a latch that moves an
image back to the CPU UMD after a fault. A fault of the GPU UMD can then cost one screen per boot, and the next
screen of that image comes up on the CPU UMD.

## Decision per image

| Image | Runs as | What a fault stops | Route |
|---|---|---|---|
| `LogonUI.exe` | Local System, secure desktop | the sign-in and the unlock | gated, latch required |
| `consent.exe` | Local System, secure desktop | every elevation, also the uninstall of this driver | gated, latch required |
| `LockApp.exe` | the user, AppContainer | the lock curtain only. `LogonUI.exe` still asks for the credential | gated, latch best effort |
| `CredentialUIBroker.exe` | the user | the one prompt that it shows | gated, latch best effort |
| `winlogon.exe` | Local System | - | always the CPU UMD |

`winlogon.exe` opens no Direct3D adapter. It starts `LogonUI.exe` and `dwm.exe` and draws nothing itself, so
the GPU UMD gives it nothing. It stays on the list so that its route does not change.

`consent.exe` has the same risk as `LogonUI.exe`, not a higher one. A fault there blocks the tools that repair
the driver, so its latch is required as well. `CredentialUIBroker.exe` has a lower risk: Windows starts it on
request, and a fault fails that one request.

`LockApp.exe` runs in an AppContainer. It cannot write `HKLM` or `HKCU\Software`, so its latch cannot be armed.
Its failure does not stop a sign-in. The route of a locked session goes through `LogonUI.exe`, which has its own
latch. For this reason the latch of `LockApp.exe` is best effort. If the AppContainer cannot read the kernel
driver's state either, the start gate stays closed and `LockApp.exe` stays on the CPU UMD. The lab script
measures which of the two is true.

## The gates

The router decides at every `OpenAdapter10_2`. For a gated image it reads the gates in this order and stops at
the first gate that is closed. Each closed gate has its reason in the route line.

1. `AppRouter\ProtectedGpu` (`REG_DWORD`). Absent or 1: on. 0, any other number or any other value kind: off,
   reason `app-protected`. Off is the router that shipped before this change, so this value is the bisect
   switch of the feature.
2. The desktop gate, reason `app-protected-desktop-cpu`. `Decide()` must send `dwm.exe` to the hosted GPU UMD
   with the same `DesktopRouter` key and the same start-latched kernel switches (`InteropLastState`). Also, the
   desktop-route record of the session (`driver/contract/bc250_desktop_route.h`) must not say that the
   compositor ended on the CPU UMD. A record that cannot be read does not close the gate.
3. The start gate, reason `app-protected-unconfirmed`. Each `StartDevice` adds one to
   `Parameters\UnconfirmedStarts`, and a durable confirmation writes 0 and the volatile mark `GuardBoot\Confirmed`
   = 1 ([wddm-start-confirmation.md](wddm-start-confirmation.md), `driver/kmd/guard.c`). The gate is open in two
   states:
   - Confirmed: the counter is 0 and the mark is 1.
   - Provisional: the counter is 1 and the mark is absent. This is the first start of a boot after a start that
     was confirmed. The image must be able to write the provisional record (next section), and the record must not
     be off for this GPU UMD (reason `app-protected-provisional-off`).

   Every other state closes the gate. A counter of 2 or more is a start after a start that was never confirmed.
   A counter of 1 with the mark is a runtime restart of the device in a confirmed boot, which stays closed until its
   own confirmation. A value that cannot be read also closes the gate.
4. The fault latch (next section): `app-protected-faulted`, `app-protected-reset`, and for `LogonUI.exe` and
   `consent.exe` also `app-protected-unlatched` when the store cannot be written.

After the gates, the image follows the `AppRouter` policy like any other image. These opt-outs therefore apply
to the logon images as well:

- `Mode` = `cpu`, the application kill switch.
- An entry in `Deny`.
- `allowlist` without an entry in `Allow`.
- `RenderOnCpu` = 1, globally or for the image ([per-app-graphics-settings.md](per-app-graphics-settings.md)).

The route line ends with `protected=<word>`. The word is `no` for every other image and `never` for
`winlogon.exe`. It is `off` when `ProtectedGpu` is off. Otherwise it gives the state of each gate, for example
`protected=desktop:gpu/absent,start:confirmed/0/1,latch:clear/hklm/2,prov:on/0/0/none,armed:1`.

## What this gives the first sign-in of a boot

The start confirmation needs at least 60 s of healthy presentation. The sign-in screen of every boot opens its
adapter before that time. Up to 2026-11-05 the start gate needed a confirmed start, so the first sign-in screen of
each boot drew on the CPU UMD (lab, 2026-11-05: `LogonUI.exe` 15 s after the boot,
`start:unconfirmed/1/0`). The vendor drivers draw that screen on the GPU from its first frame (BD-299).

The provisional state opens the gate for that screen: `UnconfirmedStarts` is 1, the count of this start only,
so the start before it was confirmed. The latch protects a provisional start as it protects a confirmed one. A
fault of `LogonUI.exe` moves the next sign-in screen to the CPU UMD, and a recovered TDR moves every gated image
to the CPU UMD for the rest of the boot.

A hang that the driver does not recover kills the boot and its volatile latch. The next start then reads 2, and
its sign-in screen draws on the CPU UMD. When that boot is confirmed, the start after it reads 1 again. Without
more state, every second boot would hang. The provisional record stops this loop.

## The provisional record

`driver/umd/router/router-provisional.h` keeps a record that survives a restart:

| Key and value | Kind | Meaning |
|---|---|---|
| `HKLM\SOFTWARE\amdgpu-wddm\ProtectedStart\Pending` | `REG_SZ` | the GPU UMD path of a provisional GPU route whose start is not known to be confirmed |
| `HKLM\SOFTWARE\amdgpu-wddm\ProtectedStart\Off` | `REG_SZ` | the GPU UMD path for which the provisional route is off |
| `HKLM\SOFTWARE\amdgpu-wddm\ProtectedStart\OffCount` | `REG_DWORD` | 1: one failed provisional boot of that path. 2: its retry failed too |
| `HKLM\SOFTWARE\amdgpu-wddm\ProtectedStart\OffAt` | `REG_DWORD` | the value of `Boots` when `Off` was written |
| `HKLM\SOFTWARE\amdgpu-wddm\ProtectedStart\Boots` | `REG_DWORD` | the boots whose first decision read a provisional start |
| `HKLM\SOFTWARE\amdgpu-wddm\ProtectedLatch\Provisional` | `REG_DWORD`, volatile | 1: a decision of this boot wrote `Pending` |
| `HKLM\SOFTWARE\amdgpu-wddm\ProtectedLatch\BootCounted` | `REG_DWORD`, volatile | 1: `Boots` counts this boot already |

- Before the GPU open of a provisional route, the router writes `Pending` and flushes the key. Then it writes the
  volatile mark. If one of the writes fails, the image takes the CPU UMD (`app-protected-unconfirmed`).
- Each decision that reads a start state also reads the record and does what it finds:
  - A confirmed start removes `Pending`.
  - `Pending` without the mark comes from an earlier boot. When the counter is 2 or more, the start before this
    one was never confirmed: `Off` takes the path, with `OffCount` 1 and `OffAt` = `Boots`, and `Pending` goes.
    When the counter is 0 or 1, that start was confirmed, and `Pending` goes.
  - When `Pending` and `Off` name the same path, that start was the retry (see below). If the retry is confirmed,
    `Off`, `OffCount` and `OffAt` go too (action `retry-ok`). If it is not, `OffCount` becomes 2.
- The first decision of a boot that reads a provisional start adds 1 to `Boots`: the start before was confirmed.
  The volatile mark `BootCounted` makes sure that each boot counts once.
- `Off` closes the provisional route for that GPU UMD path only. A confirmed start is not affected by `Off`.
  - With `OffCount` 1, the route stays closed until `Boots` - `OffAt` is 5. Then one provisional start takes the GPU
    UMD again (state `retry`), and it writes `Pending` as any provisional route does.
  - With `OffCount` 2, the route stays closed until a new driver package, or until a tool removes `Off`.
  - A new driver package has a new path, so it tries the provisional route again at once. A failure of the new
    path starts again at `OffCount` 1.

A power loss or a hang of another cause before the confirmation of a provisional boot also sets `Off`. That costs
the GPU route of the first sign-in screen for five boots, then the retry tries it again. Only a second failure of
the same path keeps it off until the next driver package. The record follows the guard's own counter, so a
tool that writes `UnconfirmedStarts` = 0 before a restart (the lab kit's `restart.ps1`) also clears this
evidence. Only Local System (`LogonUI.exe`, `consent.exe`) can write the record. `LockApp.exe` and
`CredentialUIBroker.exe` wait for the confirmation.

The route line shows the record as `prov:<state>/<offcount>/<since>/<action>`:

- The state is `on`, `off`, `retry` (the one retry of an `Off` path), `nostore` (the record cannot be written) or
  `unread` (no start state was read).
- The off count is the `OffCount` of an `Off` that names this GPU UMD, and 0 when there is none. The since value is
  `Boots` - `OffAt` of that `Off`. For example, `prov:off/1/3/none` is a first failure three boots ago.
- The action is `none`, `cleared`, `retry-ok`, `set-off`, `written`, `retry-ok+written`, `kept` or `write-failed`.

When the start gate is closed, the route line shows `latch:unread`. The router does not read the latch behind a
closed gate, so it takes no base there.

## The fault latch and the recovery

`driver/umd/router/router-latch.h` keeps the latch in a volatile key. Local System writes
`HKLM\SOFTWARE\amdgpu-wddm\ProtectedLatch`, and a user process writes the same path below `HKCU` when `HKLM`
refuses it. The configuration manager drops a volatile key at every boot, so each boot tries the GPU UMD again.

- Before the GPU open, the router writes a marker `<image>|<pid>` with the creation time of the process. It also
  pins its own module, so a `FreeLibrary` of the runtime cannot unload it.
- At `ExitProcess` the router's `DllMain` (`DLL_PROCESS_DETACH`, reserved argument not null) removes the
  marker. A failed GPU open removes it at once, because the process continues on the CPU UMD.
- A process that ends in a different way keeps its marker. Examples are an unhandled exception (the default
  handler ends the process with `TerminateProcess`), a kill and a fault inside the GPU UMD. The next decision for
  the same image finds a marker with no live process. A process ID with a different creation time counts as no
  live process. The router then writes `<image>` = 1 and removes the marker. Each later process of that image
  in this boot takes the CPU UMD (`app-protected-faulted`). The latch of one image does not change the route of
  another image.
- At the first decision that passes the start gate, the router writes `Base`, the value of
  `HangRecovery\AdapterRestarts` (`driver/kmd/guard.c`) at that time. An adapter restart after that time is a
  recovered TDR. It can come from a logon image, and the router does not try to find the process that caused
  it. Every gated image then takes the CPU UMD for the rest of the boot (`app-protected-reset`).

What Windows does after a fault, and what happens next:

| Event | What Windows does | Next screen |
|---|---|---|
| A fault of the GPU UMD ends `LogonUI.exe` | `winlogon.exe` starts `LogonUI.exe` again (to be measured, see the lab plan) | the marker has no live process: CPU UMD, `app-protected-faulted` |
| A GPU hang that the driver recovers | TDR, event 4101. The process gets device-removed | `AdapterRestarts` > `Base`: CPU UMD, `app-protected-reset` |
| Hangs that the driver does not recover | bugcheck 0x116, restart | the interop session marker closes the desktop route (`InteropClosedReason` 4). Then the desktop gate closes, and the start guard counts the start |
| A crash of the machine or a power loss | restart | the latch is gone with the boot. The next start reads 2, so its sign-in is on the CPU UMD. If the boot that died took the provisional route, `Off` closes that route for this GPU UMD. It stays closed for five boots, or until the next package after a failed retry |
| A start that the guard refuses | Windows uses its fallback adapter driver | our router does not run |

The latch does not see a fault that the process itself catches and then follows with a clean exit. Such a
process was able to continue, so it did not stop the sign-in.

## Security of the store

A standard user cannot write `HKLM\SOFTWARE`, so only Local System and administrators can set the latch of
`LogonUI.exe` and `consent.exe`. A user can write the `HKCU` store, and that store controls only the processes of
that user. A value in either store can only move a process to the CPU UMD. It cannot move a process to the
GPU UMD.

## Open points

- `LogonUI.exe` can end through `TerminateProcess` from `winlogon.exe` after a successful sign-in. In that case
  each clean sign-in looks like a fault, and the image stays on the CPU UMD after its first sign-in of the boot.
  The lab script reads the store after a lock and unlock cycle to measure this.
- What `winlogon.exe` does after a crash of `LogonUI.exe`, and how many times it starts it again, is not
  measured on this driver.
- The other UMD entries (D3D9, D3D12, OpenGL) have no list of logon images. The logon images are known to load
  the D3D10/D3D11 entry (`LogonUI.exe` loaded the CPU UMD `bc250d3d.dll` in
  `evidence/windows/2026-09-25-E27-m9-recovery/shared-cpu-cache147`).

## Tests

- Pure policy (`test-router.cpp` `ProtectedTests`, in the `policy` scenario): the kind of each image and its
  neighbours, `StartHealthOf`, `LatchOf`, `DesktopAdmitsGpu`, and a decision table. The table has one row for
  each closed gate and for each opt-out after the gates.
- Private-hive scenarios (`tools/build/test-umd-router.ps1`), with the harness under the image names:
  `protected-confirmed`, `-lockapp` and `-consent` take the GPU UMD and arm the marker, and the parent reads the
  hive after the child ended, to make sure that the clean exit removed the marker. `protected-gates` closes one gate
  at a time. `protected-latch-dead` (a process that ended), `-reused` (a process ID used again), `-set` and
  `-reset` give the CPU UMD. `protected-gpu-fails` removes the marker after a failed GPU open. `protected-winlogon`
  stays on the CPU UMD. `protected-logonui` gives `app-protected-unconfirmed`, because the fixture holds no start
  record. `protected-provisional` takes the GPU UMD on a provisional start and writes the record before the GPU
  open. `-off`, `-set-off`, `-cleared` and `-confirmed` read a record that an earlier boot or this boot left.
  The router before this change fails these five scenarios and the four that read the route word.
