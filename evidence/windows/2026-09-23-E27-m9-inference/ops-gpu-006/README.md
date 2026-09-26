# Run006: first RMS_NORM positive result, incomplete probe

Fresh boot 02:51:23. The diagnostic task lacked administrator rights, so per-node CLI log reads were refused. The callback stopped prematurely but upstream decode still returned0. The next probe must run elevated for this diagnostic and track callback failure explicitly. No full graph success is claimed.

The retained GPU node000 is RMS_NORM, not the earlier CPU GET_ROWS callback: the premature failure prevented incrementing the node index and reused the filename. Compare it to CPU node001 only. F32 input320 elements is byte-equal. comparison.json quantifies output max absolute error. The separate elevated wrapper captured final KMD hardware counters in kmd.txt before restoration. Display-only restored at02:53:47, UnconfirmedStarts0.
