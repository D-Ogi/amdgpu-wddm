#version 450
// vkwbs case 4 and 4 control: one point at the origin; the draws use rasterizer discard.
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
