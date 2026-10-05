# Renamed engine pair admission: caps006

Engine01897898/11B0 and test3E39, frozen set from engine-artifacts/01897898-11B0AC36,
run with ICDC388 under sibling name amdgpu_wddm_radv.dll. Exact hashes and in-process
ICD witness are in caps.json. The unchanged suite reports0 failures, including
injected OOM/recovery and Trim:80 ->588 ->588 ->17MiB. Engine10.508s,
supervisor29.069s; Job empty, root exit0, no timeout, CPU171 postflight unchanged,67C.
No UMD/driver registration or DWM changes. This standalone control is not native DDI.

Generated configuration amdgpu_wddm_d3d11.config is108bytes, SHA256
18ECA57740FBA1A23E3D8D1CD7E14FBC5D26DE9AFA32BF91F06CDD5EE7B5B8C4.
The accepted feature level remains11_1. Renaming does not establish D3D12 or FL12_1.
Raw records remain scratch/m14/lab-caps006; selected output lines are verbatim,
closure omits the process identifier. Frozen old DC65/C388 artifacts remain intact.
