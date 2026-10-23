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
// Density: cloudshape.glsl. Light: a short march toward the Sun (and, farther, the shadow map), two
// Henyey-Greenstein lobes, three multiple-scattering octaves (Wrenninge),
// powder for thin edges, and sky- and ground-light by height in the shell.
//
// The air (an exponential atmosphere: airColumn over airScaleHeight) is
// integrated along the march, so each sample is fogged by the air in front
// of it. The scene behind already has its own air drawn over it, which the
// clouds' alpha then hides in part: the clouds carry the air that sits in
// front of them (each bit of air weighted by how much more of the scene
// the clouds hide than of that air). Fogging everything at one weighted
// distance instead left dark rims where a ray grazed a near cloud and went
// on to far ones: the far clouds got the near cloud's thin air.

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

#define MAX_FLASHES 4          // CLOUDVOL_MAX_FLASHES
uniform int flashCount;
uniform vec3 flashPos[MAX_FLASHES];     // view axes, m
uniform vec3 flashColor[MAX_FLASHES];   // linear
uniform float flashRadius[MAX_FLASHES]; // m

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
uniform float multiScatter;
uniform float powder;
uniform float ambient;

uniform sampler2DMS sceneDepth;
uniform int sceneSamples;
uniform float depthNear;

layout(location = 0) out vec4 OutColor;
layout(location = 1) out float OutDepth;

#include "engine/gfx/tonemap.glsl"
#include "world/clouds/cloudshape.glsl"
#include "world/clouds/cloudshadowmap.glsl"

const float PI = 3.14159265;
const float NONE = 1e9;

vec3 gD;          // the view ray
float gRc, gB;    // camera's distance from the centre; its dot with the ray
float gPixA;      // one pixel's angle

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

// Cloud extinction at view-space point pv (cloudshape.glsl's density).
float densityV(vec3 pv, float a, float foot, bool detail, out vec3 alb)
{
	return density(camToMap + viewToBody * pv, a, foot, detail, alb);
}

// How far the short march toward the Sun goes (m).
float lightSpan()
{
	return clamp((shellHi - shellLo) * 0.6, 2000.0, 15000.0);
}

// Optical depth toward the Sun from view-space point pv, over lightSpan.
float lightDepth(vec3 pv, float foot)
{
	float span = lightSpan();
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
		tau += densityV(q, aq, foot + ds * 0.5, k < 2, alb) * ds;
	}
	return tau;
}

// The flashes' light at view-space point pv: diffusing through the cloud,
// it falls off over each one's radius.
vec3 flashLight(vec3 pv)
{
	vec3 E = vec3(0.0);
	for (int i = 0; i < MAX_FLASHES; i++) {
		if (i >= flashCount)
			break;
		float d = length(pv - flashPos[i]) / flashRadius[i];
		E += flashColor[i] * exp(-1.5 * d) / (0.05 + d * d);
	}
	return E;
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

// The air at distance t along the ray (r: from the centre, a: altitude):
// its light (scattered toward the camera, per unit optical depth), lit
// like the haze pass (no rays); sigma: its extinction (1/m). ph: its phase.
// airLin: its albedo in linear (the exact inverse of the curve, as the
// haze pass uses, so the air in front of the clouds matches the air over
// everything else).
vec3 airLight(float t, float r, float a, float ph, vec3 airLin, out float sigma)
{
	float mus = dot((camUp * gRc + gD * t) / r, sunDir);
	float dens = exp(-max(a, 0.0) / airScaleHeight);
	sigma = airColumn / airScaleHeight * dens;
	float tauSun = airColumn * dens * airMass(mus);
	vec3 light = sunlight(mus, 2.0 * wrap);
	// The clouds' shadow: shafts where the Sun comes through the gaps.
	float shadow = cloudShadow(gD * t, a, mus);
	return airLin * (light * exp(-tauSun) * ph * shadow + airAmbient * light + airNight);
}

// The air from the camera to distance tEnd along the ray: in-scatter
// (rgb) and transmittance (a).
vec4 airTo(float tEnd, float ph, vec3 airLin)
{
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
		float sigma;
		vec3 J = airLight(t, r, a, ph, airLin, sigma);
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
	vec3 airLin = displayToLinear(airAlbedo);
	float phA = mix(1.0, hg(cosT, 0.6) / hg(0.0, 0.6), airForward);
	// The air from the camera to the shell (in front of all the clouds),
	// then the air in the shell, step by step: A0 its in-scatter, A1 the
	// same times the clouds' transmittance in front of each bit. Ta: the
	// air's transmittance so far.
	vec4 air0 = t0 > 0.0 ? airTo(t0, phA, airLin) : vec4(0.0, 0.0, 0.0, 1.0);
	float Ta = air0.a;
	vec3 A0 = vec3(0.0), A1 = vec3(0.0);

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

		// The air over this step; the sample sees it up to its own point.
		float sa;
		vec3 Ja = airLight(t, r, a, phA, airLin, sa);
		if (flashCount > 0)
			Ja += 0.15 * airLin * flashLight(gD * t);
		float extA = exp(-sa * dt);
		float TaS = Ta * exp(-sa * dt * jit);
		vec3 dA = Ta * Ja * (1.0 - extA);
		A0 += dA;
		A1 += T * dA;
		Ta *= extA;

		vec3 pv = gD * t;
		float foot = t * gPixA;
		vec3 alb;
		// Cheap test first; the detail only where there is cloud.
		if (densityV(pv, a, foot, false, alb) <= 0.0)
			continue;
		float sigma = densityV(pv, a, foot, true, alb);
		sigma *= 1.0 - smoothstep(0.6 * maxDistance, maxDistance, t);
		if (sigma <= 0.0)
			continue;

		vec3 upS = (camUp * gRc + pv) / r;
		float mus = dot(upS, sunDir);
		float tauAir = airColumn * exp(-max(a, 0.0) / airScaleHeight) * airMass(mus);
		vec3 sunE = sunlight(mus, wrap) * exp(-tauAir);

		// The short march for the cloud nearby; the shadow map for clouds
		// farther up the Sun's ray, from where the march ends. The map has
		// no detail erosion (it overstates the depth) and light leaks
		// round whole clouds: x0.4.
		float tauL = 0.0;
		if (lightSteps > 0 && mus > -0.2) {
			tauL = lightDepth(pv, foot);
			vec3 qEnd = pv + sunDir * lightSpan();
			tauL += 0.4 * cloudShadowTau(qEnd, length(qEnd + camUp * gRc) - radius, mus);
		}
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
		// Diffusion: deep in thick cloud the light falls off slowly with
		// depth (about 1 / (1 + 0.75 (1 - g) tau)), not exponentially as
		// the orders above do, so the inside of a big cloud glows dimly
		// instead of going black. Only deep inside: near
		// the surface the orders above keep the shading's contrast.
		S += 2.0 * sunE * alb * 0.8 * smoothstep(4.0, 20.0, tauL) / (1.0 + 0.1 * tauL);
		// Low down the clouds sink into the murk: their light gives way to
		// the air's around them, so the walls' feet dissolve into the haze.
		// (Thinning the density there can't do it: any cloud thick enough
		// to see is opaque, so it only moved the level they end on.)
		S = mix(S, Ja, 0.9 * (1.0 - smoothstep(0.0, 0.3, hs)));
		// Flashes: their own colour shows through the cloud's (bright
		// white-blue light swamps the tint).
		if (flashCount > 0)
			S += mix(alb, vec3(dot(alb, vec3(0.3, 0.59, 0.11))), 0.7) * flashLight(pv);

		float ext = exp(-sigma * dt);
		float wgt = T * (1.0 - ext);
		L += S * wgt * TaS;
		dSum += t * wgt;
		wSum += wgt;
		T *= ext;
		if (T < 0.01)
			break;
	}

	float alpha = 1.0 - T;
	if (wSum > 1e-4) {
		// The air in front of the clouds, over what the clouds hide of
		// the scene's own air behind them.
		L += air0.rgb * alpha + (A1 - T * A0);
		OutDepth = dSum / wSum;
	}
	OutColor = vec4(L * exposure, alpha);
}
