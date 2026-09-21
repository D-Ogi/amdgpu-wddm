E14, the clock half (experiments/E14-vulkan-compute-reference/dpm_sweep.sh), same session, same boot.

First attempt: power_dpm_force_performance_level low and high. amdgpu refused both on this part (EINVAL, "Failed to set
performance level" in dmesg), so that run measured the default clock only: the files named *-auto.txt.
Second attempt, through pp_od_clk_voltage: 1000 MHz at the voltage the part runs its default 1500 MHz at (906 mV asked,
899 mV read back), then "r" and "c" to give clock and voltage back to the firmware: *-1000.txt and *-default.txt.
No clock above the default and no voltage above the default was set.

  vkcompute-<point>.txt   the eight tests, 21 runs each; the hashes are the same at both clocks
  bench-<point>.txt       llama-bench, TinyLlama 1.1B Q4_0, all layers on the GPU
  sensors-<point>.txt     hwmon once a second while the work ran: PPT, edge temperature, sclk, vddgfx
Copied through redact.py, nothing edited by hand.
