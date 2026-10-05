# Native graphics state control

Hypothesis: the hosted d3d10umd/Zink/RADV path implements native D3D depth,
stencil, alpha blending, conditional pixel shaders and render-to-texture with
the same defined pixel results as WARP and the retained llvmpipe baseline.

Build graphics-state-control.cpp with MSVC /W4 /WX, D3D11, DXGI and D3DCompiler.
Run only on the lab, first `warp`, then `baseline`, then `hosted`. Hardware modes
select the uniquely enumerated BC-250 adapter through the system D3D runtime.
Use the existing app-scoped hosted router; keep DWM on its CPU baseline. Verify
exact DLL hashes, STOP and temperature, retain an outer process deadline and
restore the registered libraries in finally. No window or swapchain is created.

Each mode checks all4096 pixels after eight stages: passing and rejected depth,
stencil replacement, rejected and accepted stencil references, alpha blending,
a per-pixel branch and a second render target fetching the first. Depth is
D24_UNORM_S8_UINT. A disassembly check requires an actual conditional DXBC
opcode. All expected channels are exact except alpha blending, which allows
one UNORM8 step for rounding; hashes expose cross-renderer differences anyway.
An event query must complete within10 seconds before staging readback. Resource
release must leave GetDeviceRemovedReason successful. The process runner also
bounds hangs inside runtime calls.

Any mismatched pixel, missing renderer witness, compiler failure, device loss
or timeout fails the control. Retain failures as evidence. This checks native
graphics functions; it does not itself prove DWM execution, asynchronous
cross-device sharing, display cadence or absence of CPU presentation copies.

Result: all three modes pass as M567. GPU hashes match WARP in all eight stages.
Evidence: [native graphics state](../../../evidence/windows/2026-09-27-E34-graphics-state/RESULT.md).
