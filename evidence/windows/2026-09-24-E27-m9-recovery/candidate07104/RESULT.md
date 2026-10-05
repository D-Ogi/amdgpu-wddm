# M328 - Candidate07104 full startup and context reservation

Unit A,2026-09-24. Version0.7.104.1 installed with exact SYS
9EE1AA001125B414DC7CBB0382ABF6838427223D8130F1A89505D402D32B69CF.
Package25checks pass,0errors/0warnings/14notes. M327 traced/default replay applies.
Prestate display-only07103, boot01:46:31,1000MHz/VID116,66.4C, no known probes.
STOP clear, overlay notified. Closed-gate install exits0/deviceOK/exacthash.
ONE full start, session80358 terminal0, sameboot; no Windows reboot.

CP1 scheduler-read0.221s, scheduler-write0.221s, scheduler-done0.223s;
both engines ready0.251s. Context reservation ready22020392bytes at0.257s.
Paging298submitted/298completed,0timeouts/0refused, noTDR. Recovery acquisition
terminal0 and persisted startup snapshots copied in the raw acquisition log.
Reservation creation is witnessed; no reserved-transfer use line yet, so allocation-
free transfer runtime acceptance remains missing. No inference/shader run on07104
in this step. This is first full startup in the recovered boot, not a repeated
start after workload; M32507103 warm failure is not resolved by this result.
No causal claim that diagnostic timing fixed hardware.
