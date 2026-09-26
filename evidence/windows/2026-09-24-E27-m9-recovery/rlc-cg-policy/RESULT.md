# M390 - RLC clock-gating policy fixed; candidate121 built

PROVENANCE: AMD Linux v6.18.52 amdgpu_rlc.c / amd_shared.h, MIT.
The three GFX clock-gating condition masks and an unrelated GFX_MGLS test mask
are extracted unchanged into generated/rlc_cg_flags.h. Exact source and hash
are preserved. The helper now succeeds without a request for zero or unrelated
cg_flags. Requested scopes retain ACK checking, paired exit and the existing
refusal for disabled RLC. Windows-specific stricter ACK/error handling remains;
this is not a wholesale import of upstream's void helper.

Existing SDMA scope models explicitly select GFX_MGCG. The reset model now has
8 scenarios including zero flags, unrelated MGLS and each relevant CG flag,
and still disturbs halt/ring/cache state on reset before verifying retirement.
Full GFX replay passes:354+35 writes match, four negative controls discriminate,
8 reset scenarios have0 failures. User-mode replay and kernel compile pass.

Candidate0.7.121.1 includes the prior120 capture counters and this policy fix.
WDK build/sign pass. Packagecheck reports25 checks,0 errors,0 warnings and13
notes. Stack-budget output retains3992/2248/2120/1240-byte frame advisories;
no claim of universal stack safety or resource sufficiency is made.
SYS SHA256:14E4A60D7FB36BD59CF51F1ED5CBF6DD13018AB377265AD5F7618B11C5E23321.
Package: scratch/build/bc250kmd-07121/package-umd (workspace-relative).

Not deployed. Lab remains in Linux E29;119 remains installed on Windows.
The source-policy mismatch is fixed locally, but successful warm reentry,
Windows recovery and full M9 acceptance require hardware evidence. Next restore
USB Windows fallback, preserve transition history and install121 at that
necessary transition, then first-load controls before one warm reentry trial.
