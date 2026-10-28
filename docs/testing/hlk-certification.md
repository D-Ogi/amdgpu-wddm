# HLK and WHCP certification plan

This page records the Microsoft rules that apply to a WHCP submission of a graphics adapter driver. It lists the
HLK test areas for such a driver and the order in which we plan to run them. The research was collected on
2026-10-12. Each policy claim was then compared with the cited Microsoft page on the same day. A claim that we
could not find on a Microsoft page carries the mark **Not verified**.

This page describes Microsoft policy and our plan. The dated blocker review below compares those requirements with the driver.

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

## Blockers from the documentation

Review date: 2026-10-13. The profile is Full Graphics, WDDM 2.0, Windows 11 25H2 x64, with WoW64 user-mode components.
The source review uses driver revision `878f296291ad3671c25532a56a251d9fff0918ba` and the named candidates below.
It does not replace the device-specific test list that HLK will generate. No row is a measured HLK failure.

The review uses the 25H2/26H1 component specification and the official 25H2 compatibility playlist downloaded on 2026-10-12.
Requirement names below identify sections in that specification. Job names identify entries in that playlist.
The local Microsoft driver guides are at revision `110f60eaf2ac5836e644d320c1e92c1011f2af5e`.
The DDI reference is at revision `7515063cea4c9e98db6a92986c5b4ddb0463fd16`, with WDK headers `10.0.26100.0`.
The playlist stays outside the repository. Its presence does not establish that every listed job applies to this adapter.

**Setup** means that we cannot yet run the required measurement.
**Contract gap** means that a documented requirement has no matching implementation in the reviewed path.
**Pending** means that a candidate or existing implementation still needs the relevant validation.
S means configuration, packaging or a focused check. M means a bounded implementation or validation series.
L means a subsystem change or a broad compatibility campaign. These sizes are estimates, not delivery dates.

BD numbers identify tracked defects. Environment statements come from the local environment record.
The latest dated results take precedence over older summaries. The table does not reopen defects whose later evidence records a fix.

| ID | Requirement and source | Current state and evidence pointer | Work needed |
| --- | --- | --- | --- |
| E1 | Running controller and client. [HLK prerequisites](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/getstarted/windows-hlk-prerequisites), controller and test-system requirements. | **Setup.** Hyper-V activation needs a manual host restart. The VHLK image is downloaded. The local environment record lists no running controller or registered client. | **S.** After the host restart, create the switch and VHLK VM. Install the matching client on 26200 and put it in a Ready pool. |
| E2 | Client prerequisites and reliable return after test restarts. HLK prerequisites, test-system and network requirements. | **Setup.** The client scripts have not run. Default boot returns to 22631. The local environment record describes prepared prerequisite checks and boot selection. | **S.** Check language, networking, shares, account policy and storage. Keep 26200 selected through the campaign's restarts, then restore the previous boot policy. |
| E3 | A working adapter and desktop on the full-run OS. Prerequisite for `WGF11 D3DWDDM` and the presentation jobs. | **Pending final package.** BD-135 r4 with BD-136 r1 now passes desktop controls on 22631, 26200 and 26300. R2 fixes r1's OOM contract and awaits lab validation. See the BD-136 results. | **M.** Check the replacement package on 26200, through login, rendering and device teardown. Keep CPU and GPU route results distinct. |
| E4 | Device-selected tests, matching official playlist and current filters. [Select and run tests](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/getstarted/step-6-select-and-run-tests). | **Setup.** The local environment record lists downloaded playlist and filter files. No adapter test-list export or HLK result package exists in the reviewed inputs. | **S.** Import the matching files, select the installed adapter and export its test list. Record the package, kit and filters with each run. |
| H1 | Certification hardware coverage. [Graphics prerequisites](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/graphic-adapter-or-chipset-testing-prerequisites), hardware requirements. | **Setup.** The local environment record lists one board and one attached monitor. The prerequisites ask for two computers and two adapters per INF family, with multi-display coverage. | **M.** Supply the required hardware coverage or get an applicable program exception. A single-monitor job's Continue option does not waive the submission prerequisites. |
| H2 | Test-system storage and sleep coverage. HLK prerequisites, 300 GB disk requirement. `DF - Sleep with IO During (Reliability)`. | **Setup.** The local environment record lists an 80 GB maximum for the client VHDX. Native VHDX boot does not support hibernation. Explicit HLK acceptance of this boot layout is unverified. | **M.** Resolve storage for certification. Use a supported hibernation-capable installation for selected S4 tests, or document why those tests do not apply. |
| G1 | GDI hardware acceleration. [WDDM feature caps](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-driver-and-feature-caps), mandatory Full Graphics row. `GDIHW_Functional test for GDI HW ACC (v1.1)`. | **Implemented, HLK job not run.** Since 0.7.216.105-tester.33, [wddm.c](../../driver/kmd/wddm.c) fills `DxgkDdiRenderGdi` and `DxgkDdiRenderKm` and sets `SupportKernelModeCommandBuffer` when `EnableGdiHwAccel` is 1, the installation default. Lab trials 3 to 5 of [the GDI lab plan](gdi-hw-accel-lab.md) passed (facts M926, M927). | **S.** Run the native and WoW64 jobs on the 26200 client with a package that has the capability on. A job that does not appear in the selection means that the capability did not reach the OS. |
| G2 | Virtual resolution and DWMClone. Component specification, `Device.Graphics.WDDM20.Display.VirtualModeSupport.CoreRequirement`, page 251. | **Contract gap.** The specification says WDDM 2.0 drivers report `VirtualModeSupport=1`. [wddm.c](../../driver/kmd/wddm.c) has no `DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION` case. An unhandled query returns `STATUS_NOT_SUPPORTED`. | **M.** Implement the query and its mode semantics together. Check applicability of the section's If-Implemented label against the selected WDDM 2.0 profile. Do not merely set the bit. |
| V1 | D3D11 decoder DDI. Component specification, `Device.Graphics.WDDM12.Render.D3D11VideoDecoding`, pages 229-230. | **Contract gap.** The rendering UMD does not expose the video path. See [ddi-format.cpp](../../driver/umd/dxvk/ddi-format.cpp) and [ddi-clear-view.cpp](../../driver/umd/dxvk/ddi-clear-view.cpp). The M15.11 encoder is a separate interface. | **L.** Implement the required H.264 decoder profile, NV12 and 420_OPAQUE output, and the specified x64/x86 protection support. Run the selected DXVA jobs. |
| V2 | D3D11 video processor DDI. Component specification, `Device.Graphics.WDDM12.Render.D3D11VideoProcessing`, pages 230-231. | **Contract gap.** The same UMD has no video processor table. Ordinary texture sampling and format admission do not implement video processing. See [ddi-format.cpp](../../driver/umd/dxvk/ddi-format.cpp). | **L.** Add the required formats, deinterlacing, scaling, rotation and colour conversion. Check the video processor caps and selected jobs. |
| V3 | WDDM 2.0 hardware content protection. Component specification, `Device.Graphics.WDDM20.DisplayRender.HardwareContentProtection`, page 250. | **Contract gap.** [ddi-wddm2.cpp](../../driver/umd/dxvk/ddi-wddm2.cpp) routes hardware-protection entries to `Unexpected`. There is no demonstrated protected playback path. | **L.** Establish the hardware and driver protection contract, implement it and run the applicable content-protection tests. The specification also requires access to PlayReady test servers. |
| V4 | Media files for selected video tests. HLK graphics prerequisites, supplemental content. | **Setup.** The local environment record lists the two supplemental media images as not downloaded. | **S.** Get the media required by the jobs selected for this adapter. This does not block unrelated rendering tests. |
| D1 | DDI error rules under allocation failure. [Handling errors](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/handling-errors) and the `DestroyDevice`, `SetShader`, `ResourceMap`, `QueryGetData` DDI pages. | **Pending.** BD-136 r1 could report a forbidden OOM and skip teardown. R2 allocates a device worker at creation. Its host tests exercise the production wrappers under forced allocation failure. | **M.** Check r2 on the real runtime. Cover both production architectures and all compiler frontends. An OOM result is legal only where the specific DDI permits it. |
| D2 | Runtime concurrency. `D3D12 - MultiThreaded Create Record and ExecuteTest`, plus `WGF11 D3DWDDM`. | **Pending.** Own clients and BD-136 host controls cover selected calls, callback reentry and independent devices. They are not the HLK workload. See [M15 reconciliation](../m15-reconciliation.md) and the BD-136 r2 host results. | **M.** Run the selected multithreading jobs against the final package. Check deferred contexts, concurrent creation, submission and teardown. |
| D3 | 32-bit user-mode coverage on x64 Windows. `DXGI Presentation testing - FSPresentMS (WoW64)`. | **Pending.** BD-133 and BD-134 have later D3D9 controls. The BD-136 r2 package manifest lists x86 host tests but only x64 production DLLs. | **M.** Build and check the x86 production components. Run the selected WoW64 jobs. Do not treat a passing x64 desktop as WoW64 coverage. |
| M1 | Heap residency and eviction. `WDDM2 - Make Resident - Verify Standard Test` and `WDDM2 - Memory Budget Test - Trim To Budget 11 Test`. | **Pending.** BD-052 has an offline candidate for imported heaps that retained residency. Its lab plan requires joint validation of the kernel and user-mode changes. | **M.** Check the combined candidate, then run pressure, eviction and make-resident jobs. Include heaps created not resident and mixed allocation ownership. |
| M2 | Offer, reclaim and working-set behaviour. `WDDM2 - OfferReclaim11 - Working Set Test`. | **Pending.** [ddi-dxgi-resources.cpp](../../driver/umd/dxvk/ddi-dxgi-resources.cpp) forwards runtime surfaces. It keeps engine-private resources resident without forwarding their offers. No working-set HLK result establishes adequacy under pressure. | **M.** Measure the selected job, including private suballocations. Add allocator-level offer accounting if the pressure tests expose retained memory. Keeping reclaim contents alone is not working-set evidence. |
| R1 | Hang recovery. `SimulateVSync TDR` and `TDR2 - Multiple TDR Compute Lockout and Recovery Test (Enumerated Adapter)`. | **Pending.** [Hang recovery](../design/hang-recovery.md) records working soft recovery for a bounded class. Paging-node reset and parked memory-fault waves remain outside that recovery. BD-114's watchdog fix does not establish full TDR coverage. | **L.** Run the documented fault classes and repeated recovery jobs. Extend recovery for failing classes. Preserve unrelated contexts and check desktop recovery. |
| R2 | PnP restart and mode changes. `WDDM PnPStop Test` and `WDDM PnPStopModeSwitch Test`. | **Pending.** BD-124 has a later device-restart picture fix. BD-093's second measurement round still records a stop/start performance regression. | **M.** Run repeated stop/start with rendering and mode changes. Check performance, contents, interrupts and thermal controls after restart, not only device status. |
| P1 | Presentation correctness across the supported mode and format set. `DXGI Presentation testing - FSPresentMS` and `Display Mode Filter Check`. | **Pending.** [M15 reconciliation](../m15-reconciliation.md) describes partial coverage. Later BD-105 and BD-124 records add Vulkan flip and HDMI mode results. Neither is a complete HLK matrix. | **M.** Run composed and direct presentation, scaling, intervals, formats and mode changes on the final package. Include the direct-DP control and adapter hotplug. |
| S1 | Kernel code integrity. `HyperVisor Code Integrity Readiness Test`. | **Pending.** No HLK HVCI result accompanies the reviewed package. Test signing and ordinary loading do not establish HVCI compatibility. The requirement is described under Test environment above. | **M.** Run readiness checks and the applicable HVCI workload. Fix executable-memory or mapping violations if found. Do not infer failure solely from test signing. |
| S2 | INF and submission validation. `DF - InfVerif INF Verification` and `Static Tools Logo Test`. | **Pending.** Our build gates do not supply these HLK results. The May 2026 kit also has the documented DVL version issue described above. See Kits on 2026-10-12 for the selected version. | **S.** Run the package jobs and produce the required DVL with the documented tool version and filter. Keep CodeQL's graphics-driver recommendation distinct from a mandatory result. |

Interpretation limits matter when using this table:

- G2 quotes the WDDM 2.0 rule within a section labelled If-Implemented. The missing query is a source finding, not an observed OS refusal.
- V1 does not make every codec mandatory. The specification lists VC-1, MPEG-2, HEVC and VPx under conditional support.
- V3 is the WDDM 2.0 protection requirement. OPM and COPP also depend on connector and monitor capabilities, as the graphics prerequisites explain.
- The specification permits DMA-buffer-boundary preemption. Our declared granularity is not itself a missing feature. Scheduler correctness still needs the selected preemption jobs.
- Runtime F-state power management is conditional in `Device.Graphics.WDDM12.DisplayRender.RuntimePowerMgmt.RuntimePowerMgmt`, page 227. Missing optional callbacks alone do not prove a certification failure.
- The D3D9 DDI requirement does not, by itself, prove that a new vendor D3D9 frontend is necessary. Qualify the actual D3D9On12 route.
- No reviewed DDI or HLK page specifies a universal 64 KiB caller stack. That size belongs to our BD-136 regression control.
- The BD-136 checks do not cover every D3D12 or WGL frontend before the backend compiler. Those paths need separate stack review.

### What blocks the first campaign

E1, E2 and E4 block scheduling tests on the intended controller/client pair. E3 has passing r1 controls but needs acceptance of the replacement r2 package.
The additional HLK disk budget, long campaigns and the client's required account setup are approved.
The host restart remains a manual action. Those approved decisions are not open permission requests.

The first campaign can record failures while implementation gaps remain. It need not wait for G1, V1, V2 or V3 to be complete.
H1 and H2 limit certification coverage, but do not prevent an internal rendering run on the current single-board setup.
V4 is needed when the selected media jobs run. A thermal guard must remain active throughout an HLK campaign.

### What blocks full certification

G1 and V1-V3 identify substantial missing interfaces for the selected profile. G2 needs its requirement interpretation and implementation resolved.
H1, H2 and V4 need the required environment coverage. D1-S2 need final-artifact results for the selected jobs, with fixes where those results expose defects.
The selected test list can add further requirements. This review cannot certify an adapter without that list and its results.
Reduced results for older supported Windows versions and the final submission package remain required by the policy above.

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
