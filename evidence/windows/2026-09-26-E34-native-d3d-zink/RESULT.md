# Native D3D through Zink: first bounded GPU draw

Unit A, Windows, 2026-09-26. MEASURED, not full M13 acceptance.

Run011 exits0. Native D3D runtime loads the app-local Zink-only UMD (0D1C4C34), Vulkan loader, and registered RADV from C:/BC250/m10/wsi-final. Explicit DXGI LUID selection matches PCI1002:13FE. Both the blue clear and red full-screen vertex/pixel shader triangle read back4096/4096 exact RGBA pixels. GetDeviceRemovedReason is S_OK. Device/context destruction completes. No CPU Gallium backend is linked. The runtime's D3D_DRIVER_TYPE_SOFTWARE argument is the custom UMD loading mechanism, not evidence of CPU rasterization.

Failures preserved: runs001-004 fault during the native presentation-context callback under the custom-driver runtime. The isolated offscreen build skips that callback; it is not suitable for DWM installation. Runs005-007 clear correctly but fault when CPU dummy vertex pointers reach Zink. A real zero vertex resource fixes draw. Runs008-010 render correctly but fault during teardown; a private approved minidump identifies zink_bind_vertex_elements_state -> update_existing_vbo -> update_buf_bind_count from cso_unbind_context. Frontend vertex-reference replacement now uses pipe_resource_release, transferring ownership according to Gallium's contract. Run011 passes the complete lifetime. Run009 accidentally repeated run008 unchanged after an instrumentation edit failed; it is not a separate fix/control.

Run001's PowerShell runner lost ExitCode; it is not a pass. Run002 onward use System.Diagnostics.Process and preserve the signed exit code. PASS text alone in runs008-010 is insufficient because the process subsequently crashes.

DWM1856 still loads CPU llvmpipe UMD8279AC7F at14:20:59Z, same Windows boot15:50:14.5000000+02:00. No reboot, DWM restart, system ICD/UMD registration change or M11 soak was performed by E34. The minidump stays outside the repository; raw stacks containing memory are not published.

Limitations: a64x64 constant-color shader triangle is not a desktop, texture/blend correctness test, performance comparison, conformance result, presentation test or GPU surface-sharing proof. CPU primary-import code remains and must be replaced before GPU DWM. Next gates: repeated/context-shared GPU texture ownership, texture/blend draws, native allocation sharing and presentation, then system DWM validation. Full M12/M13 requirements remain open.
