# M331 - Limits of current retirement and Linux reference evidence

Source review, not a new hardware observation. Existing E13 boot8 reload stream
and README confirm M54/M55: successful unload is available, successful reload is
not. The second-load trace is kernel messages only; module-scoped register probes
were not armed for reload. Do not treat this as a known-good Linux reentry trace.

Current shim hw_fini differs deliberately: it dequeues KIQ and clears RLC_ENABLE_F32
via bc250_rlc_stop. That helper performs one RLC_CNTL read and write, without a
post-write RLC progress/idle witness. Gfx EnginesHalted checks CP_ME_CNTL,
CP_MEC_CNTL and both SDMA_F32_CNTL registers; it contains no RLC status check.
PspStop gates Unload on GfxStopQuiet. Startup then reloads firmware before GFX.
Consequently M325 halt-register/PSP-success observations do not independently
establish RLC retirement or the conditions for another firmware load. This is an
evidence gap, not proof that the old RLC still ran or that PSP/TMR freeing caused
the failure. Do not invent a register predicate, delay or reset as a fix.

Next decisive input is M330 persisted stop/start snapshots, especially the new
scheduler-read/write/done boundaries. If a new Linux experiment is needed, L17/L34
must arm tracing before module load and preserve output across loss of SSH.
Re-running the old module-filter procedure would reproduce its missing evidence.
