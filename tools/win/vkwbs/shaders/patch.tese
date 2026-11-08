#version 450
// vkwbs case 4: the patch position for every generated vertex.
layout(triangles, equal_spacing, ccw) in;
void main() { gl_Position = gl_in[0].gl_Position; }
