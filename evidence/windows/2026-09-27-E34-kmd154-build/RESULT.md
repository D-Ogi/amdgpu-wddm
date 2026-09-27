# M615: isolated KMD154 GPU Present candidate build

Baseline c3499f1b12dcc57902fdd522c3ad10498abc5620 is the exact deployed KMD153
source. Branch g0/kmd154-gpu-present, commit d7846ec, adds only the GPU copy/list
helpers, their tests and the gated GPU Present producer/consumer, plus revision154.
Across303 existing driver/third_party files, normalized-byte comparison finds
changes only in wddm.c, bc250kmd_escape.h and bc250kmd.inf. WDDM changes are the
exact patches from3fb72d3 and8f66f9c; no main-tree recovery changes were imported.
The source import manifest and baseline comparison are attached.

The first build ran under Windows PowerShell5: compile/link/signing succeeded,
but UMD INF generation failed because utf8NoBOM is unsupported there. Its log is
retained; that partial package is not eligible for deployment. The unchanged
sources were rebuilt under PowerShell7 in a fresh directory. Both plain and UMD
packages build/sign successfully and contain the identical SYS SHA256:
2D36B7C745C8FD0AF7A3D228A0BEF5BCFF82495945F60592960DD50C6950D2FF.
All11 quick gates pass, along with link, stack-budget and source/artifact identity
checks. Source is clean/eligible. Package-UMD reuses the retained153 stub DLL;
this is not a new desktop UMD or ICD.

The candidate remains local at scratch/g0-hosted/kmd154-final002/package-umd.
No lab deployment, registry change, gate enabling, reboot or interop promotion
occurred. KMD153 remains deployed. GPU Present admission, authoritative backing
identity for real CDD/shared opens, producer visibility, lifetime and actual
BGP1 completion still need validation. The new gate defaults off. Full G0 is
not established by this build or by the earlier standalone BC2S copy controls.
