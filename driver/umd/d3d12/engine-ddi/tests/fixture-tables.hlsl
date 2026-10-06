// SPDX-License-Identifier: MIT
// Root table fixture of engine-ddi-harness (test-draw-path.cpp, root tables), compiled into fixture-tables.h. One
// thread stores seed ^ tag into the first word of each of three raw UAVs, which the test's root signatures take from
// three descriptor tables in different parameter orders.
RWByteAddressBuffer out0 : register(u0);
RWByteAddressBuffer out1 : register(u1);
RWByteAddressBuffer out2 : register(u2);
cbuffer Params : register(b0) { uint seed; };

[numthreads(1, 1, 1)]
void main() {
    out0.Store(0, seed ^ 0x10000000u);
    out1.Store(0, seed ^ 0x20000000u);
    out2.Store(0, seed ^ 0x30000000u);
}
