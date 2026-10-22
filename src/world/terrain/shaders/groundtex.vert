#version 410

// Full-screen triangle for the ground texture bake (terrain_detail.c); no
// vertex attributes.
void main()
{
	vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
