// The nebula's one density function (nebula.c): shared by the sky bake
// (nebula_sky.frag, far layer), the in-system pass (nebula_vol.frag, near
// layer) and the presence grid (nebula_grid.frag).
//
// Everything is in nebula units n = (p - sun) / Rb. Two layers that never
// overlap, so the baked sky and the runtime march can't count the same gas:
// - near: wisps inside |n| < 1, faded out over the last 15 % of the radius
//   (in the field, so both the march and the grid see the same edge);
// - far: a shell |n| in [farStart, farEnd], evaluated in shell units
//   q = n / farStart (so the shell runs 1 .. farEnd / farStart), faded at
//   both radii.
//
// Each layer: a ridged FBM (filaments) of a domain-warped point, times a
// low-frequency coverage mask (clumps). The far layer's mask also follows
// the galaxy band. Emission: duotone palette, with dark lanes where a
// second noise says the gas doesn't glow (it still absorbs).
//
// Before this include: NEBULA_OCTAVES (ridged octaves, a loop bound so the
// loop unrolls).

#include "world/clouds/cloudnoise.glsl"

#ifndef NEBULA_OCTAVES
#define NEBULA_OCTAVES 5
#endif

uniform vec3 nebulaSeed;       // lattice-unit offset from the seed (nebula.c)
uniform vec3 nebulaColA;       // palette ends (linear)
uniform vec3 nebulaColB;
uniform float nebulaTurbulence;  // domain warp, in wavelengths
uniform float nebulaContrast;    // ridge sharpness (pow)
uniform float nebulaEmission;    // emission per unit density, per unit length
uniform float nebulaDark;        // 0 = all gas glows, 1 = wide dark lanes

// Per layer: x = wavelength (layer units), y = coverage (0..1), z =
// extinction per unit density per unit length, w = density scale.
uniform vec4 nebulaNear;
uniform vec4 nebulaFar;
uniform float nebulaFarEnd;      // farEnd / farStart (shell units)
uniform vec3 nebulaBandAxis;     // galaxy band: unit normal of its plane
uniform float nebulaBand;        // 0 = no band, 1 = gas only near the band

// Far-layer light sources (nebula.c, seeded): bright cores in the shell
// (shell units) that light the dust (nebula_sky.frag), one per cloud body
// on its far side from the sun (backlit). Gas gathers around them (as
// emission nebulae do around hot stars): coverage is raised within
// lightRadius.
#define NEBULA_MAX_LIGHTS 8
uniform int nebulaLightCount;
uniform vec3 nebulaLightPos[NEBULA_MAX_LIGHTS];
uniform vec3 nebulaLightCol[NEBULA_MAX_LIGHTS];   // colour x intensity
uniform float nebulaLightRadius;   // falloff / clump radius, shell units
uniform float nebulaLightClump;    // coverage added at a light, 0..1

// Mean of one ridged octave (1 - |2n - 1|)^2 over the value noise; octaves
// below the footprint fade to it so the field keeps its level with distance.
const float RIDGE_MEAN = 0.38;

// Ridged FBM, q in wavelengths of the first octave; foot: the footprint in
// the same units.
float nebulaRidged(vec3 q, float foot)
{
	float lambda = 1.0, amp = 1.0, sum = 0.0, norm = 0.0;
	for (int k = 0; k < NEBULA_OCTAVES; k++) {
		float fade = 1.0 - smoothstep(0.5, 1.0, foot / lambda);
		float r = RIDGE_MEAN;
		if (fade > 0.0) {
			float v = 1.0 - abs(2.0 * cloudValueNoise(q) - 1.0);
			r = mix(RIDGE_MEAN, v * v, fade);
		}
		sum += amp * r;
		norm += amp;
		q = CLOUD_OCTAVE * q + vec3(17.3, -5.1, 9.7);
		lambda *= 0.5;
		amp *= 0.55;
	}
	return sum / norm;
}

// Clumps: a soft threshold of a two-octave low-frequency noise (unwarped,
// so the presence grid can bound it cheaply). q in layer wavelengths.
float nebulaMask(vec3 q, float coverage)
{
	vec3 m = q * 0.25 + vec3(41.0, 13.0, 29.0);
	float c = 0.65 * cloudValueNoise(m) + 0.35 * cloudValueNoise(CLOUD_OCTAVE * m);
	float edge = 1.0 - coverage;
	return smoothstep(edge - 0.12, edge + 0.12, c);
}

// The radial fades, in layer units.
float nearFade(float r) { return 1.0 - smoothstep(0.85, 1.0, r); }
float farFade(float r)
{
	return smoothstep(1.0, 1.15, r) * (1.0 - smoothstep(nebulaFarEnd * 0.8, nebulaFarEnd, r));
}

// The galaxy band's weight at direction d (unit).
float nebulaBandWeight(vec3 d)
{
	float y = dot(d, nebulaBandAxis);
	return mix(1.0, exp(-y * y / 0.04), nebulaBand);
}

// Upper bound of the near layer's density at n: what the presence grid
// stores (the ridged part is at most 1).
float nearBound(vec3 n)
{
	vec3 q = n / nebulaNear.x + nebulaSeed;
	return nebulaNear.w * nearFade(length(n)) * nebulaMask(q, nebulaNear.y);
}

// One layer's sample: density (x) and glow fraction (y), palette mix (z).
// q: layer point in wavelengths (seed offset added); w: the radial and
// mask weight; foot: footprint in wavelengths.
vec3 layerSample(vec3 q, float w, float foot)
{
	if (w <= 1e-4)
		return vec3(0.0);
	// One octave of domain warp (EVE-style flow).
	vec3 wq = q * 0.5;
	vec3 warp = vec3(cloudValueNoise(wq + vec3(3.1, 7.7, 1.3)),
	                 cloudValueNoise(wq + vec3(11.9, 2.3, 5.5)),
	                 cloudValueNoise(wq + vec3(6.7, 9.1, 13.7))) - 0.5;
	vec3 p = q + nebulaTurbulence * 2.0 * warp;
	float ridge = nebulaRidged(p, foot);
	float rho = w * pow(clamp((ridge - 0.2) / 0.8, 0.0, 1.0), nebulaContrast);
	// Palette and glow from coarse noise of the warped point: colour runs
	// along the flow.
	float pal = cloudValueNoise(p * 0.3 + vec3(19.0, 23.0, 31.0));
	float glowN = cloudValueNoise(p * 0.6 + vec3(5.0, 37.0, 17.0));
	float glow = mix(1.0, smoothstep(0.3, 0.6, glowN), nebulaDark);
	return vec3(rho, glow, smoothstep(0.25, 0.75, pal));
}

vec3 nearSample(vec3 n, float foot)
{
	vec3 q = n / nebulaNear.x + nebulaSeed;
	float w = nebulaNear.w * nearFade(length(n)) * nebulaMask(q, nebulaNear.y);
	return layerSample(q, w, foot / nebulaNear.x);
}

// Coverage raised near the lights (smooth, max over lights).
float lightClump(vec3 s)
{
	float c = 0.0;
	float r2 = nebulaLightRadius * nebulaLightRadius;
	for (int i = 0; i < nebulaLightCount; i++) {
		vec3 v = s - nebulaLightPos[i];
		float d2 = dot(v, v);
		if (d2 < 9.0 * r2)
			c = max(c, exp(-d2 / r2));
	}
	return c * nebulaLightClump;
}

#ifdef NEBULA_FAR
// The far layer's shapes (plan Phase 1b step 2; the sky bake only): Nubis
// clouds (nebula_noise.frag's textures). A Perlin-Worley base shape, its
// low end cut by the coverage field (clumps, band, gas around the lights)
// so the masses have defined silhouettes, then eroded at the edges by
// Worley detail so the edges billow. In layer wavelengths: the shape
// texture repeats every 4, the detail every 1 / detailFreq.
uniform sampler3D nebulaShapeTex;
uniform sampler3D nebulaDetailTex;
uniform float nebulaShapeSize;     // texels per side
uniform float nebulaDetailSize;
uniform float nebulaDetailFreq;    // detail repeats per wavelength
uniform float nebulaErosion;       // 0 = smooth blobs, 1 = deeply eroded
uniform float nebulaDensityCurve;  // pow on density: > 1 separates dense cores from thin gas

// Tiled textures repeat on a lattice. Seen through a deep, semi-
// transparent fringe the same pattern recurs at many depths along a ray,
// each copy shrunk by its distance: summed, a zoom blur centred on the
// lattice's axes (cube-face centres for an axis-aligned one). Rotation
// alone only moves the centres; the lookup is also distorted by a value-
// noise warp of more than a tile at about a tile's scale, so the repeated
// rows are no longer straight and no vanishing point survives.
const mat3 NEBULA_ROT1 = CLOUD_OCTAVE * 0.5;
const mat3 NEBULA_ROT2 = (CLOUD_OCTAVE * 0.5) * (CLOUD_OCTAVE * 0.5);

float remap01(float v, float lo, float hi)
{
	return clamp((v - lo) / max(hi - lo, 1e-4), 0.0, 1.0);
}

// Coverage as a soft field (the mask's smoothstep, widened): where it is
// high the base shape survives the cut.
float farCoverage(vec3 q, float coverage)
{
	vec3 m = q * 0.25 + vec3(41.0, 13.0, 29.0);
	float c = 0.65 * cloudValueNoise(m) + 0.35 * cloudValueNoise(CLOUD_OCTAVE * m);
	return smoothstep(0.0, 1.0, clamp((c - (1.0 - coverage)) / 0.35 + 0.5, 0.0, 1.0));
}

// Composition (plan Phase 1b step 3): the far layer's gas lives only in a
// few seeded cloud bodies (nebula.c), stretched ellipsoids biased toward
// the galaxy band, so the sky reads as a few large structures with empty
// starfield between them, not haze everywhere. nebulaBodyInv maps an
// offset from a body's centre to its unit sphere; the edge is broken up by
// low-frequency noise so no ellipse shows.
#define NEBULA_MAX_BODIES 8
uniform int nebulaBodyCount;
uniform vec3 nebulaBodyCenter[NEBULA_MAX_BODIES];
uniform mat3 nebulaBodyInv[NEBULA_MAX_BODIES];
uniform float nebulaBodyBreakup;   // edge noise, in body radii

// Filaments: cloud streamers, not hair. Inside a longer envelope along
// each body's long axis (local x), the clump noise is stretched along x
// and bent sideways, then thresholded into bands: long, sinuous strands
// trailing out of the masses. They pass through the same Nubis shape cut
// and erosion as the rest of the dust, so their edges billow like a
// cloud's.
uniform float nebulaFilament;      // 0 = none, 1 = strong streamers
uniform float nebulaFilReach;      // their envelope along the long axis, body radii
uniform float nebulaFilStretch;    // how much longer than wide
uniform float nebulaFilScale;      // strands across a body
uniform float nebulaFilBend;       // sideways sway along a strand
// Coverage cap inside bodies and strands: below 1 the billowy base shape
// still cuts folds and holes in the cores (full coverage fills them into
// flat fog).
uniform float nebulaBodyFill;
// Strands: coverage cap (below 1 the billowy cut breaks a strand into a
// chain of puffs instead of a filled tube) and tip taper (0 = none).
uniform float nebulaFilCover;
uniform float nebulaFilTaper;

float bodyCoverage(vec3 s)
{
	float c = 0.0;
	float reach = max(nebulaFilReach, 1.0);
	for (int i = 0; i < nebulaBodyCount; i++) {
		vec3 v = nebulaBodyInv[i] * (s - nebulaBodyCenter[i]);
		float e2 = dot(v, v);
		vec3 vr = vec3(v.x / reach, v.yz);
		float er2 = dot(vr, vr);
		vec3 o = nebulaSeed + vec3(float(i) * 7.3);
		if (e2 <= 2.25) {
			float n = 0.6 * cloudValueNoise(v * 1.5 + o)
			        + 0.4 * cloudValueNoise(v * 3.1 + o + vec3(11.0));
			float e = sqrt(e2) + (n - 0.5) * 2.0 * nebulaBodyBreakup;
			c = max(c, nebulaBodyFill * (1.0 - smoothstep(0.45, 1.0, e)));
		}
		if (nebulaFilament > 0.0 && er2 < 1.0) {
			// Sideways sway, slow along the strand.
			vec2 sway = vec2(cloudValueNoise(vec3(v.x * 0.7, 0.0, 0.0) + o),
			                 cloudValueNoise(vec3(v.x * 0.7, 9.0, 0.0) + o)) - 0.5;
			vec3 w = vec3(v.x / max(nebulaFilStretch, 1.0), v.yz + sway * 2.0 * nebulaFilBend) * nebulaFilScale;
			float n = 0.65 * cloudValueNoise(w + o + vec3(3.0))
			        + 0.35 * cloudValueNoise(w * 2.03 + o + vec3(17.0));
			// Toward the tip (0 by the body, 1 at the reach): unstretched,
			// finer noise roughens the strand's edge more and more, and
			// the cut rises, so a strand thins and breaks into puffs
			// instead of ending in a smooth rounded tube.
			float along = smoothstep(0.25, 1.0, abs(vr.x)) * nebulaFilTaper;
			if (along > 0.0)
				n += (cloudValueNoise(v * (1.7 * nebulaFilScale) + o + vec3(23.0)) - 0.5) * (0.2 + 0.3 * along);
			float thr = 0.55 + 0.2 * along;
			float strand = smoothstep(thr, thr + 0.13, n);
			float env = 1.0 - smoothstep(0.35, 1.0, sqrt(er2));
			c = max(c, nebulaFilament * nebulaFilCover * nebulaBodyFill * env * strand * (1.0 - 0.5 * along));
		}
	}
	return c;
}

// The coverage field: inside the bodies (and around the lights), with the
// clump noise as structure within them.
float farCoverField(vec3 s, vec3 q)
{
	float base = max(bodyCoverage(s), lightClump(s));
	if (base <= 1e-3)
		return 0.0;
	return farFade(length(s)) * base * mix(1.0, farCoverage(q, nebulaFar.y), 0.6);
}

// Emissive gas (plan Phase 1b step 4): a second medium that only glows,
// in a wider envelope than each body's dust (so it shows through the gaps
// and around the edges), smooth, and brightest near the lights (ionised
// gas around hot stars). The dust in front absorbs it: dark silhouettes
// against bright gas. No light march, so it is cheap to evaluate.
uniform float nebulaGasStrength;   // base glow (palette coloured)
uniform float nebulaGasLight;      // glow per unit light colour near lights
uniform float nebulaGasExtent;     // envelope size over the dust's
uniform float nebulaGasRadius;     // light falloff for the glow, shell units
uniform float nebulaGasContrast;   // pow of its noise: patchy vs even
uniform float nebulaGasBack;       // envelope pushed away from the sun (behind the dust)

vec3 gasEmission(vec3 s)
{
	float env = 0.0;
	for (int i = 0; i < nebulaBodyCount; i++) {
		vec3 c = nebulaBodyCenter[i];
		vec3 v = nebulaBodyInv[i] * (s - c * (1.0 + nebulaGasBack / length(c))) / nebulaGasExtent;
		float e2 = dot(v, v);
		if (e2 < 1.0)
			env = max(env, 1.0 - smoothstep(0.2, 1.0, sqrt(e2)));
	}
	env *= farFade(length(s));
	if (env <= 1e-3)
		return vec3(0.0);
	// Smooth structure: the shape texture's Perlin-Worley at a large scale
	// (rotated: no lattice-aligned repeats), mip 1.
	vec3 q = s / nebulaFar.x + nebulaSeed + vec3(31.0, 5.0, 17.0);
	float n = textureLod(nebulaShapeTex, NEBULA_ROT2 * q * 0.08, 1.0).r;
	float g = env * pow(clamp((n - 0.3) / 0.7, 0.0, 1.0), nebulaGasContrast);
	float pal = cloudValueNoise(q * 0.15 + vec3(19.0, 23.0, 31.0));
	vec3 col = nebulaGasStrength * mix(nebulaColA, nebulaColB, smoothstep(0.25, 0.75, pal));
	float R2 = nebulaGasRadius * nebulaGasRadius;
	for (int i = 0; i < nebulaLightCount; i++) {
		vec3 v = s - nebulaLightPos[i];
		col += nebulaGasLight * nebulaLightCol[i] / (1.0 + dot(v, v) / R2);
	}
	return g * col;
}

// Whether s is inside any cloud's coverage (cheap: the march takes fine
// steps only there).
float farCoverageAt(vec3 s)
{
	vec3 q = s / nebulaFar.x + nebulaSeed + vec3(101.0, 57.0, 73.0);
	return farCoverField(s, q);
}

vec3 farSample(vec3 s, float foot)
{
	vec3 q = s / nebulaFar.x + nebulaSeed + vec3(101.0, 57.0, 73.0);
	float cov = farCoverField(s, q);
	if (cov <= 1e-3)
		return vec3(0.0);
	float footQ = foot / nebulaFar.x;

	// One octave of domain warp (flow), as the near layer.
	vec3 wq = q * 0.5;
	vec3 warp = vec3(cloudValueNoise(wq + vec3(3.1, 7.7, 1.3)),
	                 cloudValueNoise(wq + vec3(11.9, 2.3, 5.5)),
	                 cloudValueNoise(wq + vec3(6.7, 9.1, 13.7))) - 0.5;
	vec3 p = q + nebulaTurbulence * 2.0 * warp;

	// Base shape, mip by footprint (a texel is 4 / size wavelengths).
	float lodS = max(log2(footQ * nebulaShapeSize / 4.0), 0.0);
	vec4 sh = textureLod(nebulaShapeTex, NEBULA_ROT1 * p * 0.25, lodS);
	float wf = sh.g * 0.625 + sh.b * 0.25 + sh.a * 0.125;
	float base = remap01(sh.r, wf - 1.0, 1.0);
	// The coverage only raises the cut (no Nubis-style * cov as well): the
	// silhouette is where the billowy base crosses it, not the soft body
	// falloff, so the masses keep hard, lumpy edges.
	float d = remap01(base, 1.0 - cov, 1.0);
	if (d <= 0.0)
		return vec3(0.0);

	// Erosion: billowy (inverted Worley) detail eats the thin edges; the
	// cores, well above it, keep their density.
	float lodD = max(log2(footQ * nebulaDetailFreq * nebulaDetailSize), 0.0);
	vec3 dp = NEBULA_ROT2 * p * nebulaDetailFreq;
	vec3 dw = vec3(cloudValueNoise(dp * 0.7 + vec3(13.1, 3.7, 29.3)),
	               cloudValueNoise(dp * 0.7 + vec3(7.9, 31.1, 2.3)),
	               cloudValueNoise(dp * 0.7 + vec3(23.5, 17.3, 11.9))) - 0.5;
	vec3 dt = textureLod(nebulaDetailTex, dp + 3.0 * dw, lodD).rgb;
	float df = dot(dt, vec3(0.625, 0.25, 0.125));
	d = remap01(d, nebulaErosion * df * 0.5, 1.0);

	float rho = nebulaFar.w * pow(d, nebulaDensityCurve);
	float pal = cloudValueNoise(p * 0.3 + vec3(19.0, 23.0, 31.0));
	float glowN = cloudValueNoise(p * 0.6 + vec3(5.0, 37.0, 17.0));
	float glow = mix(1.0, smoothstep(0.3, 0.6, glowN), nebulaDark);
	return vec3(rho, glow, smoothstep(0.25, 0.75, pal));
}
#endif

// Emission-absorption step: adds the segment's light to L (seen through
// T) and attenuates T. smp: layerSample's result; sigma: extinction per
// density per unit length; extra: added source (scattered sunlight).
void nebulaStep(vec3 smp, float sigma, float dt, vec3 extra, inout vec3 L, inout float T)
{
	float rho = smp.x;
	if (rho <= 0.0)
		return;
	vec3 col = mix(nebulaColA, nebulaColB, smp.z);
	vec3 src = rho * (nebulaEmission * smp.y * col + extra);
	float st = rho * sigma;
	float a = exp(-st * dt);
	// Exact for a constant source over the segment.
	L += T * src * (st > 1e-6 ? (1.0 - a) / st : dt);
	T *= a;
}
