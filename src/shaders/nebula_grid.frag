#version 410

// The near layer's presence grid (Nebula.c), one slice of the 3D texture
// per draw.
// - level 0 (reduce == 0): each texel holds the max of the layer's density
//   bound (nearBound, nebula.glsl) over 4^3 points spanning the texel and
//   one texel around it, so the grid stays conservative under trilinear
//   filtering and a march step's skip.
// - level k > 0 (reduce == 1): the max of the 8 texels under it in level
//   k - 1 (glGenerateMipmap averages, which would hide thin gas).

#define NEBULA_OCTAVES 1
#include "nebula.glsl"

out vec4 FragColor;

uniform int reduce;
uniform int slice;           // z texel of the level being written
uniform float gridSize;      // texels per side of the level being written
uniform sampler3D finer;     // reduce: the level below (only it is sampled)

void main()
{
	ivec3 c = ivec3(ivec2(gl_FragCoord.xy), slice);
	if (reduce != 0) {
		float m = 0.0;
		for (int k = 0; k < 8; k++)
			m = max(m, texelFetch(finer, c * 2 + ivec3(k & 1, (k >> 1) & 1, k >> 2), 0).r);
		FragColor = vec4(m, 0.0, 0.0, 1.0);
		return;
	}
	float texel = 2.0 / gridSize;
	vec3 lo = (vec3(c) - 1.0) * texel - 1.0;   // one texel of margin
	float m = 0.0;
	for (int z = 0; z < 4; z++)
	for (int y = 0; y < 4; y++)
	for (int x = 0; x < 4; x++)
		m = max(m, nearBound(lo + (vec3(x, y, z) / 3.0) * 3.0 * texel));
	// Any gas at all counts: R8 would round a faint bound down to 0.
	FragColor = vec4(m > 1e-4 ? max(m, 1.0 / 255.0) : 0.0, 0.0, 0.0, 1.0);
}
