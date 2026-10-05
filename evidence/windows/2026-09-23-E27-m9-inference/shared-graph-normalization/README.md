# Shared graph normalization - host evidence

One normalization of the captured physical-page union now feeds both validation and emission for all equal-offset byte bands. Logical subspans retain their edge ordering; graph workspaces are rebuilt per band. Previously the builder normalized each band in each pass (up to six sorts). No state survives a callback and no resource lifetime changes.

Actual extracted KMD routing tests: 15684 checks, zero failures, including byte-oracle partial-range and multipass fixtures. WDK build passes. Development image only, revision101 unchanged; not installed.

The standalone benchmark compares the old and new normalization/planning strategies on the same rotated-page inputs, with three byte bands and two planning passes. Three trials per size alternate old/new execution order. All semantic physical-move digests match; this digest is supplementary to the independent actual-packet byte oracle. For262144pages, old179/193/187ms, new47/48/48ms (median187ms to48ms). Timing includes digest calculation and uses MSVC clock with millisecond granularity; smaller inputs are below useful timer resolution. This is a host microbenchmark, not actual callback latency, a GPU transfer, or inference throughput. Total allocation remains128bytes per logical page and normalization still repeats across multipass callbacks.

No lab mutation. Network probes of both configured routes still fail TCP22; owner response about local sshd is pending. Runtime acceptance and full M9 remain open.
