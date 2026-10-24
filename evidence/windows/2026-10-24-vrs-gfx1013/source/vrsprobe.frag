#version 450
// Each invocation takes a unique id from one atomic counter and writes it, its own FragCoord and
// (READ_RATE) its ShadingRateKHR to every pixel it covers. A coarse 2x2 fragment therefore leaves
// four pixels with the same id, and the counter ends at the number of invocations.
#ifdef READ_RATE
#extension GL_EXT_fragment_shading_rate : require
#endif

layout(set = 0, binding = 0, std430) buffer Counter { uint count; } counter;
layout(location = 0) out uvec4 result;

void main()
{
    uint id = atomicAdd(counter.count, 1u);
    uvec2 fc = uvec2(gl_FragCoord.xy * 16.0);
    uint rate = 0xFFFFu;
#ifdef READ_RATE
    rate = uint(gl_ShadingRateEXT);
#endif
    result = uvec4(id, fc.x | (fc.y << 16), rate, 0xA5A5A5A5u);
}
