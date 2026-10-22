#version 410

in vec3 fSurfDir;
in vec3 fragPos;
in float fWave;

out vec4 FragColor;

uniform mat4 model;
uniform vec3 lightPosition;
uniform vec3 cameraPos;
// FFT ocean (Water.c): slopes and displacement derivatives per patch
// length, one layer per cascade, largest first.
uniform sampler2DArray waveSlope;     // dh/dx, dh/dz, (dh/dx)^2, (dh/dz)^2
uniform sampler2DArray waveJacobian;  // dDx/dx, dDz/dz, dDx/dz
uniform float waveAmp;
uniform float waveChoppy;
uniform samplerCube skybox;
// Sun shadows: cascades + the global map (shadow.glsl), 4-tap filter.
#define SHADOW_TAPS 4
#include "shadow.glsl"
uniform float waveNormalStrength;
// Each cascade's patches per body radius (Main.c: true size), on the cube
// faces of wavecube.glsl.
#include "wavecube.glsl"
uniform float waveTilings[WAVE_CASCADES];
// Each cascade's drawn size over true size: its heights scale with its
// patches, so its slopes do not change (water.vert).
uniform float waveCascadeScales[WAVE_CASCADES];
// Field units (body radii per unit waveAmp) to the true swell, as
// water.vert's geometry: slopes come out in m/m.
uniform float waveGeomScale;
uniform float time;
// Baked planet height field (heightbake.frag), the same texture quad.frag
// shades from, so the land/ocean mask lines up with the coastline the planet
// shader draws. lodK folds the pixel/texel angular-size ratio into the lod;
// it must be the same formula quad.frag uses or the discard edge drifts off
// the drawn coastline.
uniform samplerCube heightMap;
#include "planetmap.glsl"
uniform float lodK;
// Sea level + ocean depth from the baked, eroded height field (TerrainGen.c).
// The identical seaLevel quad.frag ramps with, so the discard edge lands on
// the drawn coastline; oceanDepth normalizes the depth tint (it replaces the
// old hardcoded 0.22 constant).
uniform float seaLevel;
uniform float oceanDepth;
// Seafloor shelf depth (planet radii), same profile as quad.vert, so the
// water column thickness behind each pixel matches the drawn seafloor.
uniform float oceanRelief;
// Column absorption, shared with quad.frag (which skips the seafloor this
// water hides and tints what it lets through).
#include "watercolumn.glsl"
// 1 when the camera is outside the water shell; gates the back-face early-out.
uniform float outsideShell;

const float PI = 3.14159265359;

// Camera-relative rendering: fragPos, lightPosition and bodyCenter (the
// planet centre) are all relative to the camera, which sits at the origin
// (cameraPos = lodCameraPos = 0). Planet-local positions are fragPos -
// bodyCenter.
uniform vec3 lodCameraPos;
uniform vec3 bodyCenter;

// Extinction toward the camera, sunlight colour, skylight (Atmosphere.c).
#include "atmosurface.glsl"

#include "ringshadow.glsl"
// Cloud shadows (Clouds.c).
#include "cloudlayer.glsl"

// Seascape (Alexander Alekseev aka TDM) sea palette and lighting terms.
const vec3 SEA_BASE = vec3(0.0, 0.09, 0.18);
const vec3 SEA_WATER_COLOR = vec3(0.8, 0.9, 0.6) * 0.6;

float diffuse(vec3 n, vec3 l, float p) {
	return pow(dot(n, l) * 0.4 + 0.6, p);
}

// Filtered-ocean BRDF (Bruneton, Neyret & Holzschuch 2010, "Real-time
// Realistic Ocean Lighting using Seamless Transitions from Geometry to
// BRDF"). The surface inside a pixel is a Gaussian distribution of slopes
// around the mean (filtered) normal N, with variances S = (east, north) in
// the tangent frame (Tx, Ty) of N. Where waves are resolved S is small and
// the glint sparkles off them; where a pixel averages them away they move
// into S and the same formula gives the broad, smooth sun glint seen from
// space, with no tiling and no shimmer. Anisotropic Beckmann + Smith.
// Wind-wave roughness (Cox & Munk 1954, clean sea): slope variance upwind
// and crosswind of the local wind (oceanMap below). Real wind waves span
// ~1 cm to ~100 m, far below what the FFT patch resolves at planet scale,
// so the share of them smaller than a pixel joins S: nearly all of it from
// orbit, fading only once pixels shrink to metres.
// Body radius in metres (setBodyUniforms; WorldConfig.h).
uniform float planetRadiusM;
const float WIND_WAVE_MIN_M = 0.01;
const float WIND_WAVE_MAX_M = 100.0;
// Same sun radiance as quad.frag, so glint and land share one sun.
const vec3 SUN_RADIANCE = vec3(1.0, 0.97, 0.90) * 1.15;
// Artistic gain on the sun glint only. The scene's sun is no brighter than
// the land lighting, so the physical glint (~F * p / 4) reads as muted
// seen from above. Only there: toward grazing, Fresnel and the BRDF's
// 1 / cos(view) already make the reflection blinding, and the gain washed
// the sea out into a white sheet. GLINT_GAIN_VIEW: cosine of the view
// angle to the surface over which it ramps from 1 to the full gain.
const float GLINT_INTENSITY = 4.0;
const vec2 GLINT_GAIN_VIEW = vec2(0.15, 0.6);

// ---- Living-planet sea state (TerrainGen.c bakeOcean) ----
// R wind speed / windMax, GB wind direction (east, north) * 0.5 + 0.5,
// A fetch (0 in the lee of land, 1 over a long run of open ocean). The
// same prevailing winds carry the land's rain, so stormy seas, calm
// doldrums and wet windward coasts line up.
uniform samplerCube oceanMap;
uniform float windMax;
// Whitecap area fraction (Monahan & O'Muircheartaigh 1980): 3.84e-6 U^3.41,
// ~0.3% at 7 m/s, ~4% in a 15 m/s gale. The gain lifts it a little so the
// stormy belts read from orbit.
const float WHITECAP_GAIN = 1.5;
// Displacement Jacobian range over which the shading slope hands over from
// the plain height field (folded crest, J <= x) to the choppy one (J >= y).
const vec2 FOLD_J = vec2(0.0, 0.3);
// Shore foam, in metres from the coastline (depth / seabed slope, from the
// baked maps). It used to be sized in height-map depth units, so its width
// on the ground followed the seabed slope: on a gentle shelf the "thin" wet
// line was a wide glowing ribbon, and the surf bands packed below a pixel
// apart and aliased into hatching.
// Swash: the wet foam line where the water meets the beach.
const float WASH_WIDTH_M = 25.0;
// Breaking waves: crest lines along the shore within SURF_ZONE_M of it,
// SURF_SPACING_M apart, rolling shoreward at SURF_SPEED_MPS. These are
// true-size values; the surf belongs to the swell, so it is drawn at the
// swell's Wave scale (Froude: lengths x s, speed x sqrt(s)). At true size
// the bands were below a pixel from any altitude the coast is seen from.
const float SURF_ZONE_M = 400.0;
const float SURF_SPACING_M = 60.0;
const float SURF_SPEED_MPS = 6.0;
// Mean of the crest band profile (below), what bands below a pixel average to.
const float SURF_BAND_MEAN = 0.22;

// ---- Sea ice ----
// Same temperature as quad.frag's land (latitude, planet offset, noise),
// at sea level. The sea freezes a little colder than land snow settles
// (salt, ocean heat): SEA_ICE_OFFSET (seastate.glsl).
uniform float tempOffset;
uniform float snowTemp;
uniform vec3 snowColor;           // linear, as quad.frag's snow
const vec3 ICE_BARE = vec3(0.30, 0.42, 0.52);   // linear albedo, young blue ice
const vec3 ICE_AMBIENT = vec3(0.10, 0.12, 0.16); // quad.frag's ambientSky

// Per-cascade wave strength over the planet, and the value noise (same as
// water.vert).
#include "seastate.glsl"

float erfcApprox(float x)
{
	return 2.0 * exp(-x * x) / (2.319 * x + sqrt(4.0 + 1.52 * x * x));
}

// Smith Lambda for a Gaussian slope distribution with variance s2 along the
// direction's azimuth.
float smithLambda(float cosT, float s2)
{
	float v = cosT / sqrt(max((1.0 - cosT * cosT) * 2.0 * s2, 1e-10));
	return max(0.0, (exp(-v * v) - v * sqrt(PI) * erfcApprox(v)) / (2.0 * v * sqrt(PI)));
}

// Slope variance along the azimuth of d.
float dirVar(vec3 d, vec3 Tx, vec3 Ty, vec2 S)
{
	vec2 p = vec2(dot(d, Tx), dot(d, Ty));
	float l2 = dot(p, p);
	if (l2 < 1e-10)
		return 0.5 * (S.x + S.y);
	return dot(p * p, S) / l2;
}

// Reflected sun radiance per unit sun irradiance (BRDF * cos(theta_L)).
float oceanSunBRDF(vec3 L, vec3 V, vec3 N, vec3 Tx, vec3 Ty, vec2 S)
{
	vec3 H = normalize(L + V);
	float zH = max(dot(H, N), 1e-3);
	vec2 zeta = vec2(dot(H, Tx), dot(H, Ty)) / zH;
	float p = exp(-0.5 * dot(zeta * zeta, 1.0 / S)) / (2.0 * PI * sqrt(S.x * S.y));
	float zL = max(dot(L, N), 0.01);
	float zV = max(dot(V, N), 0.01);
	float F = 0.02 + 0.98 * pow(1.0 - max(dot(V, H), 0.0), 5.0);
	float G = 1.0 + smithLambda(zL, dirVar(L, Tx, Ty, S)) + smithLambda(zV, dirVar(V, Tx, Ty, S));
	float zH2 = zH * zH;
	return F * p / (4.0 * zV * zH2 * zH2 * G);
}

// Fresnel averaged over the slope distribution seen from V (Bruneton's fit,
// plus water's F0): rough water at grazing angles mirrors less than a
// perfect plane would.
float meanFresnel(float cosV, float sigmaV)
{
	float f = pow(1.0 - cosV, 5.0 * exp(-2.69 * sigmaV)) / (1.0 + 22.7 * pow(sigmaV, 1.5));
	return 0.02 + 0.98 * f;
}

// Sky radiance for the reflection: the atmosphere dome's single-scattering
// march (atmosphere.frag inScatter) along the reflected ray, from the water
// surface out to the shell, so the sea mirrors the same sky the dome draws.
// Fewer view steps than the dome; the sun leg uses the dome's unclamped
// density so samples whose sun ray passes through the planet go dark at
// night exactly as the dome's do. viewT returns the transmittance along the
// ray (stars behind the sky). Planet-local p, unit dir and l.
uniform float E;
uniform float G_M;
const int NUM_SKY_STEPS = 3;
const int NUM_SKY_SUN_STEPS = 2;
// Fresnel range over which the reflection blends from SKY_FALLBACK (below)
// to the full march (above), and the fallback itself: a daylight sky in the
// dome's display units, scaled by the twilight fade and the sun's colour.
const vec2 SKY_MARCH_FRESNEL = vec2(0.03, 0.05);
const vec3 SKY_FALLBACK = vec3(0.12, 0.22, 0.40);

float skySunOptic(vec3 p, vec3 q, float scaleH, float scaleL)
{
	vec3 step = (q - p) / float(NUM_SKY_SUN_STEPS);
	vec3 v = p + step * 0.5;
	float sum = 0.0;
	for (int i = 0; i < NUM_SKY_SUN_STEPS; i++) {
		sum += exp(min(-(length(v) - fInnerRadius) * scaleH, 80.0));
		v += step;
	}
	return sum * length(step) * scaleL;
}

vec3 skyRadiance(vec3 p, vec3 dir, vec3 l, out vec3 viewT)
{
	float scaleH = 1.0 / fScaleHeight;
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);
	vec3 ext = K_R * C_R + K_M;

	float tEnd = max(shellIntersection(p, dir, fOuterRadius).y, 0.0);
	float len = tEnd / float(NUM_SKY_STEPS);
	float segL = len * scaleL;
	vec3 v = p + dir * (len * 0.5);
	vec3 sum = vec3(0.0);
	float viewOptic = 0.0;
	for (int i = 0; i < NUM_SKY_STEPS; i++) {
		float dv = atmoDensity(length(v) - fInnerRadius);
		float tSun = max(shellIntersection(v, l, fOuterRadius).y, 0.0);
		float n = (viewOptic + 0.5 * dv * segL
		         + skySunOptic(v, v + l * tSun, scaleH, scaleL)) * (4.0 * PI);
		sum += dv * exp(-n * ext) * ringTransmittance(v, l, 1e30);
		viewOptic += dv * segL;
		v += dir * len;
	}
	sum *= segL;
	viewT = exp(-viewOptic * (4.0 * PI) * ext);
	float c = dot(dir, -l);
	float cc = c * c;
	float g2 = G_M * G_M;
	float mb = 1.0 + g2 - 2.0 * G_M * c;
	float mie = 1.5 * (1.0 - g2) * (1.0 + cc) / (mb * sqrt(mb) * (2.0 + g2));
	return sum * (K_R * C_R * 0.75 * (1.0 + cc) + K_M * mie) * E;
}

// Tone map (and its inverse for the reflected sky), shared by every lit shader.
#include "tonemap.glsl"

void main()
{
	// Land mask: the planet renders its height field only in the fragment
	// (pure unit-sphere geometry), so the water shell discards where the
	// planet shows land, letting continents stand through the ocean.
	vec3 dObj = normalize(fSurfDir);

	// Cheap early-out: the shell is drawn double-sided (patch skirts need
	// culling off), so its back-facing far-hemisphere fragments would run
	// this whole shader only to lose the depth test against the opaque
	// planet. Discarding them first lets drivers recover early-Z. Gated off
	// while the camera is inside the shell (surface view).
	vec3 Vv = cameraPos - fragPos;
	vec3 localPos = fragPos - bodyCenter;
	if (outsideShell > 0.5 && dot(Vv, localPos) < 0.0)
		discard;

	float zc = clamp(dObj.z, -1.0, 1.0);
	float cosLat = max(sqrt(max(1.0 - zc*zc, 1e-4)), 0.02);
	float graz = max(abs(dot(normalize(localPos), normalize(Vv))), 0.15);
	float lod = log2(max(length(Vv)*lodK, 1e-4)) - log2(graz) + planetMapLodBias(dObj);
	float h = planetMapFine(heightMap, dObj, clamp(lod, 0.0, planetMapMaxLod(heightMap)));
	float e = clamp(h, 0.0, 1.0);
	if (e > seaLevel)
		discard;

	// Water depth below sea level in height units, normalized by the 95th
	// percentile depth of the eroded field so the tint spans its real range.
	float depth = seaLevel - e;
	float depthT = clamp(depth / max(oceanDepth, 1e-4), 0.0, 1.0);
	// Transmittance down to the seafloor and back along the view ray
	// (watercolumn.glsl): 1 at the coastline, so the water edge fades into
	// the shore.
	float transmit = waterTransmit(depthT, oceanRelief, graz);

	// Geographic east/north frame: the axes the wind (oceanMap) and the
	// BRDF's slope variances S are expressed in.
	vec3 T = normalize(vec3(-dObj.y, dObj.x, 1e-4));
	vec3 B = normalize(cross(dObj, T));

	// Sea state (seastate.glsl, as water.vert): each cascade's strength from
	// the shore, the baked wind and fetch, swell trains, gusts and the ice.
	// Only the resolved waves (gain) carry the swell and gust noise. The
	// roughness the mips and the BRDF average them into takes the noise-free
	// gains (gainS): from orbit the noise there turned the glint into
	// splotchy white patches.
	//
	// Pixel footprint (body radii) from distance and view angle alone, the
	// height-map lod's formula above (dist * lodK in map texels, / graz for
	// the stretch toward grazing). Not screen derivatives of fSurfDir: that
	// is the undisplaced point, while the triangles are drawn displaced
	// (swell + choppy shift), so its derivatives are constant per triangle,
	// jump at every edge and blow up where the chop squeezes one. Through
	// the mips and the roughness they lit the triangle edges up inside the
	// glint. The wave mips were isotropic anyway (no anisotropic filtering).
	float pixelAngle = length(Vv) * lodK * planetMapTexel(heightMap) / graz;
	float seaLod = log2(max(pixelAngle / planetMapTexel(oceanMap), 1e-6)) + planetMapLodBias(dObj);
	vec4 seaState = textureLod(oceanMap, dObj, clamp(seaLod, 0.0, planetMapMaxLod(oceanMap)));
	float U = seaState.r * windMax;
	float fetchF = seaState.a;
	float gust = seaGust(dObj, pixelAngle);
	float iceCalm = seaIceCalm(dObj);
	vec3 gain = waveCascadeGains(depthT, U, fetchF, iceCalm, gust, seaSwell(dObj));
	vec3 gainS = waveCascadeGains(depthT, U, fetchF, iceCalm, 1.0, 1.0);

	// Wave slopes, summed over the cube faces covering this point
	// (wavecube.glsl; one face except in the narrow edge bands) and the
	// cascades. Their mips (from the pixel footprint) filter the waves out
	// with distance and the BRDF below picks up what they lose.
	float waveRes = float(textureSize(waveSlope, 0).x);
	vec3 fw = waveFaceWeights(dObj);
	float fNorm = waveFaceNorm(fw);
	// Finest cascade's rms slope (its top mip), to normalize the surf
	// breakup noise taken from it.
	vec4 fineMs = textureLod(waveSlope, vec3(0.5, 0.5, float(WAVE_CASCADES - 1)), 16.0);
	float fineRms = sqrt(max(fineMs.b + fineMs.a, 1e-12));
	vec3 slope = vec3(0.0);   // body-local surface gradient
	vec2 S = vec2(0.0);       // slope variance the pixel averages away (east, north)
	float J = 0.0;            // displacement Jacobian - 1
	float breakupN = 0.0;     // finest cascade's slope, for the surf crests
	for (int k = 0; k < 3; k++) {
		if (fw[k] <= 0.0)
			continue;
		vec3 E0, E1;
		vec2 q = waveFace(k, dObj, E0, E1);
		// Sum the cascades' slopes, displacement derivatives and slope
		// variances on the face axes first. Each cascade's map is rotated
		// (wavecube.glsl), so what it returns along its own axes is turned
		// back: slopes by R's transpose, the displacement Jacobian (a
		// tensor) by R^T M R, the variances onto the face axes.
		vec2 sl = vec2(0.0);
		mat2 jm = mat2(0.0);
		vec2 Sq = vec2(0.0);
		for (int c = 0; c < WAVE_CASCADES; c++) {
			float t = waveTilings[c];
			mat2 R = waveCascadeRot(k, c);
			vec3 uv = vec3(R * (q * t) + waveCascadeOffset(k, c), float(c));
			// Footprint in this cascade's texels (face coordinates are
			// body radii near the face centre).
			float wlod = log2(max(pixelAngle * t * waveRes, 1e-6));
			// FFT field to true slope per unit face coordinate: amplitude,
			// field units to metres of swell, patches per radius, sea state.
			float base = waveAmp * waveGeomScale * waveCascadeScales[c] * t;
			float K = base * gain[c];
			float Ks = base * gainS[c];
			vec4 slTex = textureLod(waveSlope, uv, wlod);
			sl += (slTex.rg * K) * R;
			vec3 jt = textureLod(waveJacobian, uv, wlod).rgb * (K * waveChoppy);
			jm += transpose(R) * mat2(jt.r, jt.b, jt.b, jt.g) * R;
			// Mean square minus the square of the mean, both from the same
			// mip fetch: what this cascade's mip averaged away.
			vec2 vr = max(slTex.ba - slTex.rg * slTex.rg, 0.0) * (Ks * Ks);
			vec2 r2 = vec2(R[0][0] * R[0][0], R[0][1] * R[0][1]);   // cos^2, sin^2
			Sq += vec2(r2.x * vr.x + r2.y * vr.y, r2.y * vr.x + r2.x * vr.y);
			if (c == WAVE_CASCADES - 1)
				breakupN += fw[k] * (slTex.r + slTex.g) / fineRms;
		}
		float jxx = jm[0][0], jzz = jm[1][1], jxz = jm[0][1];
		// Jacobian of the choppy displacement: < 1 where crests pinch, < 0 folds.
		float Jk = (1.0 + jxx) * (1.0 + jzz) - jxz * jxz;
		// Slope of the displaced surface = cross of its tangents
		// (1+jxx, jzx, sl.x) x (jxz, 1+jzz, sl.y), rescaled to unit up.
		// Dividing by J steepens the pinched crests, without bound as they
		// fold (J -> 0, then < 0): a clamp there left near-vertical normals
		// facing away from the light, dark dots along the crests. Over the
		// fold the plain height-field slope sl takes over instead (the
		// crest is foam there anyway: capsResolved below).
		vec2 sheared = -vec2(jxz*sl.y - sl.x*(1.0 + jzz),
		                     sl.x*jxz - (1.0 + jxx)*sl.y) / max(Jk, FOLD_J.y);
		vec2 fftSlope = mix(sl, sheared, smoothstep(FOLD_J.x, FOLD_J.y, Jk));
		slope += fw[k] * waveFaceGrad(fftSlope * waveNormalStrength, E0, E1);

		// Variance per face axis, turned onto east/north.
		Sq *= waveNormalStrength * waveNormalStrength;
		vec3 G0 = waveFaceGrad(vec2(1.0, 0.0), E0, E1);
		vec3 G1 = waveFaceGrad(vec2(0.0, 1.0), E0, E1);
		vec2 g0 = vec2(dot(G0, T), dot(G0, B));
		vec2 g1 = vec2(dot(G1, T), dot(G1, B));
		S += fw[k] * (Sq.x * g0 * g0 + Sq.y * g1 * g1);
		J += fw[k] * (Jk - 1.0);
	}
	slope *= fNorm;
	J = 1.0 + J * fNorm;
	breakupN *= fNorm;

	// Unresolved wind waves: slope variance is spread evenly over log
	// wavelength (k^-4 height spectrum), so the share below the pixel
	// footprint is its log position in the wind-wave band.
	float footM = pixelAngle * planetRadiusM;
	float unresolved = clamp(log2(footM / WIND_WAVE_MIN_M) / log2(WIND_WAVE_MAX_M / WIND_WAVE_MIN_M), 0.0, 1.0);
	// Local wind (the baked field, without the gust noise, as gainS): short
	// wind waves develop within a few km, so fetch only trims the roughness
	// a little (it matters far more for the swell and whitecaps). Cox-Munk
	// upwind / crosswind variances, rotated from the wind's axes onto
	// east/north.
	vec2 wdir = seaState.gb * 2.0 - 1.0;
	wdir = dot(wdir, wdir) > 1e-4 ? normalize(wdir) : vec2(1.0, 0.0);
	float Ur = U * mix(0.7, 1.0, fetchF);
	vec2 coxMunk = vec2(0.00316 * Ur, 0.003 + 0.00192 * Ur);
	vec2 w2 = wdir * wdir;
	S += unresolved * vec2(coxMunk.x * w2.x + coxMunk.y * w2.y,
	                       coxMunk.x * w2.y + coxMunk.y * w2.x) + 1e-4;

	vec3 Nobj = normalize(dObj - slope);
	// model is uniform scale * rotation, so inverse(mat3(model)) * n is
	// n * mat3(model) up to a scale the normalize removes.
	vec3 N = normalize(Nobj * mat3(model));
	// East/north frame re-orthogonalized around the mean normal, in world
	// space: the axes S is expressed in (cross(N, Tx) points north, like B).
	vec3 Tw = normalize(T * mat3(model));
	vec3 Tx = normalize(Tw - N * dot(Tw, N));
	vec3 Ty = cross(N, Tx);

	vec3 V = normalize(cameraPos - fragPos);
	vec3 eye = -V;
	vec3 L = normalize(lightPosition - fragPos);
	vec3 dist = fragPos - cameraPos;

	// Cast shadows, faded out below the geometric terminator where the
	// planet body self-occludes (matches quad.frag so the coast is seamless).
	vec3 upGeo = normalize(localPos);
	float shadowWeight = smoothstep(-0.06, 0.0, dot(upGeo, L));
	/* No lookup on the night side, where the weight is 0. */
	float sun = shadowWeight > 0.0 ? mix(1.0, sunVisibility(fragPos, upGeo, L), shadowWeight) : 1.0;
	sun *= ringTransmittance(fragPos - ringCenter, L, length(lightPosition - fragPos));
	sun *= cloudShadow(localPos, L, length(dist) * cloudPixAngle);
	// Reddened, setting sunlight (quad.frag) and the twilight skylight fade.
	vec3 sunT = sunTransmittance(localPos, L, ringSunAngle);
	vec3 sunCol = sunT * sun;
	float sky = skyFraction(dot(upGeo, L));

	// Fresnel sky reflection (water F0 = 0.02, averaged over the pixel's
	// slope distribution), uncapped: at grazing angles the sea is a mirror
	// of the bright horizon sky. Rays
	// that dip under the horizon (wave backs) would really see other water
	// near the horizon, so they are flattened onto it rather than going
	// black; skyVis only trims the deepest of them.
	vec3 up = normalize(localPos);
	vec3 R = reflect(eye, N);
	float rUp = dot(R, up);
	float skyVis = smoothstep(-0.35, 0.05, rUp);
	float nDotV = clamp(dot(N, V), 0.0, 1.0);
	float fresnel = meanFresnel(nDotV, sqrt(dirVar(V, Tx, Ty, S))) * skyVis;
	vec3 Rsky = normalize(R - up * min(rUp, 0.0) + up * 0.01);
	vec3 skyDisp = vec3(0.0);
	vec3 starT = vec3(1.0);
	if (hasAtmosphere > 0.5) {
		// The march only matters where the sky shows: near-normal views
		// (most of the disc from orbit) reflect ~2% of it, so they take a
		// flat daylight-sky estimate instead, and a short blend band hides
		// the switch. Zenith transmittance stands in for the stars' path.
		vec3 cheapSky = SKY_FALLBACK * sky * mix(vec3(1.0), sunT, 0.5);
		vec3 cheapT = exp(-(K_R * C_R + K_M) * (4.0 * PI) * atmoTauZenith);
		float march = smoothstep(SKY_MARCH_FRESNEL.x, SKY_MARCH_FRESNEL.y, fresnel);
		skyDisp = cheapSky;
		starT = cheapT;
		if (march > 0.0) {
			vec3 pSky = up * max(length(localPos), fInnerRadius);
			vec3 marchT;
			vec3 marched = skyRadiance(pSky, Rsky, L, marchT);
			skyDisp = mix(cheapSky, marched, march);
			starT = mix(cheapT, marchT, march);
		}
	}
	// Dome + skybox, as the sky would draw them looking along Rsky.
	vec3 reflected = displayToLinear(skyDisp + texture(skybox, Rsky).rgb * starT);

	// Body colour (light scattered back out of the water column), linear
	// radiance under the shared tone map: teal shallows to a dark navy deep
	// sea (~6% albedo, the darkest surface on the disc, as on Earth).
	vec3 deepCol = vec3(0.003, 0.015, 0.040);
	vec3 shallowCol = vec3(0.012, 0.100, 0.130);
	vec3 waterCol = mix(shallowCol, deepCol, depthT);

	// Hemispheric sky irradiance: the night side of an ocean is lit by the
	// whole sky dome, not just the sun. Weighted toward shadow so the lit
	// side keeps its sun-driven contrast.
	float upDotN = clamp(dot(N, up), 0.0, 1.0);
	vec3 skyAmbient = mix(vec3(0.010, 0.024, 0.048), vec3(0.030, 0.058, 0.095), upDotN)
	                * (0.25 + 0.75 * (1.0 - sun)) * sky;
	// The body colour is light scattered back out of the water column, so
	// it needs light to scatter: the sun by day, the sky through twilight.
	// Sun term follows Lommel-Seeliger (single scattering in a turbid
	// layer), normalized to 1 for overhead sun and view: it fades smoothly to
	// zero at the terminator but stays flat across the day side and limb.
	float mu0 = max(dot(upGeo, L), 0.0);
	float mu = max(dot(upGeo, V), 0.0);
	float lommel = min(2.0 * mu0 / max(mu0 + mu, 1e-4), 1.0);
	vec3 bodyLight = min(sunCol * lommel + vec3(0.15 * sky) + NIGHT_FLOOR, vec3(1.0));
	vec3 baseCol = waterCol * bodyLight + skyAmbient;

	vec3 refracted = baseCol + diffuse(N, L, 80.0) * SEA_WATER_COLOR * 0.03 * sunCol;
	vec3 color = mix(refracted, reflected, fresnel);

	// Seascape crest brightening.
	float atten = 1.0 / (1.0 + dot(dist, dist) * 0.0000002);
	color += SEA_WATER_COLOR * (fWave * 30.0) * 0.18 * atten * sunCol;

	// Fast subsurface scattering: sun light through wave crests toward
	// the viewer (Jensen/Uncharted approximation).
	float crest = clamp(fWave / 0.008, 0.0, 1.0) * (0.4 + 0.6 * depthT);
	vec3 sssDir = normalize(L + N * 0.3);
	vec3 sssSun = pow(clamp(dot(V, sssDir), 0.0, 1.0), 3.0) * sunCol;
	float sssSky = pow(clamp(dot(V, up), 0.0, 1.0), 2.0) * (1.0 - sun) * sky;
	color += SEA_WATER_COLOR * crest * 1.5 * (sssSun + 0.15 * sssSky);

	// Sun glint from the filtered-ocean BRDF: sparkle where the waves are
	// resolved, a smooth glint where they are averaged into roughness.
	// Tinted by the sun's transmittance.
	float glintGain = mix(1.0, GLINT_INTENSITY, smoothstep(GLINT_GAIN_VIEW.x, GLINT_GAIN_VIEW.y, dot(upGeo, V)));
	float spec = oceanSunBRDF(L, V, N, Tx, Ty, S) * glintGain;
	color += spec * SUN_RADIANCE * sunCol;
	// The light the surface itself sends back (sky mirror + glint). It
	// stays when the column below turns clear, so it is blended on top of
	// the seafloor rather than faded out with the water body (below).
	vec3 surface = reflected * fresnel + spec * SUN_RADIANCE * sunCol;

	// Sea ice: the land's sea-level temperature (quad.frag) with the same
	// noise scale; inside the margin the pack breaks into floes. The floe
	// edge softens with the pixel footprint so it does not alias from orbit.
	// The noise moves the temperature by at most +-0.04: skip it (and all
	// the ice work) where even its coldest value stays ice-free.
	float iceT = snowTemp + SEA_ICE_OFFSET;
	float baseTemp = cosLat * 1.2 - 0.2 + tempOffset;
	float iceN1 = baseTemp - 0.04 < iceT + 0.02 ? vnoise3(dObj * 40.0) : 0.5;
	float seaTemp = baseTemp + (iceN1 - 0.5) * 0.08;
	float iceCover = 1.0 - smoothstep(iceT - 0.06, iceT + 0.02, seaTemp);
	// Floe noise only where there is ice to break up (polar seas).
	float floeN = 0.5;
	float ice = 0.0;
	if (iceCover > 0.0) {
		floeN = 0.6 * vnoise3(dObj * 240.0) + 0.4 * vnoise3(dObj * 960.0);
		float floeEdge = 0.04 + pixelAngle * 960.0 * 0.5;
		ice = smoothstep(floeN - floeEdge, floeN + floeEdge, iceCover);
	}

	// Foam. The surf is only drawn where a pixel is small enough to see it
	// (radians of the unit sphere per pixel): visible from low altitude,
	// gone from orbit, where real surf is far below a pixel.
	float surfVis = 1.0 - smoothstep(2e-5, 1.5e-4, pixelAngle);
	// Distance to the coastline (m): the depth over the seabed slope, the
	// slope by differences of the very height samples the depth (and the
	// land discard) come from, half a texel of their mip apart. Within a
	// texel the filtered field is near linear, so this is the true distance
	// to the drawn waterline; the baked slope map disagrees with it inside
	// texels and left the swash line as a row of dots at texel centres.
	float hLod = clamp(lod, 0.0, planetMapMaxLod(heightMap));
	float hEps = 0.5 * planetMapTexel(heightMap) * exp2(hLod);
	float hE = planetMapFine(heightMap, normalize(dObj + T * hEps), hLod);
	float hN = planetMapFine(heightMap, normalize(dObj + B * hEps), hLod);
	float seabed = length(vec2(hE - h, hN - h)) / hEps;
	float coastM = depth / max(seabed, 1e-6) * planetRadiusM;
	// Breaking waves roll in parallel to the shore; strongest on windward
	// coasts facing a long fetch, small on lee shores. The finest cascade
	// breaks the crest lines up along the coast. Bands closer than ~2 px
	// fade to their mean instead of aliasing.
	float surfStrength = smoothstep(0.1, 0.6, fetchF) * smoothstep(2.0, 8.0, U);
	float surfScale = waveCascadeScales[0];
	float surfZoneM = SURF_ZONE_M * surfScale;
	float surfSpacingM = SURF_SPACING_M * surfScale;
	float surfSpeed = SURF_SPEED_MPS * sqrt(surfScale);
	float surfZone = 1.0 - smoothstep(0.5 * surfZoneM, surfZoneM, coastM);
	// Only where the swell can break: water no deeper than about where it
	// calms (seastate.glsl). depth / slope extrapolates a "coast" beside
	// any underwater rise, which lit shoals with triangular surf patches
	// (the bilinear height field's tents) far out from the real shore.
	surfZone *= 1.0 - smoothstep(1.0, 2.0, seaDepthM(depthT) / max(waveShoreDepths[0], 1e-3));
	float surfW = fract(coastM / surfSpacingM + fract(time * 100.0 * surfSpeed / surfSpacingM));
	float crestBand = smoothstep(0.0, 0.06, surfW) * (1.0 - smoothstep(0.06, 0.45, surfW));
	crestBand = mix(SURF_BAND_MEAN, crestBand, 1.0 - smoothstep(0.25, 0.5, footM / surfSpacingM));
	float breakup = smoothstep(-0.4, 0.4, breakupN);
	float surf = crestBand * surfZone * mix(0.15, 1.0, surfStrength) * mix(0.4, 1.0, breakup);
	// Swash line. Narrower than a pixel, it covers the pixel at its share
	// of it rather than widening to a full pixel of foam.
	float washW = max(WASH_WIDTH_M, footM);
	float wash = (1.0 - smoothstep(0.0, washW, coastM)) * (WASH_WIDTH_M / washW);
	float shore = clamp(wash * 0.8 + surf, 0.0, 1.0) * surfVis;
	// Whitecaps: where the waves are resolved, where the choppy
	// displacement pinches (low Jacobian) in a strong, developed sea; where
	// they are not (most views at planet scale), the Monahan area fraction.
	// The resolved caps follow the gusts; the orbital coverage stays on the
	// smooth wind field, like the glint roughness.
	float capsResolved = (1.0 - smoothstep(0.3, 0.8, J)) * smoothstep(4.0, 10.0, U * gust) * fetchF;
	float capsCoverage = min(3.84e-6 * pow(U, 3.41) * fetchF * WHITECAP_GAIN, 1.0);
	float whitecaps = mix(capsResolved, capsCoverage, unresolved) * (0.25 + 0.75 * depthT);
	float foam = clamp(shore + whitecaps, 0.0, 1.0) * (1.0 - ice);
	vec3 foamCol = vec3(0.82, 0.87, 0.90)
	             * ((0.30 * sky * mix(0.25, 1.0, sun) + NIGHT_FLOOR) + 0.85 * max(dot(N, L), 0.0) * sunCol);
	color = mix(color, foamCol, foam);

	// Ice shading, lit like quad.frag's land (sky ambient + Lambert sun):
	// snow-covered pack (the land's snow colour) inside, bare blue ice and
	// floes toward the open water.
	vec3 iceAlb = mix(ICE_BARE, snowColor, smoothstep(0.5, 0.9, iceCover) * (0.6 + 0.4 * floeN));
	float iceNdl = max(dot(upGeo, L), 0.0);
	vec3 iceLit = (ICE_AMBIENT * sky + NIGHT_FLOOR) * iceAlb
	            + iceAlb / PI * iceNdl * SUN_RADIANCE * sunCol;
	color = mix(color, iceLit, ice);

	// Surface extinction, matching quad.frag so the coastline stays seamless.
	// Extinction only (a multiply), so one evaluation serves both terms.
	if (hasAtmosphere > 0.5) {
		vec3 ext = applyAtmosphere(vec3(1.0), lodCameraPos - bodyCenter, localPos);
		color *= ext;
		surface *= ext;
	}

	// Opacity: the seafloor shows through what the water column, the surface
	// reflection and foam let pass. Premultiplied (Main.c blends ONE,
	// ONE_MINUS_SRC_ALPHA): where the column turns clear the surface's own
	// reflection and glint are added over the floor instead of fading out
	// with the water body, which left a dark ring around every shallow.
	float alpha = 1.0 - transmit * (1.0 - fresnel) * (1.0 - foam);
	alpha = mix(alpha, 1.0, ice);
	vec3 outCol = toneMap(max(color, 0.0)) * alpha
	            + toneMap(max(surface, 0.0)) * (1.0 - alpha);
	FragColor = vec4(outCol, alpha); if (fWave > 1.0) FragColor = vec4(1.0, 0.0, 0.0, 1.0); if (any(isnan(outCol))) FragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
