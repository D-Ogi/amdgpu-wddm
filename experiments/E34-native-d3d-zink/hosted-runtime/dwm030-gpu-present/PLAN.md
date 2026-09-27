# DWM030 - distinguish delayed readiness from DWM adapter rejection

Exact1649B9B99D3, unchanged UMD5C74BF98/direct ICD3508416F, same kernel/capability gates. Registered baseline nowCF3948D6 per the measured promotion; preserve it on rollback. DWM029 observed only the initial enumeration phase at5.2s, a phase also present before successful device creation in027/023. This does not prove timing was the cause.

Add a separate30-second bounded readiness phase after DWM restart. Require one stable new PID/start identity, router+UMD+hosted ICD hashes and successful DWM CreateDevice before starting the180-second measured animation. Preserve durable per-attempt receipts. Missing ICD or missing CreateDevice never passes; process replacement/loss, STOP and deadline fail. A delayed positive result would support initialization timing; deadline failure would refute that explanation within this budget and preserve deeper diagnostics.

CPU pixel baseline, BGP1 checks,300-second watchdog,270-second control lifetime and durable rollback unchanged. No credited GPU measurement during readiness. Full G0 still requires correct BGP1/DWM images, GPU work owned by dwm.exe and exclusion of whole-frame CPU copies.
