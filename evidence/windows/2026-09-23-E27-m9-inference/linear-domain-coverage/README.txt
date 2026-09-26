M307 - Unequal-offset transfer access-domain and direction acceptance
Base revision bed764da5192be7132d646be0e6331c1edeb30fd plus worktree modifications; snapshots included.

Expanded actual owned-transfer fixtures to twelve combinations: source+1/destination+1, alias/disjoint, system/mixed/local physical identities. Physical pages are fragmented in local/mixed modes. Actual7DWORD direct,34DWORD local-staged,83DWORD mapped and100DWORD mapped-staged packets are decoded independently. Packet access domains match captured flags. Every fixture uses multiple DMA callbacks, disables translation after first callback, checks exact private coverage and final initial-byte snapshot plus owner release.

Routing31382checks PASS; focused --unequal-virtual-alias356checks PASS. Generated ForceForwardAlias replaces only capture backward selection with0; focused356checks/3failures native1, final byte snapshots for positive-shift alias in each access domain. No production source mutation. Initial added assertion had signedness warnings treated as errors; corrected casts and reran all affected checks before these final logs.

Tests only this turn, no driver rebuild or deployment. Latest development SYS remains M306952F05B7142D18A4E526622F160F9B8477380EF20E5AD42106D45F567D7FF556. Linear table-shadow publication still needs actual-table acceptance; general interval dependencies, OS status/lifetime/cache/initialization and GPU performance remain open. No lab access/mutation this turn; sshd reply pending.
