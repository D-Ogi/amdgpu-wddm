E08 run 001, 2026-09-21, unit A, Windows 11 Pro 22631.3880, bc250kmd 0.4.2.0, witness for the register sweeps:
bc250rd with the 5355-offset allow-list. No kernel debugger attached. Driven over SSH with
experiments/E08-vram-by-physical-address/e08_target.ps1 (and the E05 script for the sweeps). File name suffix
is the target's local time.

  install-x-090816.txt            package installed, all four gates closed by the INF, stage 61
  probe-closed-090830.txt         a script bug, no hardware access: the helper was named "Cli", which PowerShell
                                  resolves to its alias of Clear-Item; only the lines that do not use it ran
  probe-closed-090845.txt         H1: framebuffer at 0xC0000000 + 0x8CA000 reported, no VRAM, every memory command
                                  refused with STATUS_DEVICE_NOT_READY
  gate-x-090857.txt               EnableMmio = 1, EnableVram = 1, device restarted
  probe-read-090908.txt           H2: VRAM 0x270000000 + 0x200000000, MC base 0xF400000000, BAR0 0xC0000000 + 0x10000000
                                  H3: 3 x 64 framebuffer words and 16 words of the test page identical through
                                  the physical path and through BAR0, all nonzero
                                  H4: top 2 MB readable through the physical path; reads between the windows
                                  refused (STATUS_ACCESS_DENIED), beyond BAR0 through bar0 refused, writes refused
  gate-x-090926.txt               EnableVramWrite = 1, device restarted
  sweep-GC-before / before2       noise floor
  write-x-091041.txt              H5: words written through one path read back through the other, both directions;
                                  writes outside the test page refused; original values restored through the
                                  physical path. The last line reads the restored words through BAR0 and gets
                                  the values last written through BAR0: a stale view
  sweep-GC-after-091046.log, comparison.txt
                                  H6: 6 registers differ, all of them in the noise set
  hdp-stale-091140.txt            follow-up a minute later (scratch script, three rounds 5 s apart): the physical
                                  path shows the restored values; BAR0 still shows the stale ones in round 1 and
                                  the right ones from round 2 on
  gate-x-091211.txt, probe-closed-again-091222.txt
                                  all gates closed, device restarted, memory commands refused again

All logs are UTF-16 (Windows PowerShell 5.1 Tee-Object and redirect). Untouched since capture.
