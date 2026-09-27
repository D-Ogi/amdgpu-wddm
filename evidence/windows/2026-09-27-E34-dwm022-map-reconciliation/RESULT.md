# M596: reconcile DWM022 map bucket overflow

Follow-up to M595; no new lab run or binary change. The strict zero-overflow
analyzer still rejects DWM022 and its original evidence is unchanged.

The source counts image/buffer map requests and requested logical bytes before
calling the 128-entry bucket classifier. Overflow increments only after no
bucket matches. Consequently totals minus bucket sums bound omitted requests.
The retained verifier checks all 183 snapshots: nonnegative residual calls and
bytes separately for images/buffers, distinct bucket IDs, and residual call
sum exactly equal to the overflow counter. All checks pass.

At sample11200 the missing classification covers exactly eight image map
requests totaling721152 logical bytes, zero buffer requests, and zero persistent
requests. Their combined size is less than one 1920x1200x4 frame (9216000 bytes).
The overflow therefore cannot hide even one full-frame logical map request.
It does not count CPU stores through a previously returned pointer.

The retained full-frame buckets show one WRITE request and six READ requests;
the latter are consistent with the six GDI screenshots, without call-stack
attribution. Persistent buckets account for all eight persistent map requests:
seven24000-byte descriptor maps and one1MiB uploader map. No unclassified
persistent map or persistent image map appears in these snapshots.

Current source hashes match M562 for zink_context.c, zink_descriptors.c,
zink_resource.h, u_upload_mgr.c, radv_wddm2_bo.c and KMD wddm.c. Resource and
DXGI frontend sources differ. Inspection still finds hosted surface import
returning before Lock2 and borrowed RADV BO mapping rejected before cached
pointer use. Descriptor writes are descriptor-sized; buffer uploads retain
the request audit before staging allocation. This rechecks paths but does not
by itself establish full source-to-DLL provenance for every current input.

These results resolve the size and persistence uncertainty of the overflow.
They do not claim zero CPU work, zero uploads, measured CPU stores, or complete
G0 acceptance. Exact artifact/source validation, retained lifecycle gates and
the accepted engine-blit work remain open. Lab remained assigned to the other
coordinated test window; no deployment or baseline changes.
