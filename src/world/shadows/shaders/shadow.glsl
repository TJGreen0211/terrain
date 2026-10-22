// Sun shadows for the planet surface and water (quad.frag, water.frag).
// Define SHADOW_TAPS (9 or 4) before including.
//
// Two levels (Shadows.c):
// - Cascades: SHADOW_CASCADES orthographic maps along the sun direction,
//   each a bounding sphere around a slice of the camera's view distance
//   (finest first). Casters are the very mesh the main pass draws.
// - Global map: one map over the whole planet (~3 km texels on Earth), for
//   everything past the cascades and for moon eclipses.
// A point uses the first cascade whose sphere holds it, blending into the
// next level over the sphere's outer part. All positions camera-relative.

#ifndef SHADOW_TAPS
#define SHADOW_TAPS 9
#endif
#define SHADOW_MAX_CASCADES 4
// Fraction of a cascade's radius over which it hands over to the next.
#define SHADOW_BLEND_START 0.8

uniform sampler2DShadow depthMap;   // global map
uniform mat4 lightSpace;            // global map: camera-relative -> clip
uniform float shadowTexel;          // global texel size (m)

uniform sampler2DArrayShadow cascadeMap;
uniform int cascadeCount;           // 0 = global map only
uniform mat4 cascadeSpace[SHADOW_MAX_CASCADES];
uniform vec4 cascadeSphere[SHADOW_MAX_CASCADES];  // centre (camera-relative), radius
uniform float cascadeTexel[SHADOW_MAX_CASCADES];
uniform int shadowDebug;            // 1 = tint by level (debug panel)

// Receiver offset: pushed out along the normal (more at grazing light, where
// acne starts) and toward the sun by about a texel, so it never tests
// against its own rasterised depth.
vec3 shadowOffset(vec3 p, vec3 n, vec3 l, float texel)
{
	float ndl = clamp(dot(n, l), 0.0, 1.0);
	return p + n * texel * (1.0 + 2.0 * (1.0 - ndl)) + l * texel;
}

// Hardware 2x2 PCF taps (compare mode): 9 one texel apart (smooth ~4-texel
// penumbra) or 4 half a texel out (same footprint, under half the cost).
float shadowGlobal(vec3 p, vec3 n, vec3 l)
{
	vec4 c = vec4(shadowOffset(p, n, l, shadowTexel), 1.0) * lightSpace;
	vec3 s = c.xyz / c.w * 0.5 + 0.5;
	if (s.z >= 1.0)
		return 1.0;
#if SHADOW_TAPS == 4
	vec2 t = 0.5 / vec2(textureSize(depthMap, 0));
	return 0.25 * (texture(depthMap, vec3(s.xy + vec2(-t.x, -t.y), s.z))
	             + texture(depthMap, vec3(s.xy + vec2( t.x, -t.y), s.z))
	             + texture(depthMap, vec3(s.xy + vec2(-t.x,  t.y), s.z))
	             + texture(depthMap, vec3(s.xy + vec2( t.x,  t.y), s.z)));
#else
	vec2 t = 1.0 / vec2(textureSize(depthMap, 0));
	float lit = 0.0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			lit += texture(depthMap, vec3(s.xy + vec2(x, y) * t, s.z));
	return lit / 9.0;
#endif
}

float shadowCascade(int i, vec3 p, vec3 n, vec3 l)
{
	vec4 c = vec4(shadowOffset(p, n, l, cascadeTexel[i]), 1.0) * cascadeSpace[i];
	// Casters up-sun of the box were clamped onto its near plane, so depth
	// only runs out on the far side (down-sun of every caster: lit).
	vec3 s = c.xyz / c.w * 0.5 + 0.5;
	if (s.z >= 1.0)
		return 1.0;
	float layer = float(i);
#if SHADOW_TAPS == 4
	vec2 t = 0.5 / vec2(textureSize(cascadeMap, 0).xy);
	return 0.25 * (texture(cascadeMap, vec4(s.xy + vec2(-t.x, -t.y), layer, s.z))
	             + texture(cascadeMap, vec4(s.xy + vec2( t.x, -t.y), layer, s.z))
	             + texture(cascadeMap, vec4(s.xy + vec2(-t.x,  t.y), layer, s.z))
	             + texture(cascadeMap, vec4(s.xy + vec2( t.x,  t.y), layer, s.z)));
#else
	vec2 t = 1.0 / vec2(textureSize(cascadeMap, 0).xy);
	float lit = 0.0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			lit += texture(cascadeMap, vec4(s.xy + vec2(x, y) * t, layer, s.z));
	return lit / 9.0;
#endif
}

// How far p is into cascade i's sphere (0 centre, 1 edge, > 1 outside).
float cascadeReach(int i, vec3 p)
{
	return length(p - cascadeSphere[i].xyz) / cascadeSphere[i].w;
}

// Level that serves p: the first cascade holding it, else cascadeCount
// (the global map).
int shadowLevel(vec3 p)
{
	for (int i = 0; i < cascadeCount; i++)
		if (cascadeReach(i, p) < 1.0)
			return i;
	return cascadeCount;
}

// Sun visibility: 1 = lit, 0 = fully shadowed. p camera-relative, n the
// geometric normal, l the unit direction to the sun.
float sunVisibility(vec3 p, vec3 n, vec3 l)
{
	int i = shadowLevel(p);
	if (i >= cascadeCount)
		return shadowGlobal(p, n, l);
	float vis = shadowCascade(i, p, n, l);
	float blend = smoothstep(SHADOW_BLEND_START, 1.0, cascadeReach(i, p));
	if (blend > 0.0) {
		float next = (i + 1 < cascadeCount && cascadeReach(i + 1, p) < 1.0)
		           ? shadowCascade(i + 1, p, n, l) : shadowGlobal(p, n, l);
		vis = mix(vis, next, blend);
	}
	return vis;
}

// Debug tint per level: red, yellow, green, cyan for cascades 0-3, white
// for the global map.
vec3 shadowDebugTint(vec3 p)
{
	int i = shadowLevel(p);
	if (i == 0) return vec3(1.0, 0.35, 0.35);
	if (i == 1) return vec3(1.0, 1.0, 0.35);
	if (i == 2) return vec3(0.35, 1.0, 0.35);
	if (i == 3) return vec3(0.35, 1.0, 1.0);
	return vec3(1.0);
}
