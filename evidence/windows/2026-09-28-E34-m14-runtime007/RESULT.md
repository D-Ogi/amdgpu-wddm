# M734 - Optional-format error reports disappear; native device creation still fails

Runtime007 source12d52cbc, manifest8CBC4772F5E50D9C986E7E54C4F129BFF4A3E8794E6785312C751360DE64E414. New shell handles E_FAIL with zero optional FORMAT_SUPPORT2 following a successful primary format query. Other rendering artifacts/client/debugger unchanged from006; router rebuilt for007.

CPU control passes. GPU debug output no longer contains M14 DDI error messages or D3D11 Removing Device, but client D3D11CreateDevice still returns887a0020, with handled C++ first-chance exceptions and exit1. No GPU image result. This narrows the failure and validates disappearance of the previous logged error path; it does not establish successful device construction or prove the remaining origin. Further DDI table and runtime initialization audit is needed.

Supervisor failed-restored38.9584761s; CPU verified, baseline restored, independent postflight verified, tree closed. Cleanup true. No KMD or DWM restart. Full raw logs remain scratch/m14/runtime007-ops. Published diagnostics exclude module paths and memory payloads, preserving only debug strings/status/code addresses. No performance or M14 completion claim.
