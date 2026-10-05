// SPDX-License-Identifier: MIT
// Writes the indirect arguments of the indirect DispatchRays subtest on the GPU (cs_6_0, one thread): two
// D3D12_DISPATCH_RAYS_DESC records at byte 0 and byte 104 of args (the API layout, checked by static_assert in the
// client), and the count words 1 at byte 0 and 2 at byte 4 of counts. The CPU never writes either buffer.
//   record 0: raygen_a (record at table + 0), miss table + 128, hit group table + 192, 8 x 8 x 1
//   record 1: raygen_b (record at table + 64), miss table + 128, hit group table + 192, 8 x 4 x 1
// The table address arrives as two root constants; the 64-bit additions carry by hand.
RWByteAddressBuffer args : register(u0);
RWByteAddressBuffer counts : register(u1);
cbuffer Table : register(b0) {
    uint table_lo;
    uint table_hi;
    uint record_bytes;   // shader record size: one identifier, 32 bytes
    uint unused;
};

uint2 address(uint offset) {
    uint lo = table_lo + offset;
    return uint2(lo, table_hi + (lo < table_lo ? 1u : 0u));
}

void write_record(uint at, uint raygen, uint width, uint height) {
    args.Store2(at + 0, address(raygen));          // RayGenerationShaderRecord.StartAddress
    args.Store2(at + 8, uint2(record_bytes, 0));   // .SizeInBytes
    args.Store2(at + 16, address(128));            // MissShaderTable.StartAddress
    args.Store2(at + 24, uint2(record_bytes, 0));  // .SizeInBytes
    args.Store2(at + 32, uint2(record_bytes, 0));  // .StrideInBytes
    args.Store2(at + 40, address(192));            // HitGroupTable.StartAddress
    args.Store2(at + 48, uint2(record_bytes, 0));  // .SizeInBytes
    args.Store2(at + 56, uint2(record_bytes, 0));  // .StrideInBytes
    args.Store2(at + 64, uint2(0, 0));             // CallableShaderTable.StartAddress
    args.Store2(at + 72, uint2(0, 0));             // .SizeInBytes
    args.Store2(at + 80, uint2(0, 0));             // .StrideInBytes
    args.Store3(at + 88, uint3(width, height, 1)); // Width, Height, Depth
    args.Store(at + 100, 0u);                      // padding of the 104-byte record
}

[numthreads(1, 1, 1)]
void main() {
    write_record(0, 0, 8, 8);
    write_record(104, 64, 8, 4);
    counts.Store2(0, uint2(1, 2));
}
