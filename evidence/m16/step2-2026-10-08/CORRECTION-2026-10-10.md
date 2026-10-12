# Correction, 2026-10-10: criterion 3 of this session proved nothing

`docs/01-evidence-rules.md` says of an evidence directory: "Never edit afterwards. A correction
is a new file." This is that file. `README.md`, `vadd.out.txt`, `vadd.err.txt` and both driver
logs stay exactly as the session wrote them.

## What the record claims

Row 3 of the criteria table in `README.md` reads:

```
| 3 `scale` after `vadd` across an event wait | yes (criterion 2 covers it) |
```

## Why it does not hold

The `scale` kernel of `compute/hip/samples/vadd.hip` multiplied `C` by `1.0f`. The addition
writes every element of `C`, so the accepted sums are the same whether `scale` ran after the
addition, before it, or not at all. Criterion 2 is the numerical check of those sums, so it
cannot carry criterion 3: the oracle had no way to fail.

The independent audit of 2026-10-10 states it as finding HS-3, and it was answerable from the
sample's own source before the session ran.

## What the session does establish

Criterion 2 itself stands: two passes of 65536 values with zero mismatches, zero API failures,
and the first launch returning at 4.0 ms. The event and the second stream were created, the
record and the wait returned `hipSuccess`, and both kernels were dispatched. What is not
established is the order the two kernels ran in, and nothing in this directory can establish it.

## What replaces it

`samples/vadd.hip` now scales by `VADD_SCALE_FACTOR` (2.0), the expected sum carries that
factor, and `--scale-order before|omit` runs the two wrong orders on purpose, where
`--expect-compute` must fail. The oracle is `samples/vadd_expect.h` and the host test
`tests/host/test_vadd_oracle.c` holds it to its discriminating power: with the factor restored
to 1.0 that test fails 5 of its 4103 checks, and `order before` and `order omit` leave 0 of 4096
elements wrong instead of 4095.

A fresh lab arm of the corrected sample, with the two negative controls, is what can answer the
ordering question. No such arm is claimed here.
