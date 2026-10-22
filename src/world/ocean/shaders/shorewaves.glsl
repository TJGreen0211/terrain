// Shore waves: swell lines that run parallel to the coast, shoal (shorter,
// taller, steeper) as the water shallows, break, roll in as foamy bores and
// run up the beach. Shared by water.vert (geometry), water.frag (slopes,
// foam, turbid surf) and quad.frag (swash and wet sand), so all three see
// the same waves at the same time.
//
// Driven by the distance to the coastline, depth over seabed slope
// (shoreCoast): the crests are its contours, so they always lie along the
// shore. The open sea's FFT swell calms over the same shallows
// (seastate.glsl waveShoreDepths) and these take over.
//
// Needs planetmap.glsl, seastate.glsl (gnoise3, hash13), the heightMap,
// seaLevel, planetRadiusM and time uniforms, and waveCascadeScales.
//
// Sizes are true size (m, s) and drawn at the swell's Wave scale s, like
// the FFT swell: lengths x s, speeds x sqrt(s) (Froude), so the surf reads
// from the altitudes the coast is seen from.
const float SHORE_ZONE_M = 400.0;       // waves form this far out
const float SHORE_WAVELENGTH_M = 60.0;  // crest spacing at the zone edge
const float SHORE_SPEED_MPS = 6.0;      // crest speed at the zone edge
const float SHORE_HEIGHT_M = 1.0;       // crest height before shoaling
const float SHORE_BREAK = 0.35;         // break point, fraction of the zone
const float SHORE_SET = 7.0;            // waves per set (a big group, a lull)
const float SHORE_RUNUP_M = 25.0;       // swash reach up the beach
const float SHORE_LACE_M = 8.0;         // foam lace cell size
// Wet sand reaches this many SHORE_RUNUP_M up the beach (shoreSwash).
const float SHORE_WET_REACH = 2.0;

float shoreScale()
{
	return max(waveCascadeScales[0], 0.01);
}

// Seaward distance to the coastline (m; negative inland) at d with height
// h, and the seaward unit direction (body-local tangent). The seabed slope
// is a central-ish difference of the same smooth height field the coast is
// drawn from (planetMapFine), half a texel of the lod's mip apart.
float shoreCoast(vec3 d, float h, float lod, vec3 T, vec3 B, out vec3 seaward)
{
	float hEps = 0.5 * planetMapTexel(heightMap) * exp2(lod);
	float hE = planetMapFine(heightMap, normalize(d + T * hEps), lod, seaCoastBand());
	float hN = planetMapFine(heightMap, normalize(d + B * hEps), lod, seaCoastBand());
	vec2 g = vec2(hE - h, hN - h) / hEps;     // uphill: toward land
	float gl = length(g);
	seaward = -(T * g.x + B * g.y) / max(gl, 1e-12);
	return (seaLevel - h) / max(gl, 1e-6) * planetRadiusM;
}

// Wave phase at distance x (m) from the coast, time t (s): crests sit at
// whole numbers plus the crest offset, and move shoreward. Shoaling: the
// wavelength shrinks toward the shore as L(x) = L0 sqrt((x + x0)/(Z + x0))
// (shallow-water speed goes as the root of the depth), integrated so the
// spacing changes smoothly; constant L0 beyond the zone.
float shorePhase(float x, float t, out float L)
{
	float s = shoreScale();
	float Z = SHORE_ZONE_M * s;
	float L0 = SHORE_WAVELENGTH_M * s;
	float x0 = 0.1 * Z;
	float xi = max(x, 0.0);
	float ph;
	if (xi < Z) {
		ph = 2.0 * sqrt((xi + x0) * (Z + x0)) / L0;
		L = L0 * sqrt((xi + x0) / (Z + x0));
	} else {
		ph = 2.0 * (Z + x0) / L0 + (xi - Z) / L0;
		L = L0;
	}
	// Not wrapped: floor(phase) numbers each wave for its whole run in.
	float f = SHORE_SPEED_MPS * sqrt(s) / L0;
	return ph + f * t;
}

// Size of wave number n: sets of bigger waves every SHORE_SET, with a
// random spread per wave.
float shoreSet(float n)
{
	float g = 0.5 + 0.5 * cos(6.2831853 * n / SHORE_SET);
	return mix(0.45, 1.0, g) * mix(0.75, 1.25, hash13(vec3(n, 7.0, 3.0)));
}

// Along-shore variation: the waves break in segments, not one long line.
float shoreSegment(vec3 d)
{
	float s = shoreScale();
	return 1.0 + 0.8 * gnoise3(d * (planetRadiusM / (SHORE_WAVELENGTH_M * s * 2.5)) + 3.0);
}

// Foam texture: lacy cells dissolving as cover falls (cover 0..1); faded
// to the plain cover once the cells shrink toward a pixel (footM).
float shoreLace(vec3 d, float cover, float footM)
{
	float cellM = SHORE_LACE_M * shoreScale();
	float vis = 1.0 - smoothstep(0.3, 0.8, footM / cellM);
	if (vis <= 0.0 || cover <= 0.0)
		return cover;
	vec3 q = d * (planetRadiusM / cellM);
	float n = 0.5 + 0.9 * (0.65 * gnoise3(q) + 0.35 * gnoise3(q * 2.7 + 11.0));
	return mix(cover, smoothstep(n - 0.12, n + 0.12, cover), vis);
}

struct ShoreWave {
	float height;   // m above sea level (crests; the bore's set-up)
	float slope;    // dheight/dx, x seaward (m/m)
	float slopeVar; // slope variance of the waves below a pixel
	float foam;     // whitewater cover 0..1 (before the lace)
	float crest;    // crest lift 0..1, for light through the crests
	float turbid;   // sand stirred up in the surf 0..1
};

// The waves at x metres out (x >= 0), at d, of exposure strength (0 lee
// shore .. 1 open ocean coast), seen at footprint footM (m per pixel or
// mesh cell).
ShoreWave shoreWaves(float x, vec3 d, float strength, float footM)
{
	ShoreWave w = ShoreWave(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
	float s = shoreScale();
	float Z = SHORE_ZONE_M * s;
	if (x >= Z || strength <= 0.0)
		return w;
	float L;
	float ph = shorePhase(x, time * 100.0, L);
	float n = floor(ph);
	float wf = ph - n;
	float seg = shoreSegment(d);
	float size = shoreSet(n) * max(seg, 0.2) * strength;
	// Bigger waves break farther out.
	float xb = SHORE_BREAK * Z * mix(0.7, 1.3, clamp(size, 0.0, 1.5) / 1.5);
	float fadeIn = 1.0 - smoothstep(0.7 * Z, Z, x);
	float x0 = 0.1 * Z;
	// Green's law: height grows as depth^-1/4 (x stands in for depth).
	float shoal = pow((Z + x0) / (x + x0), 0.25);
	// Shoreward of the break point the crest has collapsed into a bore
	// that loses height to the shore.
	float broken = 1.0 - smoothstep(0.8 * xb, xb, x);
	float A = SHORE_HEIGHT_M * s * size * fadeIn * shoal
	        * mix(1.0, 0.6 * clamp(x / xb, 0.0, 1.0), broken);
	// Steepening: the crest leans shoreward (the front, w < c, shortens)
	// and peaks up as it nears the break point.
	float steep = 1.0 - smoothstep(xb, 0.75 * Z, x);
	float c = mix(0.5, 0.2, steep);
	float k = mix(2.0, 4.0, steep);
	float u = wf < c ? 0.5 * wf / c : 0.5 + 0.5 * (wf - c) / (1.0 - c);
	float su = sin(3.14159265 * u);
	float p = pow(max(su, 0.0), k);
	float dpdu = k * pow(max(su, 1e-6), k - 1.0) * cos(3.14159265 * u) * 3.14159265;
	float dudw = wf < c ? 0.5 / c : 0.5 / (1.0 - c);
	float slopeFull = A * dpdu * dudw / L;
	// Antialiasing: below ~3 px per wave the waves fade to their mean
	// height, and their slope moves into the roughness.
	float vis = 1.0 - smoothstep(0.15, 0.4, footM / L);
	float pMean = 0.5 - 0.0625 * (k - 2.0);
	w.height = A * mix(pMean, p, vis);
	w.slope = slopeFull * vis;
	float kA = A * 6.2831853 / L;
	w.slopeVar = (1.0 - vis) * 0.5 * kA * kA;
	w.crest = p * steep * vis;
	// Whitewater: on the breaking crest and its front face, a trail
	// left behind it, and a thinner sheet over the bore.
	float onCrest = smoothstep(c - 0.15, c, wf) * (1.0 - smoothstep(c, c + 0.04, wf));
	float trail = wf > c ? exp(-(wf - c) * 6.0) : 0.0;
	float spill = smoothstep(0.6, 1.0, steep) * (1.0 - broken) * onCrest * 0.5;
	float whitewater = broken * (1.2 * onCrest + 0.9 * trail + 0.3) + spill;
	// Unresolved, the bands average to their mean cover.
	float wwMean = broken * (1.2 * 0.19 + 0.9 * 0.16 + 0.3);
	w.foam = clamp(1.4 * mix(wwMean, whitewater, vis) * min(size, 1.2) * fadeIn, 0.0, 1.0);
	w.turbid = clamp((1.0 - smoothstep(0.5 * xb, 1.2 * xb, x)) * min(size * 1.5, 1.0), 0.0, 1.0);
	return w;
}

// Swash on the beach, inland metres up it (> 0): the thin water sheet of
// each wave running up and sliding back (sheet), the foam at its leading
// edge (edge), and the sand it has wetted (wet), drying back between sets.
void shoreSwash(float inland, vec3 d, float strength, float footM,
                out float sheet, out float edge, out float wet)
{
	float s = shoreScale();
	float L;
	// The bores reach the coastline (x = 0) at the crest offset c = 0.2.
	float ph = shorePhase(0.0, time * 100.0, L) - 0.2;
	float n = floor(ph);
	float tau = ph - n;
	float reach = SHORE_RUNUP_M * s * strength * max(shoreSegment(d), 0.2);
	float Rn = reach * shoreSet(n);
	// Fast uprush, slower backwash.
	float r = Rn * (tau < 0.3 ? sin(1.5707963 * tau / 0.3)
	                          : 1.0 - smoothstep(0.3, 1.0, tau));
	float soft = max(footM, 0.04 * reach);
	sheet = 1.0 - smoothstep(r - soft, r + soft, inland);
	float band = max(0.12 * Rn, footM);
	edge = smoothstep(r - band, r, inland) * (1.0 - smoothstep(r, r + soft, inland))
	     * (1.0 - smoothstep(0.25, 0.6, tau)) * min(band / max(footM, 1e-3), 1.0);
	// Wet sand up past the reach of the biggest waves (where the tide and
	// the storms left it); freshly glazed where this wave's sheet just drew
	// back.
	float maxReach = reach * SHORE_WET_REACH;
	wet = 1.0 - smoothstep(0.75 * maxReach, maxReach + soft, inland);
	wet = max(wet * 0.7, (inland < Rn ? 1.0 - smoothstep(0.3, 1.0, tau) : 0.0));
}
