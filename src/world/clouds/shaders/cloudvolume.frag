#version 410

// Volumetric cloud layers, marched at reduced resolution (cloud_volume.c).
// Writes premultiplied radiance and opacity (location 0) and the clouds'
// distance along the ray, weighted by what each sample adds (location 1;
// 1e9 where there are none).
//
// Geometry is in view space, metres, with the camera at the origin; camUp
// points away from the body's centre. Shell crossings and altitudes along
// the ray use the cancellation-free forms (as gasinterior.frag), exact to
// metres at 75,000 km. The weather map and the noise are sampled in the
// body's frame relative to the map's centre, so the clouds stay put as the
// camera moves and the numbers stay small.
//
// Density: per layer, the map's cover shapes Perlin-Worley noise under a
// height profile (soft base, rounded top as high as the map's "towering"
// says), stretched east-west (sweep) and leaning with height (shear);
// detail Worley noise erodes the edges, wispy low and billowy high (the
// Horizon Zero Dawn recipe). Light: a short march toward the Sun, two
// Henyey-Greenstein lobes, three multiple-scattering octaves (Wrenninge),
// powder for thin edges, and sky- and ground-light by height in the shell.
//
// The air between the camera and the clouds (an exponential atmosphere:
// airColumn over airScaleHeight) fogs them at their weighted distance, so
// they blend over a scene whose own air is already drawn.

uniform mat4 projection;
uniform vec2 lowSize;          // this target, px
uniform vec2 fullSize;         // the scene, px
uniform vec2 jitter;           // sub-pixel offset this frame, px
uniform float frameJitter;
uniform mat3 viewToBody;
uniform vec3 camUp;            // unit, body centre -> camera (view axes)
uniform vec3 sunDir;           // unit, toward the Sun (view axes)
uniform float radius;          // reference radius, m
uniform float camAlt;          // m above it
uniform vec3 camToMap;         // camera - map centre, body axes, m
uniform vec3 mapEast, mapNorth;
uniform float mapExtent;       // m

#define MAX_LAYERS 2           // CLOUDVOL_MAX_LAYERS
uniform int layerCount;
uniform float layerBase[MAX_LAYERS];
uniform float layerTop[MAX_LAYERS];
uniform float layerDensity[MAX_LAYERS];
uniform float layerCoverage[MAX_LAYERS];
uniform float layerScale[MAX_LAYERS];
uniform float layerSweep[MAX_LAYERS];
uniform float layerShear[MAX_LAYERS];
uniform float layerErosion[MAX_LAYERS];
uniform vec3 layerTint[MAX_LAYERS];
uniform float shellLo, shellHi;   // the layers' lowest base, highest top

uniform vec3 airAlbedo;        // display
uniform float airAmbient;
uniform float airNight;
uniform float airForward;      // share of the HG lobe in the air's phase
uniform float airColumn;       // optical depth above the reference radius
uniform float airScaleHeight;
uniform vec3 terminator;
uniform float wrap;
uniform vec3 skyColor;         // display
uniform float skyGain;         // x the Sun
uniform vec3 groundColor;      // display
uniform float groundGain;
uniform float exposure;

uniform int viewSteps;
uniform int lightSteps;
uniform float maxDistance;
uniform float detailScale;
uniform float multiScatter;
uniform float powder;
uniform float ambient;

uniform sampler2D weatherMap;  // r top cover, g middle cover, b towering
uniform sampler2D colorMap;    // rgb albedo (display)
uniform sampler3D shapeNoise;
uniform sampler3D detailNoise;
uniform sampler2DMS sceneDepth;
uniform int sceneSamples;
uniform float depthNear;

layout(location = 0) out vec4 OutColor;
layout(location = 1) out float OutDepth;

#include "engine/gfx/tonemap.glsl"

const float PI = 3.14159265;
const float NONE = 1e9;

vec3 gD;          // the view ray
float gRc, gB;    // camera's distance from the centre; its dot with the ray
float gPixA;      // one pixel's angle

float remap(float v, float lo, float hi, float nlo, float nhi)
{
	return nlo + (v - lo) / (hi - lo) * (nhi - nlo);
}

// The ray's crossings of the sphere at altitude a: (near, far), or
// (-1, -1) if it misses. Roots of t^2 + 2 b t + c = 0, the stable way.
vec2 shellHits(float a)
{
	float c = (camAlt - a) * (2.0 * radius + camAlt + a);
	float disc = gB * gB - c;
	if (disc < 0.0)
		return vec2(-1.0);
	float s = sqrt(disc);
	float q = gB >= 0.0 ? -(gB + s) : s - gB;
	if (abs(q) < 1e-6)
		return vec2(-1.0);
	float t0 = q, t1 = c / q;
	return vec2(min(t0, t1), max(t0, t1));
}

// Distance (m) to the scene along the ray, at the full-resolution pixel
// under this sample; NONE for the sky.
float sceneDistance(vec2 lowPx)
{
	if (sceneSamples <= 0)
		return NONE;
	ivec2 px = ivec2(clamp(lowPx * fullSize / lowSize, vec2(0.0), fullSize - 1.0));
	float d = 1.0;
	for (int i = 0; i < sceneSamples; i++)
		d = min(d, texelFetch(sceneDepth, px, i).r);
	if (d <= 0.0)
		return NONE;
	return depthNear / d / max(-gD.z, 1e-6);
}

// Cloud extinction (1/m) at view-space point pv, altitude a; foot: a pixel
// there (m), for the noise mips; detail: with the edge erosion. alb: the
// albedo there (linear).
float density(vec3 pv, float a, float foot, bool detail, out vec3 alb)
{
	alb = vec3(0.0);
	if (a <= shellLo || a >= shellHi)
		return 0.0;
	vec3 rel = camToMap + viewToBody * pv;
	vec2 xy = vec2(dot(rel, mapEast), dot(rel, mapNorth));
	vec2 uv = xy / mapExtent + 0.5;
	float edge = smoothstep(0.0, 0.06, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
	if (edge <= 0.0)
		return 0.0;
	vec4 w = textureLod(weatherMap, uv, 0.0);
	vec3 mapAlb = vec3(-1.0);

	float total = 0.0;
	vec3 albSum = vec3(0.0);
	for (int i = 0; i < MAX_LAYERS; i++) {
		if (i >= layerCount)
			break;
		float hf = (a - layerBase[i]) / (layerTop[i] - layerBase[i]);
		if (hf <= 0.0 || hf >= 1.0)
			continue;
		float sc = layerScale[i];
		// Broad regions of thick cloud and wide valleys (~5 shape periods,
		// swept east-west), so the shape noise's tiling never shows.
		vec2 big = vec2(xy.x / (layerSweep[i] * 1.6), xy.y) / (sc * 5.3) + vec2(0.31, 0.77) * float(i);
		float broad = textureLod(shapeNoise, vec3(big, 0.37), 0.0).r;
		float cover = clamp((i == 0 ? w.r : w.g) * layerCoverage[i] + (broad - 0.67) * 1.6, 0.0, 1.0);
		if (cover <= 0.01)
			continue;
		vec3 np = vec3((xy.x + (a - layerBase[i]) * layerShear[i]) / layerSweep[i], xy.y, a) / sc
		        + float(i) * vec3(0.37, 0.71, 0.13);
		float lodS = log2(max(foot / (sc / 128.0), 1.0));
		// Soft flat base; rounded tops, as high as the map's towering,
		// raised and lowered by broad cells (domes and valleys).
		// The cell noise falls off linearly from each cell's centre (a
		// cone); a circle profile makes it a dome.
		float cell = 1.0 - mix(textureLod(shapeNoise, vec3(np.xy * 0.5, 0.5), lodS).g,
		                       textureLod(shapeNoise, vec3(np.yx * 0.21 + 0.13, 0.81), lodS).g, 0.45);
		float dome = sqrt(max(1.0 - 1.8 * cell * cell, 0.0));
		float topF = clamp(mix(0.35, 1.0, w.b) * (0.15 + 0.95 * dome), 0.12, 1.0);
		float prof = smoothstep(0.0, 0.1, hf) * (1.0 - smoothstep(topF * 0.55, topF, hf));
		if (prof <= 0.0)
			continue;
		vec4 sn = textureLod(shapeNoise, np, lodS);
		float wfbm = sn.g * 0.625 + sn.b * 0.25 + sn.a * 0.125;
		// Perlin-Worley cut by the Worley cells, then stretched over its
		// actual range (about 0.5 to 0.95) so it carves the clouds.
		float shape = clamp((remap(sn.r, wfbm - 1.0, 1.0, 0.0, 1.0) - 0.5) / 0.45, 0.0, 1.0);
		float d = clamp(remap(shape * prof, 1.0 - cover, 1.0, 0.0, 1.0), 0.0, 1.0) * cover;
		if (d <= 0.0)
			continue;
		if (detail && layerErosion[i] > 0.0) {
			float lodD = log2(max(foot / (sc * detailScale / 32.0), 1.0));
			vec3 dn = textureLod(detailNoise, np / detailScale, lodD).rgb;
			float dfbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
			// Wispy at the base, billowy at the top.
			float dmod = mix(dfbm, 1.0 - dfbm, clamp(hf * 4.0, 0.0, 1.0));
			d = clamp(remap(d, dmod * layerErosion[i] * 0.6, 1.0, 0.0, 1.0), 0.0, 1.0);
		}
		d *= layerDensity[i] * edge;
		if (d <= 0.0)
			continue;
		if (mapAlb.r < 0.0)
			mapAlb = displayToLinear(textureLod(colorMap, uv, 0.0).rgb);
		total += d;
		albSum += d * mapAlb * layerTint[i];
	}
	if (total > 0.0)
		alb = albSum / total;
	return total;
}

// Optical depth toward the Sun from view-space point pv.
float lightDepth(vec3 pv, float foot)
{
	float span = clamp((shellHi - shellLo) * 0.6, 2000.0, 15000.0);
	vec3 centre = -camUp * gRc;
	float tau = 0.0, sPrev = 0.0;
	vec3 alb;
	for (int k = 0; k < lightSteps; k++) {
		float u = float(k + 1) / float(lightSteps);
		float s = span * u * u;
		float ds = s - sPrev;
		vec3 q = pv + sunDir * (sPrev + 0.5 * ds);
		sPrev = s;
		float aq = length(q - centre) - radius;
		tau += density(q, aq, foot + ds * 0.5, k < 2, alb) * ds;
	}
	return tau;
}

// Henyey-Greenstein, scaled so isotropic is 1.
float hg(float c, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * c, 1.5);
}

// Light reaching a point at sun cosine mu0 (gasgiant.glsl's gasSunlight).
vec3 sunlight(float mu0, float w)
{
	float x = clamp((mu0 + w) / (1.0 + w), 0.0, 1.0);
	return x * mix(terminator, vec3(1.0), smoothstep(0.0, 0.4, x));
}

// Relative air mass toward the Sun (Kasten and Young).
float airMass(float mu)
{
	float z = min(degrees(acos(clamp(mu, -1.0, 1.0))), 95.0);
	return 1.0 / (max(mu, 0.0) + 0.50572 * pow(96.07995 - z, -1.6364));
}

// The air from the camera to distance tEnd along the ray: in-scatter
// (rgb) and transmittance (a), lit like the haze pass (no rays).
vec4 airTo(float tEnd)
{
	float sigma0 = airColumn / airScaleHeight;
	// The exact inverse of the curve, as the haze pass uses: the air in
	// front of the clouds matches the air over everything else.
	vec3 airLin = displayToLinear(airAlbedo);
	float cosT = dot(gD, sunDir);
	float ph = mix(1.0, hg(cosT, 0.6) / hg(0.0, 0.6), airForward);
	vec3 L = vec3(0.0);
	float T = 1.0, tPrev = 0.0;
	const int N = 12;
	for (int i = 0; i < N; i++) {
		float u = float(i + 1) / float(N);
		float tNext = tEnd * u * u;
		float dt = tNext - tPrev;
		float t = tPrev + 0.5 * dt;
		tPrev = tNext;
		float r = sqrt(gRc * gRc + 2.0 * gB * t + t * t);
		float a = (camAlt * (2.0 * radius + camAlt) + 2.0 * gB * t + t * t) / (r + radius);
		float mus = dot((camUp * gRc + gD * t) / r, sunDir);
		float sigma = sigma0 * exp(-max(a, 0.0) / airScaleHeight);
		float tauSun = airColumn * exp(-max(a, 0.0) / airScaleHeight) * airMass(mus);
		vec3 light = sunlight(mus, 2.0 * wrap);
		vec3 J = airLin * (light * exp(-tauSun) * ph + airAmbient * light + airNight);
		float ext = exp(-sigma * dt);
		L += T * J * (1.0 - ext);
		T *= ext;
	}
	return vec4(L, T);
}

float ign(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main()
{
	vec2 lowPx = gl_FragCoord.xy + jitter;
	vec2 ndc = lowPx / lowSize * 2.0 - 1.0;
	gD = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
	gRc = radius + camAlt;
	gB = gRc * dot(gD, camUp);
	gPixA = 2.0 / (projection[1][1] * lowSize.y);

	OutColor = vec4(0.0);
	OutDepth = NONE;

	// The stretch of ray inside the shell.
	vec2 hiH = shellHits(shellHi), loH = shellHits(shellLo);
	float t0, t1;
	if (camAlt > shellHi) {
		if (hiH.y <= 0.0)
			return;
		t0 = max(hiH.x, 0.0);
		t1 = hiH.y;
	} else if (camAlt >= shellLo) {
		t0 = 0.0;
		t1 = hiH.y;
	} else {
		t0 = max(loH.y, 0.0);
		t1 = hiH.y;
	}
	// Down through the shell's floor: the clouds end there.
	if (camAlt >= shellLo && loH.x > 0.0)
		t1 = min(t1, loH.x);
	t1 = min(t1, min(sceneDistance(lowPx), maxDistance));
	if (t1 <= t0)
		return;

	float cosT = dot(gD, sunDir);
	vec3 skyLin = displayToLinear(skyColor) * skyGain;
	vec3 groundLin = displayToLinear(groundColor) * groundGain;
	vec3 L = vec3(0.0);
	float T = 1.0, dSum = 0.0, wSum = 0.0;
	float span = t1 - t0;
	float jit = fract(ign(gl_FragCoord.xy) + frameJitter);
	float tPrev = t0;
	for (int i = 0; i < viewSteps; i++) {
		// Quadratic spacing: fine near the camera, long strides far off.
		float u = float(i + 1) / float(viewSteps);
		float tNext = t0 + span * u * u;
		float dt = tNext - tPrev;
		float t = tPrev + dt * jit;
		tPrev = tNext;

		float r = sqrt(gRc * gRc + 2.0 * gB * t + t * t);
		float a = (camAlt * (2.0 * radius + camAlt) + 2.0 * gB * t + t * t) / (r + radius);
		vec3 pv = gD * t;
		float foot = t * gPixA;
		vec3 alb;
		// Cheap test first; the detail only where there is cloud.
		if (density(pv, a, foot, false, alb) <= 0.0)
			continue;
		float sigma = density(pv, a, foot, true, alb);
		sigma *= 1.0 - smoothstep(0.6 * maxDistance, maxDistance, t);
		if (sigma <= 0.0)
			continue;

		vec3 upS = (camUp * gRc + pv) / r;
		float mus = dot(upS, sunDir);
		float tauAir = airColumn * exp(-max(a, 0.0) / airScaleHeight) * airMass(mus);
		vec3 sunE = sunlight(mus, wrap) * exp(-tauAir);

		float tauL = lightSteps > 0 && mus > -0.2 ? lightDepth(pv, foot) : 0.0;
		// Each scattering order picks up the albedo's hue once more (its
		// colour, not its darkness): deep, much scattered light (the
		// shadows) comes out warmer and richer.
		vec3 hue = alb / max(max(alb.r, alb.g), max(alb.b, 1e-4));
		vec3 Ls = vec3(0.0), bounce = vec3(1.0);
		float ka = 1.0, kb = 1.0, kc = 1.0;
		for (int o = 0; o < 3; o++) {
			float ph = mix(hg(cosT, 0.6 * kc), hg(cosT, -0.25 * kc), 0.25);
			Ls += bounce * (kb * ph * exp(-ka * tauL));
			bounce *= hue;
			ka *= 0.5;
			kb *= multiScatter;
			kc *= 0.5;
		}
		float pw = mix(1.0, 1.0 - exp(-sigma * 1500.0), powder);

		float hs = clamp((a - shellLo) / (shellHi - shellLo), 0.0, 1.0);
		vec3 amb = (skyLin * (0.4 + 0.6 * hs) + groundLin * (1.0 - hs)) * ambient * sunlight(mus, 2.0 * wrap);
		// x2: thick cloud reflects about like a sunlit Lambert surface
		// (what the provider's own deck uses), not half as much.
		vec3 S = alb * (2.0 * sunE * Ls * pw + amb * hue);

		float ext = exp(-sigma * dt);
		float wgt = T * (1.0 - ext);
		L += S * wgt;
		dSum += t * wgt;
		wSum += wgt;
		T *= ext;
		if (T < 0.01)
			break;
	}

	float alpha = 1.0 - T;
	if (wSum > 1e-4) {
		// The air in front: over the clouds' share of the pixel.
		float dist = dSum / wSum;
		vec4 air = airTo(dist);
		L = L * air.a + alpha * air.rgb;
		OutDepth = dist;
	}
	OutColor = vec4(L * exposure, alpha);
}
