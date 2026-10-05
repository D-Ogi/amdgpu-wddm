# M693 - DWM041 bugcheck and retained VSync diagnostics

Unit A, 2026-09-27. Trial7cbcc6e, exact165/b83a594/SYS548E9D8D,
UMD49A44067/hostedICDC0CE5DCD. Preflight, package verification and hosted module
hashes passed. This is a failed trial, not G0 evidence.

Dump time20:27:17.139Z,0x116; new OS boot20:27:47.5Z. No normal done or watchdog
completion receipts. Task Scheduler Ready after a reboot is not successful
rollback evidence. Full958437049-byte dump preserved before explicit recovery,
then streamed to private workspace; SHA256
340CC3CA6C266B27098211938E59260CA1CA6A1190F5486DC5829A819972D53B.
Offline analysis uses exact165 PDB; no live debugger or power cycle.

Final driver summary: node0 submitted/completed335/335, node1 1814/1814,
zero hardware timeout/refusal counts. VSync armed,5331 ACKs; ISR7461,no-event2082,
unarmed48,no-MMIO0,flip-disabled0,read-failure0,ACK-failure0,deferred0.
The stale raw status00121904 comes from the last successful ISR read.

Using dump g_LogStart and the driver's millisecond summary timestamp gives last
ISR age about2.1713s; last ACK and returned notification about2.1893s (timing.json).
Samples are independent and rounded, not an atomic record. This locates a gap
before ISR entry but cannot distinguish stopped VUPDATE generation from lost
interrupt delivery. There is no MMIO observation inside the gap. ResetFromTimeout
refusal caused0x116; the underlying timeout cause remains unresolved.

Explicit recovery restored CPU UMD8279/ICDCF39 and experimental gates0. Inactive
trial tasks removed after crash preservation. Final20:34:30Z exact165 readback:
health15/guard0,native1000MHz/VID116,67C. No new GPU trial. M692 native-copy success
does not establish desktop stability.

Raw dump, receipts and images remain private at scratch/g0-hosted/dwm041-ops.
Selected driver counter lines are copied unchanged; unrelated identifiers and
metadata are omitted. No complete-render, no-copy or loss-free ETW claim is made.
