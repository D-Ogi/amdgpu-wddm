# M602: isolated KMD153 GDI diagnostic build

No lab deployment. Candidate0.7.153.1 SYS0C0DA4E1 is built and test-signed from
cleanc3499f1 on local branchg0/kmd153-gdi-contract, based on the recovered
KMD152 source checkpointa5b6a37. The original deployed SYS7729EF3E and its
package remain preserved. Main's later, undeployed recovery/fence-ledger
changes are excluded from this diagnostic candidate.

All158 runtime C/header inputs present in the archived152 tree compare equal
after line-ending normalization except wddm.c and bc250kmd_escape.h. Removing
the exact17-line bounded GDI log addition restores wddm.c byte-for-byte after
normalization; reversing the one version define restores the escape header.
The INF carries153.1 for the UMD-enabled package. Current build/test machinery
and warning gates are retained separately; these do not add runtime features.

Eight fast quality gates pass: KMD command export and prototype controls,
RADV and UMD prototype controls, blob ABI, surface control, scoped KMD analysis
and UMD analysis. The full KMD compiles/links, catalog signability reports no
errors/warnings, SYS/catalog signing passes. The source manifest verifies
unchanged clean committed inputs and marks the final package source-eligible.
This is not a runtime PASS receipt or proof of safe promotion.

Preserved intermediate builds are not candidates: the first was source-ineligible
because four old imported headers had inconsistent line endings, without content
changes; a later build still reported internal version152. The final build
normalizes those headers in its own commit and reports internal version153.
No intermediate package was deployed.

Next: preserve live baseline state and logs, perform a coordinated versioned
transition with recovery available, capture GDI type/phase witnesses and rerun
relevant display/paging/GPU controls. Enabling CddDwmInterop and engine Blt is
not part of this diagnostic change. G0 remains open.
