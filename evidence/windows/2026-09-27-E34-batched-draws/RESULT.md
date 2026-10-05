# M572: batched draw controls do not reproduce BD-043

Runs083 (WARP),084 (CPU UMD) and085 (hosted GPU) pass all three added cases:
256 scissored draws with constant-buffer updates,256 draws with DISCARD vertex
and constant buffers, and256 indexed draws with DISCARD buffers and nonzero
start/base offsets. Each group checks4096 final pixels exactly; all hashes are
`02e630a05dd4a325` on all three renderers. The original eight M567 checks also
pass. No explicit per-draw query wait is issued inside the new groups.

The result does not reproduce the photographed desktop corruption. It narrows
the tested combinations, not every use of those mechanisms. Only the final
image after four overwriting passes is checked; earlier transient images are
not separately captured. The groups use solid colors, one constant-buffer slot,
and simple vertex layout. Changing sampled textures, varying vertex attributes,
cross-device resources, Present and DWM remain outside the added coverage.

The exact control EXE is732DD8DC3866580F47125B52AF82128F0CE8467D8D2570174C14778D4234B43C,
built with `/W4 /WX /O2 /MT`. UMD23F5269C/hosted ICD3508416F are unchanged from
the failing desktop trial. Process witnesses are WARP1376, CPU7736 and GPU4500.
Source and binary hashes, commands, raw output and completion records are retained.

GPU run085 increases global node0 submitted/completed by23/23, with no added
timeout/refusal. Devices increase2/2, contexts4/4 and processes1/1; live objects
remain192. These are global interval counters, not per-draw attribution or a
general proof of no leaks. GetDeviceRemovedReason remains successful after
resource release. The registered ICD93B1D1FD and UMD8279AC7F are restored and
hash-checked; CPU DWM5748 remains unchanged. Preflight Tctl was66.5 C, STOP clear.

BD-043 and G0 remain open. The next isolation control must exercise batched
textured geometry and resource changes, rather than interpreting these passes
as proof that the photographed defect is fixed.
