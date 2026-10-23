#version 410

// The accumulated clouds over the scene (cloud_volume.c), upsampled from
// the low-resolution target: premultiplied, depth-tested against the
// scene at the clouds' distance (nothing written), times the provider's
// hand-off (opacity).
//
// DOME_AIR (the rocky planets, cloud_volume.h): the clouds are dimmed by
// the planet's air between them and the camera and get that air's light
// (the dome's march over the same stretch, converted as the dome converts
// it), at the clouds' distance; their alpha goes into the target's, so
// the dome adds the rest of its column times (1 - alpha). A second,
// depth-only draw (depthOnly) then writes the clouds' depth where they are
// all but opaque (0.97: the dome's in-scatter ends there, a step it must
// not show), so what is drawn after them (the ring rocks and dust; the
// sheet behind the clouds is drawn before them) stays behind.

uniform mat4 projection;
uniform vec2 fullSize;
uniform sampler2D cloudColor;   // linear
uniform sampler2D cloudDepth;   // nearest
uniform float opacity;
// Debug view (cloud_volume.c): the march's counts (r steps, g density
// calls) as a heat map, red at debugMax, over everything.
uniform int debugView;
uniform float debugMax;
uniform int depthOnly;

out vec4 FragColor;

#ifdef DOME_AIR
const float PI = 3.14159265359;
uniform vec3 camCentre;        // camera - body centre, view axes, m
uniform vec3 sunDirV;          // toward the Sun, view axes
uniform float atmoHdrGain;     // Atmosphere_HdrGain
uniform float E;               // the dome's solar brightness
uniform float G_M;             // Mie asymmetry
#include "world/atmosphere/atmosurface.glsl"
vec2 rayIntersection(vec3 p, vec3 dir, float radius)
{
	return shellIntersection(p, dir, radius);
}
// (No ring shadow on the air in front of the clouds.)
float scatterRingT(vec3 p, vec3 l)
{
	return 1.0;
}
#include "world/atmosphere/atmoscatter.glsl"
#include "engine/gfx/tonemap.glsl"
#endif

vec3 heat(float x)
{
	const vec3 c0 = vec3(0.0, 0.0, 0.0), c1 = vec3(0.0, 0.2, 1.0), c2 = vec3(0.0, 0.9, 0.3);
	const vec3 c3 = vec3(1.0, 0.9, 0.0), c4 = vec3(1.0, 0.0, 0.0), c5 = vec3(1.0, 1.0, 1.0);
	x = clamp(x, 0.0, 1.25) * 4.0;
	return x < 1.0 ? mix(c0, c1, x) : x < 2.0 ? mix(c1, c2, x - 1.0) : x < 3.0 ? mix(c2, c3, x - 2.0)
	     : x < 4.0 ? mix(c3, c4, x - 3.0) : mix(c4, c5, x - 4.0);
}

void main()
{
	vec2 uv = gl_FragCoord.xy / fullSize;
	if (debugView != 0) {
		vec4 n = texture(cloudColor, uv);
		// (As linear radiance: the tone map bends it, the order stays.)
		FragColor = vec4(heat((debugView == 1 ? n.r : n.g) / debugMax), 1.0);
		gl_FragDepth = 1.0;   // reversed-Z: in front of everything
		return;
	}
	vec4 c = texture(cloudColor, uv) * opacity;
	if (c.a < (depthOnly != 0 ? 0.97 : 0.004))
		discard;
	float dist = texture(cloudDepth, uv).r;
	vec2 ndc = uv * 2.0 - 1.0;
	vec3 d = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
#ifdef DOME_AIR
	// Tested DEPTH_LEAD of the way in front of their distance: a planet's
	// clouds sit only km over the ground, and seen from far off near the
	// limb one pixel spans more depth than that, so at their own distance
	// every other pixel of a 2x2 block fell behind the ground (a dot grid).
	// The march already ends at the scene; this only keeps what is nearer.
	const float DEPTH_LEAD = 0.02;
	vec4 cc = vec4(d * dist * (1.0 - DEPTH_LEAD), 1.0) * projection;
#else
	vec4 cc = vec4(d * dist, 1.0) * projection;
#endif
	gl_FragDepth = cc.z / cc.w;
#ifdef DOME_AIR
	if (depthOnly == 0 && hasAtmosphere > 0.5) {
		c.rgb = applyAtmosphere(c.rgb, camCentre, camCentre + d * dist);
		vec2 e = rayIntersection(camCentre, d, fOuterRadius);
		e.x = max(e.x, 0.0);
		e.y = min(e.y, dist);
		if (e.y > e.x) {
			vec3 air = inScatter(camCentre, d, e, sunDirV, 1.0 / fScaleHeight, 1.0 / (fOuterRadius - fInnerRadius));
			c.rgb += sceneFromDisplay(air) * atmoHdrGain * c.a;
		}
	}
#endif
	FragColor = c;
}
