#version 410

// Bakes the planet's shaped height field into the six faces of a cube map
// once at startup (and on every re-bake); terrain_gen.c reads it back, runs
// thermal + hydraulic erosion on the CPU, then uploads the final cube map +
// mip chain that the body shaders sample by direction (planetmap.glsl). The
// faces are stacked bottom to top in one faceSize x 6*faceSize target, in GL
// cube-map order and orientation (terrain_gen.c faceM/S/T).
//
// Shaping recipe: a plate-tectonics sketch instead of thresholded noise
// (which always yields noodly fractal continents).
//   - Plates: warped spherical Voronoi over the CPU-generated plate centres
//     (terrain_gen.c). ~40% of the area is continental crust; continents are
//     plate-sized and chunky, noise only roughens their coasts.
//   - Each plate rotates about its own pole, so every boundary is
//     convergent or divergent:
//       continent/continent converging -> plates merge, suture range +
//                                         high plateau behind (Himalaya/Tibet)
//       continent/ocean converging     -> active margin, coastal range (Andes)
//       diverging / passive            -> continent pulls back from the
//                                         boundary, shelf + open ocean (Atlantic)
//       ocean/ocean                    -> mid-ocean ridges, island arcs
//   - Continental interiors: low rolling plains (Midwest), patches of
//     raised flat plateaus with scarp edges (Colorado/Mongolia), worn-down
//     old ranges and hill country.
// Sea level is raw height 0 (terrain_gen.c keeps it through normalization),
// so the coastline is wherever the crust says, never a percentile slicing
// through a plain.
// Output: raw h in BakeOut.r; BakeOut.g carries a low-frequency climate
// anomaly (~[-0.5,0.5]) that terrain_gen.c folds into the moisture map.

in vec2 fUV;
out vec4 BakeOut;

uniform float seed;
uniform int faceSize;
#define MAX_PLATES 32
uniform int plateCount;
uniform vec4 plates[MAX_PLATES];       // xyz = centre (unit), w = 1 continental
uniform vec4 plateMotion[MAX_PLATES];  // xyz = angular velocity, w = passive margin (rad)

const float PI = 3.14159265359;

/* ---- Shaping recipe; distances are radians on the sphere. The uniforms are
   TerrainBakeSettings (terrain_gen.c, debug panel); defaults noted inline.
   The #defines are fixed here. Two kinds of distance (docs/terrain-relief.md
   section 0): the plate layout (plates, coasts, shelves, oceanic features)
   scales with the sphere; the land's ground-scale features (range widths,
   the ridges in them, hills, rock texture) are set in metres in
   WorldConfig.h and arrive here converted for this planet (upload.c), as
   radians (widths) or frequencies in 1 / radius (noise), with octave counts
   that stop at two map texels. ---- */
#define PLATE_WARP      0.28   // boundary waviness (warp of the Voronoi lookup)
#define PLATE_WARP_FREQ 1.3
uniform float COAST_FREQ;      // coastline roughness: bays, peninsulas, islands [default 2.2]
#define COAST_OCT       5
uniform float COAST_AMT;       // coastline roughness amplitude [default 0.12]
#define COAST_BLEND     0.035  // land/ocean transition width
#define SHELF_WIDTH     0.10   // continental shelf + slope beyond the coast
#define CONV_FULL       0.6    // convergence speed counted as fully active

uniform float PLAINS_H;        // continental interior base height [default 0.06]
#define PLAINS_NOISE    0.035  // gentle rolling relief on the plains
uniform float HILL_AMT;        // hill country relief where the hill mask allows [default 0.10]
uniform float PLATEAU_H;       // raised flat plateaus [default 0.30]
#define PLATEAU_LO      0.16   // plateau mask noise band (lower = more plateaus)
#define PLATEAU_HI      0.22   //   narrow band -> steep scarps, flat tops
#define TIBET_H         0.30   // extra plateau behind continental collisions
uniform float TIBET_WIDTH;     // rad (TERRAIN_TIBET_WIDTH_M)
uniform float COLLIDE_H;       // suture range height (continent/continent) [default 1.30]
uniform float COLLIDE_WIDTH;   // rad (TERRAIN_COLLIDE_WIDTH_M)
uniform float ANDES_H;         // coastal range height (continent over ocean) [default 0.95]
uniform float ANDES_WIDTH;     // rad (TERRAIN_ANDES_WIDTH_M)
uniform float OLD_RANGE_H;     // eroded intraplate ranges (Appalachians/Urals) [default 0.35]
uniform float FREQ_MOUNT;      // the ranges' ridges (TERRAIN_MOUNT_WAVELENGTH_M)
uniform int OCT_MOUNT;
uniform float GAIN_MOUNT;      // their amplitude per octave (TERRAIN_MOUNT_GAIN)
uniform float FREQ_OLD;        // old ranges (TERRAIN_OLD_RANGE_WAVELENGTH_M)
uniform int OCT_OLD;
uniform float FREQ_HILL;       // hill country (TERRAIN_HILL_WAVELENGTH_M)
uniform int OCT_HILL;

uniform float OCEAN_BASE;      // abyssal plain depth [default -0.70]
#define OCEAN_NOISE     0.10
#define SHELF_H        -0.08   // shelf depth at the coast
uniform float RIDGE_H;         // mid-ocean ridge rise (divergent ocean/ocean) [default 0.28]
#define RIDGE_WIDTH     0.05
#define TRENCH_H       -0.30   // subduction trench (ocean side)
#define TRENCH_WIDTH    0.025
#define ARC_H           2.20   // island arcs (ocean/ocean convergent); only
#define ARC_WIDTH       0.035  //   the ridged crests break the surface
#define HOTSPOT_FREQ    7.0    // volcanic cones in the open ocean (Hawaii)
#define HOTSPOT_H       1.30
uniform float HOTSPOT_LO;      // cone noise threshold: higher = rarer islands [default 0.50]
#define SHELF_ISLE_H    0.35   // shelf islands (British Isles, Sri Lanka)
#define SHELF_ISLE_LO   0.05   // shelf bump noise threshold: higher = fewer
#define FRAG_FREQ       2.3    // microcontinent fragments on oceanic crust
uniform float FRAG_LO;         //   (Madagascar / New Zealand) threshold; higher = fewer [default 0.42]

// Land roughness (BakeOut.b, km; bake_job.c adds it in metres): the
// land's roughness spectrum (TERRAIN_ROUGH_*, shared with the detail noise
// below the map, detail.glsl) from TERRAIN_ROUGH_MAP_WAVELENGTH_M down to
// two map texels.
uniform float FREQ_ROUGH;      // first octave's frequency (1 / radius)
uniform int OCT_ROUGH;
uniform float ROUGH_L0;        // first octave's wavelength / 10 km
uniform vec2 ROUGH_A10;        // amplitude at a 10 km wavelength (km): plains, mountains
uniform vec2 ROUGH_H;          // amplitude ~ wavelength^H: plains, mountains
uniform vec2 ROUGH_KNEE;       // knee wavelength / 10 km: plains, mountains
uniform float ROUGH_H_LONG;    // amplitude ~ wavelength^H_LONG above the knee
uniform float DETAIL_AMT;      // x the roughness [default 1.0]
#define FREQ_ANOMALY    1.7    // climate anomaly: continent-scale wet/dry regions
#define OCT_ANOMALY     3

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

// Ridged noise with amplitude gain per octave (0.5: the classic sum,
// higher: rougher at short wavelengths), normalized so its range does not
// grow with the octave count or the gain.
float ridgedG(vec3 p, float frq, int oct, float k, float gain) {
	float v = 0.0, norm = 0.0;
	float amp = 0.5;
	for (int i = 0; i < oct; i++) {
		float n = 1.0 - abs(snoise(p * frq));
		v += amp * pow(n, k);
		norm += amp;
		frq *= 2.0;
		amp *= gain;
	}
	return v / max(norm, 1e-6);
}

float ridged(vec3 p, float frq, int oct, float k) {
	return ridgedG(p, frq, oct, k, 0.5);
}

// The land's roughness (km) at ruggedness r (0 plains .. 1 mountains):
// octaves of amplitude A10 (lambda / 10 km)^H up to the knee, growing as
// lambda^H_LONG above it; A10 and the knee between the plains' and the
// mountains' (geometrically), H linearly. Soft noise on plains, ridged
// (sharp crests, V valleys) in mountains.
float roughness(vec3 p, float r) {
	float a10 = exp(mix(log(ROUGH_A10.x), log(ROUGH_A10.y), r));
	float H = mix(ROUGH_H.x, ROUGH_H.y, r);
	float knee = exp(mix(log(ROUGH_KNEE.x), log(ROUGH_KNEE.y), r));
	float l = ROUGH_L0, frq = FREQ_ROUGH, v = 0.0;
	for (int i = 0; i < OCT_ROUGH; i++) {
		float amp = a10 * pow(min(l, knee), H) * pow(max(l, knee) / knee, ROUGH_H_LONG);
		float n = snoise(p * frq + vec3(31.0, 47.0, 11.0) * float(i));
		v += amp * mix(n, 1.0 - 2.0 * abs(n), r);
		frq *= 2.0;
		l *= 0.5;
	}
	return v;
}

// Vector-valued warp field (three decorrelated fbm samples).
vec3 warp3(vec3 p, float frq) {
	return vec3(
		fbm(p + vec3(11.3,  5.7,  8.1), frq, 3),
		fbm(p + vec3(27.7, 19.2, 33.4), frq, 3),
		fbm(p + vec3( 7.9, 41.6, 13.2), frq, 3));
}

// Gaussian band around a boundary: 1 on it, ~0 beyond `width`.
float band(float dist, float width) {
	float t = dist / width;
	return exp(-t*t);
}

// Direction through face point (sc, tc) in [-1, 1]^2 of GL cube face f
// (+X, -X, +Y, -Y, +Z, -Z): the spec's major-axis table, inverted.
vec3 cubeFaceDir(int f, vec2 st)
{
	if (f == 0) return vec3( 1.0, -st.y, -st.x);
	if (f == 1) return vec3(-1.0, -st.y,  st.x);
	if (f == 2) return vec3( st.x,  1.0,  st.y);
	if (f == 3) return vec3( st.x, -1.0, -st.y);
	if (f == 4) return vec3( st.x, -st.y,  1.0);
	return vec3(-st.x, -st.y, -1.0);
}

void main()
{
	int face = int(gl_FragCoord.y) / faceSize;
	vec2 st = vec2(gl_FragCoord.x, gl_FragCoord.y - float(face * faceSize)) / float(faceSize) * 2.0 - 1.0;
	vec3 d = normalize(cubeFaceDir(face, st));
	// Simplex is unseeded: the seed shifts the noise into a new domain
	// region (hashed into a small box so float precision holds at the
	// detail frequencies).
	vec3 p = d + fract(seed*vec3(0.1031, 0.1137, 0.1379))*97.0 + vec3(17.0);

	// ---- Plates: two nearest centres of the warped lookup direction.
	vec3 q = normalize(d + PLATE_WARP * warp3(p, PLATE_WARP_FREQ));
	int i0 = 0, i1 = 0;
	float b0 = -2.0, b1 = -2.0;
	for (int k = 0; k < MAX_PLATES; k++) {
		if (k >= plateCount) break;
		float c = dot(q, plates[k].xyz);
		if (c > b0) { b1 = b0; i1 = i0; b0 = c; i0 = k; }
		else if (c > b1) { b1 = c; i1 = k; }
	}
	vec3 cA = plates[i0].xyz, cB = plates[i1].xyz;
	// Distance (radians, approx) to the bisector of the two cells.
	float bdist = (b0 - b1) / max(length(cA - cB), 1e-4);
	float contA = plates[i0].w, contB = plates[i1].w;

	// Boundary kinematics: relative surface velocity along the normal
	// pointing from this plate toward the neighbour (>0 = converging).
	vec3 nrm = cB - cA;
	nrm = normalize(nrm - dot(nrm, q) * q + 1e-6);
	vec3 vA = cross(plateMotion[i0].xyz, q);
	vec3 vB = cross(plateMotion[i1].xyz, q);
	float conv = clamp(dot(vA - vB, nrm) / CONV_FULL, -1.0, 1.0);
	float convergent = smoothstep(0.0, 0.35, conv);
	float divergent  = smoothstep(0.0, 0.35, -conv);

	// ---- Continental crust: the continent pulls back from passive /
	// divergent boundaries (margin), reaches the boundary at subduction
	// zones and spills across it in collisions (both sides merge).
	// Evaluated for both plates (signed distance into each) and combined
	// with max(), so the result is continuous across the bisector.
	float coastNoise = COAST_AMT * fbm(p + vec3(3.1, 9.4, 1.7), COAST_FREQ, COAST_OCT);
	float mA = mix(plateMotion[i0].w, contB > 0.5 ? -0.08 : 0.0, convergent);
	float mB = mix(plateMotion[i1].w, contA > 0.5 ? -0.08 : 0.0, convergent);
	float coastA =  bdist - mA + coastNoise;
	float coastB = -bdist - mB + coastNoise;
	float land  = max(contA * smoothstep(-COAST_BLEND, COAST_BLEND, coastA),
	                  contB * smoothstep(-COAST_BLEND, COAST_BLEND, coastB));
	// Shelf profile: shallow and nearly flat for most of its width, then
	// the continental slope drops to the abyss (so shelf bumps surface as
	// separate offshore islands instead of merging into the coast).
	float shelf = max(contA * smoothstep(-SHELF_WIDTH, -SHELF_WIDTH * 0.55, coastA),
	                  contB * smoothstep(-SHELF_WIDTH, -SHELF_WIDTH * 0.55, coastB));

	// Microcontinents: rare continental fragments out on oceanic crust,
	// kept clear of the mainland shelf; they get full land terrain + shelf.
	float fragN = fbm(p + vec3(91.0, 23.0, 57.0), FRAG_FREQ, 4) + 0.5 * coastNoise;
	float fragOk = 1.0 - max(contA, contB) * smoothstep(-SHELF_WIDTH * 2.5, -SHELF_WIDTH, max(coastA, coastB));
	land  = max(land,  fragOk * smoothstep(FRAG_LO, FRAG_LO + 0.03, fragN));
	shelf = max(shelf, fragOk * smoothstep(FRAG_LO - 0.10, FRAG_LO - 0.04, fragN));

	// ---- Continental elevation.
	// Plains: low and gently rolling, the default for interiors.
	float h_land = PLAINS_H + PLAINS_NOISE * fbm(p, 3.0, 4);
	// Hill country in patches.
	float hillMask = smoothstep(0.05, 0.35, fbm(p + vec3(51.0, 2.0, 17.0), 1.6, 2));
	h_land += hillMask * HILL_AMT * (0.5 + fbm(p + vec3(8.0, 31.0, 4.0), FREQ_HILL, OCT_HILL));
	// Plateaus: narrow noise band -> flat tops with scarp edges.
	float plateau = smoothstep(PLATEAU_LO, PLATEAU_HI, fbm(p + vec3(83.0, 7.0, 59.0), 1.2, 3));
	// Tibet: broad uplift behind continental collisions.
	float collide = convergent * contA * contB;
	float tibet = collide * smoothstep(TIBET_WIDTH, TIBET_WIDTH * 0.35, bdist);
	plateau = max(plateau, tibet);
	h_land += plateau * PLATEAU_H + tibet * TIBET_H;
	h_land += plateau * 0.03 * fbm(p + vec3(4.0, 4.0, 90.0), 4.0, 3);   // keep tops nearly flat
	// Mountain belts along active boundaries only.
	float mount = ridgedG(p, FREQ_MOUNT, OCT_MOUNT, 2.0, GAIN_MOUNT);
	// The coastal range sits just inland of an active margin, on whichever
	// side is continental (both terms keep it continuous at the bisector).
	float andes = contA * (1.0 - contB) * band(bdist - ANDES_WIDTH, ANDES_WIDTH)
	            + contB * (1.0 - contA) * band(-bdist - ANDES_WIDTH, ANDES_WIDTH);
	float belt = collide * band(bdist, COLLIDE_WIDTH) * COLLIDE_H
	           + convergent * andes * ANDES_H;
	h_land += belt * mount;
	// Old worn ranges inside the plates, low and rounded.
	float oldMask = smoothstep(0.25, 0.50, snoise(p * 1.4 + vec3(61.0, 13.0, 37.0)));
	h_land += oldMask * OLD_RANGE_H * ridged(p + vec3(19.0, 3.0, 7.0), FREQ_OLD, OCT_OLD, 1.2);

	// ---- Oceanic elevation.
	float h_ocean = OCEAN_BASE + OCEAN_NOISE * fbm(p + vec3(29.0, 71.0, 5.0), 2.4, 4);
	float oceanPair = (1.0 - contA) * (1.0 - contB);
	h_ocean += oceanPair * divergent * band(bdist, RIDGE_WIDTH) * RIDGE_H;
	// Trench along a subduction zone (only shows where the crust is oceanic).
	h_ocean += (contA + contB - 2.0 * contA * contB) * convergent * band(bdist, TRENCH_WIDTH) * TRENCH_H;
	// Island arcs along ocean/ocean convergence: squared ridged noise so
	// only the crests surface, segmented into separate islands.
	float arcR = ridged(p + vec3(7.0, 77.0, 3.0), 4.0, 3, 2.0);
	float arcSeg = smoothstep(-0.2, 0.3, snoise(p * 3.0 + vec3(41.0, 5.0, 66.0)));
	h_ocean += oceanPair * convergent * band(bdist, ARC_WIDTH) * ARC_H * pow(arcR, 1.5) * arcSeg;
	// Hotspot volcanoes: sparse cones in the deep ocean, clustered into
	// chains by a low-frequency mask; the tallest break the surface.
	float cone = smoothstep(HOTSPOT_LO, 0.80, snoise(p * HOTSPOT_FREQ + vec3(13.0, 29.0, 3.0)));
	float chain = smoothstep(0.1, 0.45, snoise(p * 1.2 + vec3(71.0, 17.0, 47.0)));
	h_ocean += (1.0 - shelf) * cone * chain * HOTSPOT_H;
	// Continental shelf and slope rise toward the coast.
	h_ocean = mix(h_ocean, SHELF_H, shelf);
	// Shelf islands: bumps on the shallow shelf that clear sea level, a
	// little offshore of the mainland.
	float isle = smoothstep(SHELF_ISLE_LO, 0.35, fbm(p + vec3(2.0, 61.0, 38.0), 4.0, 4));
	h_ocean += shelf * isle * SHELF_ISLE_H;

	float h = mix(h_ocean, h_land, land);

	// Land roughness, in km: the ruggedness from the shaper's own masks
	// (ranges, hill country, old ranges, plateaus) and from height (high
	// ground is broken ground: range flanks, uplands), on land only.
	float rugged = clamp(belt * 1.5 + hillMask * 0.6 + oldMask * 0.6 + plateau * 0.5
	                     + 0.6 * smoothstep(0.25, 0.8, h_land), 0.0, 1.0);
	float rough = DETAIL_AMT * land * roughness(p + vec3(23.0, 5.0, 61.0), rugged);

	float anomaly = fbm(p + vec3(5.3, 71.9, 23.1), FREQ_ANOMALY, OCT_ANOMALY);
	BakeOut = vec4(h, anomaly, rough, 1.0);
}
