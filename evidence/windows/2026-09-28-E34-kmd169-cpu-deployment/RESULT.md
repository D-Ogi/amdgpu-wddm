# M717: exact KMD169 retained on confirmed CPU desktop

Unit A, deploy001 runner94d3aef04bf9bdb616bfc0f49f63b40729e590cf, manifest8B8BF7F5CBB8B50E72026CE9E85946661D9AD17151F1B7DA2F432B7420DDE0CF. Deployment finishes97.5396062s, closed with candidate_verified=true, candidate_retained=true and restored=false. This intentionally retains169, not a rollback claim.

Registered DIF installs exact169, SYS AF715A5641E577E1D3023F22A38026D2E0189802F5E7146C7A868E9CD6D9D987, version0.7.169.1, ABI000700A9, source5985a4164fda6717a19c322f0ca3711a6bad7735. CPU DWM2652 retains exact UMD8279. Checked health confirmation returns flags15 at ready60412ms/completion age1211ms, generation43455941387/epoch5. Strict CPU gate/guard checks pass; prior SetupAPI log setting restored. Terminal task removed.

Independent final preflight03:59:22Z confirms exact169, CPU UMD8279/ICDCF39, health15,67.0C, same Windows boot and DWM. No reboot or AC cycle.166 remains staged/preserved for recovery;169 is now the CPU baseline. The old transition Capture admits166 only and must not be used blindly from this169 baseline.

This is deployment and CPU-health validation, not GPU desktop or VSync-fix proof. DWM048 is still unrun; G0 remains open. Selected raw receipts are unmodified; final-selected.json explicitly omits identifiers and raw driver/registry contents from the full local preflight. Full local receipts: scratch/g0-hosted/kmd169-deploy001-ops/receipts.tar.
