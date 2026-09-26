# Hosted RADV integration after M534

Decision: prototype a per-D3D-device hosted RADV winsys as the primary M13 path.
M532/M533 imported explicitly shareable app-owned memory successfully. M534 proves
runtime virtual-context/allocation callbacks on the real hardware adapter, but
returns no hKMResource suitable for the existing NT export path. None of these
results establishes GPU DWM or asynchronous native Present.

Local sources: ref/m13-notes/hosted-icd-and-dxbc-nir.md; WDK 10.0.26100 in
ref/ddi-display; Microsoft threading-model-of-user-mode-display-driver.md.
The AGPL prototype is read-only prior art; no source is imported from it.
The local IRC discussion proposes an internal extension, not a Vulkan standard.

Implementation sequence:

1. Device ownership: our current Adapter creates a Zink screen before CreateDevice
   supplies runtime callbacks. Hosted rendering needs a separate screen/instance
   per runtime device, with callback copies and explicit lifetime. Retain adapter
   capability queries without attaching a shared global screen to one device.
2. Private bridge: version and size the callback descriptor. Runtime hRTDevice is
   an opaque callback token, not a D3DKMT device handle. Do not substitute it into
   thunks. Keep standalone Vulkan's existing winsys unchanged.
3. Dispatch per winsys: map allocation, deallocation, GPU VA, residency, context,
   submission, synchronization and teardown to their runtime callbacks. A global
   dispatch-table replacement would mix independent D3D devices and is unsuitable.
4. Threading: callbacks must stay in the runtime's device serialization domain.
   Compiler workers may compile; defer GPU submission/allocation callbacks back
   to the entry thread. Inventory Zink deferred submit and RADV worker paths.
5. Resource contract: borrow the runtime allocation without owning/deallocating it;
   carry LB7A metadata and GPU VA explicitly. Do not reinterpret an allocation
   handle as standard OPAQUE_WIN32_KMT outside the private hosted contract.
6. Native Present: submit rendering, signal the monitored fence, enqueue a wait
   on the runtime presentation context, then PresentCb with its allocation handle.
   Verify exact pixels and completion ordering before deploying the DWM UMD.
7. Device/resource teardown: destroy hosted Vulkan work before runtime callbacks
   become invalid; never close a runtime-owned device through D3DKMTDestroyDevice.

Follow with flip/blt application and DWM allocation inventories plus PresentMon
ETW, then real desktop GPU work and M12/M13 acceptance. Keep CPU readback oracles,
but do not use full-frame CPU copying as the final rendering architecture.

Shader follow-up: preserve TTN while establishing a frontend NIR boundary; evaluate
MIT dxbc-spirv separately for semantic mapping/binding correctness. M531 does not
prove TGSI removal compatibility. System D3D11 FL11, D3D9 DDI, D3D12 DDI and WoW64
are separate capabilities and are not implied by this M13 prototype.
