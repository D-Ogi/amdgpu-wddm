## Result

01:22-01:34Z, **functional-restored** (elapsed 392 s), **T6 passed**. Route gpu 01:22:27Z, game launched 01:25:19Z,
menu transition at 135 s, world 01:28:57Z (shot-001 the bedroom), route-184 with the extra walk; route cpu exit 0
at 01:34:43Z. Shots 001-011 correct (bedroom, door, balcony with the valley, back inside), no glitch.
**Window B (01:29:20Z, 40 s): game 35.5/s, median 26.7 ms, p95 40.9 ms, p99 62.4 ms, 0 > 100 ms** against 214
(CPU DWM, same engine/ICD/preset/KMD) 21.1/s, median 48.1 ms: +68 % frames. DPM: **raises 120, 2000 MHz / 1000
mV at busy 69-77 %** (214: 0 raises, 1000 MHz, busy 31-46 %), Tctl up to 79.0 C, no throttle. KMD: 0 VM faults, GPU
Present calls 110 refused 0, blit gate open 0 blits. Counters (offgpu\gpuctr-217.txt, world window 01:28:40-
01:32:20Z, 160 samples): BC-250 3D avg 61.7 % max 94.7 % (attributed to the game pid 5308), dedicated up to 2820
MB, DWM 8.6 % of one CPU, machine CPU 55.4 %. So Task Manager does see the game's GPU work. UAF log
(post\uaf-5308.log): 144 quarantined, 3265 tombstones, gate_refs 0 to 32768 submits.
