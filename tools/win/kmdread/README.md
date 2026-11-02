# kmdread - KMD readings without a process per reading

`kmd-reader.ps1` reads the clock, the start-health line and the driver log ring of the installed driver in the
PowerShell process that calls it. It loads the release `bc250control.dll` and calls its exports `Bc250ClockControl`
(READ), `Bc250StartHealth` (READ) and `Bc250LogRead`. These are the same escapes and the same exported code that
`bc250kmd_cli clock read`, `bc250kmd_cli health read` and `bc250kmd_cli log <from>` use, and the text is the same. No
process starts for a reading.

## Why

A polling loop that starts `bc250kmd_cli` (or another tool) once a second is one half of the BD-051 sshd accept
stall. While such a loop runs inside an ssh session or beside it, sshd completes the TCP handshake of new
connections and sends no banner, for minutes (trials 152, 154 and 220, game sessions 424 and 427). The sshd watch of
`tools/win/lab-emerg` heals the stall. The lab tools that poll use this reader, so the trigger goes away.
`tools/quality/spawn_loop_lint.py` refuses a new polling loop that starts a process.

## Use

```powershell
. C:\path\to\kmd-reader.ps1
$null = Initialize-KmdReader        # once per process; Add-Type compiles the class once (one csc.exe start)
$c = Read-KmdClock                  # .Ok .Line .Reason .MHz .Vid .TemperatureMc .Ready
$h = Read-KmdHealth                 # the line of "bc250kmd_cli health read", or $null
$t = Read-KmdLog -From 1234         # the text of "bc250kmd_cli log 1234", or $null
Get-KmdReaderLoss                   # why the last reading was lost
```

- **Bounded readings.** One resident worker thread sends each escape, and the caller waits for it at most `-TimeoutMs`
  (default 2000). If no answer comes in time, the reading is a loss. While a hung reading stays out, the next reading
  is a loss at once and sends nothing. So a hung escape never piles up threads or escapes.
- **The DLL.** The reader takes `-Dll`, else `$env:BC250_KMD_READER_DLL`, else the installed release
  (`HKLM\SOFTWARE\amdgpu-wddm\Release` InstallRoot): `tools\bc250control.dll` beside `bc250kmd_cli.exe`, else
  `control\bc250control.dll`. The process holds the DLL loaded until it ends, as the overlay does.
- **What stays a process.** `log summary` is a HardwareAccess escape, and the DLL does not export it. A sampler sends
  it once at its start with the CLI, never from its loop.

## Tests

`test_kmdread.py` builds `test/fake_driver.c` with `test/build-fake.ps1`. The fake includes the unchanged
`bc250kmd_cli.c` and replaces SetupAPI and the D3DKMT calls, so no adapter opens. The build gives a `fake_cli.exe` and
a fake `bc250control.dll`. The tests run under Windows PowerShell 5.1 and PowerShell 7 and check four things:

- The reader's text is the same as the CLI's text, byte for byte, for the clock, the health line, log pages and a
  refusal.
- A hung escape is a loss inside its bound, and no second escape goes out while it is out.
- Eighty readings start no process. A job object counts the processes of the shell, and a positive control (one CLI
  start) shows that the count works.
- A DLL that is missing or that has no such exports is refused.

```
python -B -m unittest discover -s tools/win/kmdread -p "test_*.py"
```

The `kmdread` check of `tools/quality/quick.ps1` runs them.
