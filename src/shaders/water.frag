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
uniform sampler2D waveSlope;     // dh/dx, dh/dz
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
uniform vec3 C_R;

// Keep the miss sentinel beyond the largest supported shell radius.
const float ATMO_MAX = 1e30;
const float K_R = 0.166;
const float K_M = 0.0025;
const int NUM_ATMO_STEPS = 5;

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
		sum += exp(-max(length(v) - fInnerRadius, 0.0) * scaleH);
		v += step;
	}
	return sum * length(step) * scaleL;
}

vec3 applyAtmosphere(vec3 color, vec3 eye, vec3 frag)
{
	float scaleH = 4.0 / (fOuterRadius - fInnerRadius);
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
	float scaleH = 4.0 / (fOuterRadius - fInnerRadius);
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);
	p = up * max(length(p), fInnerRadius);
	float tSun = shellIntersection(p, l, fOuterRadius).y;
	float tau = atmoOptic(p, p + l * max(tSun, 0.0), scaleH, scaleL);
	float tauZenith = atmoOptic(up * fInnerRadius, up * fOuterRadius, scaleH, scaleL);
	float excess = max(tau - tauZenith, 0.0) * (4.0 * PI) * SUN_EXTINCTION;
	return exp(-(K_R * C_R + K_M) * excess) * horizon;
}

#define NIGHT_FLOOR vec3(0.003, 0.004, 0.006)
float skyFraction(float sunUp)
{
	return smoothstep(-0.2, 0.3, sunUp);
}

// Sun visibility from the shadow map (same lookup as quad.frag): 1 = lit.
float sunVisibility(vec3 p, vec3 n, vec3 l) {
	float ndl = clamp(dot(n, l), 0.0, 1.0);
	p += n * shadowTexel * (1.0 + 2.0 * (1.0 - ndl)) + l * shadowTexel;
	vec4 c = vec4(p, 1.0) * lightSpace;
	vec3 s = c.xyz / c.w * 0.5 + 0.5;
	if (s.z >= 1.0)
		return 1.0;
	vec2 texel = 1.0 / vec2(textureSize(depthMap, 0));
	float lit = 0.0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			lit += texture(depthMap, vec3(s.xy + vec2(x, y) * texel, s.z));
	return lit / 9.0;
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

float specular(vec3 n, vec3 l, vec3 e, float s) {
	float nrm = (s + 8.0) / (PI * 8.0);
	return pow(max(dot(reflect(e, n), l), 0.0), s) * nrm;
}

// Sky radiance for the reflection: the atmosphere dome's single-scattering
// march (atmosphere.frag inScatter) along the reflected ray, from the water
// surface out to the shell, so the sea mirrors the same sky the dome draws.
// Fewer view steps than the dome; the sun leg uses the dome's unclamped
// density so samples whose sun ray passes through the planet go dark at
// night exactly as the dome's do. viewT returns the transmittance along the
// ray (stars behind the sky). Planet-local p, unit dir and l.
uniform float E;
const float G_M = -0.85;
const int NUM_SKY_STEPS = 4;
const int NUM_SKY_SUN_STEPS = 4;

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
	float scaleH = 4.0 / (fOuterRadius - fInnerRadius);
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);
	vec3 ext = K_R * C_R + K_M;

	float tEnd = max(shellIntersection(p, dir, fOuterRadius).y, 0.0);
	float len = tEnd / float(NUM_SKY_STEPS);
	float segL = len * scaleL;
	vec3 v = p + dir * (len * 0.5);
	vec3 sum = vec3(0.0);
	float viewOptic = 0.0;
	for (int i = 0; i < NUM_SKY_STEPS; i++) {
		float dv = exp(-max(length(v) - fInnerRadius, 0.0) * scaleH);
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
	vec2 sl = textureGrad(waveSlope, uv, dx*waveTiling, dy*waveTiling).rg * waveAmp / patchLen;
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

	// Tangent basis of the equirectangular (long/lat) wave map on the sphere.
	vec3 T = normalize(vec3(-dObj.y, dObj.x, 1e-4));
	vec3 B = normalize(cross(dObj, T));
	vec3 Nobj = normalize(dObj - (slope.x * T + slope.y * B));
	// model is uniform scale * rotation, so inverse(mat3(model)) * n is
	// n * mat3(model) up to a scale the normalize removes.
	vec3 N = normalize(Nobj * mat3(model));

	vec3 V = normalize(cameraPos - fragPos);
	vec3 eye = -V;
	vec3 L = normalize(lightPosition - fragPos);
	vec3 dist = fragPos - cameraPos;

	// Cast shadows, faded out below the geometric terminator where the
	// planet body self-occludes (matches quad.frag so the coast is seamless).
	vec3 upGeo = normalize(localPos);
	float shadowWeight = smoothstep(-0.06, 0.0, dot(upGeo, L));
	float sun = mix(1.0, sunVisibility(fragPos, upGeo, L), shadowWeight);
	sun *= ringTransmittance(fragPos - ringCenter, L, length(lightPosition - fragPos));
	// Reddened, setting sunlight (quad.frag) and the twilight skylight fade.
	vec3 sunT = sunTransmittance(localPos, L, ringSunAngle);
	vec3 sunCol = sunT * sun;
	float sky = skyFraction(dot(upGeo, L));

	// Fresnel sky reflection (Schlick, water F0 = 0.02), uncapped: at
	// grazing angles the sea is a mirror of the bright horizon sky. Rays
	// that dip under the horizon (wave backs) would really see other water
	// near the horizon, so they are flattened onto it rather than going
	// black; skyVis only trims the deepest of them.
	vec3 up = normalize(localPos);
	vec3 R = reflect(eye, N);
	float rUp = dot(R, up);
	float skyVis = smoothstep(-0.35, 0.05, rUp);
	float nDotV = clamp(dot(N, V), 0.0, 1.0);
	float fresnel = (0.02 + 0.98 * pow(1.0 - nDotV, 5.0)) * skyVis;
	vec3 Rsky = normalize(R - up * min(rUp, 0.0) + up * 0.01);
	vec3 skyDisp = vec3(0.0);
	vec3 starT = vec3(1.0);
	if (hasAtmosphere > 0.5) {
		vec3 pSky = up * max(length(localPos), fInnerRadius);
		skyDisp = skyRadiance(pSky, Rsky, L, starT);
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

	// Sun glint (Seascape specular, softened with distance), tinted by the
	// sun's transmittance; the scalar keeps driving the opacity below.
	float spec = specular(N, L, eye, 6000.0 * inversesqrt(dot(dist, dist)));
	float glint = spec * sun * max(sunT.r, max(sunT.g, sunT.b));
	color += spec * sunCol;

	// Foam: a thin shore band where water is shallow + whitecaps where the
	// choppy displacement compresses the surface (low Jacobian).
	// The surf band is a fixed depth range, so seen from far out it would
	// cover kilometres of coast in a white halo. It fades as one pixel spans
	// more ground (radians of the unit sphere per pixel): visible from low
	// altitude, gone from orbit, where real surf is far below a pixel.
	float pixelAngle = max(length(dDirX), length(dDirY));
	float surfVis = 1.0 - smoothstep(2e-5, 1.5e-4, pixelAngle);
	float shore = (1.0 - smoothstep(0.0, 0.02, depth)) * surfVis;
	float whitecaps = (1.0 - smoothstep(0.3, 0.8, J)) * (0.25 + 0.75 * depthT);
	float foam = clamp(shore * 0.9 + whitecaps, 0.0, 1.0);
	vec3 foamCol = vec3(0.82, 0.87, 0.90)
	             * ((0.30 * sky * mix(0.25, 1.0, sun) + NIGHT_FLOOR) + 0.85 * max(dot(N, L), 0.0) * sunCol);
	color = mix(color, foamCol, foam);

	// Surface extinction, matching quad.frag so the coastline stays seamless.
	if (hasAtmosphere > 0.5) {
		color = applyAtmosphere(color, lodCameraPos - bodyCenter, localPos);
	}

	// Opacity: the seafloor shows through what the water column, the surface
	// reflection and foam let pass; the glint stays opaque so it survives
	// the shallows.
	float alpha = 1.0 - transmit * (1.0 - fresnel) * (1.0 - foam);
	alpha = max(alpha, clamp(glint, 0.0, 1.0));
	FragColor = vec4(toneMap(max(color, 0.0)), alpha);
}
