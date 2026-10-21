#version 410

in vec3 fSurfDir;
in vec3 fragPos;
in float fWave;

out vec4 FragColor;

uniform mat4 model;
uniform mat4 lightSpace;
uniform vec3 lightPosition;
uniform vec3 cameraPos;
// FFT ocean (Water.c): slopes and displacement derivatives per patch length.
uniform sampler2D waveSlope;     // dh/dx, dh/dz, (dh/dx)^2, (dh/dz)^2
uniform sampler2D waveJacobian;  // dDx/dx, dDz/dz, dDx/dz
uniform float waveAmp;
uniform float waveChoppy;
uniform sampler2D detailNormal;
uniform samplerCube skybox;
uniform sampler2DShadow depthMap;   // see quad.frag sunVisibility
uniform float shadowTexel;
uniform float waveNormalStrength;
uniform float waveTiling;
uniform float time;
// Baked planet height field (heightbake.frag), the same texture quad.frag
// shades from, so the land/ocean mask lines up with the coastline the planet
// shader draws. lodK folds the pixel/texel angular-size ratio into the lod;
// it must be the same formula quad.frag uses or the discard edge drifts off
// the drawn coastline.
uniform sampler2D heightMap;
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
// Light absorbed per planet radius of water path: clear shallows, opaque abyss.
// quad.frag uses the same value to skip seafloor this water hides.
const float WATER_ABSORB = 500.0;
// 1 when the camera is outside the water shell; gates the back-face early-out.
uniform float outsideShell;

const float PI = 3.14159265359;

// Camera-relative rendering: fragPos, lightPosition and bodyCenter (the
// planet centre) are all relative to the camera, which sits at the origin
// (cameraPos = lodCameraPos = 0). Planet-local positions are fragPos -
// bodyCenter.
uniform vec3 lodCameraPos;
uniform vec3 bodyCenter;

// Atmospheric extinction (OpenGLSolar planet.frag port): dims the surface by
// the transmittance along the surface-to-camera path through the dome's shell.
// The atmosphere dome pass adds the in-scattered light over both the limb and
// the planet disc; adding it here as well would count that radiance twice.
// applyAtmosphere takes planet-local eye/fragment positions (shell-centred).
uniform float hasAtmosphere;
uniform float fInnerRadius;
uniform float fOuterRadius;
uniform float fScaleHeight;   // metres per e-fold of density (Atmosphere.c)
#include "atmodensity.glsl"
uniform vec3 C_R;

// Keep the miss sentinel beyond the largest supported shell radius.
const float ATMO_MAX = 1e30;
uniform float K_R; // Rayleigh / Mie constants, shared with atmosphere.frag (Atmosphere.c)
uniform float K_M;
const int NUM_ATMO_STEPS = 5;
// atmoOptic straight up from fInnerRadius to fOuterRadius (the same
// 5-sample midpoint march), computed once on the CPU (Atmosphere.c).
uniform float atmoTauZenith;

vec2 shellIntersection(vec3 p, vec3 dir, float radius)
{
	float b = dot(p, dir);
	float c = dot(p, p) - radius * radius;
	float d = b * b - c;
	if (d < 0.0) {
		return vec2(ATMO_MAX, -ATMO_MAX);
	}
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

float atmoOptic(vec3 p, vec3 q, float scaleH, float scaleL)
{
	vec3 step = (q - p) / float(NUM_ATMO_STEPS);
	vec3 v = p + step * 0.5;
	float sum = 0.0;
	for (int i = 0; i < NUM_ATMO_STEPS; i++) {
		// Clamped at the ground (same as quad.frag).
		sum += atmoDensity(length(v) - fInnerRadius);
		v += step;
	}
	return sum * length(step) * scaleL;
}

vec3 applyAtmosphere(vec3 color, vec3 eye, vec3 frag)
{
	float scaleH = 1.0 / fScaleHeight;
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);

	// Ray in the shell-centred frame: eye = camera, fragment at distance tFrag.
	vec3 ray = frag - eye;
	float tFrag = length(ray);
	vec3 dir = ray / max(tFrag, 1e-6);

	vec2 e = shellIntersection(eye, dir, fOuterRadius);
	if (e.x > e.y) {
		return color; // view ray misses the shell entirely
	}
	vec2 f = shellIntersection(eye, dir, fInnerRadius);
	// Clip the path at the shell exit, the inner sphere, and the actual
	// fragment hit: the tFrag term handles displaced surfaces, where f.x
	// would overshoot or fall short of the visible surface.
	// camera inside the shell: the path starts at the camera
	e.x = max(e.x, 0.0);
	e.y = min(e.y, tFrag);
	if (f.x > e.x) {
		e.y = min(e.y, f.x);
	}
	if (e.y <= e.x) {
		return color;
	}

	float tau = atmoOptic(eye + dir * e.x, eye + dir * e.y, scaleH, scaleL) * (4.0 * PI);
	return color * exp(-(K_R * C_R + K_M) * tau);
}

// Sunlight colour relative to an overhead sun and skylight fraction; must
// match quad.frag (see the comments there) so the coastline stays seamless.
#define SUN_EXTINCTION 0.5
vec3 sunTransmittance(vec3 p, vec3 l, float sunAngle)
{
	vec3 up = normalize(p);
	float horizon = smoothstep(-sunAngle, sunAngle, dot(up, l));
	if (hasAtmosphere < 0.5 || horizon <= 0.0)
		return vec3(horizon);
	float scaleH = 1.0 / fScaleHeight;
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);
	p = up * max(length(p), fInnerRadius);
	float tSun = shellIntersection(p, l, fOuterRadius).y;
	float tau = atmoOptic(p, p + l * max(tSun, 0.0), scaleH, scaleL);
	float excess = max(tau - atmoTauZenith, 0.0) * (4.0 * PI) * SUN_EXTINCTION;
	return exp(-(K_R * C_R + K_M) * excess) * horizon;
}

#define NIGHT_FLOOR vec3(0.003, 0.004, 0.006)
float skyFraction(float sunUp)
{
	return smoothstep(-0.2, 0.3, sunUp);
}

// Sun visibility from the shadow map (quad.frag's lookup and offsets): 1 =
// lit. Four bilinear-PCF taps half a texel out cover the same ~3x3-texel
// footprint as quad.frag's nine, at under half the cost.
float sunVisibility(vec3 p, vec3 n, vec3 l) {
	float ndl = clamp(dot(n, l), 0.0, 1.0);
	p += n * shadowTexel * (1.0 + 2.0 * (1.0 - ndl)) + l * shadowTexel;
	vec4 c = vec4(p, 1.0) * lightSpace;
	vec3 s = c.xyz / c.w * 0.5 + 0.5;
	if (s.z >= 1.0)
		return 1.0;
	vec2 texel = 0.5 / vec2(textureSize(depthMap, 0));
	float lit = texture(depthMap, vec3(s.xy + vec2(-texel.x, -texel.y), s.z))
	          + texture(depthMap, vec3(s.xy + vec2( texel.x, -texel.y), s.z))
	          + texture(depthMap, vec3(s.xy + vec2(-texel.x,  texel.y), s.z))
	          + texture(depthMap, vec3(s.xy + vec2( texel.x,  texel.y), s.z));
	return lit * 0.25;
}

// Ring shadow (same analytic lookup as quad.frag).
uniform float hasRing;
uniform sampler1D ringProfile;
uniform vec3 ringCenter;     // camera-relative planet centre
uniform vec3 ringNormal;
uniform vec2 ringRadii;      // inner, outer (world units)
uniform float ringSunAngle;

float ringTransmittance(vec3 p, vec3 l, float maxT) {
	float mu = dot(l, ringNormal);
	if (hasRing < 0.5 || abs(mu) < 1e-4)
		return 1.0;
	float t = -dot(p, ringNormal) / mu;
	if (t <= 0.0 || t >= maxT)
		return 1.0;
	float r01 = (length(p + l * t) - ringRadii.x) / (ringRadii.y - ringRadii.x);
	if (r01 <= 0.0 || r01 >= 1.0)
		return 1.0;
	float texelW = (ringRadii.y - ringRadii.x) / float(textureSize(ringProfile, 0));
	float tau = textureLod(ringProfile, r01, log2(max(t * ringSunAngle / texelW, 1.0))).a;
	return exp(-tau / abs(mu));
}

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
uniform float detailVar;          // mean squared slope of one detail layer
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
// the land lighting, so the physical glint (~F * p / 4) peaks near 0.05 and
// reads as muted; this restores the bright glint of the old Seascape lobe
// (~1.0 at its peak) while the BRDF keeps its shape and width.
const float GLINT_INTENSITY = 15.0;

// ---- Living-planet sea state (TerrainGen.c bakeOcean) ----
// R wind speed / windMax, GB wind direction (east, north) * 0.5 + 0.5,
// A fetch (0 in the lee of land, 1 over a long run of open ocean). The
// same prevailing winds carry the land's rain, so stormy seas, calm
// doldrums and wet windward coasts line up.
uniform sampler2D oceanMap;
uniform float windMax;
// Whitecap area fraction (Monahan & O'Muircheartaigh 1980): 3.84e-6 U^3.41,
// ~0.3% at 7 m/s, ~4% in a 15 m/s gale. The gain lifts it a little so the
// stormy belts read from orbit.
const float WHITECAP_GAIN = 1.5;
// Breaking waves: crest lines along the depth contours (waves break where
// the depth falls to about their height), rolling shoreward. SURF_DEPTH is
// the surf zone's width in height units, SURF_FREQ crests per height unit,
// SURF_SPEED crests per time unit (time = seconds / 100).
const float SURF_DEPTH = 0.02;
const float SURF_FREQ = 200.0;
const float SURF_SPEED = 8.0;

// ---- Sea ice ----
// Same temperature as quad.frag's land (latitude, planet offset, noise),
// at sea level. The sea freezes a little colder than land snow settles
// (salt, ocean heat): must match SEA_ICE_OFFSET in water.vert.
uniform float tempOffset;
uniform float snowTemp;
uniform vec3 snowColor;           // linear, as quad.frag's snow
const float SEA_ICE_OFFSET = -0.04;
const vec3 ICE_BARE = vec3(0.30, 0.42, 0.52);   // linear albedo, young blue ice
const vec3 ICE_AMBIENT = vec3(0.10, 0.12, 0.16); // quad.frag's ambientSky

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

// Tone map shared with quad.frag (ACES fit, exposure, gamma 2.2), so land
// and sea go through the same curve and the coastline matches.
const float EXPOSURE = 1.05;
const float GAMMA = 2.2;
vec3 toneMap(vec3 c)
{
	vec3 x = c * EXPOSURE;
	x = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
	return pow(x, vec3(1.0 / GAMMA));
}

// Inverse of toneMap. The dome and skybox are added straight to the
// framebuffer (display values, never tone mapped), so a reflected sky
// converts back to linear radiance here and comes out of toneMap looking
// like the sky it mirrors.
vec3 displayToLinear(vec3 d)
{
	vec3 y = pow(clamp(d, 0.0, 0.95), vec3(GAMMA));
	vec3 a = 2.51 - 2.43 * y;
	vec3 b = 0.03 - 0.59 * y;
	vec3 x = (-b + sqrt(b * b + 4.0 * a * 0.14 * y)) / (2.0 * a);
	return x / EXPOSURE;
}

void main()
{
	// Land mask: the planet renders its height field only in the fragment
	// (pure unit-sphere geometry), so the water shell discards where the
	// planet shows land, letting continents stand through the ocean.
	vec3 dObj = normalize(fSurfDir);
	// Screen-space derivatives must be taken while the whole 2x2 quad is
	// still alive, i.e. before the discards below.
	vec3 dDirX = dFdx(dObj);
	vec3 dDirY = dFdy(dObj);

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
	vec2 huv = vec2((atan(dObj.y, dObj.x)/PI + 1.0)*0.5,
	                (asin(zc)/PI + 0.5));
	float cosLat = max(sqrt(max(1.0 - zc*zc, 1e-4)), 0.02);
	float graz = max(abs(dot(normalize(localPos), normalize(Vv))), 0.15);
	float lod = log2(max(length(Vv)*lodK, 1e-4)) - log2(graz) + log2(1.0/cosLat);
	float h = textureLod(heightMap, huv, clamp(lod, 0.0, 11.0)).r;
	float e = clamp(h, 0.0, 1.0);
	if (e > seaLevel)
		discard;

	// Water depth below sea level in height units, normalized by the 95th
	// percentile depth of the eroded field so the tint spans its real range.
	float depth = seaLevel - e;
	float depthT = clamp(depth / max(oceanDepth, 1e-4), 0.0, 1.0);
	// Beer-Lambert transmittance down to the seafloor and back along the
	// view ray: 1 at the coastline, so the water edge fades into the shore.
	float column = oceanRelief * depthT * depthT;
	float transmit = exp(-WATER_ABSORB * column / graz);

	vec2 duv = huv;
	vec2 uv = duv * waveTiling;

	// Long/lat UV gradients for the mipmapped wave/detail maps, chained
	// analytically from the direction's derivatives (taken before any
	// discard): no 1 -> 0 jump at the atan seam to pick the smallest mip.
	float rxy2 = max(dObj.x*dObj.x + dObj.y*dObj.y, 1e-6);
	vec2 dx = vec2((dObj.x*dDirX.y - dObj.y*dDirX.x) / (2.0*PI*rxy2),
	               dDirX.z / (PI*sqrt(rxy2)));
	vec2 dy = vec2((dObj.x*dDirY.y - dObj.y*dDirY.x) / (2.0*PI*rxy2),
	               dDirY.z / (PI*sqrt(rxy2)));

	// World size (unit-sphere radii) of one wave patch along east/north on
	// the long/lat map; turns the per-patch derivatives into world slopes.
	vec2 patchLen = vec2(2.0*PI*cosLat, PI) / waveTiling;
	vec4 slTex = textureGrad(waveSlope, uv, dx*waveTiling, dy*waveTiling);
	vec2 sl = slTex.rg * waveAmp / patchLen;
	vec3 jt = textureGrad(waveJacobian, uv, dx*waveTiling, dy*waveTiling).rgb * (waveAmp * waveChoppy);
	float jxx = jt.r / patchLen.x;
	float jzz = jt.g / patchLen.y;
	float jxz = jt.b / patchLen.y;   // dDx/dz
	float jzx = jt.b / patchLen.x;   // dDz/dx (same spectrum, east scale)
	// Jacobian of the choppy displacement: < 1 where crests pinch, < 0 folds.
	float J = (1.0 + jxx) * (1.0 + jzz) - jxz * jzx;

	// Normal of the displaced surface = cross of its tangents
	// (1+jxx, jzx, sl.x) x (jxz, 1+jzz, sl.y) in (east, north, up),
	// rescaled to unit up so it reads as a slope.
	vec2 fftSlope = -vec2(jzx*sl.y - sl.x*(1.0 + jzz),
	                      sl.x*jxz - (1.0 + jxx)*sl.y) / max(J, 0.05);

	// FFT swell slope + two scrolling tiled detail-normal layers for
	// high-frequency sparkle (the two-scale method).
	vec2 d1 = (textureGrad(detailNormal, duv*(waveTiling*2.0) + vec2(0.30, 0.17)*time,
	                       dx*(waveTiling*2.0), dy*(waveTiling*2.0)).rg - 0.5) * 2.0;
	vec2 d2 = (textureGrad(detailNormal, duv*(waveTiling*5.0) - vec2(0.19, 0.31)*time,
	                       dx*(waveTiling*5.0), dy*(waveTiling*5.0)).rg - 0.5) * 2.0;
	vec2 slope = waveNormalStrength * fftSlope + d1 * 0.35 + d2 * 0.2;

	// Slope variance the pixel averages away (the BRDF roughness), in the
	// same east/north world units as the slopes. FFT: mean square minus the
	// square of the mean, both from the same mip fetch as sl.
	float ws = waveAmp * waveNormalStrength;
	vec2 sm = sl * waveNormalStrength;
	vec2 S = max(slTex.ba * (ws * ws) / (patchLen * patchLen) - sm * sm, 0.0);
	// Detail layers (isotropic): their mips flatten the sampled normal, so
	// the share they lose returns as variance. Their finest features span a
	// couple of texels, so most of it is gone within ~4 mip levels.
	float lod1 = log2(max(max(length(dx), length(dy)) * waveTiling * 2.0 * 128.0, 1e-6));
	float lod2 = lod1 + log2(2.5);   // second layer is tiled 5/2 as densely
	S += detailVar * (0.35 * 0.35 * smoothstep(0.0, 4.0, lod1)
	                + 0.20 * 0.20 * smoothstep(0.0, 4.0, lod2));
	// Unresolved wind waves: slope variance is spread evenly over log
	// wavelength (k^-4 height spectrum), so the share below the pixel
	// footprint is its log position in the wind-wave band.
	float pixelAngle = max(length(dDirX), length(dDirY));
	float footM = pixelAngle * planetRadiusM;
	float unresolved = clamp(log2(footM / WIND_WAVE_MIN_M) / log2(WIND_WAVE_MAX_M / WIND_WAVE_MIN_M), 0.0, 1.0);
	// Local wind: short wind waves develop within a few km, so fetch only
	// trims the roughness a little (it matters far more for the swell and
	// whitecaps). Cox-Munk upwind / crosswind variances, rotated from the
	// wind's axes onto east/north.
	vec4 seaState = textureGrad(oceanMap, huv, dx, dy);
	float U = seaState.r * windMax;
	float fetchF = seaState.a;
	vec2 wdir = seaState.gb * 2.0 - 1.0;
	wdir = dot(wdir, wdir) > 1e-4 ? normalize(wdir) : vec2(1.0, 0.0);
	float Ur = U * mix(0.7, 1.0, fetchF);
	vec2 coxMunk = vec2(0.00316 * Ur, 0.003 + 0.00192 * Ur);
	vec2 w2 = wdir * wdir;
	S += unresolved * vec2(coxMunk.x * w2.x + coxMunk.y * w2.y,
	                       coxMunk.x * w2.y + coxMunk.y * w2.x) + 1e-4;

	// Tangent basis of the equirectangular (long/lat) wave map on the sphere.
	vec3 T = normalize(vec3(-dObj.y, dObj.x, 1e-4));
	vec3 B = normalize(cross(dObj, T));
	vec3 Nobj = normalize(dObj - (slope.x * T + slope.y * B));
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
	// Tinted by the sun's transmittance; the scalar keeps driving the
	// opacity below.
	float spec = oceanSunBRDF(L, V, N, Tx, Ty, S) * GLINT_INTENSITY;
	float glint = spec * sun * max(sunT.r, max(sunT.g, sunT.b));
	color += spec * SUN_RADIANCE * sunCol;

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
	// Breaking waves roll in along the depth contours; strongest on
	// windward coasts facing a long fetch, small on lee shores. The detail
	// layers break the crest lines up along the coast.
	float surfStrength = smoothstep(0.1, 0.6, fetchF) * smoothstep(2.0, 8.0, U);
	float surfZone = 1.0 - smoothstep(0.5 * SURF_DEPTH, SURF_DEPTH, depth);
	float surfW = fract(depth * SURF_FREQ + time * SURF_SPEED);
	float crestBand = smoothstep(0.0, 0.06, surfW) * (1.0 - smoothstep(0.06, 0.45, surfW));
	float breakup = smoothstep(-0.4, 0.4, d1.x + d2.y);
	float surf = crestBand * surfZone * mix(0.15, 1.0, surfStrength) * mix(0.4, 1.0, breakup);
	// Swash: the thin wet foam line where the water meets the beach.
	float wash = 1.0 - smoothstep(0.0, 0.004, depth);
	float shore = clamp(wash * 0.8 + surf, 0.0, 1.0) * surfVis;
	// Whitecaps: where the waves are resolved, where the choppy
	// displacement pinches (low Jacobian) in a strong, developed sea; where
	// they are not (most views at planet scale), the Monahan area fraction.
	float capsResolved = (1.0 - smoothstep(0.3, 0.8, J)) * smoothstep(4.0, 10.0, U) * fetchF;
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
	if (hasAtmosphere > 0.5) {
		color = applyAtmosphere(color, lodCameraPos - bodyCenter, localPos);
	}

	// Opacity: the seafloor shows through what the water column, the surface
	// reflection and foam let pass; the glint stays opaque so it survives
	// the shallows.
	float alpha = 1.0 - transmit * (1.0 - fresnel) * (1.0 - foam);
	alpha = max(alpha, clamp(glint, 0.0, 1.0));
	alpha = mix(alpha, 1.0, ice);
	FragColor = vec4(toneMap(max(color, 0.0)), alpha);
}
