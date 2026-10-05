# M353 - Candidate111 reset bit reads asserted/released; busy remains

Exact0.7.111.1 SYS EAF9DA6CFFB0947FC7B8C92061AB3E59B8D309CBE40CD8FD993F1182C72936BA.
Installed with all gates closed over110display-only in the same Windows
boot2026-09-24T05:20:01. Exact installed hash/version and deviceOK verified.
This is a changed-sequence trial after M350; no fresh111first-start content
control is claimed. The same boot's earlier110control passed (M350).

One startup selects EnableRlcReloadReset2. Before PSP, RLC_CNTL0 and
GRBM_STATUS2 0x01000008, both reads successful. CP/MEC/SDMA halt words match
M350:15000000,50000000,00000001,00000001. Reset readback logs initial00000000,
asserted00000004,released00000000 (GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK).
The register observation therefore follows the requested assertion/deassertion.
It does not establish reset completion inside RLC or that every reset domain
is idle. Immediate post-reset RLC snapshot remains CNTL0/STATUS2 0x01000008.
The helper returns-62 and startup status0xC0000185 before PSP firmware loading.
Attempted0x3/completed0x1,ready0,unwind1,quarantine0. Independent SSH remains
available; native trial exits1 because the adapter start is unhealthy.

Explicit post-write readbacks did not satisfy the immediate busy-clear
postcondition in this state. Do not call the register locked or claim that no
reset occurred. The busy-clear criterion remains a project diagnostic, and
there is no evidence here for inference correctness after this attempted reset.

Logs were preserved before closed-gate PnPrecovery. Independent final info:
111,stage61,43presents,full gate0,reset gate0; CLIconfirm exit0,guard counter0.
Same boot throughout; no AC cycle, OS restart, DWM reset or owner action.
Host recovery/info logs redact hardware/interface lines and PCI identity;
raw KMD logs are unchanged. The12snapshot bundle includes prior control/stop
history; only snapshots containing this reset readback are trial witnesses.

Next: return to the first observed retirement transition (GFXHUB TLB flush),
review its completion/consumer/cache contract with disabled RLC and GART, and
compare RLC/GFXOFF dependencies against the actual startup/stop sequence.
Repeating the same reset variants or expanding reset masks is not justified
by this result. Warm reentry and broader M9 acceptance remain unresolved.

Reset readback witnesses:
- ring-20260924-035415-909.log
