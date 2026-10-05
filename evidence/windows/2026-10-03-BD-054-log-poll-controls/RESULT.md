# BD-054 log poll: the host controls, 2026-10-03

Date: 2026-10-03, repeated on 2026-10-05 against the landing branches. Machine: the development PC. No lab unit
and no GPU take part: both tests run against a fake driver.

## What BD-054 was

The overlay polled `bc250kmd_cli log summary` every 5 s. The CLI at `C:\BC250\kmd\` was older than KMD
0.7.184.1 and sent all 16 pages of the log ring as HardwareAccess escapes. Each one is a Level Two call, so
dxgkrnl idles the GPU for it: trial 317 measured 280-420 ms of game lock waits per poll and a 2 % frame-rate
cost.

## The CLI side: log_test and its mutation controls

`log_test.c` compiles the tool against a fake driver that answers as `display.c`'s `LogEscape` does, and checks
which escapes each form of `log` sends, the lines it prints and the count line. `build.ps1` runs it before it
compiles the tool.

A test nobody has tried to break is a guess, so `mutate-log-test.ps1` builds four mutants of the tool and one
unchanged copy. Each mutant must fail.

| Build | Exit | log_test | FAIL lines |
|---|---|---|---|
| unchanged | 0 | 19 checks, 0 failures | 0 |
| cli196-original (the CLI the lab ran, 28b89a4c) | 1 | 19 checks, 12 failures | 12 |
| pages-hardware-access (pages through SendEscape) | 1 | 19 checks, 8 failures | 8 |
| no-sentinel-fixup | 1 | 19 checks, 8 failures | 8 |
| no-count-line | 1 | 19 checks, 4 failures | 4 |

`mutate-log-test.txt` is that run. `log_test.txt` is the unchanged build's own output, with the three usage
refusals that keep `only` a keyword and `-1` or `4294967295` a bad number.

## The overlay side: GraphicsSummaryTest

The panel now runs `log summary only` and reads the CLI's count line: more than one Level Two escape, or no
count line at all, is a warning, and a CLI that does not know `only` is named as too old.

| Build | Result |
|---|---|
| the new provider | `graphics summary pause: 21 checks, 0 failures` (`graphics-summary.txt`) |
| the previous provider of main | fails, `unexpected subprocess` in `KmdInfoProvider.Run` (`graphics-summary-control.txt`) |

The control is what makes the 21 checks worth reading: the same test against the provider that polled
`log summary` dies on the arguments it does not expect.

## Scope

These are host controls. What they do not show is the lab: the frame-rate cost of the poll was measured in
trial 317 and is recorded with BD-054 in the defect list, and removing the
`C:\BC250\mon\graphics-summary.pause` workaround from the lab is an overlay update, not a trial.
