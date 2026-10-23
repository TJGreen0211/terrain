// Sea state over the planet, shared by water.vert (geometry) and water.frag
// (slopes, roughness, foam) so both see the same waves: how strong each FFT
// cascade (water.c) is at a point. Needs wavecube.glsl and the tempOffset,
// snowTemp, oceanRelief and planetRadiusM uniforms declared before it is
// included.
//
// The cascades alone tile the same sea everywhere, which reads as a pattern
// however well the tiling is hidden. Real seas vary over tens to hundreds
// of km: swell trains arrive from distant storms, and the short wind waves
// follow gusts and lulls within minutes (rough cat's paws next to glassy
// slicks), which from orbit shows as a patchy, wind-shaped sun glint.

// Wind speed (m/s) of the sea the FFT amplitude was tuned for. Wave height
// goes as U^2 in a developed sea, cut back where the fetch is short.
const float SEA_STATE_REF_WIND = 7.0;
// Water depth (m) over which each cascade's waves ramp from flat at the
// coast to full height: a few of its drawn rms heights, about where waves
// that size break (terrain_draw.c). In metres, not normalized depth: the seafloor
// drops oceanRelief * depthT^2 (11 km at depthT 1 on Earth), so a
// normalized ramp calmed the sea out to kilometres of depth and left every
// shelf a glassy mirror. The short wind waves now run right up to the
// shore; only the big (scaled) swell is held off the shallows, where its
// troughs would dip below the seafloor.
uniform float waveShoreDepths[WAVE_CASCADES];
// Sea ice sets in this far below the land snowline (salt, ocean heat).
const float SEA_ICE_OFFSET = -0.04;
// Variation noise frequencies, per body radius (~400 km swell trains,
// ~100 km gust fields with a ~25 km octave, at Earth's radius).
const float SWELL_NOISE_FREQ = 16.0;
const vec2 GUST_NOISE_FREQ = vec2(60.0, 240.0);

float hash13(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float vnoise3(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(hash13(i), hash13(i + vec3(1, 0, 0)), f.x),
	               mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
	           mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x),
	               mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// 3D gradient noise, ~[-0.7, 0.7]. The sea-state fields use it rather than
// vnoise3: value noise pushed through any contrast shows its lattice as
// blocky, stair-stepped cells.
vec3 hash33(vec3 p)
{
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.xxy + p.yxx) * p.zyx) * 2.0 - 1.0;
}

float gnoise3(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	return mix(mix(mix(dot(hash33(i), f),
	                   dot(hash33(i + vec3(1, 0, 0)), f - vec3(1, 0, 0)), u.x),
	               mix(dot(hash33(i + vec3(0, 1, 0)), f - vec3(0, 1, 0)),
	                   dot(hash33(i + vec3(1, 1, 0)), f - vec3(1, 1, 0)), u.x), u.y),
	           mix(mix(dot(hash33(i + vec3(0, 0, 1)), f - vec3(0, 0, 1)),
	                   dot(hash33(i + vec3(1, 0, 1)), f - vec3(1, 0, 1)), u.x),
	               mix(dot(hash33(i + vec3(0, 1, 1)), f - vec3(0, 1, 1)),
	                   dot(hash33(i + vec3(1, 1, 1)), f - vec3(1, 1, 1)), u.x), u.y), u.z);
}

// 1 on open water, 0 under the pack: the sea ice holds the surface flat.
// The land's sea-level temperature (quad.frag) without its noise.
float seaIceCalm(vec3 d)
{
	float seaTemp = sqrt(max(1.0 - d.z * d.z, 0.0)) * 1.2 - 0.2 + tempOffset;
	return smoothstep(snowTemp + SEA_ICE_OFFSET - 0.06, snowTemp + SEA_ICE_OFFSET + 0.02, seaTemp);
}

// Local wind multiplier, log-normal around 1 (~0.6 in a lull to ~1.6 in a
// gust). foot: the footprint (radians) of a pixel or mesh cell; the finer
// octave fades out once its cells shrink toward it, so it never aliases.
const float GUST_STRENGTH = 1.1;   // log2 range of the multiplier per unit noise

float seaGust(vec3 d, float foot)
{
	float fine = 1.0 - smoothstep(0.1, 0.5, foot * GUST_NOISE_FREQ.y);
	float n = 0.7 * gnoise3(d * GUST_NOISE_FREQ.x)
	        + 0.3 * fine * gnoise3(d * GUST_NOISE_FREQ.y + 17.0);
	return exp2(GUST_STRENGTH * n);
}

// Swell-train multiplier, log-normal around 1: swell from far storms comes
// and goes over hundreds of km.
float seaSwell(vec3 d)
{
	return exp2(gnoise3(d * SWELL_NOISE_FREQ + 5.0));
}

// Water depth (m) at normalized depth depthT: the seafloor profile quad.frag
// draws (terrain_draw.c's height bounds use the same).
float seaDepthM(float depthT)
{
	return oceanRelief * depthT * depthT * planetRadiusM;
}

// Heights (height-map units) where the shore effects need the smooth
// height filter (planetmap.glsl planetMapFine): from the deepest water the
// surf reaches (twice the swell's calm depth, as shorewaves.glsl's gate)
// to the top of quad.frag's beaches (BEACH_OFF). Needs seaLevel and
// oceanDepth. Every shader passes this same band, so they all filter the
// coast alike.
vec2 seaCoastBand()
{
	if (oceanRelief <= 0.0)
		return vec2(1.0, -1.0);   // no sea: an empty band, plain bilinear
	float dT = sqrt(2.0 * waveShoreDepths[0] / max(oceanRelief * planetRadiusM, 1e-3));
	return vec2(seaLevel - dT * oceanDepth, seaLevel + 0.012);
}

// Amplitude of each cascade (largest first). U: wind speed (m/s), fetchF:
// 0 in the lee of land to 1 over open ocean (oceanMap), depthT: normalized
// depth (0 at the coast), gust: seaGust, swellN: seaSwell (1, 1 for the
// noise-free sea state).
vec3 waveCascadeGains(float depthT, float U, float fetchF, float iceCalm, float gust, float swellN)
{
	float dM = seaDepthM(depthT);
	vec3 shore = vec3(smoothstep(0.0, waveShoreDepths[0], dM),
	                  smoothstep(0.0, waveShoreDepths[1], dM),
	                  smoothstep(0.0, waveShoreDepths[2], dM)) * iceCalm;
	// Swell: a developed sea grows as U^2 and needs a long fetch.
	float swell = clamp(pow(U / SEA_STATE_REF_WIND, 2.0) * mix(0.3, 1.0, fetchF), 0.15, 2.0)
	            * swellN;
	// Wind sea: the short waves answer the local wind, gusts and all, and
	// barely need fetch.
	float wind = clamp(U / SEA_STATE_REF_WIND, 0.2, 1.8) * gust;
	return vec3(swell, mix(swell, wind, 0.5), wind) * shore;
}
