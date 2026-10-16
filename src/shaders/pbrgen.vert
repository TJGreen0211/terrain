#version 410
layout (location = 0) in vec2 aUv;

uniform int faceIndex;

out vec3 fSurfDir;

void main()
{
	// u runs left->right as seen in the file, t runs geometric up (t=+1 ends up
	// at the top of the PNG after the FBO readback is flipped before writing).
	float u = aUv.x * 2.0 - 1.0;
	float t = aUv.y * 2.0 - 1.0;

	// Each cube face maps the unit square onto one axis-aligned square of the
	// inscribed cube; normalizing projects it onto the unit sphere, so the same
	// 3D noise field used by the planet shader stays seamless across all faces.
	vec3 p;
	if (faceIndex == 0)      p = vec3( u,  t,  1.0); // +Z
	else if (faceIndex == 1) p = vec3( u,  t, -1.0); // -Z
	else if (faceIndex == 2) p = vec3( 1.0, t,  u);  // +X
	else if (faceIndex == 3) p = vec3(-1.0, t,  u);  // -X
	else if (faceIndex == 4) p = vec3( u,  1.0, t);  // +Y
	else                     p = vec3( u, -1.0, t);  // -Y

	fSurfDir = normalize(p);
	gl_Position = vec4(aUv * 2.0 - 1.0, 0.0, 1.0);
}
