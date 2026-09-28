# M742 - Nested active-console launcher refusal before the D3D client

Window002 source867f245c, manifest
07633CE64DBA5C3C390A8DB72A6E3764B262925DBE4FADEAF9CDB9FEB848E438.
SYSTEM telemetry now passes. The inner active-console helper exits125 with
launch_error5 before the interactive client starts. No Cpu.json, Cpu-debug.txt
or GPU result is produced. Unlike standalone M740, this helper runs inside an
outer bounded phase. That difference is a diagnostic lead, not a proven cause;
the existing error lacks the exact failing API stage.

Independent supervisor failed-restored25.9233204s. Baseline/postflight/tree
closure verified; CPU171 restored, task cleaned and Missing independently
observed. No GPU phase, no KMD/DWM/OS restart, no Present measurement.
Raw scratch/m14/window002-ops. Saved result and helper error contain no secrets.

Next: a no-GPU nested-launch control with explicit failure-stage diagnostics,
before another UMD routing trial. Diagnostic build adds operation names only;
it does not relax token, session, suspended-start or Job ownership requirements.
