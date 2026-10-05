# R16 indexed list isolation

After M585's persistent desktop triangles, extend the state-transition matrix
with an R16_UINT indexed triangle list. Use four vertices at BaseVertex3,
six indices after two sentinel entries,4-byte IA index-buffer binding offset,
nonzero StartIndexLocation, disjoint NO_OVERWRITE ranges and DISCARD between
passes. The existing seven-case control now checks56 images/229376 pixels
and3584 draws across the two Flush patterns, plus the original graphics cases.

Hypothesis: the R16 list/binding-offset path reproduces the corruption that
the existing R32 strip case did not. Expected pixels and per-pass hashes
remain identical to the tile-color oracle. WARP105 and CPU106 precede GPU107
on exact EXE D569BAA6. GPU uses unchanged UMD67E4C9F5/ICD3508416F; CPU DWM
remains active and baseline DLL hashes are verified/restored by each runner.
A pass narrows this sequence only and does not resolve M585 or BD-043.
