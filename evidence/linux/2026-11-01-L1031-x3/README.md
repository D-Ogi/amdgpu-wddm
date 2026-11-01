# L1031 X3: a GRUB start counter on the diagnostic stick, sources and two read-backs

Date: 2026-11-01. Unit A, the diagnostic stick (label `BC250DIAG`). X3 is an extra item of this campaign,
not a wishlist row. This directory holds text sources and two read-backs. It holds no binary, no GRUB
image and no camera frame.

## Aim

A Windows that does not start correctly must come back to a Linux that answers over SSH, without a
person at the lab. X3 uses only files on the stick: no firmware boot entry and no NVRAM write. The
stick's GRUB counts the starts of the Windows entry in `boot/grub/grubenv`. A Windows task clears the
count after a good start. After three counted starts with no clear, GRUB boots entry 4 (network only,
amdgpu blacklisted).

## Revisions

- `scripts/v1/`: the first revision, installed on the stick at about 01:00 UTC. The files come from the
  archive of the originals that the campaign kept. A static review found four high defects in it:
  - The Windows entry set the root to the Windows ESP, so a failed chainload left fallback entry 4
    without its kernel path.
  - The task ran at system start plus 3 min and checked no logon or desktop.
  - The head did not branch on a failed `load_env` or `save_env`.
  - `to-windows-x3.sh` could reboot after a failed write.
- `scripts/deployed/`: the corrected revision. `x3-head.cfg` (SHA-256 prefix `ca02d087`) boots entry 4
  when `load_env` or `save_env` fails, and when the counter is 3 or invalid. `x3-entry.cfg` (`cd51ed1b`)
  sets the root to the ESP only for `chainloader` and then back to the stick. `embedded.cfg` finds the
  stick by the marker file `boot/grub/bc250diag.id`. `x3-files.py`, `x3-install.sh` and
  `to-windows-x3.sh` are the Linux tools. The `*.ps1` files are the Windows acknowledgement and its task
  ("Lab boot counter reset", at logon, interactive token, delay 90 s). `bounded-child.sha256` pins the
  helper binary, which is not in this directory.
- `scripts/acceptance/`: the helpers of a live acceptance of the corrected revision.
- `scripts/tests/`: offline tests. `test-x3.py` runs the GRUB branches as mocked shell code. It tests the
  branch logic, not GRUB parsing, persistence or an EFI start. `test-x3.ps1` and `test-x3-process.ps1`
  test the PowerShell policy and process parts.

## Read-backs (`out/`)

- `x3-test2-boot-wait.log`: the bounded boot wait after the stick was switched to the Windows entry
  (default 6) at about 01:07 UTC. After 96 s the wait reported Windows up, with a boot time of 01:08:04
  UTC. This proves only that Windows became reachable. It is no read of the counter.
- `live-20261101T050818Z-grubenv` and `live-20261101T050818Z-grub.cfg`, with
  `live-20261101T050818Z-receipt.json`: a read-only copy of the stick files at 05:08:18 UTC
  (`"remote_mutations": false`). The configuration is the first revision with `set default=6`. The
  environment block holds `bc250_try=2`. So two Windows starts were counted and no clear followed them.

## What this directory does not prove

- The campaign notes describe a live acceptance of the corrected revision on 2026-11-01, from about
  19:43 to 20:05 UTC. Its raw outputs (boot waits, task log, counter bytes) are not in this directory.
  So this directory does not prove that acceptance.
- No receipt of the offline tests on the deployed revision is in this directory.
- The counter counts the evaluations of the configuration with default 6, not real Windows starts. A
  manual choice of another entry also uses one count.
- X3 acts only on the next start. It is not a watchdog: a hung Windows still needs a restart.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. One lab host name is removed from an acceptance
helper. `sha256.txt` holds the SHA-256 of every file in this directory except itself.
