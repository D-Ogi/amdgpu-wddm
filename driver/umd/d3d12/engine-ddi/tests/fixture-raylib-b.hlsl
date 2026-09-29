// SPDX-License-Identifier: MIT
// Second ray tracing library of engine-ddi-harness (test-raytracing.cpp), compiled into fixture-raylib-b.h: one miss
// shader, "miss_far", with the payload of fixture-raylib.hlsl. The harness passes it with no export list (every
// export) beside fixture-raylib, which lists its exports, to show that one library's listed names survive another's
// exporting everything. No table of the harness names miss_far, so no ray runs it.
struct Payload {
    uint value;
};

[shader("miss")]
void miss_far(inout Payload payload) {
    payload.value = 3u;
}
