#version 410

// Analytic ring sheet (Rings.c). fragPos is the camera-relative ray-plane
// hit; its radius indexes the baked profile (RGB single-scattering albedo,
// A normal optical depth). Shaded as a single-scattering slab, output as
// premultiplied alpha (blend ONE, ONE_MINUS_SRC_ALPHA).
in vec3 fragPos;

out vec4 FragColor;

#include "tonemap.glsl"

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

const float PI = 3.14159265359;
// Single scattering alone is too dark for an optically thick ring; this
// stands in for the multiple-scattering contribution.
const float RING_GAIN = 3.0;

// Henyey-Greenstein, normalized over the sphere. c = cos(scattering angle).
float hg(float g, float c)
{
	float gg = g * g;
	return (1.0 - gg) / (4.0 * PI * pow(1.0 + gg - 2.0 * g * c, 1.5));
}

// Planet shadow: p relative to the planet centre, l the unit direction to
// the sun. Soft edge from the sun's angular size over the distance to the
// limb, plus a little atmospheric softening.
float planetShadow(vec3 p, vec3 l)
{
	float b = -dot(p, l);
	if (b <= 0.0)
		return 1.0;
	float d = length(p + l * b);
	float w = planetRadius * 0.01 + b * ringSunAngle;
	return smoothstep(planetRadius - w, planetRadius + w, d);
}

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
	vec3 albedo = prof.rgb;
	float tau = prof.a;

	vec3 V = normalize(-fragPos);
	vec3 L = normalize(lightPosition - fragPos);
	float vn = dot(V, ringNormal);
	float ln = dot(L, ringNormal);
	float mu = max(abs(vn), 1e-3);
	float mu0 = max(abs(ln), 1e-3);

	// Phase: dense icy regions backscatter (with an opposition surge when
	// the sun is behind the camera), thin dusty ringlets forward-scatter,
	// so they glow when the ring is backlit.
	float cosScatter = dot(-L, V);
	float dustiness = mix(0.7, 0.1, smoothstep(0.05, 1.0, tau));
	float phase = mix(hg(-0.3, cosScatter), hg(0.65, cosScatter), dustiness);
	phase *= 1.0 + 0.5 * exp(-acos(clamp(-cosScatter, -1.0, 1.0)) / 0.05);

	// Single scattering in a uniform slab: lit face (sun and camera on the
	// same side) reflects, the far face transmits diffusely.
	float scat;
	if (vn * ln > 0.0)
		scat = mu0 / (mu0 + mu) * (1.0 - exp(-tau * (1.0 / mu0 + 1.0 / mu)));
	else if (abs(mu0 - mu) > 1e-3)
		scat = mu0 / (mu0 - mu) * (exp(-tau / mu0) - exp(-tau / mu));
	else
		scat = tau / mu0 * exp(-tau / mu0);

	vec3 sunRadiance = vec3(1.0, 0.97, 0.90) * 1.15;   // matches quad.frag
	vec3 I = sunRadiance * planetShadow(local, L) * albedo * phase * scat * RING_GAIN;

	// Coverage along the view ray. I is already coverage-weighted; divide it
	// back out so the tone map sees the ring's own radiance.
	float alpha = 1.0 - exp(-tau / mu);
	vec3 radiance = I / max(alpha, 1e-5) + albedo * 0.01;
	alpha *= smoothstep(ringFade.x, ringFade.y, length(fragPos));

	// The scene's tone map (tonemap.glsl).
	vec3 col = toneMap(radiance);
	FragColor = vec4(col * alpha, alpha);
}
