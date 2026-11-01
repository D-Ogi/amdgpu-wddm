// SPDX-License-Identifier: MIT
// The libraries of the vkd3d-deferred case (ddicap.cpp): the shape of the first COLLECTION of the vkd3d-proton test
// test_raytracing_deferred_compilation (tests/d3d12_raytracing.c). Written for this program, not copied.
// -D RAYGEN: one ray generation shader "main" that writes a UAV no root signature of the collection binds, so its
//            compilation depends on a definition outside the collection.
// -D BINDINGS: four ray generation shaders; the collection exports Entry3 only, with a local root signature of one
//              descriptor table of the UAVs u1-u4.
#if defined(RAYGEN)
RWStructuredBuffer<uint> RW : register(u0);

[shader("raygeneration")]
void main()
{
    RW[0] = 0;
}
#elif defined(BINDINGS)
RWStructuredBuffer<uint> U0 : register(u0);
RWStructuredBuffer<uint> U1 : register(u1);
RWStructuredBuffer<uint> U2 : register(u2);
RWStructuredBuffer<uint> U3 : register(u3);

[shader("raygeneration")]
void Entry1()
{
    U0[0] = 0;
    U3[0] = 0;
}

[shader("raygeneration")]
void Entry2()
{
    U1[0] = 0;
    U3[0] = 0;
}

[shader("raygeneration")]
void Entry3()
{
    U2[0] = 0;
    U1[0] = 0;
}

[shader("raygeneration")]
void Entry4()
{
    U3[0] = 0;
    U1[0] = 0;
}
#endif
