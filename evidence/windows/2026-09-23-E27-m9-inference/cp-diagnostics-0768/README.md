# CP initialization diagnostics, local0768

Candidate only; no deployment, hardware run, reset or lab access in this turn.
The prior run015 stream stops after STEP_BEGIN gfx-6. It does not isolate
an operation within CP initialization or establish the cause.

The source now logs begin/end/result for KIQ init, each compute/GFX queue init,
queue mapping, GFX start and each ring test, plus compute unhalt boundaries.
These use the existing shim/guard logger and add no register access. They run
only at initialization, not in the submission hot path. Guard logging is buffered:
a hard hang may prevent retrieval/persistence, and a missing end marker alone
cannot distinguish a hung call from lost logs. Successful return of compute
unhalt is not independent proof of hardware state.

Validation: run_gfx.ps1 -Out P:\bc-250\scratch\build\cp-diagnostics-replay
returned0; 354 initialization and35 interrupt writes match the Linux reference,
with the existing24 address exceptions. Existing teardown/rerun, stuck queue,
unclean recovery and negative controls pass in the host model. Full WDK build
and package signing pass. No new claim of real GPU recovery or paging acceptance.

Package: scratch/build/bc250kmd-0768/package-umd, version0.7.68.1.
SYS SHA256: 8CDADC84B4E66B898FF36CC7E418F27F2DD2CF934AF242ABDFE929E6EE641679.
The obsolete stage-A wddm.c preamble was replaced with current scope and fault
policy; there is no functional WDDM change in this revision.

Next hardware experiment must collect the guard stream while initializing and
retain the final available substep; do not repeat a stalled job automatically.
Actual reset/recovery and the full M9 acceptance gates remain open.
