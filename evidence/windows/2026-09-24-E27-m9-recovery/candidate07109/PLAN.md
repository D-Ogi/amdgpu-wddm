# M348 - Candidate109 first-start and retirement control

Follow M347 procedure with exact SYS551B0BE13C4DA83AA8B9C3BA283F9320969607D9D541D3A6182E275CB145A019.
Install closed, preserve old state, normal shutdown and one verified8secondAC
cycle for a clean first-start control. Confirm guard before opening gates. Run
unchanged64KiB GPU residency probe and stop into display-only. Inspect all new
phase records and quarantine before one separate full warm startup attempt.
If stop or its evidence fails, do not proceed to full warm startup. If that start
hangs, preserve transport state, verify connectivity independently and recover
through the authorized plug; no automatic repeat of the same test. Telemetry
is auxiliary, never an OS/GPU liveness verdict. Record all output and native
probe exit/byte-oracle results. M347 is the exact source snapshot/build record.
