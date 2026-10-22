#version 410

// The far layer's 3D noise textures (Nebula.c), one slice per draw, as in
// Guerrilla's Nubis clouds (Schneider, "The Real-time Volumetric
// Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015):
// - shape (128^3): R = Perlin-Worley (Perlin FBM remapped onto Worley FBM:
//   billowy, connected), G, B, A = Worley FBM at 4, 8, 16 cells;
// - detail (32^3): R, G, B = Worley FBM at 2, 4, 8 cells, for eroding edges.
// Every noise tiles over the texture (lattices and cells wrap), so the
// textures repeat seamlessly. Worley is inverted (1 at feature points):
// round billows, not cells.

out vec4 FragColor;

uniform int kind;         // 0 = shape, 1 = detail
uniform int slice;
uniform float size;       // texels per side
uniform uint seed;

uvec3 pcg3d(uvec3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	return v;
}

vec3 hash3(ivec3 c, int period, uint salt)
{
	ivec3 w = ((c % period) + period) % period;
	uvec3 h = pcg3d(uvec3(w) + uvec3(seed * 747796405u, salt * 2891336453u, seed ^ salt));
	return vec3(h >> 8u) / 16777215.0;
}

// Tileable gradient noise in [0, 1]: lattice of `period` cells over [0, 1).
float perlin(vec3 p, int period, uint salt)
{
	vec3 x = p * float(period);
	vec3 fl = floor(x);
	ivec3 i = ivec3(fl);
	vec3 f = x - fl;
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float v[8];
	for (int k = 0; k < 8; k++) {
		ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
		vec3 g = hash3(i + o, period, salt) * 2.0 - 1.0;
		v[k] = dot(g, f - vec3(o));
	}
	float n = mix(mix(mix(v[0], v[1], u.x), mix(v[2], v[3], u.x), u.y),
	              mix(mix(v[4], v[5], u.x), mix(v[6], v[7], u.x), u.y), u.z);
	return clamp(0.5 + 0.9 * n, 0.0, 1.0);
}

float perlinFbm(vec3 p, int period, int octaves, uint salt)
{
	float sum = 0.0, amp = 1.0, norm = 0.0;
	for (int k = 0; k < octaves; k++) {
		sum += amp * perlin(p, period, salt + uint(k));
		norm += amp;
		period *= 2;
		amp *= 0.5;
	}
	return sum / norm;
}

// Inverted, tileable Worley (F1): 1 at a feature point, 0 a cell away.
float worley(vec3 p, int cells, uint salt)
{
	vec3 x = p * float(cells);
	ivec3 c0 = ivec3(floor(x));
	float d = 1e9;
	for (int z = -1; z <= 1; z++)
	for (int y = -1; y <= 1; y++)
	for (int k = -1; k <= 1; k++) {
		ivec3 c = c0 + ivec3(k, y, z);
		vec3 fp = vec3(c) + hash3(c, cells, salt);
		vec3 v = fp - x;
		d = min(d, dot(v, v));
	}
	return 1.0 - clamp(sqrt(d), 0.0, 1.0);
}

float worleyFbm(vec3 p, int cells, uint salt)
{
	return worley(p, cells, salt) * 0.625
	     + worley(p, cells * 2, salt + 11u) * 0.25
	     + worley(p, cells * 4, salt + 23u) * 0.125;
}

float remap(float v, float lo, float hi, float a, float b)
{
	return a + (v - lo) / (hi - lo) * (b - a);
}

void main()
{
	vec3 p = (vec3(gl_FragCoord.xy, float(slice) + 0.5)) / size;
	if (kind == 0) {
		float pf = perlinFbm(p, 4, 6, 100u);
		float wf = worleyFbm(p, 4, 200u);
		float pw = clamp(remap(pf, 0.0, 1.0, wf, 1.0), 0.0, 1.0);
		FragColor = vec4(pw, wf, worleyFbm(p, 8, 300u), worleyFbm(p, 16, 400u));
	} else {
		FragColor = vec4(worleyFbm(p, 2, 500u), worleyFbm(p, 4, 600u), worleyFbm(p, 8, 700u), 1.0);
	}
}
