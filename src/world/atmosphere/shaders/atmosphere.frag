#version 410

uniform vec3 camPosition;
uniform vec3 C_R;
uniform float E;
uniform vec4 lightPos;
uniform vec3 shellCenter;   // camera-relative body centre (shell sphere origin)

uniform float fInnerRadius;
uniform float fOuterRadius;
uniform float fScaleHeight;   // metres per e-fold of density (Atmosphere.c)
#include "atmodensity.glsl"

// Rings (Rings.c). Everything here is planet-local, and the ring is centred
// on the planet, so positions need no ringCenter offset.
uniform float hasRing;
uniform sampler1D ringProfile;
uniform vec3 ringNormal;
uniform vec2 ringRadii;      // inner, outer (world units)
uniform float ringSunAngle;
uniform vec2 ringFade;       // ring.frag's near-camera sheet fade

in vec3 fLocalPos;

// The main pass's depth (Atmosphere_SetSceneDepth): reversed-Z infinite,
// depth = depthNear / view z. Rays end at the visible surface, so terrain
// above sea level is not fogged by the air behind it.
uniform sampler2DMS sceneDepth;
uniform int sceneSamples;      // 0 = no depth (clip at the sea-level sphere only)
uniform float depthNear;
uniform vec3 camForward;       // camera forward axis, same frame as dir

out vec4 FragColor;

const float PI = 3.14159265359;
// Intersection miss sentinel must exceed the whole scaled scene. The
// atmosphere shells can be millions of world units across.
const float MAX = 1e30;

uniform float K_R; // Rayleigh constant > thicker (default 0.166, Atmosphere.c)
uniform float K_M; // Mie constant > thicker (default 0.0025)
uniform float G_M; // Mie phase asymmetry, -0.75..-0.999 (default -0.85)

// SCALE_H / SCALE_L are computed per invocation in main() and passed down:
// non-constant global initializers reading uniforms are not valid core GLSL
// at #version 410 (they were only legalised in 4.20 / 420pack).

vec3 rayDirection(vec3 camPosition) {
	// fLocalPos (dome vertex minus planet centre) and camPosition (camera
	// minus planet centre) are both planet-local, matching the origin-centred
	// rayIntersection spheres. Identical to the old m*fPosition form for the
	// T*S dome matrix, but robust if the model ever gains rotation.
	return normalize(fLocalPos - camPosition);
}

vec2 rayIntersection(vec3 p, vec3 dir, float radius ) {
	float b = dot( p, dir );
	float c = dot( p, p ) - radius * radius;

	float d = b * b - c;
	if ( d < 0.0 ) {
		return vec2( MAX, -MAX );
	}
	d = sqrt( d );

	float near = -b - d;
	float far = -b + d;

	return vec2(near, far);
}

// Distance along dir to the visible surface (MAX for sky). The farthest of
// the pixel's MSAA samples: at a silhouette the sky sample keeps its full
// glow (as before this clip existed) instead of a dark fringe.
float sceneDistance(vec3 dir) {
	if (sceneSamples <= 0)
		return MAX;
	ivec2 px = ivec2(gl_FragCoord.xy);
	float d = 1.0;
	for (int i = 0; i < sceneSamples; i++)
		d = min(d, texelFetch(sceneDepth, px, i).r);
	if (d <= 0.0)
		return MAX;   // cleared depth: nothing drawn
	return depthNear / d / max(dot(dir, camForward), 1e-6);
}

// Ring shadow on a scattering sample (same lookup as quad.frag): the sky
// darkens in the ring's shadow just like the ground under it.
float ringTransmittance(vec3 p, vec3 l) {
	float mu = dot(l, ringNormal);
	if (hasRing < 0.5 || abs(mu) < 1e-4)
		return 1.0;
	float t = -dot(p, ringNormal) / mu;
	if (t <= 0.0)
		return 1.0;
	float r01 = (length(p + l * t) - ringRadii.x) / (ringRadii.y - ringRadii.x);
	if (r01 <= 0.0 || r01 >= 1.0)
		return 1.0;
	float texelW = (ringRadii.y - ringRadii.x) / float(textureSize(ringProfile, 0));
	float tau = textureLod(ringProfile, r01, log2(max(t * ringSunAngle / texelW, 1.0))).a;
	return exp(-tau / abs(mu));
}

// The sun-lit air along a view segment (atmoscatter.glsl), shadowed by the
// ring as above.
float scatterRingT(vec3 p, vec3 l) {
	return ringTransmittance(p, l);
}
#include "atmoscatter.glsl"

void main (void)
{
	vec3 dir = rayDirection(camPosition);
	vec3 eye = camPosition;

	float scaleH = 1.0 / fScaleHeight;
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);

	// Ring sheet along the view ray. The dome is additive, so where the
	// sheet lies in front of the shell its coverage has to be taken out of
	// the glow here (same alpha and near-camera fade as ring.frag). Sampled
	// before any discard so the implicit-derivative mip is defined.
	float ringMu = dot(dir, ringNormal);
	float ringT = -dot(eye, ringNormal) / (abs(ringMu) > 1e-6 ? ringMu : 1e-6);
	float ringR01 = (length(eye + dir * ringT) - ringRadii.x) / (ringRadii.y - ringRadii.x);
	float ringTau = texture(ringProfile, clamp(ringR01, 0.0, 1.0)).a;

	vec2 e = rayIntersection(eye, dir, fOuterRadius);
	if ( e.x > e.y ) {
		discard;
	}
	vec2 f = rayIntersection(eye, dir, fInnerRadius);
	// camera inside the atmosphere: the ray starts at the camera (A = camera)
	e.x = max(e.x, 0.0);
	// Only stop at the planet when its near-side intersection is ahead of the
	// camera. For rays pointing away from the planet, f.x is behind the eye;
	// clipping to it would discard the outward-facing part of the dome.
	if (f.x > e.x) {
		e.y = min(e.y, f.x);
	}
	// ...and at whatever the scene drew first: terrain above sea level, the
	// water, the moon or ring rocks in front of the air (which then discard).
	e.y = min(e.y, sceneDistance(dir));
	if ( e.y <= e.x ) {
		discard;
	}

	// direction from the scattering path towards the light, evaluated at the
	// path midpoint rather than at the dome fragment: the dome vertex sits on
	// the outer shell, so using its world position parallaxes the sun
	// direction by up to the shell thickness against the surface shading that
	// quad.frag computes at the surface. Same light position both passes
	// use (the sun, FrameContext.lightPos) so the terminators agree.
	vec3 l = (lightPos.w != 0.0)
		? normalize(lightPos.xyz - shellCenter - (eye + dir * (e.x + e.y) * 0.5))
		: normalize(lightPos.xyz);

	vec3 I = inScatter(eye, dir, e, l, scaleH, scaleL);

	// The ring lies wholly outside the shell, so its hit is either before
	// the scattering segment (sheet in front) or after it (sky over ring).
	if (hasRing > 0.5 && ringT > 0.0 && ringT < e.x && ringR01 > 0.0 && ringR01 < 1.0) {
		float a = (1.0 - exp(-ringTau / max(abs(ringMu), 1e-3)))
		        * smoothstep(ringFade.x, ringFade.y, ringT);
		I *= 1.0 - a;
	}

	FragColor = vec4(I, 1.0);
}
