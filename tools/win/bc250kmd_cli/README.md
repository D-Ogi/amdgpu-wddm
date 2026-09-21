# bc250kmd_cli - the user-mode end of the M3 miniport

A small native tool that talks to `driver/kmd` (bc250kmd) from user mode. It exists for two reasons: to settle
one of the open questions of ADR 0006 - **does `D3DKMTEscape` reach a display-only miniport at all?** - and to
read the driver's breadcrumbs on a machine with no kernel debugger.

```
bc250kmd_cli info [hardware-id]   the escape, to the adapter with that PnP hardware id
                                  (default PCI\VEN_1002&DEV_13FE)
bc250kmd_cli list                 every graphics adapter, with hardware id, interface path, handle and LUID
bc250kmd_cli stages               LastStage / StageHistory / UnconfirmedStarts, with names
bc250kmd_cli confirm              UnconfirmedStarts = 0 (elevated)
```

Exit codes: `0` done, `1` the operation failed (the failing call and its NTSTATUS are printed), `2` bad usage
or the driver is not installed, `3` (`stages` only) the start budget is used up.

## How the adapter is found

`D3DKMTOpenAdapterFromGdiDisplayName` names a *display*, not a device, so with more than one adapter, or while
ours is installed but not driving the screen, it can open the wrong one. `D3DKMTEnumAdapters2` hands out
handles with no way back to the PnP device. So the tool enumerates device interfaces with SetupAPI, matches
`SPDRP_HARDWAREID` against the id it was given, and opens that device's interface path with
`D3DKMTOpenAdapterFromDeviceName`.

The interface class must be **`GUID_DISPLAY_DEVICE_ARRIVAL`**, which dxgkrnl registers for every graphics
device, display-only ones included. Measured on the development PC on 2026-09-21: paths of
`GUID_DEVINTERFACE_DISPLAY_ADAPTER`, the obvious-looking name, are refused by
`D3DKMTOpenAdapterFromDeviceName` with `STATUS_INVALID_PARAMETER` (0xC000000D) for every adapter, while the
`GUID_DISPLAY_DEVICE_ARRIVAL` path of the same device opens.

## The escape, and what a failure means

`info` sends `BC250_ESCAPE_GET_INFO` as `D3DKMT_ESCAPE_DRIVERPRIVATE` with the `BC250_ESCAPE` structure of
`driver/kmd/bc250kmd_escape.h` (included by relative path, never copied). Every step prints its own NTSTATUS,
because the failure is a measurement too:

- against a driver that has no such escape, the call fails and the hex status is the answer;
- against bc250kmd, a failure means dxgkrnl does not route escapes to a display-only miniport, and M4 needs
  another control channel (ADR 0006, open questions).

Measured on unit A, 2026-09-21, same request every time, evidence
`evidence/windows/2026-09-21-escape-probe/`:

| Driver on the device | Hardware id | `D3DKMTEscape` |
|---|---|---|
| Microsoft Basic Display Adapter | `PCI\VEN_1002&DEV_13FE` | `0xC000000D` STATUS_INVALID_PARAMETER |
| Microsoft KMDOD sample (E05, display-only) | `PCI\VEN_1002&DEV_13FE` | `0xC00000BB` STATUS_NOT_SUPPORTED |
| Microsoft Basic Render Driver | `ROOT\BasicRender` | `0xC00000BB` STATUS_NOT_SUPPORTED |

`D3DKMTOpenAdapterFromDeviceName` returned STATUS_SUCCESS in all three, so the adapter was reached and the
escape itself is what failed. None of these drivers has a private escape of ours to answer with, so this is
a baseline, not an answer: the question is settled by the same command against bc250kmd, which does fill
`DxgkDdiEscape`.

## Stage names

`g_Stages` repeats `enum BC250_STAGE` from `driver/kmd/bc250kmd.h`. `test_stages.py` parses both and fails the
build if they drift apart (`build.ps1` runs it; the monitor keeps a third copy with its own test).

## Build

```powershell
pwsh tools\win\bc250kmd_cli\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd_cli
python -m unittest discover -s tools/win/bc250kmd_cli
```

Headers and import libraries come from the SDK NuGet packages, the compiler from the installed Visual Studio:
the same flow as `tools\win\bc250rd\build.ps1`, without the driver and the signing. On the target it lives in
`C:\BC250\kmd\`.
