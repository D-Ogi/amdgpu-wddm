# M381 - Capture demand and lifetime source review

Hypothesis: sizing capture storage from queued GPU buffer count overstates its
actual lifetime; source frees a plan when construction finishes. Inspect exact
builder, arena and Microsoft buffer/transfer contracts without hardware changes.
Separate a proven per-operation bound from unproved simultaneous continuations.
This source review cannot prove OS callback delivery or PFN ownership.
