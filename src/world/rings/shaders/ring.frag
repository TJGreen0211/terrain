#version 410

// Analytic ring sheet (rings.c). fragPos is the camera-relative ray-plane
// hit; its radius indexes the baked profile (RGB single-scattering albedo,
// A normal optical depth). Shaded as a single-scattering slab, output as
// premultiplied alpha (blend ONE, ONE_MINUS_SRC_ALPHA).
in vec3 fragPos;

out vec4 FragColor;

#include "engine/gfx/tonemap.glsl"

uniform vec3 lightPosition;   // camera-relative
uniform vec3 ringCenter;      // camera-relative planet centre
uniform vec3 ringNormal;
uniform vec2 ringRadii;       // inner, outer (world units)
uniform sampler1D ringProfile;
uniform float planetRadius;
uniform float ringSunAngle;   // sun angular radius: shadow penumbra width
// The sheet fades out between these camera distances, where the instanced
// rocks and dust take over (atmosphere.frag applies the same fade).
uniform vec2 ringFade;

#include "ringshade.glsl"

void main()
{
	vec3 local = fragPos - ringCenter;
	float r01 = (length(local) - ringRadii.x) / (ringRadii.y - ringRadii.x);
	// Implicit-derivative lookup before any discard: the mip follows the
	// pixel's radial footprint, so ringlets finer than a pixel average out
	// instead of aliasing at distance or at grazing angles.
	vec4 prof = texture(ringProfile, r01);
	if (r01 <= 0.0 || r01 >= 1.0 || prof.a < 1e-4)
		discard;

	vec3 V = normalize(-fragPos);
	vec3 L = normalize(lightPosition - fragPos);
	vec4 ring = ringShade(prof, V, L, ringNormal, ringPlanetShadow(local, L, planetRadius, ringSunAngle));
	float alpha = ring.a * smoothstep(ringFade.x, ringFade.y, length(fragPos));

	// The scene's tone map (tonemap.glsl).
	FragColor = vec4(toneMap(ring.rgb) * alpha, alpha);
}
