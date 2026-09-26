# Classifying G0 map requests

PROVENANCE: Mesa, MIT.

Apply after audit.patch. Fixed128 buckets retain cumulative map counts/bytes by
resource dimensions, format, bind, effective usage, requested box, runtime import
and user-pointer status. Overflow is explicit; nonzero overflow rejects exhaustive
classification. Existing cumulative samples emit each occupied bucket. Persistent
user-pointer mappings include the effective flag even when absent from the request.

Control063 repeats the known3x1 image-readback test and verifies3 READ maps/144
bytes, zero overflow and correct pixels. See M561 evidence. No actual-copy-byte
claim is made, and persistent writes still require caller/ownership inspection.

DWM010 repeats the DWM009 GDI-only overlap/scanout procedure with this candidate.
ETW uses1024KiB buffers, minimum64/maximum256 buffers, capped128MiB trace file.
The same60-second rollback watchdog and75-second window lifetime apply. Compare
map buckets between steady samples before the final diagnostic capture, correlate
loss-free DMA execution to DWM, and audit KMD blit counters. Reject G0 if copies
remain unexplained, ETW loses required events, or the image oracle differs.
