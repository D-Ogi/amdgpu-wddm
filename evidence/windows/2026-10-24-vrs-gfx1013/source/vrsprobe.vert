#version 450
// One triangle that covers the whole viewport: (-1,-1), (3,-1), (-1,3). No vertex buffer.
// WRITE_RATE: the per-vertex (primitive) shading rate, RATE from specialization constant 0
// (Vulkan encoding: 1 = 2 pixels high, 4 = 2 pixels wide, 5 = 2x2).
#ifdef WRITE_RATE
#extension GL_EXT_fragment_shading_rate : require
layout(constant_id = 0) const int RATE = 5;
#endif

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0);
#ifdef WRITE_RATE
    gl_PrimitiveShadingRateEXT = RATE;
#endif
}
