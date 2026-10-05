# app-route - which UMD an application's Direct3D 10/11 loads on the lab

The desktop and the applications do not have to share one user-mode driver. The registered router file decides
per process: it reads a policy key at every `OpenAdapter` call and loads either the DXVK-based GPU UMD or the
CPU UMD. This directory is the control plane of that decision for the lab unit (M14.1): it stages the package,
swaps the router file, writes the policy, reads back what every process actually mapped, and runs one bounded
client under the new route.

Every lab step is one SSH call through `tools/win/target.py`. Nothing here polls the lab, because the lab's
sshd penalises repeated connections.

## The kill switch

```
python tools/win/app-route/approute.py policy cpu
```

This writes `Mode=cpu` into `HKLM\SOFTWARE\amdgpu-wddm\AppRouter` and changes nothing else. Every application
that opens an adapter after it goes to the CPU UMD. The allow list and the paths stay in the key, so a later
`policy allowlist` or `policy gpu-default` returns without a repackage.

Two facts about the kill switch, and both matter under pressure:

- A process that already opened its adapter keeps the UMD it mapped. The switch acts at the next
  `OpenAdapter`, so a hung application needs an end of its process as well.
- It does not move the desktop. DWM has its own route and its own switch (`DwmForceCpu`, the control
  application's `desktop-cpu`). The application policy and the desktop route are separate controls.

The rollback ladder, in order of reach: `policy cpu`, then `swap rollback` (the frozen desktop router back at
its registered path, never refused by the owner STOP flag), then the lab baseline restored by the operator.

## Commands

```
python tools/win/app-route/approute.py package            host: assemble the package; refuses a changed input
python tools/win/app-route/approute.py --self-test        host: package checks and the ops tests; no lab
python tools/win/app-route/approute.py push               LAB: copy the package over and admit it
python tools/win/app-route/approute.py swap install|rollback|status|cleanup
python tools/win/app-route/approute.py policy cpu|allowlist|gpu-default|remove [--allow a.exe,b.exe] [--deny c.exe]
python tools/win/app-route/approute.py status [--no-mappers]     LAB, read-only
python tools/win/app-route/approute.py run CLIENT [--args "..."] [--seconds N] [--interactive]
                                                          [--exe PATH] [--screenshot-at 20,60]
powershell -ExecutionPolicy Bypass -File tools\win\app-route\ops\tests\test-approute-lib.ps1 -Out <fresh dir>
```

`CLIENT` is `d3d11bench`, `d3d11mt`, `d3d11fl12`, `dxdiag`, `taskmgr` or `exe`. A client run is at most 170 s.
`status` is the witness that matters: it reports the registered UMD names, the active router file, the policy
key, DWM's filtered modules with hashes, and, per UMD, the processes that map it by name and PID.

## The lab side

`ops\` holds the scripts that run on the lab in an elevated SSH session, which lives in session 0 and has no
desktop.

| Script | What it does |
|---|---|
| `app-stage.ps1` | admits the pushed package. Every hash must be equal. Then it sets the ACLs that let a low-integrity or AppContainer process append its route line and load the GPU UMD |
| `router-swap.ps1` | swaps the registered router file in place, under the same path. The registration does not change and the adapter needs no restart. It renames the old file aside and never deletes a file that a process maps |
| `app-policy.ps1` | writes and reads back the policy key. `cpu` is the kill switch |
| `app-status.ps1` | read-only report of the active route and of every process that maps each UMD |
| `app-run.ps1` | one bounded client run. An interactive run goes through a one-shot task as the logged-on user |
| `app-child.ps1` | the interactive half: it starts the client, waits, and ends its process tree |
| `approute-lib.ps1` | the pure functions behind all of these, tested on the development PC |
| `durable.ps1` | a verified byte copy. A durable artifact exists before any mutation |

## What it needs

- The lab configuration outside this repository, at `<BC250_ROOT>\secrets\client\target.json`. See
  `tools/win/README.md`. No address and no key is in this repository.
- A work directory in the workspace, by default `<BC250_ROOT>\scratch\m15\app-route`
  (`BC250_APPROUTE_WORK` moves it). It holds the built router, the host gate runs, the assembled package, the
  receipt of every lab call and the pulled run directories. The repository holds no binary and receives no
  output.
- Windows PowerShell 5.1 for the ops test. The lab's remote shell is 5.1, so the tests run there as well.

## Hazards

- **The swap acts on the next `OpenAdapter`, not at once.** DWM keeps the router it mapped until its next
  restart. Read `status` before you draw a conclusion from a frame on the screen.
- **An interactive run puts a window on the owner's screen.** The overlay carries the announcement and the
  owner STOP flag stops a run. `swap rollback` ignores the STOP flag by design, because a rollback must always
  work.
- **The package pins exact inputs by SHA-256.** The pinned quartet and the frozen baseline router are those of
  the 2026-10-01 attempt. `package` and `--self-test` therefore refuse today: the lab baseline moved on, and
  the registered router is no longer the frozen one. The lab steps (`push`, `swap`, `policy`, `status`, `run`)
  do not depend on that refusal. Repackage under a new name when the baseline moves, and never edit a package
  that a trial already named.
- **`witcher3.exe` and the two oracle windows stay on the CPU UMD in every mode.** The deny list is part of the
  package. A game process that mapped a second copy of the Vulkan driver from another directory was the reason.
- Every lab call writes a receipt into the work directory. Keep it. A route claim without a receipt and a
  `status` reading is an opinion.
