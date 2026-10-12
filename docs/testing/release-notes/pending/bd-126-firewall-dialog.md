# Pending release-notes lines: the firewall notification window (BD-126)

This file is not a release. It holds the tester-facing lines for the next tester release notes. The release
step copies the lines into `docs/testing/release-notes/<version>-tester.N.md` and deletes this file.

## Changed

- The window that Windows shows when a program wants to open a network port ("Windows Security", with the
  Allow and Cancel buttons) stayed white and empty, with only the program's icon in it. It did not draw its
  text or its buttons, so there was nothing to answer. The program behind that window is `PickerHost.exe`, the
  File Picker UI Host, which also draws the file open and save windows of Windows. This driver now gives that
  program the processor driver instead of the adapter, as it does for the sign-in screens. We expect the window
  to draw its text and buttons there, as the other system windows do, but we have not yet seen it on a test
  machine: please tell us what you see, for the firewall window and for a file open or save window.

  Nothing changes for your other programs: this one program name is the whole change, and your own entries in
  the `Allow` and `Deny` lists under `HKLM\SOFTWARE\amdgpu-wddm\AppRouter` keep their meaning.

  You can put that program back on the adapter, and see the old behaviour again: add `PickerHost.exe` to the
  `Allow` list under `HKLM\SOFTWARE\amdgpu-wddm\AppRouter`. That also helps us find the cause.

  To see the window at all: start a program that listens on a network port from a folder that has no firewall
  rule yet (a game or a server you have just copied to another disk, for example).

## Known behaviour

- A program that draws on the processor gets the Direct3D feature level 10_0 from this driver (the known
  limitation of the processor route). A window of this kind should not need more. If a file open or save
  window stops appearing after this release, put `PickerHost.exe` in the `Allow` list, which undoes the
  change for that program, and tell us.
