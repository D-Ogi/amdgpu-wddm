# KMD 0.7.193.1 journal fill rate under the three presets

## 2. Journal ring fill rate

Measured on 193 itself (deployed 11:51Z) inside session 246 (Witcher 3 LOW, native 1080p, adapter107, Kaer Morhen
interior, walking): `journal-follow.ps1 -Tag 246` = `journal follow 30 250`, 12:03:58Z-12:04:28Z, raw file
`C:\BC250\tmp\journal-follow-246.txt` on the lab.

    journal follow: done, 272 reads, 12815 records printed, 0 lost to the ring, next 90570; 0 failed reads,
    1 adapter opens, 0 driver reloads; escapes: 273 without adapter synchronization, 0 with HardwareAccess
    kinds: gfx-submit 8202, update 2540, destroy 230

- 427 records/s in total, 273/s of them gfx-submit, 85/s update, 8/s destroy. 0 lost at a 250 ms poll.
- The 1024-record ring covers 1024 / 427 = 2.4 s at this load: above the 2 s the design asks for, but only by 20 %.
  245's pre-hang burst (480 submits/s plus the other kinds) would be ~1.6 s. The 2048 question stays open with a
  lean towards 2048; decide after a HIGH and a HIGH+RT session give their rates.
- gfx-submit records carry `ctx` equal to `alloc` in every printed line (the context pointer is printed twice);
  harmless, but the printer could drop one.

Same measurement at HIGH and HIGH+RT (registered triplet adapter107, KMD 193, DPM 2000):

| session | preset | records/s | gfx-submit/s | update/s | destroy/s | lost | reads in 30 s | ring 1024 covers |
|---|---|---|---|---|---|---|---|---|
| 246 | LOW | 427 | 273 | 85 | 8 | 0 | 272 | 2.4 s |
| 249 | HIGH | 207 | 190 | 13 | 0.4 | 0 | 125 | 4.9 s |
| 250 | HIGH+RT | 142 | 101 | 27 | 2.5 | 0 | 129 | 7.2 s |

Decision: LOW is the worst case and the only one under 3 s; a 245-like burst (480 submits/s plus the rest) would be
~1.6 s. Take the ring to 2048 records in the next KMD that is built for another reason (no deploy only for this).
Reads: 120 is nominal for 30 s at a 250 ms poll; HIGH and RT are near it (125, 129), LOW's 272 means the loop
needed extra reads per poll to drain the higher record rate. No loss in any of the three.
