# Renamed native module set passes errors003

UMD113729d8/C064, engine01897898/11B0, ICDC388 and exact-pair configuration18ECA577
from caps006/M755. Files use amdgpu_wddm_d3d11.dll/config, amdgpu_wddm_dxvk.dll
and amdgpu_wddm_radv.dll. Frozen old packages remain unchanged. Client113729d8/E6E3
retains M754's oracle, updating only the required exact sibling module names.

CPU and GPU pass all three independent-device controls. GPU creation returns
E_OUTOFMEMORY/null, then recovers. WRITE_DISCARD has runtime fallback with sticky
0x887A0005; staging READ after removal returns0x887A0005/null. Update exposes sticky
removal. Exact module-path checks pass. Unused per-case read_hr/recovery fields are
E_FAIL sentinels, not executed calls. Full results and pinned file hashes are included.

Supervisor52.5358219s; CPU171 restoration/postflight/tree closure verified. Cleanup
and subsequent Inspect confirm task Missing. No OS/DWM restart or driver update;
lab free. Raw scratch/m14/errors003-ops. Only selected DDI logs exported without
raw addresses. This is native loader/error-path validation, not a new render/Present
content test or D3D12/FL12_1 acceptance. KMD/service rename remains separate.
