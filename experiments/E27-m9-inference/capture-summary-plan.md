# M335 - Persistent capture-path observation

Hypothesis: adapter-lifetime counters can distinguish reserved storage from heap fallback after per-context logs wrap or contexts are destroyed.

Add two atomic 64-bit counters to WDDM state. Count each successfully attached new capture once, before multipass delivery; do not count continuation callbacks or terminal repeats. Print totals on every WddmSummaryOf, including stop. Retain per-owner diagnostics.

Validation: extend the existing actual-source capture test to observe both paths, multipass behavior and totals after owner destruction. Run KmdRouting and the WDK build. These host results do not prove runtime reservation use. No lab restart or deployment is part of this validation.
