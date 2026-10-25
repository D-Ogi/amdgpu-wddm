// One stage of d3d12ace: data[i] = rounds of mix over (input ^ seed), where the input is i (mode 0) or
// data[i] (mode 1). Stage() in d3d12ace.cpp is the same function on the CPU, the oracle of every check, and
// tools/win/vkace/shaders/stage.comp is the same function for Vulkan.
#define STAGE_RS "RootConstants(num32BitConstants=4, b0), UAV(u0)"

cbuffer Params : register(b0)
{
    uint seed;
    uint count;
    uint rounds;
    uint mode;
};

RWByteAddressBuffer data : register(u0);

uint mixw(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

[RootSignature(STAGE_RS)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= count)
        return;
    uint v = (mode == 0u ? i : data.Load(i * 4u)) ^ seed;
    for (uint r = 0u; r < rounds; r++)
        v = mixw(v + r);
    data.Store(i * 4u, v);
}
