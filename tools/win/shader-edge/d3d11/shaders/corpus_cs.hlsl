// SPDX-License-Identifier: MIT
// Compute corpus of the D3D11 shader edge-case client. build.ps1 compiles every entry with FXC and with FXC2, and
// d3d11shaderedge.cpp runs both bytecodes on the same device. The expected values come from a C copy of each
// function in d3d11shaderedge.cpp (Expect*), not from either compiler, so a failure of one bytecode and a pass
// of the other places the fault between that compiler and the driver; a failure of both points at the driver.
//
// Edge cases from Michael Moroz's FXC2 (https://github.com/MichaelMoroz/FXC2): fu_main is the float - uint
// miscompile of its patch 47 (a - b was built as a + (-b) with the negation in b's unsigned type, so an unsigned 3
// became 4294967293.0); the other entries use the constructs where FXC2's code differs in structure from FXC's:
// loops it does not unroll (patch 1), switches it lowers to if chains (patches 20, 26, 29), indexable temps,
// firstbithigh and firstbitlow (patch 6), shifts and masks that become ubfe (patch 53) and unsigned division by a
// constant (patch 15).
//
// Every entry: thread i (256 threads, 4 workgroups) reads In[i] and writes the eight words Out[8i .. 8i+7]. Every
// word gets + neg; neg = 1 is the negative control, every case must then fail. Every value is exact: the floats are
// small multiples of powers of two, so no rounding mode or contraction can change a result.
StructuredBuffer<uint> In : register(t0);
RWStructuredBuffer<uint> Out : register(u0);
cbuffer Control : register(b0)
{
    uint neg;
    uint3 pad;
};

void Put(uint i, uint k, uint v) { Out[8 * i + k] = v + neg; }

// float - uint, uint - float, float2 - uint2 and float - int, with unsigned values on both sides of 2^31.
[numthreads(64, 1, 1)]
void fu_main(uint i : SV_DispatchThreadID)
{
    const uint x = In[i];
    const float f = (float)(x & 255) * 0.5f + 10.25f;
    const uint u = (x >> 8) & 1023;
    const uint big = 0x80000000u + ((x & 0xFFFF) << 8);
    const float fb = (float)((x >> 16) & 255) * 256.0f + 512.0f;
    const float2 at = float2(f, f * 2.0f);
    const uint2 cell = uint2(u, u >> 1);
    const float2 d = at - cell;
    const int s = (int)(x & 511) - 256;
    Put(i, 0, asuint(f - u));
    Put(i, 1, asuint(d.x));
    Put(i, 2, asuint(d.y));
    Put(i, 3, asuint(u - f));
    Put(i, 4, asuint(f - s));
    Put(i, 5, asuint(fb - big));
    Put(i, 6, asuint((float)big));
    Put(i, 7, (uint)(f - u + 4096.0f));
}

// Loops with continue and break and a data-dependent trip count; [loop] keeps FXC from unrolling them.
[numthreads(64, 1, 1)]
void loop_main(uint i : SV_DispatchThreadID)
{
    const uint x = In[i];
    uint acc = 0;
    [loop] for (uint k = 0; k < (x & 31) + 1; ++k) {
        if ((k & 3) == 1)
            continue;
        acc = (acc * 31 + k) ^ x;
        if (acc > 100000u && (x & 1) != 0)
            break;
    }
    uint nested = 0;
    [loop] for (uint a = 0; a < 3; ++a) {
        [loop] for (uint b = 0; b < (x % 5); ++b) {
            if (a * b == 2)
                break;
            nested += a * 7 + b;
        }
    }
    uint w = x, steps = 0;
    [loop] while (w > 1 && steps < 64) {
        w = (w & 1) ? 3 * (w >> 1) + 2 : w >> 1;
        ++steps;
    }
    Put(i, 0, acc);
    Put(i, 1, nested);
    Put(i, 2, steps);
    Put(i, 3, w);
    Put(i, 4, 0);
    Put(i, 5, 0);
    Put(i, 6, 0);
    Put(i, 7, 0);
}

uint SwitchReturn(uint sel, uint x)
{
    switch (sel) {
    case 0: return x * 3;
    case 1:
    case 2: return x + 100;
    case 4: return x ^ 0x55u;
    case 6:
        if (x > 128)
            return 1;
        return 2;
    default: break;
    }
    return 0xDEADu;
}

// Switches: shared case labels, a nested branch, a loop in a case, a default, and a switch that returns.
[numthreads(64, 1, 1)]
void switch_main(uint i : SV_DispatchThreadID)
{
    const uint x = In[i];
    const uint sel = x % 9;
    uint r;
    switch (sel) {
    case 0: r = x * 3; break;
    case 1:
    case 2: r = x + 100; break;
    case 3: r = x ^ 0x55u; break;
    case 5:
        if (x > 128) r = 1;
        else r = 2;
        break;
    case 7: {
        uint t = 0;
        [loop] for (uint k = 0; k < (x & 7); ++k)
            t += k * k;
        r = t;
        break;
    }
    default: r = 0xDEADu; break;
    }
    Put(i, 0, r);
    Put(i, 1, SwitchReturn(sel, x));
    Put(i, 2, SwitchReturn((x >> 4) % 8, x >> 8));
    Put(i, 3, 0);
    Put(i, 4, 0);
    Put(i, 5, 0);
    Put(i, 6, 0);
    Put(i, 7, 0);
}

// Local arrays indexed with run-time values: indexable temps (x# registers) in DXBC.
[numthreads(64, 1, 1)]
void indexed_main(uint i : SV_DispatchThreadID)
{
    const uint x = In[i];
    uint arr[16];
    [loop] for (uint k = 0; k < 16; ++k)
        arr[k] = x * k + 1;
    const uint j = x % 16;
    arr[j] += 1000;
    arr[(j * 7) % 16] ^= 0xF0F0u;
    uint s = 0;
    [loop] for (uint m = 0; m < 16; ++m)
        s = s * 3 + arr[(m * 5 + j) % 16];
    uint4 v[4] = { uint4(1, 2, 3, 4), uint4(x, x >> 1, x >> 2, x >> 3), uint4(5, 6, 7, 8), uint4(9, 10, 11, x) };
    const uint4 row = v[x & 3];
    Put(i, 0, s);
    Put(i, 1, arr[x % 13]);
    Put(i, 2, row.x);
    Put(i, 3, row.y);
    Put(i, 4, row.z);
    Put(i, 5, row.w);
    Put(i, 6, arr[15 - j]);
    Put(i, 7, 0);
}

// Bit operations and unsigned division.
[numthreads(64, 1, 1)]
void bits_main(uint i : SV_DispatchThreadID)
{
    const uint x = In[i];
    Put(i, 0, firstbithigh(x));
    Put(i, 1, firstbitlow(x));
    Put(i, 2, (uint)firstbithigh((int)x));
    Put(i, 3, countbits(x) | (reversebits(x) & 0xFFFFFF00u));
    Put(i, 4, (x >> 5) & 0x3Fu);
    Put(i, 5, x / 7);
    Put(i, 6, x % 13);
    Put(i, 7, x / ((x & 15) + 1));
}
