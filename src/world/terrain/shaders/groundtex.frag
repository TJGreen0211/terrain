#version 410

// Bakes one layer of the procedural ground textures (terrain_detail.c) once
// at startup: tileable height fields for close-up surface detail, shaded in
// quad.frag on the terrain detail lattice.
//   layer 0 rock, 1 ground (soil / vegetation), 2 sand, 3 snow
// Output: RG = height slope per tile width (d/du, d/dv), B = albedo factor
// (1 = unchanged: the biome colours keep the palette), A = height 0..1.
// Every noise has an integer period that divides the tile, so the layers
// tile seamlessly; the slope is a central difference of the same function.

out vec4 Out;

uniform int layer;
uniform float size;   // texels per tile edge

const float TAU = 6.28318531;

uvec3 pcg3d(uvec3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	return v;
}

vec2 latticeGrad(ivec2 p, int period, int seed)
{
	// Wrap without % (undefined for negative operands in GLSL).
	p -= period * ivec2(floor(vec2(p) / float(period)));
	float a = float(pcg3d(uvec3(p, uint(seed))).x) * (TAU / 4294967296.0);
	return vec2(cos(a), sin(a));
}

// Gradient noise with lattice period `period` (x in lattice units), ~[-0.7, 0.7].
float gnoise(vec2 x, int period, int seed)
{
	vec2 i = floor(x), f = x - i;
	ivec2 p = ivec2(i);
	vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float a = dot(latticeGrad(p, period, seed), f);
	float b = dot(latticeGrad(p + ivec2(1, 0), period, seed), f - vec2(1.0, 0.0));
	float c = dot(latticeGrad(p + ivec2(0, 1), period, seed), f - vec2(0.0, 1.0));
	float d = dot(latticeGrad(p + ivec2(1, 1), period, seed), f - vec2(1.0, 1.0));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// fBm over the tile (uv in [0, 1) tiles): base period p0 cells, doubling.
float fbm(vec2 uv, int p0, int octaves, float gain, int seed)
{
	float s = 0.0, amp = 1.0, norm = 0.0;
	int p = p0;
	for (int k = 0; k < octaves; k++) {
		s += amp * gnoise(uv * float(p), p, seed + k * 101);
		norm += amp;
		amp *= gain;
		p *= 2;
	}
	return s / norm;
}

// Ridged fBm in [0, 1]: sharp crests, the shape of weathered rock.
float ridged(vec2 uv, int p0, int octaves, float gain, int seed)
{
	float s = 0.0, amp = 1.0, norm = 0.0;
	int p = p0;
	for (int k = 0; k < octaves; k++) {
		float n = 1.0 - abs(gnoise(uv * float(p), p, seed + k * 101)) * 1.4;
		s += amp * n * n;
		norm += amp;
		amp *= gain;
		p *= 2;
	}
	return s / norm;
}

float heightAt(vec2 uv)
{
	if (layer == 0) {
		// Rock: ridged blocks, tilted strata, fine grain.
		float r = ridged(uv, 3, 7, 0.55, 11);
		float warp = fbm(uv, 2, 3, 0.5, 21);
		float strata = 0.5 + 0.5 * sin(TAU * (uv.y * 9.0 + uv.x * 2.0 + 0.6 * warp));
		strata = smoothstep(0.2, 0.8, strata);
		return clamp(0.7 * r + 0.12 * strata + 0.18 * (0.5 + fbm(uv, 32, 3, 0.5, 31)), 0.0, 1.0);
	}
	if (layer == 1) {
		// Ground: soft rolling soil, clumps.
		float base = 0.5 + 0.7 * fbm(uv, 4, 7, 0.55, 41);
		float clumps = smoothstep(0.0, 0.5, fbm(uv, 16, 3, 0.5, 51));
		return clamp(0.8 * base + 0.2 * clumps, 0.0, 1.0);
	}
	if (layer == 2) {
		// Sand: wind ripples (steep lee side), warped, fine grain.
		float phase = uv.x * 12.0 + uv.y * 4.0 + 0.9 * fbm(uv, 2, 4, 0.5, 61);
		float r = fract(phase);
		float ripple = smoothstep(0.0, 0.8, r) * (1.0 - smoothstep(0.8, 1.0, r));
		float dunes = 0.5 + 0.6 * fbm(uv, 2, 3, 0.5, 71);
		return clamp(0.35 * ripple + 0.55 * dunes + 0.1 * (0.5 + fbm(uv, 64, 2, 0.5, 81)), 0.0, 1.0);
	}
	// Snow: smooth drifts and faint sastrugi.
	float drift = 0.5 + 0.6 * fbm(uv, 3, 5, 0.45, 91);
	float sastrugi = 0.5 + 0.5 * sin(TAU * (uv.x * 14.0 + uv.y * 2.0 + 0.7 * fbm(uv, 4, 3, 0.5, 97)));
	return clamp(0.85 * drift + 0.15 * sastrugi, 0.0, 1.0);
}

float albedoAt(vec2 uv, float h)
{
	if (layer == 0)   // rock: dark crevices, pale weathered tops, mineral speckle
		return 0.65 + 0.6 * (h - 0.5) + 0.25 * fbm(uv, 48, 3, 0.5, 13) + 0.15 * fbm(uv, 4, 3, 0.5, 17);
	if (layer == 1)   // ground: patchy cover, darker hollows
		return 0.9 + 0.35 * fbm(uv, 3, 4, 0.5, 43) + 0.2 * fbm(uv, 64, 2, 0.5, 47) + 0.2 * (h - 0.5);
	if (layer == 2)   // sand: lighter crests, coarse grains in the troughs
		return 0.97 + 0.12 * (h - 0.5) + 0.06 * fbm(uv, 128, 2, 0.5, 67);
	return 0.98 + 0.05 * fbm(uv, 8, 3, 0.5, 93);   // snow
}

void main()
{
	vec2 uv = gl_FragCoord.xy / size;
	float e = 1.0 / size;
	float h = heightAt(uv);
	float hx = (heightAt(uv + vec2(e, 0.0)) - heightAt(uv - vec2(e, 0.0))) / (2.0 * e);
	float hy = (heightAt(uv + vec2(0.0, e)) - heightAt(uv - vec2(0.0, e))) / (2.0 * e);
	Out = vec4(hx, hy, max(albedoAt(uv, h), 0.0), h);
}
