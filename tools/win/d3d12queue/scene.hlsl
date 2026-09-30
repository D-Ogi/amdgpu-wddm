// SPDX-License-Identifier: MIT
// Programs of the scene variant (interactive-scene.h). Compiled with fxc into scene-programs.h; the commands
// are recorded at the top of that file.
struct VSIn { float3 pos : POSITION; uint tag : TAG; };
struct VSOut { float4 pos : SV_Position; nointerpolation uint tag : TAG; };

VSOut vs_main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 1.0f);
    o.tag = i.tag;
    return o;
}

cbuffer Params : register(b0) { uint seed; };
Texture2D<uint> source : register(t0);

uint ps_main(VSOut i) : SV_Target0 {
    return seed ^ i.tag ^ source.Load(int3((int)i.pos.x, (int)i.pos.y, 0));
}
