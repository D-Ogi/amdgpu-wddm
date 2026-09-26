# M234: shared hardware initialization entry points

2026-09-23. Source/host only.

Gart/Psp/Ih/Gfx command bodies are now static Execute implementations shared
by existing diagnostic Escape entry points and new InitializeHardware functions.
Source comparison confirms all four execution bodies are unchanged by extraction.

The internal initializers take caller-owned nonpaged diagnostic reports rather
than allocating large reports on the kernel stack. They require PASSIVE_LEVEL,
a device and report, and no sticky unconfirmed-stop flag. Each selects exactly
ENABLE/LOAD/INIT/RUN-to-stage8, clears input report state and retains detailed
partial-progress output. Native errors propagate; a refused command, nonzero shim
result or incomplete reported state cannot become success. PSP requires ring/TMR/
GART plus nonempty completed command count; IH requires Active; GFX requires stage8
and no failed stage. Report status is normalized to the returned NTSTATUS.

141 controls run extracted actual entry points against modeled Execute outcomes
and real escape-report declarations. They cover success, warning/error statuses,
shim errors, refused/incomplete commands, missing input, wrong IRQL, quarantine,
preserved partial diagnostic output and unchanged diagnostic forwarding.
Omitting completion refusal in generated test code fails30 checks as expected.

Full WDK development build/sign passes, version0793 retained:
P:/bc-250/scratch/build/init-entrypoints-dev/package-umd
SYS SHA256:88E66DA8AEF710F34FA7F13695EBEA346692134F0A6D7554A8CCBC607DD81947
Development-only, not a replacement official0793 artifact. Not deployed.

No startup coordinator or automatic hardware activation is connected yet.
The future coordinator must allocate reports before touching hardware, preflight
all required gates/resources and firmware, track partial ownership, unwind in
the verified order and check actual paging readiness before WDDM admission.
Generic GFX stage completion alone does not prove the paging gate is enabled.
Tests do not execute hardware sequences, demonstrate interrupt delivery, prove
GPU halt or validate automatic PnP startup. No lab access, reboot or USB changes.
