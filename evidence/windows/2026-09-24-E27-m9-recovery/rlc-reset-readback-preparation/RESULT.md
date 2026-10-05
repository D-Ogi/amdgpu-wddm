# M352 - Candidate111 explicit RLC reset readback variant, local only

Candidate0.7.111.1 SYS SHA256
EAF9DA6CFFB0947FC7B8C92061AB3E59B8D309CBE40CD8FD993F1182C72936BA.
Not deployed; latest lab state remains M350,110display-only, reset gate0.

EnableRlcReloadReset0 remains off;1 retains the original AMD isolated callback;
2 selects the explicit post-write read variant. Other values do not select it.
The guard, serialized unpublished startup, halt bits and busy-clear acceptance
remain unchanged. No CP/GFX reset domain is added and no extra register is
allow-listed. Baseline callback source body still matches AMD.

The new helper imports the assert/read/delay/deassert/read/delay portion of
AMD gfx_v10_0_soft_reset, restricted to SOFT_RESET_RLC. It reads the initial
reset register, ORs RLC, writes and reads back, waits50us, clears RLC from that
read value, writes and reads back, then waits50us. The initial/asserted/released
values are logged after the sequence, avoiding logging while reset is asserted.
The backend still stops subsequent writes on a recorded access fault. There
is no new fault bypass. Reads are observations, not a proof of reset completion.
This is an experimental sequence; M351 shows why it is not ordinary Linux
recovery for this ASIC. The immediate busy-clear criterion is still a project
trial condition, not an upstream generic soft-reset success rule.

Validation:
- 22 isolated scenarios pass,11 for each variant: no-write controls, each
  missing halt bit, modeled busy-clear success and stuck-busy refusal.
- Access hooks discriminate baseline RWRW from new RWRWR at GRBM_SOFT_RESET;
  exactly two reset writes preserve the seeded unrelated CPreset bit in both.
  Hooks observe register order, not elapsed time or delay call positions.
- Ordinary startup replay remains exact354+35writes with24address exceptions;
  four existing negative controls fail as intended.
- Actual startup coordinator327checks pass. It does not execute the KMD wrapper.
- WDK build and25package checks pass,0errors/warnings.
- Model busy-clear is assumed, not measured hardware behavior.

Next hardware trial must retain exact hash/gate witnesses and record reset
readbacks together with resulting RLCstatus and subsequent startup outcome.
Do not repeat unchanged gate1 or a known failing gate0 warm start as control.
Use the existing M350 same-boot baseline to select a meaningful changed test;
if a fresh first-start control is needed, explicitly document its isolation.
No inference workload may run unless full startup and its content control pass.
M350 is still the latest hardware result; warm reentry remains unresolved.
