# Pending release-notes lines: the VM flush on the ring (`EnableRingVmFlush`)

This file is not a release. It holds the tester-facing lines for the next tester release notes. The release
step copies the lines into `docs/testing/release-notes/<version>-tester.N.md` and deletes this file.

## New, and off until you turn it on

- The driver has a second way to give the graphics engine the page tables of the program that is drawing. The
  old way writes two hardware registers from the processor and then waits for the graphics memory unit to
  answer, before each piece of work reaches the card. The new way puts the same two steps into the work itself,
  so the card does them in order and the processor waits for nothing. The setting is `EnableRingVmFlush` under
  the driver's `Parameters` key. It is off in this release, and the installer does not write it. With it off the
  driver sends the very same commands as version 0.7.216.24.

- Turn it on with `EnableRingVmFlush` set to 1 and a restart. The driver says which way it uses in its log at
  every start, and it counts the pieces of work that carried the new one at the end of a run. To go back, set the
  value to 0 and restart. Nothing else has to be undone.

## Known behaviour

- The new way has not run on the lab machine yet. If a game or the desktop stops responding with it on, the
  picture freezes until Windows resets the display driver, and on this hardware that reset is a restart. Keep
  `TdrDelay` at 10 seconds (the driver's own window sets it) before you try it, and set `EnableRingVmFlush` back
  to 0 after any freeze.
