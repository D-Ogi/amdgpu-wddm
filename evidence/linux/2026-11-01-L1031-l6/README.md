# L1031 L6: two s2idle attempts without amdgpu, no return over the network

Date: 2026-10-31, about 23:33 to 23:48 UTC. Unit A, the diagnostic stick's Linux (Alpine, kernel
`6.18.52-0-lts`), GRUB entry `bc250.mode=network` with `modprobe.blacklist=amdgpu`. amdgpu was not
loaded in either attempt. Wishlist row: L6 (suspend and resume). Earlier result: M56.

## The clock

The Linux clock of unit A read local time (UTC+1) and was about 28 s ahead. The UTC times here come
from the development PC: the stream prefixes of the kernel log and the times of the console output.
The kernel sets the system clock from the RTC at 0.58 s after boot. That stamp, minus 1 h and about
28 s, gives the boot times below. The start lines of the two attempts agree with this offset
(`out/console-pm-state.txt`).

## Method

Before attempt 1, `/sys/power/mem_sleep` read `[s2idle]` and `/sys/power/pm_test` did not exist
(`out/console-pm-state.txt`). So the `freezer` and `devices` test levels of the script were not
available, and only a real s2idle ran.

`scripts/l6-s2idle.sh s2idle none` saves the power states, the ACPI wake sources and the kernel log.
It then writes `+20` to `/sys/class/rtc/rtc0/wakealarm` and `freeze` to `/sys/power/state`. After a
return it reads the wake IRQ and the kernel log. The script writes its own log `steps.txt` on the lab.
That log is not in this directory.

The script in `scripts/` is the revision of attempt 2. Before attempt 2 it got the console markers on
`tty1`, the RTC reads and the kernel console level 7. Attempt 1 ran the revision without these
additions. The two revisions do the same s2idle step.

The development PC streamed `/dev/kmsg` of the lab over SSH (`out/dmesg-stream-*.txt`). It also read
the smart plug of unit A (`out/console-power.txt`).

## Results

| Step | UTC | Data |
|---|---|---|
| boot of attempt 1 | about 23:33:30 | RTC stamp `00:33:58` in the kernel log |
| attempt 1, stream | from 23:35:23 | boot log to 21.7 s of uptime |
| attempt 1, start | 23:35:41.6 | the script started (`started 00:36:10` on the lab clock) |
| attempt 1, stream end | 23:35:58.9 | SSH timeout, no line after the boot log |
| plug | 23:36:36, 23:41:19 | 107.1 W, 107.5 W |
| AC off and on | 23:41:29 to 23:41:42 | relay read back off, then on |
| boot of attempt 2 | about 23:42:05 | RTC stamp `00:42:33` |
| plug, idle | 23:42:44 to 23:43:06 | 110.5 W to 111.0 W |
| attempt 2, start | 23:43:23.7 | `started 00:43:52` on the lab clock |
| attempt 2, stream | 23:43:23.9 | `PM: suspend entry (s2idle)` at 80.52 s of uptime, then `Filesystems sync: 0.000 seconds` |
| attempt 2, stream end | 23:43:43.9 | SSH timeout, no line after the sync line |
| plug | 23:44:23 to 23:44:40 | 107.7 W to 107.8 W |
| AC off and on | 23:48:10 to 23:48:23 | the next boot is Boot A of the campaign |

- The first stream command of attempt 1 (`dmesg -w`) failed, because BusyBox `dmesg` has no `-w`
  option. The stream that follows it reads `/dev/kmsg` with `dd`. Both are in
  `dmesg-stream-s2idle-none.txt`. The stream of attempt 1 holds no line of the suspend.
- `probe-after.txt` and `probe-after-2.txt` hold the last SSH probes after each attempt: "Connection
  timed out".
- No SSH connection came back in about 5 min after attempt 1 and about 4.5 min after attempt 2. The
  script calls a step hung when SSH is absent for 120 s after the expected wake.
- The kernel log of both boots has `rtc_cmos PNP0B00:00: error -ENXIO: IRQ index 0 not found` and
  `rtc_cmos PNP0B00:00: no alarms`. This agrees with M56: the RTC has no alarm. So the RTC wake that the
  script requests had no alarm to use.
- During both attempts the wall power stayed at 107 W to 108 W, about 3 W below the idle reading before
  attempt 2. The plug's readings are not calibrated for this model and their age is unknown.
- Lab camera frames of the screen during both attempts, not kept, had a static text console on the
  firmware framebuffer.

## What this answers

Attempt 2 entered s2idle. The stream of attempt 1 received no line after the script started, so it
does not tell how far that attempt got. Neither attempt came back over SSH. The data do not tell a missing wake from a hung resume. The missing RTC
alarm agrees with a missing wake, but it does not prove that the board did not resume. amdgpu was not
loaded, so no GPU result after a wake exists. The GPU part of L6 needs a wake by the power button, as
in M56, with a person at the lab.

## Files

- `out/dmesg-stream-s2idle-none.txt`: attempt 1, the failed `dmesg -w` and the `dd` stream.
- `out/dmesg-stream-s2idle-none-2.txt`: attempt 2.
- `out/probe-after.txt`, `out/probe-after-2.txt`: the SSH probes after each attempt.
- `out/console-pm-state.txt`: three console blocks from the session tool log: `/sys/power` before
  attempt 1 and the start lines of both attempts.
- `out/console-power.txt`: six console blocks of plug telemetry and the two AC cycles.
- `scripts/l6-s2idle.sh`: the step script, revision of attempt 2.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. The stream lines carry `<lab address>` and
`<MAC removed>` in place of the removed values. `sha256.txt` holds the SHA-256 of every file in this
directory except itself.
