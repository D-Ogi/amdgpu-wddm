# Physical page identity normalization, host only, 2026-09-23

PagingPermutationNormalize builds an O(N log N) sorted physical source identity index and binary-searches each destination page. Output indices preserve original source ordering and feed the existing cycle planner. Only complete aligned pages forming a bijection of the same distinct physical set qualify. Duplicates, different sets and unaligned addresses do not qualify; callers still need the general-alias route. Arrays are caller-owned scratch on failure; no MDLs retained. No MMIO addresses are introduced.

All46233permutations1..8 now pass through normalization with unsorted64-bit physical addresses, repeated low32-bit patterns and physical page zero. Their plans and12batch budgets still match the independent initial snapshot (554796batch scenarios). Classification controls reject duplicate/different-set/unaligned inputs. Generated-source mutation truncating source identities to32bits reports46233failures,exit1; that run preceded the additional large-scale test.

Large positive normalization:262144distinct page identities (1GiB logical page span), shuffled destination rotation, every result index checked,0.014CPU seconds in this host run. Source/destination/work/index/seen arrays total roughly9.25MiB on this compiler. This is CPU normalization only, not moving1GiB or fitting that cycle in a DMA buffer. MSVC /W4 /WX /O2 /std:c11 passes.

Not wired into KMD build, endpoint resolution or hardware admission. No deployment or lab access. Integration still needs captured physical identities under builder ownership, GPU packet budget/emission, scratch lifetime and a general solution for oversized, partial-page and non-bijective aliases. Full M9 remains incomplete.
