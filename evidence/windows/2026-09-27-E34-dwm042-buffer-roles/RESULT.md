# M695 supplement - roles of the eight persistent buffers

Same DWM042 log and exact Mesa13e623af70d45982aeb6d3dc9261f9c6fde0d81e.
roles.json joins live map resource pointers with the existing uploader stream,
then removes pointer identities from the exported result.

Seven24000-byte buffers have bind0x08008000. Zink's private ZINK_BIND_DESCRIPTOR
is1<<27 (zink_resource.h:31); do not interpret that bit as Gallium's public video
flag. zink_descriptors.c:1663-1668 creates/maps per-batch descriptor buffers;
zink_resource.c:331-336 assigns Vulkan sampler/resource descriptor-buffer usage.
Known writers are GetDescriptorEXT and descriptor memcpy paths (descriptors.c:
1152,1157,1222,1227,1427,1439,1442). Their role follows the source and logged flag;
this is not a measured count of every store through db_map.

The1048576-byte map18/resource16 matches an uploader map by resource identity.
Two allocation events hand out4 bytes at offset0 and16 bytes at offset4096.
No other allocation or helper-copy event occurs for that resource in the observed
log. Default uploader construction in u_upload_mgr.c:125-131 requests vertex,
index and constant bindings; resource.c:2913 can use that uploader as staging.
Do not label it a vertex-only buffer or equate handed-out bytes with actual stores.

The strict uploader analyzer rejects the log because managers lack terminal
records. --allow-live succeeds only as an explicitly incomplete prefix:8 events,
2 allocations,20 requested bytes,0 helper-copy bytes. This is not proof of zero
writes after allocation. The source explicitly documents that limitation at
u_upload_mgr.c:72-75. No hidden conversion to a closed-lifetime pass was made.

This narrows the remaining audit to descriptor writes and two persistent ranges,
plus any paths absent from these logs. It does not by itself close G0. No new lab
run or driver change. Raw log and prefix analysis remain in dwm042-ops.