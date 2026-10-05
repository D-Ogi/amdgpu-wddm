# M626: corrected KMD157 identity diagnostic build

Isolated source885922a91f37d15316edfe01110c386b56a0e10b adds f58e193 to156/cf1aabb.
The303-file baseline audit retains the same six changed existing driver files.
Full build passes12 gates, compile/link/stack/catalog/signing and clean source
identity. Both packages contain SYS SHA256:
9B4AEBED0A0D4001E03BC3D39CD6B2E8AC051F98A9BD0A1B150C3EFB9FC0E067.

Candidate and rollback153 are staged under C:/BC250/m12/candidate07157. Remote
hashes and PowerShell5 syntax pass; the durable task is prepared, not started in
this evidence. Preflight at07:06:16Z reports153, STOP false, no test processes and
66C. Only the identity probe will be enabled; GPU Present remains off and CPU
UMD/ICD settings are retained. Refer to the separate transition result for any
subsequent runtime outcome. This build alone does not establish G0 acceptance.

Logs/manifests are copied unchanged; no private identifiers removed.
