#version 410

// The in-system nebula (Nebula_Draw): the near layer, marched along each
// pixel's ray through the unit sphere |n| < 1 (nebula units, nebula.glsl),
// ended at the scene depth so wisps in front of a planet veil it.
//
// - The chord comes from a ray-sphere intersection from the camera, which
//   works inside the sphere or outside it looking through.
// - A presence grid (Nebula.c: max of the near layer's density bound over
//   [-1, 1]^3, dilated, max-reduced mips) skips empty rays with a coarse
//   walk, then empty steps.
// - Light: the layer's emission plus a weak forward-scatter of the sun (at
//   the origin), through the scene's tone map. Premultiplied output with
//   alpha = 1 - T, blended ONE, ONE_MINUS_SRC_ALPHA: the sky and the scene
//   behind are dimmed by the gas in front, never re-added (display-space
//   approximation, as every blended pass here).

#ifndef NEBULA_OCTAVES
#define NEBULA_OCTAVES 4
#endif
#include "nebula.glsl"
#include "engine/gfx/tonemap.glsl"

out vec4 FragColor;

uniform mat4 projection;
uniform vec2 viewportSize;
uniform mat3 viewToWorld;
uniform vec3 camN;             // camera in nebula units (from double)
uniform float radiusM;         // metres per nebula unit
uniform float pixAngle;        // radians per pixel at the view centre
uniform int steps;
uniform float frameJitter;     // per-frame offset of the step jitter
uniform float scatter;         // sunlight forward-scatter strength
uniform vec3 sunColor;

uniform sampler3D presence;    // R: near-layer density bound, mips = max
uniform float presenceCoarseLod;

// The main pass's depth, as the dome reads it (atmosphere.frag).
uniform sampler2DMS sceneDepth;
uniform int sceneSamples;
uniform float depthNear;
uniform vec3 camForward;

const float MAX = 1e30;
const float PI = 3.14159265359;

// Distance (m) along dir to the visible surface; MAX for the sky. The
// farthest MSAA sample, as the dome does: at a silhouette the sky sample
// keeps the gas in front of the sky.
float sceneDistance(vec3 dir)
{
	if (sceneSamples <= 0)
		return MAX;
	ivec2 px = ivec2(gl_FragCoord.xy);
	float d = 1.0;
	for (int i = 0; i < sceneSamples; i++)
		d = min(d, texelFetch(sceneDepth, px, i).r);
	if (d <= 0.0)
		return MAX;
	return depthNear / d / max(dot(dir, camForward), 1e-6);
}

float grid(vec3 n, float lod)
{
	return textureLod(presence, n * 0.5 + 0.5, lod).r;
}

// Interleaved gradient noise (Jimenez 2014): the march's start jitter.
float ign(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Henyey-Greenstein phase.
float hg(float c, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

void main()
{
	vec2 ndc = gl_FragCoord.xy / viewportSize * 2.0 - 1.0;
	vec3 dView = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
	vec3 d = viewToWorld * dView;

	// Chord through the unit sphere (stable form: miss distance |o x d|).
	vec3 o = camN;
	vec3 oc = cross(o, d);
	float h2 = dot(oc, oc);
	if (h2 >= 1.0)
		discard;
	float mid = -dot(o, d);
	float halfChord = sqrt(1.0 - h2);
	float t0 = max(mid - halfChord, 0.0);
	float t1 = mid + halfChord;
	float tScene = sceneDistance(d);
	if (tScene < MAX)
		t1 = min(t1, tScene / radiusM);
	if (t1 <= t0)
		discard;

	// Coarse walk: nothing anywhere along the chord, nothing to do.
	const int COARSE = 12;
	float seen = 0.0;
	for (int i = 0; i <= COARSE; i++)
		seen = max(seen, grid(o + d * mix(t0, t1, float(i) / float(COARSE)), presenceCoarseLod));
	if (seen <= 0.0)
		discard;

	float dt = (t1 - t0) / float(steps);
	float j = fract(ign(gl_FragCoord.xy) + frameJitter);
	vec3 L = vec3(0.0);
	float T = 1.0;
	for (int i = 0; i < steps; i++) {
		float t = t0 + (float(i) + j) * dt;
		vec3 n = o + d * t;
		if (grid(n, 0.0) <= 0.0)
			continue;
		vec3 smp = nearSample(n, max(t, 1e-6) * pixAngle);
		// Sunlight scattered toward the camera: forward-peaked about the
		// sun's direction, falling off away from the sun (softened inverse
		// square, so the gas by the sun doesn't blow out).
		float r = length(n);
		float c = dot(n / max(r, 1e-6), -d);
		vec3 sun = scatter * hg(c, 0.6) * sunColor / (1.0 + r * r / 0.04);
		nebulaStep(smp, nebulaNear.z, dt, sun, L, T);
		if (T < 0.003)
			break;
	}

	float alpha = 1.0 - T;
	vec3 display = toneMap(L);
	if (alpha < 0.002 && max(display.r, max(display.g, display.b)) < 0.002)
		discard;
	FragColor = vec4(display, alpha);
}
