# M708 - DWM047 combined-client bugcheck and recovery

Runnerf22ce7f, exact166/UMDd7948d8e/C0CE. Hosted DWM11056 and native GPU
client5448 ran together. The owner observed correct desktop/animation without
glitches. Last retained client log contains successful Present774; DWM process
sample2 is at22.527s with68.1C. Three checkpoint receipts survive. These are
last preserved observations, not an exact failure time or a completed trial.

SSH stopped answering; Windows restarted itself after0x116, Arg4=3, with new
boot00:35:43.5Z September28. No normal done/restored/client-done receipts existed.
The922189881-byte dump is preserved on lab and host with matching SHA256.
Interrupted ETL and original logs are preserved; no trace-loss-free claim applies.
No agent-initiated power cycle occurred. The timeout cause is not established.
The successful visual observation does not establish stability or G0 acceptance.

After preservation, explicit recovery restored baseline8279/CF39 and gates0.
Fresh00:41:12Z preflight confirms exact166,health15,1000MHz/VID116,67C.
Recovery cleanup00:42:23Z removed only this trial's terminal tasks; CPU DWM8440.
No active GPU test. Original terminal receipts were not fabricated: recovery
has a separate receipt. Raw material: scratch/g0-hosted/dwm047-ops.
Export contains reduced results and hashes; raw memory stays outside the repo.
