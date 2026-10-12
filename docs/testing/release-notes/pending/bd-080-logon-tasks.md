# Pending release-notes lines: the start confirmation runs at any logon (BD-080)

This file is not a release. It holds the tester-facing lines for the next tester release notes. The release
step copies the lines into `docs/testing/release-notes/<version>-tester.N.md` and deletes this file.

## Fixed

- An account without administrator rights kept the driver. Until now the task that confirms a driver start ran
  only at the logon of an administrator. On a computer whose user has a standard account nothing confirmed a
  start, and after two restarts Windows went back to Microsoft Basic Display Adapter, with no way for that user
  to bring the driver back. The confirmation now runs at the logon of any user. The work is split in two
  scheduled tasks: "amdgpu-wddm start confirm" runs as the Local System account and confirms the start, and
  "amdgpu-wddm start window" runs as the user who logs on and displays the small blue window that keeps the
  desktop drawing. Both are registered at install time, and the uninstaller unregisters both.

## Known behaviour

- The small blue window in the top-left corner looks as before, and the second task displays it now, so an
  account with administrator rights sees the same window as a standard account. One change: the window now
  stays for the full two minutes at every logon. Earlier releases closed it as soon as the start was confirmed,
  normally after about one minute, because one task did both halves of the work. The two tasks are independent,
  and the window does not read the confirmation's progress. Do not close the window. It closes itself.
- The confirmation's log, `C:\ProgramData\amdgpu-wddm\start-confirm.log`, reports at the end of each logon what
  the window task did: its state, its last run time and its last result (`0` the window was shown and closed,
  `2` it could not be shown).
- Where the window does not run at all, for example at a logon that closes at once, the confirmation still
  keeps the computer off Microsoft Basic Display Adapter, and the next start runs at the fixed 1000 MHz clock
  until the automatic clock control is confirmed again.
