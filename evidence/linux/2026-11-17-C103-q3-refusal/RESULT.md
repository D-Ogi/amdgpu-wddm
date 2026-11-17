# C103: q3 read request refused before a data sample

## Result

On unit A, one request to the stock P3 q3 register-read handler returned
`0xFD`. The trial did not pass its address-mapping test. It produced no target
data sample and did not activate VCN.

The raw receipt is [q3-control-execute-C103-r2-01.json](q3-control-execute-C103-r2-01.json).
The worker and controller both exited with code 2. The final worker record
states `finished`, `no_retry: true`, and `mailbox refusal 0xfd`.

## Procedure and observations

The trial ran on 2026-11-17 under Linux 6.18.52-0-lts with stock P3 firmware.
The preflight checked the board, kernel, absent amdgpu driver and temperature.
The exact runtime manifest SHA256 was
`c5d766fb6a5f3e225582503328a22aba74832306fee5e0cc28feba20882c689b`.
The source body SHA256 was
`5c805026581ace101ba43ef4d0f6b34354f4a61461a1c78d2fa6a335a53d9936`.
The runtime build ID was `d073d7b7da060ba3dc9d883f76438aa242cd28b1`.
This was a RAM-staged diagnostic trial, not an overlay deployment.

The intended test compared two changing q0 signatures with values returned
through q3. Its only target was the q0 parameter register, derived from the
pinned P3 descriptor and the MP1_Public mapping. No UVD register was a target.
See [the generated registers](q3-control-registers-C103.json) and
[the fixed protocol](q3-control-core-C103.py).

1. q0 message 2 returned the expected version `0x00580600`.
2. Two q3 snapshots agreed: response 1, command 0, parameter 0.
3. q0 message 1 changed `0x12345670` to `0x12345671`.
4. The worker cleared the q3 response and read back zero. It wrote the generated
   target argument and read it back. The response was still zero before the
   command write.
5. q3 message `0x2A` returned a fresh `0xFD`. No subsequent SMU I/O occurred.

The complete command sequence was q0/2, q0/1, q3/0x2A. The second challenge
did not run. There was no DMA transfer, permission change or VCN access.
Tctl was 73.500 C at preflight and 73.875 C in the final worker samples.

The q0 controls prove that the mailbox transport responded. They do not prove
the q3 address translation: q3 returned no target data. The static P3 analysis
associates `0xFD` with a dispatcher permission refusal. This is an interpretation
from the firmware code, not a measurement of the permission variable itself.
The result does not establish the reachability or power state of VCN.

## Recovery

Clean shutdown was scheduled at 16:08:00Z. Two standby samples were 13.2 W.
The relay readback was OFF at 16:09:28Z and ON at 16:09:40Z. The bounded boot wait
reported Linux up after 167 seconds. Its archived receipt is an explicit
transcription of the terminal result, not a raw process log. It preserves the
remote clock's known one-hour offset.

At 16:12:42Z the original default-6 configuration was restored. The configuration,
environment and loader hashes matched. The postboot check at 16:13:11Z found
network-only Linux, Tctl 72.750 C, and no diagnostic provider, amdgpu driver,
IVRS table or previous trial directories. No new experiment ran in that check.
The recovery receipts are included in [SHA256.json](SHA256.json).

## Source and evidence scope

The first worker revision had a STOP/deadline check defect. It was found before
execution. Revision 2 checks the latch and deadline again after the legacy
guard returns. The retained guard check and protocol tests cover that change.
They are host tests and do not establish hardware cancellation guarantees.

12 of the 13 archived files have exactly the bytes of their retained originals.
No redactions were needed. One excerpt, `q3-controller-C103-r2.py`, changed after
the trial and was not run again. It names the directory of a support kit on the
unit. That directory name had a prefix that does not belong in a public
repository. The archived excerpt has `bc250` in place of the prefix. Its digest
before this change is
`13d77eca5a178ff452e644784ad4b04a0a67f45da3bf41d448ec542943b573eb`.
No firmware blob or compiled module is included.
These source excerpts are not a standalone deployment kit. Shared transport
and process-helper excerpts are in the adjacent C100 evidence directory.

PROVENANCE: project diagnostic source under the repository licence. Private
local firmware analysis, with no firmware binary redistributed.
