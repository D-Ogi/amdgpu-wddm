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
3. The start gate, reason `app-protected-unconfirmed`. The kernel driver must have confirmed this start:
   `Parameters\UnconfirmedStarts` is 0 and the volatile `GuardBoot\Confirmed` is 1
   ([wddm-start-confirmation.md](wddm-start-confirmation.md), `driver/kmd/guard.c`). Each `StartDevice` adds one
   to the counter, and a durable confirmation writes 0 and the volatile mark. A runtime restart of the device in a
   confirmed boot adds one again, so it is unconfirmed until its own confirmation. A value that cannot be read
   closes the gate.
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
`protected=desktop:gpu/absent,start:confirmed/0/1,latch:clear/hklm/2,armed:1`.

## What this gives the first sign-in of a boot

The start confirmation needs at least 60 s of healthy presentation. The sign-in screen of a boot without
automatic sign-in opens its adapter before that time, so it draws on the CPU UMD. This is the intended result:
before the confirmation, nothing shows that this start can draw a screen. Later screens use the GPU UMD:
`LogonUI.exe` exits after each sign-in, and Windows starts it again for each lock, unlock and Ctrl+Alt+Del.
A sign-in screen that waits for a long time stays on the CPU UMD until it closes.

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
| A crash of the machine or a power loss | restart | the latch is gone with the boot. The start is unconfirmed until the confirmation, so the first sign-in is on the CPU UMD |
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
  record.
