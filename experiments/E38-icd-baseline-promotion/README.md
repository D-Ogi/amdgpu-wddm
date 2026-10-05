# E38: registered ICD baseline promotion (9C40083C to 93B1D1FD)

Question: can the registered Vulkan ICD on unit A move off the pre-M496 build (DXGI loaded by module
name, deadlocks under app-local DXVK, E37) to the fork branch with the M496 fix and D3DKMT adapter
enumeration, without losing any other agent's ICD work and with the recorded controls passing?

Scope: source comparison of the live ICD trees (fork 940ab0eb versus the hosted candidate's tree), the
M546 flip regression on the candidate with the E36 procedure, then the promotion itself with a rollback
copy and the E14 smoke on the promoted file in place. No UMD, DWM, KMD or registry change.

Result: `evidence/windows/2026-09-27-E38-icd-baseline-promotion/RESULT.md` (M569). The trees are the same
code apart from licence headers; the flip control passed (120 Presents, 0/76800, three 46800-pixel ROIs);
the registered ICD is 93B1D1FD with `vulkan_radeon.9C40083C.dll` next to it for rollback.

Open: rerun the E37 probe with sparse ungated (expected FL 12_1); the DWM probes on the new baseline are
the other agent's next window; the WSI interop helpers still load DXGI/D3D12 by path.
