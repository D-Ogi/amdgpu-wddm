# M759: RT-fixed ICD non-RT regression

Engine002 runs the frozen standalone D3D12 engine from M757 against exact ICD949669FF containing the RT node-address fix. GPU copy/readback and DXIL compute each match all16384 words. Independent device creation/release and ABI negatives pass. Exact in-process ICD hash witness matches the staged artifact. Full hashes in receipt.json.

Engine7.728s; supervisor25.207s,67C. Exit0, empty Job, no timeout/STOP. CPU171 pre/postflight matches boot, DWM, driver hashes, registration, generation, epoch and health flags. No system deployment/restart.

Reported FL11_1/tiled tier0/SM6.8/RT1.1 remain capability queries. This is a non-RT regression; no traversal, native DDI, FL12_1 or game RT acceptance. Next are bounded ray-query and RT-pipeline controls. Raw logs remain scratch/m15/lab-engine002; selected assertion/capability/result lines omit LUID, addresses, handles and process identifiers.
