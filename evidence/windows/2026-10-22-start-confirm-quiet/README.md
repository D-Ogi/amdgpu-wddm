# Start confirmation without the start window: the QUIET witness (2026-10-22)

Unit A, Windows 11 22631, the tester.32 candidate package (0.7.216.104-tester.32, release flavour) with one file
changed: the kernel driver `bc250kmd.sys` of train/b35 plus the QUIET change, SHA256
`6A982C80BACE47608CDE084BB88C09F9CC7F4089E382E410AEEEBFA0C164FEE3`, put in place with
`scratch/kmd-lab-kit/swap-sys.ps1`. The three start scripts of the same working tree replaced the installed ones.

## What was measured

1. `idle-cadence.txt`: on the candidate kernel driver before the change (D754962F), an idle desktop completes one
   flip each minute, at the minute change of the taskbar clock. The CONFIRM rule before this change needs a
   completion in the last 15 s at an age of the start of 60 s or more. An idle desktop meets that rule only by
   chance. The blue start window met it because it drew a countdown every second.
2. `start-confirm.log`, the lines from 02:38:03Z: the logon with the changed scripts. No window opened ("window:
   none"). The start was confirmed at 02:38:55Z with flags 31. That confirmation was a fresh one (age_ms 51),
   because the lab overlay `bc250mon` draws its panel every few seconds. So this logon does not test the new rule.
3. `c85-kill2.log`: the test of the new rule. The script suspended `bc250mon` (no drawing, no confirmation of its
   own), restarted the GPU device with `pnputil /restart-device`, and waited. The new start had 9 completions in
   78 s. At `ready_ms` 78390 and `age_ms` 17583 the reading was flags 23: REQUIRED (7) and QUIET (16), not
   confirmed. `bc250kmd_cli health confirm` then returned flags 31 (confirmed). The last picture was 17.6 s old, so
   the 15 s freshness rule alone refuses this reading. The script resumed `bc250mon` after the test, and the device
   was OK.

## Readings in busy periods

While the desktop draws, the QUIET flag is clear in a reading that falls between a programmed flip and its
completion (flags 15 in the logon readings and in the first form of the test). This is the intended behaviour:
QUIET means that no programmed flip is waiting for its completion.

## Files

| File | What it is |
|---|---|
| `idle-cadence.txt` | the idle readings of item 1 |
| `start-confirm.log` | the start confirmation log of unit A, two boots (01:35Z without the change, 02:37Z with it) |
| `c85-kill2.log` | the lab-side log of the test of item 3 |
| `scripts/health-idle.ps1` | the idle reader of item 1 |
| `scripts/c85-kill2.ps1` | the test of item 3 (170 s bound) |
| `scripts/c85-read.ps1` | the read-only state reader used after each step |
| `sha256.txt` | the hashes of these files |
