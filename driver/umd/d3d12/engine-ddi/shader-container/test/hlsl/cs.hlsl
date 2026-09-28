// SPDX-License-Identifier: MIT
// Compute programs: cs_5_0 (CS50) and cs_5_1 with register spaces (CS51). No signatures. Root descriptors and
// root constants only, so the test needs no descriptor heap.

#if defined(CS50)

RWByteAddressBuffer outBuf : register(u0);
RWStructuredBuffer<uint> counters : register(u1);

cbuffer Params : register(b0)
{
    uint scale;
    uint bias;
};

groupshared uint lds[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID,
            uint gi : SV_GroupIndex)
{
    lds[gi] = dtid.x * 31u + dtid.y * 17u + gid.y;
    GroupMemoryBarrierWithGroupSync();
    uint v = lds[63u - gi] * scale + bias + gtid.x;
    float f = sin(float(v) * 0.001);
    uint idx = dtid.y * 32u + dtid.x;
    outBuf.Store2(idx * 8u, uint2(v, asuint(f)));
    InterlockedAdd(counters[gid.x % 4u], v & 0xffu);
}

#elif defined(CS51)

RWByteAddressBuffer outBuf : register(u3, space2);

cbuffer Params : register(b1, space1)
{
    uint mul;
    uint xorValue;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    outBuf.Store(dtid.x * 4u, (dtid.x * mul) ^ xorValue);
}

#else
#error define CS50 or CS51
#endif
