# monitors - development-PC watchers and the game-start alarm

These scripts run on the development PC and not on the BC-250. They tell the operator what a lab trial does.
They read local files and they play one sound. They touch no hardware, and they open no connection to the
target.

| File | What it is |
|---|---|
| `alarm.ps1` | One Windows alarm sound at the start of a game trial |
| `watch-trial-log.ps1` | The new key lines of a trial log, one line for each event |
| `watch-dir.ps1` | One line for each poll that finds a new or a changed file in a directory |
| `selftest.ps1` | Offline checks of the three scripts above |

## The game-start alarm

The owner asked for an audible signal at every game trial start on 2026-09-29. A game session runs for up to
20 minutes and it heats the unit, so the owner watches the lab screen and the case during that time. The alarm
makes the start of the session impossible to miss.

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\win\monitors\alarm.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\win\monitors\alarm.ps1 -Times 3
```

Each call plays the sound for a few seconds and then exits. It opens no window, and it leaves no resident
process. `-Times` accepts 1 to 3. `-Sound` names a different sound file. The default file is
`Media\Alarm01.wav` in the Windows directory. If that file is missing, the script makes three console beeps
instead. Start the script in the background, because the sound plays to the end before the call returns.

## Watching a trial log

`watch-trial-log.ps1` reads a growing trial log and prints only the lines that matter to an operator. These
are the step markers, the failures, the STOP brake, the trace lines, the status and state lines, and the
closure markers. The script stops at a closure marker or after `-Minutes` minutes.

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\win\monitors\watch-trial-log.ps1 `
  -Log $env:BC250_ROOT\scratch\m15\native-caps001\trial-262.log -Minutes 7
```

The script prints one line for each event and it flushes after each poll. A watcher that reads standard
output therefore reports each line as it arrives. `-IntervalSeconds` sets the poll period, which is 5 seconds
by default. `-Width` cuts long lines, which is 220 characters by default.

## Watching a directory

`watch-dir.ps1` is the same event source for a directory. It prints one line for each poll that finds new or
changed files, and it names those files. Use it when work arrives as files, for example in a coordination
message directory outside this repository.

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\win\monitors\watch-dir.ps1 `
  -Path $env:BC250_ROOT\scratch\queue -Filter *.md
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\win\monitors\watch-dir.ps1 `
  -Path $env:BC250_ROOT\scratch\queue -Filter *.md -Minutes 30 -ExitOnChange
```

`-Minutes 0`, which is the default, runs until the operator stops the process. A positive `-Minutes` stops the script
at that deadline and prints `no change in N min` if nothing changed. `-ExitOnChange` exits with code 0 after
the first line, which makes the script a wait and not a stream. `-Label` changes the word in front of the
file names. The script exits with code 2 if the directory does not exist. It never reports a file twice for
the same write time and length.

Give the watched directory on the command line, because these directories live outside this repository.
`BC250_ROOT` is the workspace root, which is the parent directory of this repository by default.

## The self-test

`selftest.ps1` runs the two watchers against temporary files and checks each line that they print. It needs
no lab, no network and no administrator. It makes no sound, because it runs `alarm.ps1` only in the case that
fails the parameter check before the player runs. The run takes about two minutes, because two checks wait
for a one-minute deadline.

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\win\monitors\selftest.ps1
```

The script prints one `PASS` or `FAIL` line for each check. It exits with code 0 if every check passed, and
with code 1 if one failed.

## The operator copy

The game trial scripts in the workspace start the alarm by its path under `<BC250_ROOT>\scratch\monitors\`.
That copy stays in place, so those scripts keep working. This directory holds the source of record. Change
the file here first, and then copy it to the operator directory.
