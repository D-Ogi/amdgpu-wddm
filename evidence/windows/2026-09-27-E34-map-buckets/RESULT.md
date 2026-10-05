# M561: map classification positive control

Source: bc250-win9da65ba plus audit-buckets.patch, recorded Mesa/E34 stack.
Exact UMD and control identities are in manifest.json. Control063 passes the
three VertexID cases and identifies3 image READ requests of3x1 RGBA32_FLOAT,
144 logical bytes. Buffer requests are classified separately, including their
persistent flags. All buckets fit: overflow=0. Eight gates and three-file replay
pass. CPU DWM5324 and baseline libraries remain unchanged after restoration.

The fixed128-bucket table keys dimensions, target, format, bind, effective usage,
requested box, runtime import and user-pointer status. Overflows are counted and
would invalidate exhaustive classification. User-pointer buffer maps are counted
as persistent even if the caller omitted that flag. Requests still do not measure
actual writes through previously returned pointers. No G0 acceptance follows.
