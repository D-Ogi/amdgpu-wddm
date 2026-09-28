# Startup identity receipts and ETW process control (M671)

Base24b97fe; this commit updates the not-yet-run DWM032 procedure and its manifest.
All candidate DWM PID/start identities are recorded before ambiguity is rejected.
The runner records the UTC restart boundary and previous PID list before stopping
DWM. Processes started before that boundary cannot satisfy the new-DWM witness.
Two new processes still fail; the gate has not been replaced with an arbitrary PID
choice. Five wrapper controls pass: ready, missing process retry, missing modules
retry, previous-process overlap and ambiguity rejected with a durable receipt.
Seven core deadline/identity/STOP controls continue to pass.

A provider file retains DxgKrnl and adds Kernel-Process keyword0x10 (process events).
Target21-file hashes, PS5 parsing, exact163 rollback presence and no-marker gate
restore pass12:50:44Z. No DWM032 trial launched. A separate short ETW control starts
and stops one hidden PowerShell process on the lab. Start/stop events match PID5332
and process sequence9351; exit0, same CPU DWM11616 before and after. Its ETW session
is stopped successfully. Private trace hash retained; only selected process fields
are published, not unrelated process/image information from the trace.

Preflight12:50:14Z exact164/8279AC7F/CF3948D6,health15/guard0,1000MHz/66.375C.
The observation control changes no display driver or DWM.032 still needs runtime
CreateDevice, image/content and no-full-frame-CPU-copy validation for G0.
