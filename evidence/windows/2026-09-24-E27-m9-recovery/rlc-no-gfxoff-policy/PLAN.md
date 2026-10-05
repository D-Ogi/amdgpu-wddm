# M403 - Use AMD no-GFXOFF RLC startup policy in full WDDM

M402 has no measured CP/GFX busy discriminator. Current KMD forces pp_gfxoff
true to reproduce E03. AMD supplies a false-policy branch in rlc_start that
suppresses RLC-SMU messages through its existing handshake control helper.
Test this source-defined policy, not a new SMU command or inferred reset mask.

Full WDDM selects false; display-only diagnostic replay retains true. Compare
actual shim helpers with extracted original6.18.52 helpers, both policies,
initial register states, complete read/write/delay sequences. A reversed policy
branch is the negative control. Run original full GFX replay and WDK/package.
Only then first-load GPU oracle and one changed warm trial, tracking RLC/GRBM.
No causal or warm-recovery claim before hardware evidence.
