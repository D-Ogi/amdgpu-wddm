# M389 - Reset scope and physical engine attribution

Decode the preserved E29 reset window through tools/regcalc and original AMD headers. Compare stop/reset/restore and RLC policy with Windows119. Preserve exact6.18.52 RLC/context sources. Read live scheduler mask and per-engine fence counters; if the two SDMA gfx queues and mask3 are verified, restrict a new positive control to engine1 using source-derived scheduler mask2, restore mask in a trap and verify fence progress. No further timeout or reset. This closes the ambiguity of client ring0 versus physical engine1, not warm reload.
