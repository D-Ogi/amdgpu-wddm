// SPDX-License-Identifier: MIT
// Generic reconstruction controls: compile MIN16 / NATIVE16 / INT64 / FLOAT64 separately.
struct Varyings {
    float4 position : SV_Position;
#if defined(INT64)
    nointerpolation int64_t2 signed_value : SIGNED_VALUE;
    nointerpolation uint64_t2 unsigned_value : UNSIGNED_VALUE;
#elif defined(FLOAT64)
    nointerpolation double2 value : WIDE_VALUE;
#elif defined(NATIVE16)
    nointerpolation vector<int16_t, 2> signed_value : SIGNED_VALUE;
    nointerpolation vector<uint16_t, 2> unsigned_value : UNSIGNED_VALUE;
    vector<float16_t, 2> value : HALF_VALUE;
#else
    nointerpolation min16int2 signed_value : SIGNED_VALUE;
    nointerpolation min16uint2 unsigned_value : UNSIGNED_VALUE;
    min16float2 value : HALF_VALUE;
#endif
};
Varyings VSMain(uint vertex : SV_VertexID) {
    Varyings o;
    o.position = float4(float(vertex), 0, 0, 1);
#if defined(FLOAT64)
    o.value = double2(vertex, vertex + 1);
#else
    o.signed_value = int2(vertex, -int(vertex));
    o.unsigned_value = uint2(vertex, vertex + 1);
#if !defined(INT64)
    o.value = float2(vertex, vertex + 1);
#endif
#endif
    return o;
}
float4 PSMain(Varyings i) : SV_Target {
#if defined(FLOAT64)
    return float4(float2(i.value), 0, 1);
#elif defined(INT64)
    return float4(float2(i.signed_value), float2(i.unsigned_value));
#else
    return float4(float2(i.signed_value + i.unsigned_value), float2(i.value));
#endif
}
