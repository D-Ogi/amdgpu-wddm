# Correction, 2026-10-10: "as every D3D9 application does" overstates the coverage

`evidence/` is immutable (`docs/01-evidence-rules.md`: "Never edit afterwards. A correction is
a new file."), so `RESULT.md` and the raw output next to it stay as the run wrote them. The
independent audit of 2026-10-10 raised this wording in its appendix on M843 to M849. The fact
row [M843](../../../docs/facts/d3d.md#m843) needs no change, because it names the two tested
entry points and claims nothing about the others.

## What stands

Every number stands. The audit re-read `d3d9-run.out`: all four modes report 32896 triangle
pixels, 32640 clear pixels, other = 0 and 0 wrong samples, and the sum 65536 is 256 x 256. The
four draw rates in the fact are the rates in the raw output. The driver state stands too: 342
new completed fences, flags 15, no change of generation or epoch, and no GPU fault and no
timeout or reset line. The elapsed seconds in the raw output are rounded to three decimals, so
the last digit of a draw rate cannot be recomputed from them alone.

## What is corrected

`RESULT.md`, Method step 3, reads: "`default` calls `Direct3DCreate9Ex`, as every D3D9
application does." Applications that call the older `Direct3DCreate9` exist, and no arm of this
run called it. The sentence is about an untested population.

| Section | The write-up says | What the run covers |
|---|---|---|
| Method, step 3 | "`default` calls `Direct3DCreate9Ex`, as every D3D9 application does" | Two entry points in four arms: `Direct3DCreate9Ex`, and `Direct3DCreate9On12Ex` with the mapping layer forced on. `Direct3DCreate9` was not called |

This is the same class of inference as BD-081: the name or the generation of an API does not
establish which driver entry point or route Windows selects. A capture of the modules the
process loaded is the measurement that does, and this run has one: every arm in `d3d9-run.out`
prints its loaded modules, with `d3d9.dll`, `d3d9on12.dll` and our own three files among them.
`stack-hashes.out` holds the hashes of those files on disk.

## What would close it

One more arm of the same probe, with `Direct3DCreate9` in place of `Direct3DCreate9Ex`, and the
same module capture. The probe needs no new tool and the run is bounded like the four arms
here. A D3D9 game session remains open as well, as `RESULT.md` and the fact both state.
