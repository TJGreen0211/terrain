#version 410

// The haze and murk inside a gas giant (gas_interior.c), marched from the
// camera to the scene depth and blended over the scene (premultiplied:
// in-scatter, 1 - transmittance).
//
// Above the cloud tops (altitude 0, the stand-in sphere) the haze density
// falls off exponentially: sigma0 * exp(-a / scaleHeight), the same column
// optical depth the stand-in's veil uses, so the two agree where they
// cross-fade (share). Below them a murk thickens and takes the belt colour,
// and the Sun's light dies away with depth.
//
// The haze is lit like the stand-in's veil (gasgiant.glsl): wrap light
// reddened toward the terminator, a Henyey-Greenstein forward peak scaled
// to 1 at 90 degrees, plus scattered skylight. Soft rays streak the glow
// around the Sun.
//
// Everything is in view space, metres: the camera is the origin, camUp
// points away from the giant's centre. Altitudes along a ray use the
// cancellation-free form (r^2 - R^2) / (r + R), so they stay exact to a few
// metres at 75,000 km.

uniform mat4 projection;
uniform vec2 viewportSize;
uniform vec3 camUp;          // unit, giant centre -> camera
uniform float camSun;        // Sun's cosine above the camera's horizon
uniform vec3 sunDir;         // unit, camera -> Sun
uniform float radius;        // cloud tops, m
uniform float camAlt;        // m above the cloud tops
uniform float scaleHeight;   // m
uniform float sigma0;        // haze extinction at the cloud tops, 1/m
uniform float hazeTop;       // m: the march ends above this
uniform float murkSigma;     // extra extinction below the cloud tops, 1/m
uniform vec3 hazeColor;      // display
uniform vec3 murkColor;      // display
uniform vec3 terminator;
uniform float wrap;
uniform float rim;
uniform float nightAmbient;
uniform float exposure;
uniform float ambient;       // scattered skylight, x the Sun
uniform float rays;
uniform float glare;         // the bright aureole right around the Sun
uniform float share;         // the part of the haze this pass draws
uniform int steps;
uniform float frameJitter;

uniform sampler2DMS sceneDepth;
uniform int sceneSamples;
uniform float depthNear;

out vec4 FragColor;

#include "engine/gfx/tonemap.glsl"

const float PI = 3.14159265;
const float MAX = 1e30;

// Distance (m) along view ray dir to the visible surface; MAX for the sky.
// The farthest MSAA sample, so a silhouette keeps the haze over the sky.
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
	return depthNear / d / max(-dir.z, 1e-6);
}

// Interleaved gradient noise (Jimenez 2014): the march's jitter.
float ign(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Henyey-Greenstein, scaled to 1 at 90 degrees (gasgiant.glsl's gasPhase).
float phaseHG(float cosT)
{
	const float g = 0.6;
	float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5);
	float side = (1.0 - g * g) / pow(1.0 + g * g, 1.5);
	return hg / side;
}

// Light reaching a point at sun cosine mu0: wrapped past the terminator
// and reddened near it (gasgiant.glsl's gasSunlight).
vec3 sunlight(float mu0, float w)
{
	float x = clamp((mu0 + w) / (1.0 + w), 0.0, 1.0);
	return x * mix(terminator, vec3(1.0), smoothstep(0.0, 0.4, x));
}

// Relative air mass toward the Sun (Kasten and Young): the slant column
// over the vertical one, finite at the horizon.
float airMass(float mu)
{
	float z = min(degrees(acos(clamp(mu, -1.0, 1.0))), 95.0);
	return 1.0 / (max(mu, 0.0) + 0.50572 * pow(96.07995 - z, -1.6364));
}

// Periodic 1D value noise over n cells around the circle (x in [0, 1)).
float hash1(float i)
{
	return fract(sin(i * 127.1) * 43758.5453);
}
float ringNoise(float x, float n)
{
	float c = x * n;
	float i = floor(c), f = c - i;
	f = f * f * (3.0 - 2.0 * f);
	// Wrap the cell index by hand: mod() can round n/n below 1.
	float i0 = i >= n ? i - n : i;
	float i1 = i0 + 1.0 >= n ? i0 + 1.0 - n : i0 + 1.0;
	return mix(hash1(i0), hash1(i1), f);
}

// Per-ray values the march shares (set in main).
vec3 gD, gHazeLin, gMurkLin;
float gRc, gB, gFwd, gJit;

// March [t0, t1] in n steps (quadratic spacing: fine near t0), adding the
// in-scatter to L and the extinction to T.
void march(float t0, float t1, int n, bool quadratic, inout vec3 L, inout float T)
{
	float R = radius;
	float column = sigma0 * scaleHeight;   // the whole haze column, straight down
	float tPrev = t0;
	for (int i = 0; i < n; i++) {
		float u = float(i + 1) / float(n);
		float tNext = t0 + (t1 - t0) * (quadratic ? u * u : u);
		float dt = tNext - tPrev;
		float t = tPrev + dt * gJit;
		tPrev = tNext;

		float r2 = gRc * gRc + 2.0 * gB * t + t * t;
		float r = sqrt(r2);
		float a = (camAlt * (2.0 * R + camAlt) + 2.0 * gB * t + t * t) / (r + R);
		vec3 upS = (camUp * gRc + gD * t) / r;
		float mus = dot(upS, sunDir);

		float below = max(-a, 0.0);
		float deep = smoothstep(0.0, 2000.0, below);
		float sigma = (a >= 0.0 ? sigma0 * exp(-a / scaleHeight) : sigma0) + murkSigma * deep;
		vec3 alb = mix(gHazeLin, gMurkLin, smoothstep(0.0, 3000.0, below));

		// Sunlight here: through the haze above, and the murk.
		float tauSun = column * exp(-max(a, 0.0) / scaleHeight) * airMass(mus)
		             + murkSigma * below / max(mus, 0.05);
		vec3 sun = sunlight(mus, 2.0 * wrap) * exp(-tauSun);
		float sky = exp(-murkSigma * below * 0.7);
		vec3 J = alb * (sun * gFwd + ambient * sunlight(mus, 2.0 * wrap) * sky + nightAmbient);

		float ext = exp(-sigma * dt);
		L += T * J * (1.0 - ext);
		T *= ext;
		if (T < 1e-3)
			break;
	}
}

void main()
{
	vec2 ndc = gl_FragCoord.xy / viewportSize * 2.0 - 1.0;
	vec3 d = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));

	float R = radius;
	float rc = R + camAlt;               // camera's distance from the centre
	float mu = dot(d, camUp);
	float b = rc * mu;

	// Where the ray leaves the haze (the camera is always inside it).
	float c = (camAlt - hazeTop) * (2.0 * R + camAlt + hazeTop);   // < 0
	float q = sqrt(max(b * b - c, 0.0));
	float tExit = b >= 0.0 ? -c / (b + q) : q - b;
	float tEnd = min(tExit, sceneDistance(d));
	if (tEnd <= 0.0)
		discard;

	// Soft rays: streaks by angle around the Sun, in a frame tied to the
	// giant's up, so they don't turn with the camera.
	float cosT = dot(d, sunDir);
	vec3 ru = cross(sunDir, camUp);
	ru = dot(ru, ru) > 1e-6 ? normalize(ru) : normalize(cross(sunDir, vec3(0.0, 1.0, 0.0)));
	vec3 rv = cross(sunDir, ru);
	float ang = atan(dot(d, rv), dot(d, ru)) / (2.0 * PI) + 0.5;
	// Few, broad, soft rays; none right at the Sun (no pinwheel there).
	float streak = 0.7 * ringNoise(ang, 26.0) + 0.3 * ringNoise(ang, 61.0);
	float rayZone = smoothstep(0.6, 0.97, cosT) * (1.0 - smoothstep(0.985, 0.9995, cosT));
	float rayGain = 1.0 + rays * (2.0 * streak - 1.0) * rayZone;
	float phase = mix(1.0, phaseHG(cosT), clamp(0.25 * rim, 0.0, 1.0));
	// The aureole: a narrow forward lobe (HG g = 0.9, per steradian), ~0
	// away from the Sun.
	const float g2 = 0.9;
	float aureole = (1.0 - g2 * g2) / (4.0 * PI * pow(1.0 + g2 * g2 - 2.0 * g2 * cosT, 1.5));
	float fwd = 1.0 + (phase - 1.0) * rayGain + glare * aureole;

	gHazeLin = displayToLinear(hazeColor);
	gMurkLin = displayToLinear(murkColor);
	gD = d;
	gRc = rc;
	gB = b;
	gFwd = fwd;
	gJit = fract(ign(gl_FragCoord.xy) + frameJitter);

	vec3 L = vec3(0.0);
	float T = 1.0;
	if (camAlt < 0.0) {
		// Below the cloud tops: fine, even steps through the murk up to
		// where the ray leaves it (or it has gone dark), then the haze.
		float cIn = camAlt * (2.0 * R + camAlt);   // < 0
		float qIn = sqrt(max(b * b - cIn, 0.0));
		float tDeck = b >= 0.0 ? -cIn / (b + qIn) : qIn - b;
		float tMurk = min(min(tDeck, 8.0 / max(murkSigma, 1e-9)), tEnd);
		march(0.0, tMurk, steps / 2, false, L, T);
		if (tMurk < tEnd && T > 1e-3)
			march(tMurk, tEnd, steps / 2, true, L, T);
	} else {
		march(0.0, tEnd, steps, true, L, T);
	}

	// By day the haze hides the stars and the nebula behind it (it is
	// far brighter than they are; the Sun's disc has depth, so it stays).
	float hide = 0.0;
	if (tEnd >= tExit)
		hide = smoothstep(-0.15, 0.1, camSun) * smoothstep(0.0, 0.15, 1.0 - T);
	FragColor = vec4(L * exposure * share, max(1.0 - T, hide) * share);
}
