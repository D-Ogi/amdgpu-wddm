Escape probe with tools/win/bc250kmd_cli, unit A (BC250-A), Windows 11 Pro, 2026-09-21.

Question (ADR 0006, open questions): does D3DKMTEscape from an elevated user-mode tool reach a display-only
WDDM miniport? bc250kmd is not installed yet, so this run measures what the answer looks like on the drivers
that were on the machine, as a baseline to compare against once bc250kmd is installed.

Method: bc250kmd_cli info [hardware-id]. It finds the device by PnP hardware id among the
GUID_DISPLAY_DEVICE_ARRIVAL interfaces, opens it with D3DKMTOpenAdapterFromDeviceName, and sends
BC250_ESCAPE_GET_INFO as D3DKMT_ESCAPE_DRIVERPRIVATE with the 64-byte BC250_ESCAPE structure of
driver/kmd/bc250kmd_escape.h, Flags.Value = 0. Run over SSH, elevated, session 0. Read-only: the tool's
'confirm' command was not used, and it refuses to create the bc250kmd service key.

Files:
  probe-basicdisplay.txt   07:17:42, the GPU on Microsoft Basic Display Adapter (the state of the machine
                           today, and again after E05 was rolled back)
  probe-kmdod-e05.txt      07:12, captured from the console while the main session had Microsoft's KMDOD
                           sample bound to the same device (experiment E05). Console transcript, not a file
                           written on the target
  probe-basicdisplay-after-e05-rollback.txt
                           07:22:49, the same probe repeated after E05 was rolled back. Identical statuses,
                           so the measurement survives a driver cycle on the device. At this moment the
                           service bc250kmdod still existed but was Stopped and the device was bound to
                           BasicDisplay (DEVPKEY_Device_Service = BasicDisplay, driver 10.0.22621.1)

Results, all three with the identical request:

  Microsoft Basic Display Adapter   PCI\VEN_1002&DEV_13FE   D3DKMTEscape -> 0xC000000D STATUS_INVALID_PARAMETER
  Microsoft KMDOD sample (E05)      PCI\VEN_1002&DEV_13FE   D3DKMTEscape -> 0xC00000BB STATUS_NOT_SUPPORTED
  Microsoft Basic Render Driver     ROOT\BasicRender        D3DKMTEscape -> 0xC00000BB STATUS_NOT_SUPPORTED

D3DKMTOpenAdapterFromDeviceName returned STATUS_SUCCESS in every case, so the adapter was reached and the
failure is the escape itself.

What this does and does not show. It shows that the adapter can be opened by PnP hardware id and that an
escape can be issued to a display-only driver; it does not by itself show that a miniport's DxgkDdiEscape was
called, because none of the three drivers has a private escape of ours to answer with. The two statuses
differ, which is worth noting but is not yet evidence of where each request stopped. The question is settled
only by the same command against bc250kmd, which does fill DxgkDdiEscape (driver/kmd/entry.c).

Side result, measured on the development PC the same day: D3DKMTOpenAdapterFromDeviceName refuses paths of
the GUID_DEVINTERFACE_DISPLAY_ADAPTER interface class with STATUS_INVALID_PARAMETER for every adapter, and
accepts the GUID_DISPLAY_DEVICE_ARRIVAL path of the same device. Only the latter is usable.
