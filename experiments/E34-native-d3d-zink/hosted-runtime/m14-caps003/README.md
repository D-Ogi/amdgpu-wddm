# ABI1.4 exact pair capability admission

Hypothesis: frozen engineDC65 (df9ae2c8), test5D7D and ICDC0CE retain valid
capability answers and pass engine1.4 OOM/trim controls on unit A.
Run the existing direct-ICD engine test headless, with a125s outer Job deadline,
its120s inner limit, pre/post CPU171 identity checks and170s absolute bound.
STOP/temperature checked by the runner. No DDI, deployment or registry change.
Require all tests pass, exact ICD witness, closed Job and unchanged postflight
before encoding caps as a new UMD configuration. Do not reuse old cap values
with a new engine hash. This does not prove hosted UMD error propagation.

Result: M749 fails the memory-budget/Trim assertions; controlled OOM passes. Closed Job and unchanged CPU171 postflight verified28.107s. No new config admitted.
