# Caps004: physical BO accounting alone does not reach the budget query

PROVENANCE: Mesa, MIT; local WDDM2 tree under mesa-current-src.

Same engine DC65/test5D7D as M749, new ICD CE63E49463786BCF0C68262ED5CF889D07B5AB88E3F91C64260B1F3575B9E392.
The three-file incremental patch is driver/icd/mesa-wddm2-memory-accounting.patch;
source-hashes.json identifies normalized before/after files. The prior C0CE binary is retained locally.
Build completed with MSVC and the existing Mesa Ninja configuration.

The unchanged Trim test still reports zero at all four stages and fails two assertions.
Engine exit1 after7.389s, supervisor24.680s, no timeout, Job empty. Postflight passes:
CPU171 baseline, boot, DWM and device generation unchanged. No driver registration changed.
No caps.json was admitted. Native ABI1.4 error handling remains unmeasured.

Static follow-up finds radv_physical_device.c's _WIN32 radv_query_heap_info only
memsets its output to zero. The budget API does not call winsys query_value at all.
Thus M749's reservation mapping was a real source defect but not a sufficient explanation
of the observed zero. The next change must connect the physical-device query to adapter-wide
allocation accounting, including multiple winsys/device sessions. The current counters are
per winsys, not yet a process-wide heapUsage implementation. OS Budget integration remains open.

Raw logs stay in scratch/m14/lab-caps004. Selected memory lines are verbatim;
closure omits the process identifier. No allocation handles or private identifiers exported.
