// Cloud noise (cloudmap.frag, clouds.frag): value noise from a lattice
// texture, FBM that fades its octaves out below a footprint, and the
// two-phase flow map that moves it along the wind.
//
// The lattice lives in a 2D RG texture (Clouds.c): R holds lattice slice z
// at (x + 37 z, y + 17 z), G the next slice at the same texel, so one
// bilinear fetch gives both z-neighbours and a mix in z finishes the
// trilinear interpolation (Quilez's 3D noise from a 2D texture). Sampling at
// the cell corner plus the smoothstep of the position in the cell makes it
// C1 value noise. A 2D fetch caches far better than a 3D one: the slab takes
// 18 of these per pixel. Indices wrap before the fraction is added, so large
// coordinates (planet metres over a short wavelength) keep their precision.

uniform sampler2D cloudNoiseTex;
uniform float cloudNoiseSize;     // texels per side; the lattice repeats

// Value noise in [0, 1], C1 across cells.
float cloudValueNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = p - i;
	f = f * f * (3.0 - 2.0 * f);
	vec2 cell = mod(i.xy + vec2(37.0, 17.0) * mod(i.z, cloudNoiseSize), cloudNoiseSize);
	vec2 rg = textureLod(cloudNoiseTex, (cell + f.xy + 0.5) / cloudNoiseSize, 0.0).rg;
	return mix(rg.r, rg.g, f.z);
}

// Gradient noise in [0, 1] (Perlin's, with hashed gradients: pcg3d, Jarzynski
// and Olano, "Hash Functions for GPU Rendering", JCGT 2020). About eight
// times the cost of the value noise, but its extremes are round where the
// value noise's sit on the lattice: the weather maps (built a couple of
// times per frame at most) threshold their tails, and value noise showed
// there as square holes. Selected with CLOUD_NOISE before this include.
uvec3 pcg3d(uvec3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	return v;
}

float cornerDot(ivec3 c, vec3 f)
{
	vec3 g = vec3(pcg3d(uvec3(c)) >> 8u) * (2.0 / 16777215.0) - 1.0;
	return dot(g, f);
}

float cloudGradientNoise(vec3 p)
{
	vec3 fl = floor(p);
	ivec3 i = ivec3(fl);
	vec3 f = p - fl;
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float v = mix(mix(mix(cornerDot(i,                 f),
	                      cornerDot(i + ivec3(1, 0, 0), f - vec3(1, 0, 0)), u.x),
	                  mix(cornerDot(i + ivec3(0, 1, 0), f - vec3(0, 1, 0)),
	                      cornerDot(i + ivec3(1, 1, 0), f - vec3(1, 1, 0)), u.x), u.y),
	              mix(mix(cornerDot(i + ivec3(0, 0, 1), f - vec3(0, 0, 1)),
	                      cornerDot(i + ivec3(1, 0, 1), f - vec3(1, 0, 1)), u.x),
	                  mix(cornerDot(i + ivec3(0, 1, 1), f - vec3(0, 1, 1)),
	                      cornerDot(i + ivec3(1, 1, 1), f - vec3(1, 1, 1)), u.x), u.y), u.z);
	// Scaled to about the value noise's spread around 0.5.
	return clamp(0.5 + 0.8 * v, 0.0, 1.0);
}

#ifndef CLOUD_NOISE
#define CLOUD_NOISE cloudValueNoise
#endif

// Octave step: doubles the frequency and turns the lattice, so octaves don't
// line up along the axes (a scaled rotation, |det| = 8).
const mat3 CLOUD_OCTAVE = mat3( 0.00,  1.60,  1.20,
                               -1.60,  0.72, -0.96,
                               -1.20, -0.96,  1.28);

// FBM over `octaves`, the first at wavelength lambda0 (p in the same units),
// gain 0.5. An octave fades to the noise's mean (0.5) as its wavelength
// shrinks to `foot` (a pixel or a map texel), and is skipped below that, so
// the result keeps its mean and range at any distance. Normalised to [0, 1].
float cloudFbm(vec3 p, float lambda0, float foot, int octaves)
{
	vec3 q = p / lambda0;
	float lambda = lambda0, amp = 1.0, sum = 0.0, norm = 0.0;
	for (int k = 0; k < octaves; k++) {
		float fade = 1.0 - smoothstep(1.0, 2.0, foot / lambda);
		float n = fade > 0.0 ? mix(0.5, CLOUD_NOISE(q), fade) : 0.5;
		sum += amp * n;
		norm += amp;
		q = CLOUD_OCTAVE * q + vec3(17.3, -5.1, 9.7);
		lambda *= 0.5;
		amp *= 0.5;
	}
	return octaves > 0 ? sum / norm : 0.5;   // no octaves: the mean
}

// Two-phase flow map (Vlachos, "Water Flow in Portal 2", SIGGRAPH 2010): two
// copies of the noise, each displaced by wind x tau, tau cycling over the
// flow period half a cycle apart, blended by triangle weights. Each cycle
// re-seeds its copy (seed offsets in lattice units), so the pattern never
// repeats, and nothing is integrated, so a latitude-varying wind never shears
// it apart. Computed on the CPU in double (Clouds.c).
uniform vec2 cloudFlowTau;      // seconds into each phase's cycle
uniform vec2 cloudFlowWeight;   // triangle weights, sum 1
uniform vec3 cloudFlowSeed0;
uniform vec3 cloudFlowSeed1;

// Blends the two phases' values (both in [0, 1], mean 0.5) and restores the
// contrast the average loses mid-way, so coverage does not pulse.
float cloudFlowBlend(float n0, float n1)
{
	vec2 w = cloudFlowWeight;
	return 0.5 + (w.x * (n0 - 0.5) + w.y * (n1 - 0.5)) / max(length(w), 1e-3);
}

// East and north at body-fixed direction d (pole = body +Z), and the wind
// vector there (m/s) from a wind map texel (TerrainGen.c's ocean map: R =
// speed / windMax, G, B = east, north components * 0.5 + 0.5).
vec3 cloudWindAt(vec3 d, vec4 texel, float windMax)
{
	vec3 east = cross(vec3(0.0, 0.0, 1.0), d);
	float le = length(east);
	east = le > 1e-4 ? east / le : vec3(1.0, 0.0, 0.0);
	vec3 north = cross(d, east);
	return (east * (texel.g * 2.0 - 1.0) + north * (texel.b * 2.0 - 1.0)) * texel.r * windMax;
}
