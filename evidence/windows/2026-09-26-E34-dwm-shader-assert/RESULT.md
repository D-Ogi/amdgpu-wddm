# DWM initialization stops on unsupported TGSI SAMPLE translation

M548, unit A, 2026-09-26. Probe007 identifies a CPU shader-translation assertion,
not a GPU fence wait or measured GPU hang. The compositor thread waits in a CRT
modal assertion dialog reached from ttn_src_for_file_and_index, source line748
UNREACHABLE("bad src file"). The stack operand is TGSI_FILE_SAMPLER_VIEW(10),
index0, in TGSI_OPCODE_SAMPLE(145/0x91), with three sources and one destination.
This occurs in the second traced CreatePixelShader. See stack-summary.txt and
the private dump/PDB hashes; raw dump, registers and memory remain outside Git.

The restored legacy D3D frontend emits SAMPLE with separate view and sampler
operands. This tree's tgsi_to_nir has no SAMPLE lowering and routes SAMPLER_VIEW
through its ordinary value loader, which rejects that file. The local restored
SAMPLE opcodes and their consumer need integration; this is not evidence of an
upstream Zink regression. Enabling the old texture path indiscriminately would
lose independent texture/sampler indices and is not a valid general correction.
Next: implement the needed TGSI-NIR texture contract and exercise independent
texture/sampler slots with a synthetic native D3D textured shader before DWM.

Probe004's global2048-call trace was exhausted by format/MSAA queries. Probe005
limits each entry point to8 records and identifies unmatched CreatePixelShader
sequence119. Probes004/005 retain the capture/flags timeout and rollback failure
verdicts. Probe006 removes early screenshots to observe CPU/stacks but its live
Get-Content collides with the writer, causing early rollback; no dump collected.
Probe007 removes live log reads, observes30 seconds, and obtains a successful
thread dump at about20 seconds. Across all six samples, CPU time is1.234375s and
log size46848 bytes. This supports a wait, not ongoing expensive compilation.

Candidate007 uses ICD3508416F and diagnostic UMDB2B0CB27. Both trace builds pass
all8 scoped gates. Initial and per-function trace patches replay with exact LF
hashes; neither changes rendering semantics. No hosted submit/Present is observed.
No image correctness or no-copy/G0 claim. Normal runner exit0 for007 means bounded
observation and rollback completed, not GPU rendering passed.

After007, DWM9648 loads CPU UMD8279AC7F; registered ICD9C40083C restored; overlay
STOP endpoint responds false. The independent60-second watchdog verifies the
baselines. No KMD update or requested OS reboot. Transcripts redact Username,
RunAs User and Machine. No screenshot or dump content is published.
PROVENANCE: Mesa MIT; Windows SDK/WDK10.0.26100 declarations.
