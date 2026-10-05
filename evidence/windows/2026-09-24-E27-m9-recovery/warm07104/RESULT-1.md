# M330 - Warm07104 start after shader workload unresolved

Same binary0.7.104.1, after accepted M329 shader/residency workload in boot01:46:31.
STOP clear, overlay notified; script verifies firmware, loadedfull104/deviceOK,
clock1000MHz/VID116/temp<85C, saves before.log and confirms prior startup guard.
One PnPdisable succeeds; PnPenable at02:14:59 reports success. Subsequent status
has not returned. Host session55183 remains live/SSHpid37248, no duplicate trial.
Original scratch/m9/warm07104-run.log remains the ongoing stream; observation-1
is an immutable partial copy. No Windows reboot was requested.

Full configured local /24TCP22 discovery finds one SSH listener and zero matching
pinned lab identities, so no configuration update. Owner monitor/mouse observation
requested, no reset yet. Read-only warm07104-recover.ps1 prepared for persisted
logs since02:14 plus before.log; NOT executed. Exact stop/start failure stage is
unknown. M328 first full startup and M329 contents remain accepted, but current
warm transition is unaccepted. Checkpoint instrumentation is not a proven fix.
