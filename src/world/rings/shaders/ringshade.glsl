// Ring sheet shading (rings.c), shared by the full sheet (ring.frag) and
// the stand-ins' rings (planet_impostor.frag) so a ring looks the same at
// every distance, plus the planet's shadow (also rock.frag, ringdust.vert).
// prof is a ring profile sample: RGB single-scattering albedo, A normal
// optical depth.

const float RING_PI = 3.14159265359;
// Single scattering alone is too dark for an optically thick ring; this
// stands in for the multiple-scattering contribution.
const float RING_GAIN = 3.0;

// Henyey-Greenstein, normalized over the sphere. c = cos(scattering angle).
float ringHG(float g, float c)
{
	float gg = g * g;
	return (1.0 - gg) / (4.0 * RING_PI * pow(1.0 + gg - 2.0 * g * c, 1.5));
}

// Planet shadow: p relative to the planet centre, l the unit direction to
// the sun, R the planet radius (p's units). Soft edge from the sun's
// angular size over the distance to the limb, plus a little atmospheric
// softening.
float ringPlanetShadow(vec3 p, vec3 l, float R, float sunAngle)
{
	float b = -dot(p, l);
	if (b <= 0.0)
		return 1.0;
	float d = length(p + l * b);
	float w = R * 0.01 + b * sunAngle;
	return smoothstep(R - w, R + w, d);
}

// The sheet as a single-scattering slab. V, L: unit directions to the
// camera and the sun; N the ring normal; sun the sunlight reaching it
// (planet shadow). Returns the ring's own radiance (linear, before the tone
// map) and its coverage along the view ray.
vec4 ringShade(vec4 prof, vec3 V, vec3 L, vec3 N, float sun)
{
	vec3 albedo = prof.rgb;
	float tau = prof.a;
	float vn = dot(V, N);
	float ln = dot(L, N);
	float mu = max(abs(vn), 1e-3);
	float mu0 = max(abs(ln), 1e-3);

	// Phase: dense icy regions backscatter (with an opposition surge when
	// the sun is behind the camera), thin dusty ringlets forward-scatter,
	// so they glow when the ring is backlit.
	float cosScatter = dot(-L, V);
	float dustiness = mix(0.7, 0.1, smoothstep(0.05, 1.0, tau));
	float phase = mix(ringHG(-0.3, cosScatter), ringHG(0.65, cosScatter), dustiness);
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
	vec3 I = sunRadiance * sun * albedo * phase * scat * RING_GAIN;

	// I is already coverage-weighted; divide it back out so the tone map
	// sees the ring's own radiance.
	float alpha = 1.0 - exp(-tau / mu);
	return vec4(I / max(alpha, 1e-5) + albedo * 0.01, alpha);
}
