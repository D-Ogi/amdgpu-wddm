// SPDX-License-Identifier: MIT
// Manual diagnostic pair body, not a general shader transformation.
RWByteAddressBuffer result : register(u0);
cbuffer Options : register(b0) { uint negative; uint nativeSamples; };
static const uint LogicalBytes = 6 * 64 * 16 * 64;
static const uint PhysicalBytes = 6 * 64 * 16 * 8;
static const uint Header = LogicalBytes + PhysicalBytes;
static const int2 Positions8[8] = {int2(9,5),int2(7,11),int2(13,9),int2(5,3),int2(3,13),int2(1,7),int2(11,15),int2(15,1)};
static const int2 Positions16[16] = {int2(9,9),int2(7,5),int2(5,10),int2(12,7),int2(3,6),int2(10,13),int2(13,11),int2(11,3),int2(6,14),int2(8,1),int2(4,2),int2(2,12),int2(0,8),int2(15,4),int2(14,15),int2(1,0)};
struct Input { float4 position : POSITION; float2 affine : TEXCOORD0; float2 projective : TEXCOORD1; };
struct Vertex { float4 position : SV_Position; noperspective float2 affine : TEXCOORD0; float2 projective : TEXCOORD1; noperspective float2 zw : TEXCOORD2; };
Vertex probe_vs(Input input, uint id : SV_VertexID) {
    uint ignored;
    if (id < 18) result.InterlockedAdd(Header + id * 4, 1, ignored);
    else result.InterlockedAdd(Header + 31 * 4, 1, ignored);
    Vertex v;
    v.position = input.position; v.affine = input.affine; v.projective = input.projective;
    v.zw = float2(input.position.z / input.position.w, 1.0 / input.position.w);
    return v;
}
struct Values { float4 position; float2 affine; float2 projective; };
bool valid_record(float4 position, uint primitive, uint id, uint limit) {
    bool valid = all(isfinite(position.xy)) && all(position.xy >= 0) && all(position.xy < 8) && primitive < 6 && id < limit;
    if (!valid) { uint ignored; result.InterlockedAdd(Header + 31 * 4, 1, ignored); }
    return valid;
}
Values pull(float4 position, float2 affine, float2 projective, float2 zw, int2 location, int2 actualOffset) {
    Values v;
    v.affine = EvaluateAttributeSnapped(affine, actualOffset);
    v.projective = EvaluateAttributeSnapped(projective, actualOffset);
    float2 pulledZW = EvaluateAttributeSnapped(zw, actualOffset);
    v.position = float4(floor(position.xy) + float2(location) / 16.0, pulledZW.x, 1.0 / pulledZW.y);
    return v;
}
void physical(float4 position, uint primitive, uint id, uint coverage) {
    uint pixel = uint(position.y) * 8 + uint(position.x), ignored;
    uint address = LogicalBytes + ((primitive * 64 + pixel) * 16 + id) * 8;
    result.InterlockedAdd(address, 1, ignored);
    result.InterlockedOr(address+4, coverage, ignored);
    result.InterlockedAdd(Header + 18 * 4, 1, ignored);
}
void body(Values v, uint primitive, uint id, uint apiCoverage, bool front, uint owner) {
    if (!valid_record(v.position, primitive, id, 16)) return;
    uint pixel = uint(v.position.y) * 8 + uint(v.position.x);
    uint address = ((primitive * 64 + pixel) * 16 + id) * 64, previous, ignored;
    result.InterlockedAdd(address, 1, previous);
    result.InterlockedOr(address + 4, apiCoverage, ignored);
    result.InterlockedAdd(address + 8, (primitive + 1) * 257 + id + 1, ignored);
    result.InterlockedAdd(Header + 29 * 4, 1, ignored);
    if (id >= 8) result.InterlockedAdd(Header + 30 * 4, 1, ignored);
    if (previous == 0) {
        result.Store(address + 12, front ? 1 : 0);
        result.Store4(address + 16, asuint(v.position));
        result.Store2(address + 32, asuint(v.affine));
        result.Store2(address + 40, asuint(v.projective));
        result.Store2(address + 48, asuint(float2(v.affine.x * v.projective.y + v.position.z, v.projective.x + v.affine.y * v.position.w)));
        result.Store(address + 56, owner);
    }
}
void audit(Values a, Values b) {
    float errors[8] = {abs(a.position.x-b.position.x),abs(a.position.y-b.position.y),abs(a.position.z-b.position.z),abs(a.position.w-b.position.w),abs(a.affine.x-b.affine.x),abs(a.affine.y-b.affine.y),abs(a.projective.x-b.projective.x),abs(a.projective.y-b.projective.y)};
    uint ignored; bool bad = false;
    result.InterlockedAdd(Header + 19 * 4, 1, ignored);
    [unroll] for (uint i=0;i<8;i++) {
        result.InterlockedMax(Header + (21+i)*4, asuint(errors[i]), ignored);
        bad = bad || !isfinite(errors[i]) || errors[i] > 1e-5;
    }
    if (bad) result.InterlockedAdd(Header + 20 * 4, 1, ignored);
}
void pair(float4 position, float2 affine, float2 projective, float2 zw, uint primitive, bool front, uint physicalID, uint coverage) {
    int2 low = Positions16[physicalID], high = Positions16[physicalID+8];
    int2 lowOffset = low - 8, highOffset = high - 8;
    if (negative == 3) highOffset = lowOffset;
    if (negative == 4) { lowOffset = 0; highOffset = 0; }
    // Pull both locations before either coverage branch. Sample/centroid location
    // modifiers are ignored by pull evaluation; perspective modes are preserved.
    Values a = pull(position, affine, projective, zw, low, lowOffset);
    Values b = pull(position, affine, projective, zw, high, highOffset);
    if (coverage & (1u << physicalID)) body(a, primitive, physicalID, 1u << physicalID, front, physicalID);
    bool upper = (coverage & (1u << (physicalID+8))) != 0;
    if (negative == 1) upper = false;
    if (negative == 2) upper = true;
    if (upper) {
        body(b, primitive, physicalID+8, 1u << (physicalID+8), front, physicalID);
        if (negative == 5) body(b, primitive, physicalID+8, 1u << (physicalID+8), front, physicalID);
    }
}
void native_ps(float4 position : SV_Position, sample noperspective float2 affine : TEXCOORD0,
               sample float2 projective : TEXCOORD1, sample noperspective float2 zw : TEXCOORD2,
               uint id : SV_SampleIndex, uint coverage : SV_Coverage,
               uint primitive : SV_PrimitiveID, bool front : SV_IsFrontFace) {
    if (nativeSamples != 8 && nativeSamples != 16) { uint ignored; result.InterlockedAdd(Header+31*4,1,ignored); return; }
    if (!valid_record(position,primitive,id,nativeSamples)) return;
    Values direct; direct.position=position; direct.affine=affine; direct.projective=projective;
    int2 location = nativeSamples == 16 ? Positions16[id] : Positions8[id];
    Values evaluated = pull(position, affine, projective, zw, location, location - 8);
    audit(direct, evaluated);
    physical(position,primitive,id,coverage);
    // Reference payload is the native input, not the pull expression.
    body(direct,primitive,id,coverage,front,id);
}
void paired_ps(float4 position : SV_Position, sample noperspective float2 affine : TEXCOORD0,
               sample float2 projective : TEXCOORD1, sample noperspective float2 zw : TEXCOORD2,
               uint id : SV_SampleIndex, uint coverage : SV_Coverage,
               uint primitive : SV_PrimitiveID, bool front : SV_IsFrontFace) {
    if (!valid_record(position,primitive,id,8)) return;
    physical(position,primitive,id,coverage);
    if (id < 8) pair(position,affine,projective,zw,primitive,front,id,coverage);
}
void hostpaired_ps(float4 position : SV_Position, noperspective float2 affine : TEXCOORD0,
                   float2 projective : TEXCOORD1, noperspective float2 zw : TEXCOORD2,
                   uint coverage : SV_Coverage, uint primitive : SV_PrimitiveID, bool front : SV_IsFrontFace) {
    if (!valid_record(position,primitive,0,1)) return;
    physical(position,primitive,0,coverage);
    // Native16 pixel-frequency surrogate: explicitly not the AMD eight-ID mode.
    [unroll] for (uint i=0;i<8;i++) pair(position,affine,projective,zw,primitive,front,i,coverage);
}
