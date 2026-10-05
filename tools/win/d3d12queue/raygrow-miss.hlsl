// SPDX-License-Identifier: MIT
// Second library of the grow variant (interactive-raypipeline.h, build.ps1 -RayGrow): the miss shader that
// AddToStateObject adds to the pipeline of raygrow.hlsl. It writes 3, which no shader of the first library writes.
// Compiled with dxc into raygrow-program.h; the command is recorded at the top of that file.
struct Payload {
    uint value;
};

[shader("miss")]
void miss_new(inout Payload payload) {
    payload.value = 3;
}
