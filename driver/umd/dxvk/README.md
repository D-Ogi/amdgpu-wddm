# System D3D10/11 UMD on DXVK: integration in progress

ADR 0017 selects DXVK as the engine behind the system runtime DDI. This directory starts the new UMD integration; it is not a loadable driver, does not advertise FL11 and is not deployed. The working G0 Mesa/Zink driver remains the desktop control.

`runtime-domain.h` implements device-scoped, thread-local authority for runtime callbacks. The DDI wrapper creates a `RuntimeDomain::Scope`; the hosted dispatch callback checks the same domain in this UMD module. A DXVK worker has no scope and must marshal work back to a runtime entry instead of calling the callbacks directly. This component does not implement that marshalling queue, locking, lifetime ownership or the engine interface. A domain must outlive its scopes and engine activity; its address cannot be moved or copied. Do not duplicate this guard independently in both DLLs.

The design carries the existing G0 entry-scope semantics, including nested device entries and migration between runtime threads. It avoids binding a device to its creation thread. Tests prove denial without calling the callback for an unentered device, sibling device and worker thread, plus nesting, exception unwinding, exact callback return forwarding and subsequent runtime entry on another thread.

Build/run from the workspace:

```powershell
& .\bc250-win\tools\build\test-umd-domain.ps1
```

Current source basis for integration: Mesa UMD revision `71f2e28c1b14f60ecbe11e543279bc76ea0c1237`, hosted contract version 5; WDK 10.0.26100.0. The existing `State.h` defines `SUPPORT_D3D11 0`, and `CreateDevice` accepts D3D10/10.1 interfaces only. FL11 needs an actual D3D11 table, feature/capability negotiation and mapped DXVK operations; changing the support macro is insufficient. Shader DDI input is a token stream plus DDI signatures, so a COM boundary must account for DXBC container construction or expose an engine shader entry that accepts these inputs.

Next integration requirements: agree the versioned engine boundary; wire device creation/destruction and the runtime domain; preserve allocation/import/Present fence ownership; implement resource/shader/state/draw and FL11-specific operations; prove a system-runtime client uses the driver without app-local D3D DLLs. Image/sharing/lifecycle and the ADR performance comparison still require measurement. All lab trials remain bounded to 180 seconds.

## Input-layout translation

`input-layout.cpp` consumes actual WDK `D3D10DDIARG_CREATEELEMENTLAYOUT` entries, preserving their input register numbers, binding slots, resolved offsets and instance divisors (including zero). Binding storage is compacted without renumbering slots. Conflicting per-slot rates/divisors, duplicate registers, invalid bounds/formats and offsets that cannot fit DXVK's packed representation are rejected transactionally. The engine supplies vertex-format lookup/support information; there is no duplicated DXGI format table.

`input-layout-dxvk.h` produces actual `DxvkVertexAttribute` and `DxvkVertexBinding` arrays for the engine. WDK and DXVK headers live in separate translation units: including both exposed conflicting extern-C KMT declarations from DXVK's `util_gdi.h`. The internal data header uses Vulkan types only. This is a compiled adapter component, not a complete or versioned cross-DLL ABI; it is not yet wired into a runtime-loaded DDI table.

```powershell
& .\bc250-win\tools\build\test-umd-input-layout.ps1 -DxvkSource .\scratch\m14\dxvk
```

Validated against DXVK `52fe923ca1496c8e44789b613fab8611dfcb5c4a` and WDK 10.0.26100.0 with MSVC C++20, `/W4 /WX` for project code (`/external:W0` for upstream/WDK headers). The test passes the translated data across separate WDK and DXVK units, covering sparse slot31/register17, instance divisor zero, conflicting bindings, overflow/alignment/packing bounds and preservation of output after failure. No GPU operation is exercised by this test.

PROVENANCE: DXVK (github.com/doitsujin/dxvk), zlib; its existing vertex attribute/binding types are included from the external source checkout, not copied into this repository.

## Hosted runtime bridge

`runtime-bridge.cpp` extracts the G0 callback bridge from Mesa Device.cpp revision `71f2e28c1b14f60ecbe11e543279bc76ea0c1237`. It replaces the Gallium device dependency with runtime-owned handles, callback tables and `RuntimeDomain`. `host_descriptor` supplies the existing version-5 bootstrap descriptor (including adapter LUID) without creating a second runtime device. The ICD borrows its userdata until Vulkan destruction finishes. The caller must initialize the state, own cleanup and drain workers; the component is not yet connected to the new UMD's CreateDevice/DDI table.

The inherited operation set covers allocation, GPU VA, residency, context translation, synchronization and submission. It retains existing unsupported-operation errors; this is not a claim that every operation enumerated in the private header is implemented. Present callback orchestration and primary import will be wired at the device layer. Fault-injection switches and stderr diagnostics from the G0 bring-up copy are removed. Status/loss and submission counters remain available to the owner of the bridge.

```powershell
& .\bc250-win\tools\build\test-umd-runtime-bridge.ps1
```

The callback control builds with `/W4 /WX` for project code and checks allocation outputs, context-token translation/destruction, worker denial, wait-before-submit handling with no duplicate wait for the same Present value, pending residency translation and sticky device loss. These are controlled host callbacks, not a new hardware measurement. The private host contract is copied unchanged from the cited G0 revision.

PROVENANCE: Mesa (gitlab.freedesktop.org/mesa/mesa), MIT; callback bridge derived from the project's Mesa fork and original license retained.
