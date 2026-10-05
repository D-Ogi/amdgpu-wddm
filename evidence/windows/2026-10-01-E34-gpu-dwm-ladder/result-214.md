## Result

00:49-01:06Z, **functional-restored** (elapsed 395 s, tree closed, baseline restored), **no 0x116**. Game pid 11712
launched 00:51:34Z, menu transition at 145 s, world (the balcony) at 00:55:01Z, route-184 with the extra walk sent
00:55:17-01:00:05Z (the last four commands after the 420 s bound ended the game at about 00:58:34Z). The owner
played by hand alongside, "much more steps, moves and actions than the automatic route", and saw no glitches;
the shots (002, 004-007) are correct. Window B (00:55:48Z, 40 s): game 21.1/s, median 48.1 ms, p99 75 ms, 0 > 250
ms (not comparable to 190/209: non-LTO no-copy ICD with the diagnostic, owner input changed the scene).
UAF log (post\uaf-11712.log, 78 lines, two device headers): 180 32-bit BOs quarantined and released (early 0;
logged classes cpu-upload 65, application-or-zero 3, shader-ring-cs 2), 1930 tombstones (overflow 0), **gate_refs 0,
gate_bos 0** up to 16384 submits (00:56:04Z, the last power-of-two summary; no finish line for device 2, the game
ended at its bound). So the gate never saw a destroyed BO named by a submission: either no recurrence (201
survived the same stack once) or an address-only reference that the quarantine protected silently. Not a
decider for the owner of K51. Structural finding (source, not the run): radv_wddm2_bo_destroy evicts, frees the GPU
VA and destroys the allocation immediately with no wait for submitted work (radv_wddm2_bo.c:1186-1240), where the
Linux kernel would keep the BO and its mapping until the jobs that used it retire; next is a retire-deferred
destroy for all BOs with an in-flight-destroy witness (in progress).
