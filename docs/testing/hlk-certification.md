# HLK and WHCP certification plan

This page records the Microsoft rules that apply to a WHCP submission of a graphics adapter driver. It lists the
HLK test areas for such a driver and the order in which we plan to run them. The owner collected the research on
2026-10-12. Each policy claim was then compared with the cited Microsoft page on the same day. A claim that we
could not find on a Microsoft page carries the mark **Not verified**.

This page describes Microsoft policy and our plan. It does not describe the state of the driver.

## Path

We run the full HLK set on Windows 11, version 25H2 x64. On the older versions that the submission covers, we run
the reduced set. The WHCP Policies and Processes document for 25H2 and 26H1 defines this split in the section
"Graphic Adapters Test Optimization" (pages 25 and 26). It asks for complete HLK logs for 25H2. For the older
versions in its table, it asks for reduced HLK logs that include the job `DF - InfVerif INF Verification`.

| Windows version | HLK run | Basis |
| --- | --- | --- |
| Windows 11, version 25H2 x64 | The full HLK set. This run is the base of the submission. | Policies, pages 25 and 26 |
| Windows 11, version 26H2 x64 | No separate certification for a component that has a 25H2 certification. Our own regression run is still useful. | Microsoft post of 2026-09-29 |
| Windows 11, version 24H2 x64 | The reduced run (`DF - InfVerif INF Verification`), if the submission covers 24H2. | Policies, page 25 |
| Windows 11, versions 22H2 and 23H2 x64 | The reduced run with the older "22H2" qualification. Use the HLK and the playlist that match 23H2. | Policies, pages 15 and 26 |
| Windows 10 x64 | Optional. The reduced run. | Policies, page 26 |
| Windows Server, Arm64 | Separate targets. They are not in the first submission. | Our plan |
| Windows 11, version 26H1 | Microsoft intends 26H1 for select hardware platforms. It is not a natural target for the BC-250. | Policies, page 15 |

Notes on the table:

- The kit matrix on page 15 of the policies names one kit, "22H2", for Windows 11 versions 22H2 and 23H2. That kit
  accepts device submissions for both versions. The Microsoft post about 26H2 does not discuss 23H2.
- The research called the Windows 10 program a wind-down. **Not verified**: the cited policy lists Windows 10
  x64 and x86 with reduced logs, and it does not say anything about an end of the program.
- The policy table also lists Windows 11 21H2 x64 and Windows 10 x86 with reduced logs. This plan does not target
  them.

## Windows 11, version 26H2

Microsoft published the rules for 26H2 on 2026-09-29.

- Microsoft does not introduce a separate WHCP designation, HLK kit, submission process or driver-signing program
  for 26H2.
- Components and drivers that have a 25H2 certification do not need a new certification for 26H2. Their 25H2
  signatures stay valid on systems that run 26H2.
- Partner Center and the Hardware Compatibility List give the 25H2 designation for these products. A 26H2
  designation is not available.

## Editions and 32-bit games

We do not plan separate runs for Home and Pro. **Not verified**: the policy applies to "the Client editions of
Windows 11" as one scope, and we found no rule that asks for a run per edition.

We do not install a 32-bit Windows to support 32-bit games. The x64 playlist holds WoW64 jobs, which run the
32-bit graphics path on 64-bit Windows. One example is `DXGI Presentation testing - FSPresentMS (WoW64)`. One
`DXGIPresent.exe` command runs a job and its WoW64 twin. A WoW64 job loads the 32-bit user-mode components of the
driver package, so the package must contain them. This last sentence is our reading. The test page does not state
it.

## How HLK selects the tests

HLK Studio lists the tests for the features that it finds on the device. The official playlist then limits the
list to the jobs of the program. The playlist version must match the kit version, and a submission must use the
official playlist without changes. The exact list for our adapter therefore comes from HLK itself: the device,
its detected features and the official playlist.

## Test areas

On 2026-10-12 we searched the official 25H2 playlist for each example job below. Each name is in that playlist.
The list of jobs that HLK selects for our adapter can be longer or shorter.

| Area | Example jobs |
| --- | --- |
| Driver package | `DF - InfVerif INF Verification` |
| Code analysis and security | `Static Tools Logo Test`, `HyperVisor Code Integrity Readiness Test` |
| Direct3D and the UMD | `WGF11 D3DWDDM`, the other WGF11 jobs, the D3D9 and D3D12 sets |
| DXGI and present | `DXGI Presentation testing - WinpresentMS`, `DXGI Presentation testing - FSPresentMS`, `DXGI Presentation testing - Other` |
| WDDM 2.0 memory | The `WDDM2 - Make Resident`, `OfferReclaim`, `Memory Budget` and `Page Fault` jobs |
| GPU scheduling | `TestPreemptionGranularity`, `TestHighPriorityOverLowPriority` |
| Hang and recovery | The `TDR2` jobs |
| PnP, restart and power | `WDDM PnPStop Test`, `WDDM PnPStopModeSwitch Test`, the `DF` jobs |
| Display | `Verify VESA and CEA required display modes`, `Display Mode Filter Check` |
| Video and output protection | DXVA, video processing, OPM and HDCP jobs. They depend on the hardware and its features. |

The DXGI Presentation jobs make swap chains in full-screen mode and in windows. They make back buffers in all
supported formats, with sizes equal to the window or different from it. They use flip and copy presents. They
also test the swap effects and the present intervals. The test reads each presented image with
`IDXGIOutput::GetDisplaySurfaceData` and compares it with the surface that filled the back buffer.

## Requirements that come with the driver model

- WDDM 2.0 does not force WDDM 3.x. The component specification describes WDDM 2.0, and it calls WDDM 3.0
  "an optional version level". A driver that implements a model must meet all requirements of that model.
- A cleared cap does not exempt a feature that the profile marks mandatory. **Not verified** as a Microsoft
  sentence. It is our reading of the "M" rows in the caps tables.
- The graphics prerequisites page says that OPM and COPP requirements follow feature detection. A full WDDM
  driver must support OPM and COPP if it has a capable connector and monitor.
- The WDDM 2.0 requirements table lists the mandatory UMD DDIs by DirectX hardware class. Every class in it
  includes the D3D9 UMD DDI. The D3D12 class adds the D3D12 UMD DDI. The research put this table on the caps page.
  We found it in the component specification, under `Device.Graphics.WDDM20.Core.Base`.
- The WDDM 1.2 caps table marks GDI hardware acceleration mandatory for the Full Graphics and Render-Only driver
  types. Determine which row applies to our profile before the 25H2 run.

## Test environment

The environment has these parts:

1. An HLK controller. It can be a virtual machine, the Virtual HLK (VHLK).
2. The physical hardware with our driver. HLK does not support a test system in a virtual machine.
3. The HLK client of the matching kit on that hardware.
4. The official playlist of the matching kit.
5. The current HLK filters, which remove known false failures.

The graphics prerequisites ask for at least two test computers and at least two adapters for each device family in
the INF. One adapter must support more than one display and have a monitor attached. Multihead jobs have a
documented path for a family that does not support more than one monitor. On a single-monitor system the job asks
for a second monitor, and "Continue" runs it in single-monitor configuration.

Run CodeQL and the HVCI tests early:

- The `HyperVisor Code Integrity Readiness Test` must pass before Microsoft signs a driver.
- The `Static Tools Logo Test` requires CodeQL Must-Fix results for all kernel-mode drivers, except graphics
  drivers. For graphics drivers, Microsoft calls CodeQL "highly recommended".
- The Windows HLK (Updated May 2026) for 25H2 has a known issue. Its `Static Tools Logo Test` reports that the
  driver verification log (DVL) does not meet a version requirement. Microsoft says: generate the DVL with WDK
  28000.1761, then apply waiver 320241. Microsoft expects a fix in the next 25H2 kit.

## Kits on 2026-10-12

| Target | Kit | Version |
| --- | --- | --- |
| Windows 11, version 25H2 | Windows HLK (Updated May 2026) for Windows 11, version 25H2. The same link also serves 24H2. | 10.1.26100.8328 |
| Windows 11, version 25H2 | Virtual HLK (VHLK) for Windows 11, version 25H2 | 26100.8328 |
| Windows 11, versions 22H2 and 23H2 | Windows HLK for Windows 11 23H2 and 22H2 | 10.1.22621.5413 |

We read each version from the file version of the signed installer, or from the file name of the VHLK image.

## Order of work

1. Fix the profile: Full Graphics, WDDM 2.0, x64, and the real set of DDIs and caps.
2. On 25H2, let HLK make the test list for the installed driver package.
3. Run these first: InfVerif, CodeQL and the DVL, HVCI, `WGF11 D3DWDDM`, DXGI Present, residency and TDR.
4. Then run the full set, with the WoW64 jobs.
5. Make the reduced results for 24H2 and, if wanted, for 22H2 and 23H2. Run our own regression on 26H2.

The most useful next result is the export of the HLK test list for our adapter. That list turns the requirements
into a checklist. Each line gets one of three marks: implement, test, or not applicable.

## Sources

- WHCP specifications and policies, with the 25H2 and 26H1 document package:
  <https://learn.microsoft.com/en-us/windows-hardware/design/compatibility/whcp-specifications-policies>
- Windows Hardware Compatibility Program guidance for Windows 11, version 26H2 (2026-09-29):
  <https://techcommunity.microsoft.com/blog/windows-hardware-certification/windows-hardware-compatibility-program-guidance-for-windows-11-version-26h2/4560836>
- `DXGI Presentation testing - FSPresentMS` (lists the WoW64 jobs):
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/45c2c5be-26d0-4fc0-a0f3-7149f214f29c>
- Step 6, select and run tests:
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/getstarted/step-6-select-and-run-tests>
- `WDDM2 - Make Resident - Verify Standard Test`:
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/aeb6d8f0-9c34-45ac-a9c4-8f91ba83025c>
- `WGF11 D3DWDDM`:
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/88f5272c-253e-47ee-a30a-c0e4377f103a>
- Graphic adapter or chipset testing prerequisites:
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/graphic-adapter-or-chipset-testing-prerequisites>
- WDDM driver and feature caps:
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-driver-and-feature-caps>
- Windows Hardware Lab Kit, the kit table and the VHLK:
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/>
- Driver compatibility with HVCI:
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/driver-compatibility-with-device-guard>
- CodeQL FAQ for the WHCP:
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/codeql-faq>
- How to create a driver verification log:
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/creating-a-driver-verification-log>
- Windows HLK prerequisites:
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/getstarted/windows-hlk-prerequisites>
- Windows HLK filters:
  <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/user/windows-hardware-lab-kit-filters>
- The official playlists: <https://aka.ms/HLKPlaylist>
