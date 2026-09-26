# Current07102 large residency acceptance

Hypothesis: four full GPU readbacks preserve the CPU pattern across three explicit eviction/pressure/restoration cycles on the deployed captured-paging implementation. Repeat the M257 exact probe (SHA E9566E495A4FB5BB0F37B8A2583AB10F573D0F9ECEE2B84D94E264242529E9C3), first64KiB positive control, then1GiB only if it passes. No GPU initialization, PnP action or Windows reboot. STOP/overlay,1000MHz/820mV,<85C; same-session driver and boot witnesses.

Native probe started directly, output redirected to new files; its process handle is retained and temperature monitored. Failure stops follow-up workloads. Collect native exit, all-range readbacks, residency/fence witnesses and driver counters; apply M257 independent validator. NOTRESIDENT does not identify the physical PFNs; this CP DMA probe does not establish shader cache policy or every dependency shape. Result pending.
