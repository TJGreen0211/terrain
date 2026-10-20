#version 410

// Bakes the planet's shaped height field into an equirectangular map once at
// startup (and on every re-bake); TerrainGen.c reads it back, runs thermal +
// hydraulic erosion on the CPU, then uploads the final R16F + mip chain that
// quad.frag/water.frag sample. The UV convention matches the runtime samples
// (water.vert wave UV, quad.frag heightAt): u = (atan(y, x)/PI + 1.0)*0.5,
// v = asin(z)/PI + 0.5.
//
// Shaping recipe: a plate-tectonics sketch instead of thresholded noise
// (which always yields noodly fractal continents).
//   - Plates: warped spherical Voronoi over the CPU-generated plate centres
//     (TerrainGen.c). ~40% of the area is continental crust; continents are
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
// Sea level is raw height 0 (TerrainGen.c keeps it through normalization),
// so the coastline is wherever the crust says, never a percentile slicing
// through a plain.
// Output: raw h in BakeOut.r; BakeOut.g carries a low-frequency climate
// anomaly (~[-0.5,0.5]) that TerrainGen.c folds into the moisture map.

in vec2 fUV;
out vec4 BakeOut;

uniform float seed;
#define MAX_PLATES 32
uniform int plateCount;
uniform vec4 plates[MAX_PLATES];       // xyz = centre (unit), w = 1 continental
uniform vec4 plateMotion[MAX_PLATES];  // xyz = angular velocity, w = passive margin (rad)

const float PI = 3.14159265359;

/* ---- Shaping recipe (tweak here); distances are radians on the sphere ---- */
#define PLATE_WARP      0.28   // boundary waviness (warp of the Voronoi lookup)
#define PLATE_WARP_FREQ 1.3
#define COAST_FREQ      2.2    // coastline roughness: bays, peninsulas, islands
#define COAST_OCT       5
#define COAST_AMT       0.12
#define COAST_BLEND     0.035  // land/ocean transition width
#define SHELF_WIDTH     0.10   // continental shelf + slope beyond the coast
#define CONV_FULL       0.6    // convergence speed counted as fully active

#define PLAINS_H        0.06   // continental interior base height
#define PLAINS_NOISE    0.035  // gentle rolling relief on the plains
#define HILL_AMT        0.10   // hill country relief where the hill mask allows
#define PLATEAU_H       0.30   // raised flat plateaus
#define PLATEAU_LO      0.16   // plateau mask noise band (lower = more plateaus)
#define PLATEAU_HI      0.22   //   narrow band -> steep scarps, flat tops
#define TIBET_H         0.30   // extra plateau behind continental collisions
#define TIBET_WIDTH     0.22
#define COLLIDE_H       1.30   // suture range height (continent/continent)
#define COLLIDE_WIDTH   0.09
#define ANDES_H         0.95   // coastal range height (continent over ocean)
#define ANDES_WIDTH     0.075
#define OLD_RANGE_H     0.35   // eroded intraplate ranges (Appalachians/Urals)
#define FREQ_MOUNT      2.6
#define OCT_MOUNT       5

#define OCEAN_BASE     -0.70   // abyssal plain depth
#define OCEAN_NOISE     0.10
#define SHELF_H        -0.08   // shelf depth at the coast
#define RIDGE_H         0.28   // mid-ocean ridge rise (divergent ocean/ocean)
#define RIDGE_WIDTH     0.05
#define TRENCH_H       -0.30   // subduction trench (ocean side)
#define TRENCH_WIDTH    0.025
#define ARC_H           2.20   // island arcs (ocean/ocean convergent); only
#define ARC_WIDTH       0.035  //   the ridged crests break the surface
#define HOTSPOT_FREQ    7.0    // volcanic cones in the open ocean (Hawaii)
#define HOTSPOT_H       1.30
#define HOTSPOT_LO      0.50   // cone noise threshold: higher = rarer islands
#define SHELF_ISLE_H    0.35   // shelf islands (British Isles, Sri Lanka)
#define SHELF_ISLE_LO   0.05   // shelf bump noise threshold: higher = fewer
#define FRAG_FREQ       2.3    // microcontinent fragments on oceanic crust
#define FRAG_LO         0.42   //   (Madagascar / New Zealand); higher = fewer

#define FREQ_DETAIL     13.0   // high-frequency rock texture
#define OCT_DETAIL      3
#define DETAIL_AMT      0.06
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

void main()
{
	float lon = (fUV.x*2.0 - 1.0)*PI;
	float lat = (fUV.y - 0.5)*PI;
	vec3 d = normalize(vec3(cos(lat)*cos(lon), cos(lat)*sin(lon), sin(lat)));
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
	h_land += hillMask * HILL_AMT * (0.5 + fbm(p + vec3(8.0, 31.0, 4.0), 5.0, 4));
	// Plateaus: narrow noise band -> flat tops with scarp edges.
	float plateau = smoothstep(PLATEAU_LO, PLATEAU_HI, fbm(p + vec3(83.0, 7.0, 59.0), 1.2, 3));
	// Tibet: broad uplift behind continental collisions.
	float collide = convergent * contA * contB;
	float tibet = collide * smoothstep(TIBET_WIDTH, TIBET_WIDTH * 0.35, bdist);
	plateau = max(plateau, tibet);
	h_land += plateau * PLATEAU_H + tibet * TIBET_H;
	h_land += plateau * 0.03 * fbm(p + vec3(4.0, 4.0, 90.0), 4.0, 3);   // keep tops nearly flat
	// Mountain belts along active boundaries only.
	float mount = ridged(p, FREQ_MOUNT, OCT_MOUNT, 2.0);
	// The coastal range sits just inland of an active margin, on whichever
	// side is continental (both terms keep it continuous at the bisector).
	float andes = contA * (1.0 - contB) * band(bdist - ANDES_WIDTH, ANDES_WIDTH)
	            + contB * (1.0 - contA) * band(-bdist - ANDES_WIDTH, ANDES_WIDTH);
	float belt = collide * band(bdist, COLLIDE_WIDTH) * COLLIDE_H
	           + convergent * andes * ANDES_H;
	h_land += belt * mount;
	// Old worn ranges inside the plates, low and rounded.
	float oldMask = smoothstep(0.25, 0.50, snoise(p * 1.4 + vec3(61.0, 13.0, 37.0)));
	h_land += oldMask * OLD_RANGE_H * ridged(p + vec3(19.0, 3.0, 7.0), 3.5, 4, 1.2);

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

	// High-frequency ridged detail (rock texture), weighted to rugged land.
	float rugged = clamp(belt + hillMask * 0.4 + oldMask * 0.4, 0.0, 1.0);
	float detail = ridged(p, FREQ_DETAIL, OCT_DETAIL, 1.5);
	h += (detail - 0.5) * DETAIL_AMT * (0.2 + 0.8 * rugged) * (0.3 + 0.7 * land);

	float anomaly = fbm(p + vec3(5.3, 71.9, 23.1), FREQ_ANOMALY, OCT_ANOMALY);
	BakeOut = vec4(h, anomaly, 0.0, 1.0);
}
