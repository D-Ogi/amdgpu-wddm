# Isolated KMD160 build

2026-09-27. Source a1f3bf9f900e571a3da79459b1133ac7069b62eb,
derived from exact KMD159/e898ff1 with selected source imports in audit.json.
SYS8E676C810190EF38BACF3E8332EDC2760E07844FD779E18CB32B22D24AC90218.
UMD package0.7.160.1; escape0x000700A0.

Adds the source GDI policy, unlock-boundary tests, complete-page CPU diagnostic
range validation, Present status classification and producer/submit counters.
EnableCddDwmInterop is separately start-latched and default off; when explicitly
enabled it advertises interop with8192-texel maximum dimensions. RenderKm and
allocation admission remain unchanged by that setting.

303 existing driver/third_party files compared with exact153/c3499f1b. Only the
six listed existing files differ; new helper/test files are captured by the
source manifest. All13 quick gates, full compile/link, stack check, catalog and
signing commands succeed. Largest fixed stack frame3992 bytes (existing
GfxPagingBuildUpdate warning). Source manifest reports clean and eligible,
and both packages contain the same signed SYS hash.

Additional host Get-AuthenticodeSignature checks on both160 and the existing159
return UnknownError: chain ends at a root not trusted by this host. This is not
a valid host trust verdict. No host trust-store change was made. The lab's test
certificate is part of the retained installation procedure; target load and
signature acceptance are not established by this build.

Preparation initially stopped on a documentation patch because its newer
context was absent in the isolated159 tree. The final setting's code patch was
applied separately and its documentation copied from exact54f4d39. The resulting
source was audited and committed before building; no fuzzy code patch applied.

Build command:
pwsh -NoProfile -File scratch/g0-kmd160-source/driver/kmd/build.ps1
-Kits P:/bc-250/toolchain/nuget -Out P:/bc-250/scratch/g0-hosted/kmd160-final001
-UmdStub P:/bc-250/scratch/g0-hosted/kmd157-final001/package-umd
-QualityWorkspace P:/bc-250

Local artifact only. No staging, adapter transition or setting change on the lab.
KMD159 remains deployed. Interop/BGP1 runtime, residency and G0 remain open.
