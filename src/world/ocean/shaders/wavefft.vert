#version 410

// Fullscreen triangle from gl_VertexID (no vertex buffers) for the GPU
// ocean passes in Water.c: one fragment per spectrum texel.
void main()
{
	vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
