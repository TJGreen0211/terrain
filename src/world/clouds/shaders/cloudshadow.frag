#version 410

// The cloud layers' shadow map (cloud_volume.c): over a square of the map
// plane around the camera, the clouds' optical depth toward the Sun. Each
// texel is a ray from the shell's floor under it, along the Sun, out
// through the top; channel k (r, g, b) holds the depth above height
// lo + k/3 (hi - lo) along that ray. cloudshadowmap.glsl reads it back.
// No detail noise: the shafts it casts are soft anyway.

uniform vec3 mapCenterB;       // the map's centre on the reference sphere, body axes, m
uniform vec2 shadowCenter;     // the shadow map's centre in the map plane, m
uniform float shadowExtent;    // m
uniform float shadowSize;      // texels a side
uniform vec3 sunB;             // toward the Sun, body axes
uniform float radius;          // reference radius, m
uniform int steps;

out vec4 Tau;

#include "engine/gfx/tonemap.glsl"
#include "world/clouds/cloudshape.glsl"

void main()
{
	vec2 xy = shadowCenter + (gl_FragCoord.xy / shadowSize - 0.5) * shadowExtent;
	vec3 up = normalize(mapCenterB + mapEast * xy.x + mapNorth * xy.y);
	// Up to the top along the Sun (low Suns clamped, as the reader does).
	float mu = max(dot(up, sunB), 0.1);
	float ds = (shellHi - shellLo) / mu / float(steps);
	float foot = shadowExtent / shadowSize;
	float lv1 = mix(shellLo, shellHi, 1.0 / 3.0), lv2 = mix(shellLo, shellHi, 2.0 / 3.0);
	vec3 base = up * (radius + shellLo);
	float tau = 0.0, at1 = -1.0, at2 = -1.0;
	vec3 alb;
	for (int k = 0; k < steps; k++) {
		vec3 q = base + sunB * ((float(k) + 0.5) * ds);
		float a = length(q) - radius;
		if (at1 < 0.0 && a >= lv1)
			at1 = tau;
		if (at2 < 0.0 && a >= lv2)
			at2 = tau;
		tau += density(q - mapCenterB, a, foot, false, alb) * ds;
	}
	at1 = at1 < 0.0 ? tau : at1;
	at2 = at2 < 0.0 ? tau : at2;
	Tau = vec4(tau, tau - at1, tau - at2, 1.0);
}
