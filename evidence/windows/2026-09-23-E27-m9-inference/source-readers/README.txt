M310 - Include all readers of repeated physical source pages
Base revision bed764da5192be7132d646be0e6331c1edeb30fd plus worktree changes; snapshots included.

PagingPageAliasDirection now records first and last source index for each normalized identity. Extremal signed dependencies cover all readers: positive lastDelta requires backward order; negative firstDelta requires forward order. Compatible dependencies are accepted; conflicting signs remain for general interval planning. Destination identity injectivity remains required. Caller workspace grows to2*Identities, using existing contiguous Readers/Writer arrays in captures; no per-page allocation increase.

8600accepted whole-memory snapshot fixtures cover five unique/repeated source patterns,216destination identity triples and16offset pairs plus controls. Actual owned-transfer fixtures add source[A,A,B] to destination[C,D,A], both byte-shift directions and system/mixed/local/table domains, with independent packet replay, multiple callbacks, capture release and per-callback table metadata checks.

Routing304073checks PASS. WDK26100build PASS, SYSAA55F016D6AFCCA7DC4DA07C932266F8E942A48AB38437B9750B21072B0A3AFF, not deployed. Tests prove this compatible-direction class, not arbitrary repeated-source graphs or cyclic intervals. OS status/lifetime/cache/initialization and GPU performance acceptance remain open. No lab access/mutation this turn; sshd reply pending.
