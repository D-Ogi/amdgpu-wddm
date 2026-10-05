# M745 - Mismatched engine ABI rejected before window diagnostics

Window004 source5777ec32, manifest75C8B751B0222D13D4D13825E5942442C109FC17FA38CE99E2166AA7B01BA257.
CPU window control passes. GPU module load returns E_NOINTERFACE80004002;
D3D11CreateDevice returns887a0004, client exit1. No GPU device, scene or Present
and no runtime-resource descriptor were measured. The DISPLAYABLE_SURFACE
hypothesis from M744 remains untested.

The diagnostic shell was rebuilt using the evolving DXVK checkout with ABI1.4
while staging frozen engine253A with ABI1.3. Production loader admission rejects
this version pair before loading the ICD. A no-GPU host probe compiled with the
same loader reproduces80004002 with the ABI1.4 header (negative control), and
returns S_OK with the80352134 ABI1.3 header and the same actual engine/ICD files
(positive control). Receipts include exact header and module hashes. This is
module admission, not a GPU rendering test.

Independent supervisor failed-restored41.8003343s: CPU verified, GPU false,
baseline restoration/postflight/tree closure true. Task Missing confirmed at
2026-09-28T14:18:51.9610902Z. No KMD/DWM/OS restart; CPU171 remains baseline.
Raw logs remain in scratch/m14/window004-ops. Selected excerpt excludes module
paths and exception addresses; result contains only phase/status/timing data.
The shell build now accepts exact engine/ICD paths for a pre-build admission gate.
