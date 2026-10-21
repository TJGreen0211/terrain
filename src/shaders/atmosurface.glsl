// Atmosphere as seen by lit surfaces (quad.frag, water.frag, clouds.frag):
// extinction toward the camera, the sun's colour after its path through the
// air, and the skylight fraction. Needs PI defined first. Uniforms are set
// by setAtmosphereExtinction (Atmosphere.c).

// Atmospheric extinction (OpenGLSolar planet.frag port): dims the surface by
// the transmittance along the surface-to-camera path through the dome's shell.
// The atmosphere dome pass adds the in-scattered light over both the limb and
// the planet disc; adding it here as well would count that radiance twice.
// applyAtmosphere takes planet-local eye/fragment positions (shell-centred).
uniform float hasAtmosphere;
uniform float fInnerRadius;
uniform float fOuterRadius;
uniform float fScaleHeight;   // metres per e-fold of density (Atmosphere.c)
#include "atmodensity.glsl"
uniform vec3 C_R;

// Keep the miss sentinel beyond the largest supported shell radius.
const float ATMO_MAX = 1e30;
uniform float K_R; // Rayleigh / Mie constants, shared with atmosphere.frag (Atmosphere.c)
uniform float K_M;
const int NUM_ATMO_STEPS = 5;
// atmoOptic straight up from fInnerRadius to fOuterRadius (the same
// 5-sample midpoint march), computed once on the CPU (Atmosphere.c).
uniform float atmoTauZenith;

vec2 shellIntersection(vec3 p, vec3 dir, float radius)
{
	float b = dot(p, dir);
	float c = dot(p, p) - radius * radius;
	float d = b * b - c;
	if (d < 0.0) {
		return vec2(ATMO_MAX, -ATMO_MAX);
	}
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

float atmoOptic(vec3 p, vec3 q, float scaleH, float scaleL)
{
	vec3 step = (q - p) / float(NUM_ATMO_STEPS);
	vec3 v = p + step * 0.5;
	float sum = 0.0;
	for (int i = 0; i < NUM_ATMO_STEPS; i++) {
		// Clamped at the ground (atmoDensity).
		sum += atmoDensity(length(v) - fInnerRadius);
		v += step;
	}
	return sum * length(step) * scaleL;
}

vec3 applyAtmosphere(vec3 color, vec3 eye, vec3 frag)
{
	float scaleH = 1.0 / fScaleHeight;
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);

	// Ray in the shell-centred frame: eye = camera, fragment at distance tFrag.
	vec3 ray = frag - eye;
	float tFrag = length(ray);
	vec3 dir = ray / max(tFrag, 1e-6);

	vec2 e = shellIntersection(eye, dir, fOuterRadius);
	if (e.x > e.y) {
		return color; // view ray misses the shell entirely
	}
	vec2 f = shellIntersection(eye, dir, fInnerRadius);
	// Clip the path at the shell exit, the inner sphere, and the actual
	// fragment hit: the tFrag term handles displaced surfaces, where f.x
	// would overshoot or fall short of the visible surface.
	// camera inside the shell: the path starts at the camera
	e.x = max(e.x, 0.0);
	e.y = min(e.y, tFrag);
	if (f.x > e.x) {
		e.y = min(e.y, f.x);
	}
	if (e.y <= e.x) {
		return color;
	}

	float tau = atmoOptic(eye + dir * e.x, eye + dir * e.y, scaleH, scaleL) * (4.0 * PI);
	return color * exp(-(K_R * C_R + K_M) * tau);
}

// Sunlight colour at p after the sun-ward path through the shell, relative to
// an overhead sun (so the midday tuning of sunRadiance is unchanged): white at
// noon, warming and dimming toward the terminator. SUN_EXTINCTION scales the
// excess optical depth (1 = same air as the sky dome, which reads a little
// orange across the whole day side). The horizon factor covers the sun's disc
// setting behind the geometric limb, sized by its angular radius.
#define SUN_EXTINCTION 0.5
vec3 sunTransmittance(vec3 p, vec3 l, float sunAngle)
{
	vec3 up = normalize(p);
	float horizon = smoothstep(-sunAngle, sunAngle, dot(up, l));
	if (hasAtmosphere < 0.5 || horizon <= 0.0)
		return vec3(horizon);
	float scaleH = 1.0 / fScaleHeight;
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);
	// Start on or above the ground: the shell geometry and displaced
	// fragments can sit slightly under fInnerRadius.
	p = up * max(length(p), fInnerRadius);
	float tSun = shellIntersection(p, l, fOuterRadius).y;
	float tau = atmoOptic(p, p + l * max(tSun, 0.0), scaleH, scaleL);
	float excess = max(tau - atmoTauZenith, 0.0) * (4.0 * PI) * SUN_EXTINCTION;
	return exp(-(K_R * C_R + K_M) * excess) * horizon;
}

// Skylight on a surface as a fraction of its daytime strength: the sky
// itself is lit by the sun, so it fades through twilight (sun up to ~12 deg
// below the horizon) instead of lighting the night side. NIGHT_FLOOR is the
// starlight/airglow left over, so the dark side reads as dark, not black.
#define NIGHT_FLOOR vec3(0.003, 0.004, 0.006)
float skyFraction(float sunUp)
{
	return smoothstep(-0.2, 0.3, sunUp);
}
