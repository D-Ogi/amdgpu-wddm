# M722 - diagnostic KMD170 retained on confirmed CPU desktop

Deploy002 runner c7181e9, manifest CD0C5696D72214687A6EFF2F4750CD2899673305EA91CCDE29D8F2343968D985. Exact169->170 transition completes98.8257185s with candidate_verified/retained=true, restored=false, logging_restored=true and closed. This intentionally retains170; it is not a rollback measurement.

Candidate source a3c2d2b, SYS67F0241506200D1053E6136ACDEA54F447BA945E65AA11C7EB250B8ECE195503, version0.7.170.1, ABI000700AA. Registered package oem135.inf. [Selected receipts](observations.json) retain the checked health acceptance and process-tree closure. No private device identifiers or raw registry graphs exported.

Independent04:51:00Z CPU preflight confirms170, UMD8279/ICDCF39, health15 generation74571373152/epoch5,67.1C. Same boot02:44:58.5Z and CPU DWM2440. Gates remain0, task removed, prior SetupAPI log setting restored. No reboot or AC cycle. Exact169 rollback remains in C:\BC250\m13\kmd170-deploy002\rollback169 and Driver Store. Earlier166 package is also preserved.

The diagnostic change separates four suppressed-VSync exits and resets their counters with device start. No170 GPU test has run. This establishes the CPU deployment baseline only; G0 is not closed. Raw receipts remain scratch/g0-hosted/kmd170-deploy002-ops.
