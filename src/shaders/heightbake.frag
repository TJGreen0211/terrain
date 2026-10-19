#version 410

// Bakes the planet's shaped height field into an equirectangular map once at
// startup (and on every re-bake); TerrainGen.c reads it back, runs thermal +
// hydraulic erosion on the CPU, then uploads the final R16F + mip chain that
// quad.frag/water.frag sample. The UV convention matches the runtime samples
// (water.vert wave UV, quad.frag heightAt): u = (atan(y, x)/PI + 1.0)*0.5,
// v = asin(z)/PI + 0.5.
//
// Shaping recipe (all noise sampled on the unit sphere -> seam-free):
//   continent mask -> 2-level domain warp -> warped fBm base,
//   ridged mountains gated by a localized mask band, carved ocean basins,
//   masked plateau quantize, high-freq ridged detail.
// Output: raw h (~[-1,1] scale) in BakeHeight; normalization and the sea
// level happen on the CPU after erosion.

in vec2 fUV;
out float BakeHeight;

uniform float seed;

const float PI = 3.14159265359;

/* ---- Shaping recipe (tweak here) ---- */
#define FREQ_CONTINENT  1.25   // continent mask: few, broad features
#define OCT_CONTINENT   2
#define MASK_WARP       0.35   // mask only lightly warped (full warp shreds it)
#define CONT_SCALE      3.0    // mask dominance: keeps continents connected
#define LAND_RAMP_LO   -0.35   // coast ramp around sea level (~0): the width
#define LAND_RAMP_HI    0.35   // is the continental shelf / coastal slope
#define WARP_FREQ       2.4
#define WARP_OCT        2
#define WARP_AMT        0.45   // relief warp: each level shifts by WARP_AMT*fbm3
#define FREQ_TERRAIN    2.6
#define OCT_TERRAIN     5
#define TERRAIN_AMT     0.6    // land relief riding on the continent mask
#define FREQ_MOUNT      2.0
#define OCT_MOUNT       5
#define MOUNT_EXP       2.5    // ridged multifractal exponent
#define MOUNT_AMT       1.1
#define FREQ_BAND       1.35   // mountain region noise
#define FREQ_BASIN      2.4
#define OCT_BASIN       3
#define BASIN_AMT       0.35   // seafloor relief inside ocean basins
#define FREQ_PLATEAU    1.1
#define OCT_PLATEAU     2
#define PLATEAU_LEVELS  4.0    // vertical quantize resolution
#define PLATEAU_AMT     0.35
#define FREQ_DETAIL     13.0   // ~5x the terrain frequency
#define OCT_DETAIL      3
#define DETAIL_AMT      0.10   // ~8 % of the relief, peak-to-peak

// 3D simplex noise (Ashima Arts / I. McEwan, texture-free)
vec3 mod289v(vec3 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 mod289v(vec4 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289v(((x*34.0)+1.0)*x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

float snoise(vec3 v) {
	const vec2 C = vec2(1.0/6.0, 1.0/3.0);
	const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);
	vec3 i  = floor(v + dot(v, C.yyy));
	vec3 x0 = v - i + dot(i, C.xxx);
	vec3 g = step(x0.yzx, x0.xyz);
	vec3 l = 1.0 - g;
	vec3 i1 = min(g.xyz, l.zxy);
	vec3 i2 = max(g.xyz, l.zxy);
	vec3 x1 = x0 - i1 + C.xxx;
	vec3 x2 = x0 - i2 + C.yyy;
	vec3 x3 = x0 - D.yyy;
	i = mod289v(i);
	vec4 p = permute(permute(permute(
			i.z + vec4(0.0, i1.z, i2.z, 1.0))
			+ i.y + vec4(0.0, i1.y, i2.y, 1.0))
			+ i.x + vec4(0.0, i1.x, i2.x, 1.0));
	float n_ = 0.142857142857;
	vec3 ns = n_ * D.wyz - D.xzx;
	vec4 j = p - 49.0 * floor(p * ns.z * ns.z);
	vec4 x_ = floor(j * ns.z);
	vec4 y_ = floor(j - 7.0 * x_);
	vec4 x = x_ * ns.x + ns.yyyy;
	vec4 y = y_ * ns.x + ns.yyyy;
	vec4 h = 1.0 - abs(x) - abs(y);
	vec4 b0 = vec4(x.xy, y.xy);
	vec4 b1 = vec4(x.zw, y.zw);
	vec4 s0 = floor(b0)*2.0 + 1.0;
	vec4 s1 = floor(b1)*2.0 + 1.0;
	vec4 sh = -step(h, vec4(0.0));
	vec4 a0 = b0.xzyw + s0.xzyw*sh.xxyy;
	vec4 a1 = b1.xzyw + s1.xzyw*sh.zzww;
	vec3 p0 = vec3(a0.xy, h.x);
	vec3 p1 = vec3(a0.zw, h.y);
	vec3 p2 = vec3(a1.xy, h.z);
	vec3 p3 = vec3(a1.zw, h.w);
	vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));
	p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
	vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
	m = m*m;
	return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}

float fbm(vec3 p, float frq, int oct) {
	float v = 0.0;
	float amp = 0.5;
	for (int i = 0; i < oct; i++) {
		v += amp * snoise(p * frq);
		frq *= 2.0;
		amp *= 0.5;
	}
	return v;
}

float ridged(vec3 p, float frq, int oct, float k) {
	float v = 0.0;
	float amp = 0.5;
	for (int i = 0; i < oct; i++) {
		float n = 1.0 - abs(snoise(p * frq));
		v += amp * pow(n, k);
		frq *= 2.0;
		amp *= 0.5;
	}
	return v;
}

// Vector-valued warp field (three decorrelated fbm samples).
vec3 warp3(vec3 p) {
	return vec3(
		fbm(p + vec3(11.3,  5.7,  8.1), WARP_FREQ, WARP_OCT),
		fbm(p + vec3(27.7, 19.2, 33.4), WARP_FREQ, WARP_OCT),
		fbm(p + vec3( 7.9, 41.6, 13.2), WARP_FREQ, WARP_OCT));
}

void main()
{
	float lon = (fUV.x*2.0 - 1.0)*PI;
	float lat = (fUV.y - 0.5)*PI;
	vec3 d = normalize(vec3(cos(lat)*cos(lon), cos(lat)*sin(lon), sin(lat)));
	// Simplex is unseeded: the seed shifts the whole planet into a new
	// domain region instead of re-wiring the noise.
	vec3 p = d + vec3(seed*11.7, seed*91.3, seed*47.9);

	// Two-level domain warp for the relief: the fix for the blobby simplex
	// look. The second level's amplitude is modulated by the first
	// (coherent curls). The mask samples a lightly warped point so coasts
	// gain curves without the continent topology being shredded.
	vec3 w1 = warp3(p);
	vec3 q = p + WARP_AMT*w1;
	vec3 w2 = warp3(q + vec3(37.1, 53.7, 29.3));
	q += WARP_AMT*(0.25 + 0.75*abs(w1.x))*w2;

	// Continent/ocean mask dominates the elevation: scaled well past the
	// detail relief so the sea-level threshold sits in the gap between
	// ocean and land instead of cutting through every noise octave. That
	// keeps continents broad and connected; the octaves only roughen the
	// coastline.
	float m = fbm(p + MASK_WARP*w1, FREQ_CONTINENT, OCT_CONTINENT);
	float h = m * CONT_SCALE;
	float land01 = smoothstep(LAND_RAMP_LO, LAND_RAMP_HI, h);
	float ocean01 = 1.0 - land01;

	// Land relief: warped fBm base, plus ridged mountain ranges gated by a
	// low-frequency mask band so crests form localized chains.
	float terrain = fbm(q, FREQ_TERRAIN, OCT_TERRAIN) * TERRAIN_AMT;
	float band = smoothstep(0.05, 0.45, snoise(q*FREQ_BAND + vec3(61.0, 13.0, 37.0)));
	float mountain = ridged(q, FREQ_MOUNT, OCT_MOUNT, MOUNT_EXP);
	h += land01 * (terrain + band * mountain * MOUNT_AMT);

	// Plateau shaping: masked vertical quantize (mesas/terraces), land only.
	float plateauMask = smoothstep(0.15, 0.45, fbm(q + vec3(83.0, 7.0, 59.0), FREQ_PLATEAU, OCT_PLATEAU)) * land01;
	float quant = floor(h*PLATEAU_LEVELS + 0.5) / PLATEAU_LEVELS;
	h = mix(h, quant, plateauMask * PLATEAU_AMT);

	// Seafloor: its own fBm so the depth tint in water.frag has real range.
	h += ocean01 * fbm(q, FREQ_BASIN, OCT_BASIN) * BASIN_AMT;

	// High-frequency ridged detail (rock texture), amplitude capped well
	// below the shaped relief so it never dominates the ranges.
	float detail = ridged(q, FREQ_DETAIL, OCT_DETAIL, 1.5);
	h += (detail - 0.5) * DETAIL_AMT * (0.25 + 0.75*land01);

	BakeHeight = h;
}
