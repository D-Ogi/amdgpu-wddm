# Uploader event verification

The opt-in source instrumentation from M680 is in Mesa9f8a1ff9 and built into
candidate UMD04186900. `analyze-uploader-events.py LOG` validates every observed
manager using address plus creation timestamp, buffer generations, resource and
capacity, nonoverlapping suballocation ranges, and exact cumulative allocation/
helper-copy deltas. A helper copy must match a preceding uncopied allocation.
Buffers must be released before rollover or manager destruction.

By default every observed manager needs a destroy record. With `--allow-live`,
a consistent prefix can be decoded, but its output explicitly reports incomplete
lifetime coverage. Even a closed stream cannot detect a manager omitted entirely;
the source/environment/artifact witness and broader map reconciliation remain
necessary. These events count handed-out ranges and u_upload_data memcpy calls,
not writes made later through returned pointers or copies elsewhere in the stack.

`test-uploader-events.py` uses the actual M680 control log as its positive input.
It confirms4 allocations/4144 bytes,4112 copied bytes and two manager lifetimes
that reuse the same address. Negative controls remove create/map/alloc/copy/release
records, duplicate an allocation, exceed capacity, truncate the stream and supply
empty input. The first12 records remain a valid prefix only with --allow-live.
No new lab run or exhaustive no-copy claim follows from these tests.
