# GPU Present producer/submit observations

2026-09-27, source parent ee3164b with accompanying instrumentation.
Host compilation/tests only, not deployed.

Adapter-local64-bit atomic totals count gated producer calls, emitted BGP1
records, insufficient-buffer returns, refusals and four status groups.
Consumer totals distinguish accepted hardware submissions, rejected records
and hardware submission failures. Existing on-demand WddmSummary exposes them.
They are individual atomic reads, not a transactional snapshot; collect closure
after work has quiesced. Counters continue after the detailed logging budget.

First16 producer calls report offered DMA/private sizes and before/after
MultipassOffset. First16 records report context, IB VA/size and both validated
LB7A descriptor values and handles. First16 accepted submissions report context,
fence, VA and length. The builder reads only its already locked descriptor
snapshots; it does not dereference allocation objects for logging.

Totals do not prove retirement, per-call ordering or residency. Accepted
submissions include any scheduler resubmission; they need not equal records.
Record count can increase on a partial multipass return. Rotation count can
increase without a record for an exhausted tail. Bounded detailed output is
not a loss-accounted lifetime trace. Hardware completion evidence is separate.

All13 quick gates pass. No new ABI, cap, fallback, or admission relaxation.
