#version 410

// The sky bake (Nebula_BakeSky): one cubemap face per draw (in strips), the
// far layer marched from the sun through its shell, stars at infinity
// behind it, as linear radiance into an HDR cube (nebula_post.frag then
// adds the glow and tone maps it). The skybox draws its texels
// straight into the scene (display values), as it does skybox3's.
//
// Face basis: the GL cube-map table (spec 8.13), so the direction that
// samples a texel is the one that rendered it. Texture t runs with the
// framebuffer's rows (row 0 = t 0), and Nebula.c reads the rows back in
// that order for the PNGs.

#ifndef NEBULA_OCTAVES
#define NEBULA_OCTAVES 6
#endif
#define NEBULA_FAR
#include "nebula.glsl"
#include "tonemap.glsl"

out vec4 FragColor;

uniform int face;            // 0..5 = POSITIVE_X + face
uniform float faceSize;      // texels
uniform int steps;           // coarse steps through the shell (empty space)
uniform float starDensity;   // 0..1
uniform float starBrightness;
uniform int axisDebug;       // 1: colour by axis (orientation check)

// Lit dust (plan Phase 1b step 1). Every far-layer sample scatters the
// lights' radiance toward the camera: a short march toward each light for
// its transmittance (self-shadowing: thick cores go dark, thin edges facing
// a light glow), Beer + powder, a two-lobe Henyey-Greenstein phase (silver
// lining when a light is behind the dust), and Wrenninge's octaves for the
// multiple scattering that keeps cores from going pure black.
uniform float dustAlbedo;    // scattered fraction
uniform vec3 dustAmbient;    // light from everywhere (shadow fill), per density
uniform float phaseG;        // forward lobe; the back lobe is -0.3
uniform float powder;        // 0 = off, 1 = full dark-edge-on-front effect
uniform float coreSize;      // visible light cores: angular size (radians)
// Luminous edges: where the dust thins out at a cloud's boundary it glows
// in the colour of the lights near it (the bright, billowy fringes of the
// references; ionisation fronts). Independent of density, so even thin
// fringes show; the gas in front still dims it.
uniform float edgeGlow;
uniform float edgeWidth;     // fraction of full density counted as "edge"

// Cap on the adaptive march (fine steps through deep, thin gas).
#define MAX_STEPS 4096

#ifndef LIGHT_STEPS
#define LIGHT_STEPS 12
#endif

const float PI = 3.14159265359;

float hg(float c, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

float phase2(float c, float g)
{
	return mix(hg(c, g), hg(c, -0.3), 0.25);
}

// Optical depth from p toward light i (to the light or lightRadius * 3,
// whichever is nearer). Coarse: the footprint is the step, so the octaves
// below it fade out (nebula.glsl).
float lightDepth(vec3 p, int i, float jit)
{
	vec3 to = nebulaLightPos[i] - p;
	float dist = min(length(to), 3.0 * nebulaLightRadius);
	vec3 dir = normalize(to);
	float ds = dist / float(LIGHT_STEPS);
	float od = 0.0;
	for (int k = 0; k < LIGHT_STEPS; k++) {
		vec3 q = p + dir * ((float(k) + jit) * ds);
		od += farSample(q, ds).x * ds;
	}
	return od * nebulaFar.z;
}

// Light scattered toward the camera at p, per unit density. d: view ray.
vec3 dustLight(vec3 p, vec3 d, float jit)
{
	vec3 L = dustAmbient;
	for (int i = 0; i < nebulaLightCount; i++) {
		vec3 v = p - nebulaLightPos[i];
		float r2 = dot(v, v);
		float R2 = nebulaLightRadius * nebulaLightRadius;
		// Beyond ~4 radii a light adds < 6 %: no shadow march for it.
		if (r2 > 16.0 * R2)
			continue;
		float fall = 1.0 / (1.0 + r2 / R2);
		float c = dot(v * inversesqrt(max(r2, 1e-8)), -d);
		float od = lightDepth(p, i, jit);
		// Wrenninge's octaves: each scatters again with less extinction
		// and a broader phase.
		float ms = 0.0, a = 1.0, b = 1.0, cg = 1.0;
		for (int o = 0; o < 3; o++) {
			ms += a * exp(-od * b) * phase2(c, phaseG * cg);
			a *= 0.5; b *= 0.4; cg *= 0.5;
		}
		// Powder: thin gas facing the light scatters less of it back.
		float pw = mix(1.0, 1.0 - exp(-2.0 * od), powder);
		L += nebulaLightCol[i] * fall * ms * pw * 4.0 * PI;
	}
	return L * dustAlbedo;
}

// The lights' colour reaching p (no shadowing): gas glow radius falloff.
vec3 lightNear(vec3 p)
{
	vec3 c = vec3(0.0);
	float R2 = nebulaGasRadius * nebulaGasRadius;
	for (int i = 0; i < nebulaLightCount; i++) {
		vec3 v = p - nebulaLightPos[i];
		c += nebulaLightCol[i] / (1.0 + dot(v, v) / R2);
	}
	return c;
}

vec3 faceDir(vec2 st)
{
	float s = st.x, t = st.y;
	if (face == 0) return vec3( 1.0, -t, -s);
	if (face == 1) return vec3(-1.0, -t,  s);
	if (face == 2) return vec3( s,  1.0,  t);
	if (face == 3) return vec3( s, -1.0, -t);
	if (face == 4) return vec3( s, -t,  1.0);
	return vec3(-s, -t, -1.0);
}

uvec3 starHash(ivec3 c, uint salt)
{
	return pcg3d(uvec3(c) + uvec3(salt * 1664525u, salt * 22695477u, salt * 2891336453u));
}

// Stars at infinity: one candidate per cell of a 3D grid over the unit
// sphere (n cells across), a Gaussian about the texel's size. Every
// neighbouring cell is checked so stars cross cell edges.
vec3 starLayer(vec3 d, float cells, float prob, float pix, uint salt)
{
	vec3 sum = vec3(0.0);
	vec3 g = d * cells;
	ivec3 c0 = ivec3(floor(g));
	for (int z = -1; z <= 1; z++)
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++) {
		ivec3 c = c0 + ivec3(x, y, z);
		uvec3 h = starHash(c, salt);
		vec3 r = vec3(h >> 8u) / 16777215.0;
		if (r.x > prob)
			continue;
		uvec3 h2 = starHash(c, salt + 7u);
		vec3 j = vec3(h2 >> 8u) / 16777215.0;
		vec3 sp = (vec3(c) + j) / cells;
		float l = length(sp);
		// Only cells the sphere passes through hold a star (no clumping
		// toward the cube's edges: the point is projected onto the sphere).
		if (abs(l - 1.0) > 0.5 / cells)
			continue;
		sp /= l;
		float ang = length(d - sp);
		float sigma = 0.8 * pix;
		float mag = pow(r.y, 6.0);                          // few bright, many faint
		vec3 tint = mix(vec3(1.0, 0.75, 0.55), vec3(0.7, 0.85, 1.0), r.z);
		sum += tint * mag * exp(-ang * ang / (sigma * sigma));
	}
	return sum;
}

void main()
{
	vec2 st = gl_FragCoord.xy / faceSize * 2.0 - 1.0;
	vec3 d = normalize(faceDir(st));
	// A texel's angle at the face's centre (the march's footprint).
	float pix = 2.0 / faceSize;

	if (axisDebug != 0) {
		// +X red, +Y green, +Z blue, the negative axes darker, with a
		// checker by face texel so mirrored faces show.
		vec3 a = abs(d);
		vec3 col = a.x > a.y && a.x > a.z ? vec3(1.0, 0.1, 0.1) * (d.x > 0.0 ? 1.0 : 0.35)
		         : a.y > a.z ? vec3(0.1, 1.0, 0.1) * (d.y > 0.0 ? 1.0 : 0.35)
		         : vec3(0.1, 0.1, 1.0) * (d.z > 0.0 ? 1.0 : 0.35);
		// A bright wedge toward each face's +s, +t corner (orientation).
		if (st.x > 0.5 && st.y > 0.5 && st.x + st.y > 1.6)
			col = vec3(1.0);
		FragColor = vec4(col, 1.0);
		return;
	}

	// The far shell in shell units (q = n / farStart): from 1 to farEnd.
	float t0 = 1.0, t1 = nebulaFarEnd;
	// Adaptive steps: coarse through empty space, about a pixel's footprint
	// apart inside a cloud's coverage (the erosion detail is finer than any
	// fixed step the bake can afford, and undersampling it streaks along
	// the rays). Start offset: white noise per texel (a structured pattern
	// like IGN shows its lines through the detail).
	float dtCoarse = (t1 - t0) / float(steps);
	float jitter = float(pcg3d(uvec3(uvec2(gl_FragCoord.xy), uint(face) + 1u)).x >> 8u) / 16777215.0;
	vec3 L = vec3(0.0);
	float T = 1.0;
	// Transmittance to each light's depth along this ray (its core).
	float tLight[NEBULA_MAX_LIGHTS];
	float TLight[NEBULA_MAX_LIGHTS];
	for (int k = 0; k < NEBULA_MAX_LIGHTS; k++) {
		tLight[k] = k < nebulaLightCount ? dot(nebulaLightPos[k], d) : -1.0;
		TLight[k] = 1.0;
	}
	float t = t0 + jitter * dtCoarse;
	for (int i = 0; i < MAX_STEPS && t < t1; i++) {
		vec3 p = d * t;
		float fine = 1.5 * t * pix;
		float dt = farCoverageAt(p) > 1e-3 ? fine : dtCoarse;
		vec3 smp = farSample(p, max(t * pix, dt));
		// The light march's start is jittered too (a different sequence per
		// step): a fixed start bands into streaks radiating from the lights.
		vec3 lit = smp.x > 1e-3 ? dustLight(p, d, fract(jitter + float(i) * 0.61803398875)) : vec3(0.0);
		// The emissive gas shares the segment (it glows, the dust there
		// absorbs): its light seen through what is in front.
		L += T * gasEmission(p) * dt;
		if (smp.x > 0.0 && edgeGlow > 0.0) {
			float dn = smp.x / max(nebulaFar.w, 1e-4);
			float edge = 1.0 - smoothstep(0.0, edgeWidth, dn);
			L += T * edgeGlow * edge * lightNear(p) * dt;
		}
		nebulaStep(smp, nebulaFar.z, dt, lit, L, T);
		for (int k = 0; k < NEBULA_MAX_LIGHTS; k++)
			if (t < tLight[k])
				TLight[k] = T;
		if (T < 0.003)
			break;
		t += dt;
	}

	// The light cores themselves, behind whatever gas is in front of them.
	for (int k = 0; k < nebulaLightCount; k++) {
		float c = dot(normalize(nebulaLightPos[k]), d);
		float ang = acos(clamp(c, -1.0, 1.0));
		float core = exp(-(ang * ang) / (coreSize * coreSize))
		           + 0.15 * exp(-ang / (4.0 * coreSize));
		L += TLight[k] * nebulaLightCol[k] * core;
	}

	// Stars behind the gas: two densities, more along the band.
	float band = nebulaBandWeight(d);
	// ~4 pi cells^2 candidates each: a few thousand bright, ~30k faint
	// over the sky at density 1.
	vec3 stars = starLayer(d, 60.0, 0.12 * starDensity * (0.5 + band), pix, 1u) * 4.0
	           + starLayer(d, 160.0, 0.10 * starDensity * (0.3 + band), pix, 2u) * 0.6;
	L += T * stars * starBrightness;

	// Linear radiance: the glow pass (nebula_post.frag) blooms and tone
	// maps it.
	FragColor = vec4(L, 1.0);
}
