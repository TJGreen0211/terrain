// Gas giant shading for the stand-in (planet_impostor.frag; uniforms from
// GasGiant_BindStandIn, gas_giant.c). Needs tonemap.glsl included first.
//
// The bands are a 1D latitude profile (RGB band colour as display values,
// A turbulence weight). The lookup latitude is spun around each storm and
// around the polar cyclones, then warped by two levels of domain-warped
// noise, stretched east-west, so band edges curl into streamers. Thin
// ridges of the warped noise are the wisps: bright cirrus-like strands
// along the streamers. A second noise is the cloud texture: it varies
// brightness and, through its slope toward the Sun, gives the cloud
// masses a sculpted look. Octaves finer than a pixel fade out, so close
// views gain detail and far ones stay smooth.
//
// Light: Minnaert limb darkening, wrap light past the terminator tinted
// warm, a forward-scattering haze veil, and a glow beyond the limb.
// Colours are lit in linear (displayToLinear) and tone mapped back, so
// full sunlight straight down returns the profile colour.
//
// Body-fixed axes: pole = +Z, latitude asin(p.z), longitude atan(p.y, p.x).

const float GAS_PI = 3.14159265;
#define GAS_MAX_STORMS 10   // GASGIANT_MAX_STORMS
#define GAS_MAX_POLAR  8    // GASGIANT_MAX_POLAR

uniform int gasOn;
uniform sampler1D gasProfile;   // RGB band colour (display), A turbulence weight
uniform sampler3D gasNoiseTex;  // 4 independent periodic value noises
uniform vec3 gasSeedOffset;
uniform float gasFreq;          // noise frequency north-south, per radius
uniform float gasStreak;        // east-west stretch
uniform float gasTurbulence;
uniform float gasDetail;
uniform float gasRelief;
uniform float gasWisps;
uniform float gasHazeTau;
uniform vec3 gasHazeColor;
uniform int gasStormCount;
uniform vec4 gasStorm[GAS_MAX_STORMS];       // lat, lon (rad), half-length (rad), aspect
uniform vec4 gasStormColor[GAS_MAX_STORMS];  // display rgb, opacity
uniform float gasStormSpin[GAS_MAX_STORMS];  // swirl at the centre, rad
uniform int gasPolarCount;      // cyclones in each polar ring, plus one on the pole; -1 = none
uniform vec4 gasPolar;          // ring colatitude, size (rad), spin (rad), phase
uniform vec3 gasPolarColor;     // their cores (display)
uniform float gasLimbK;
uniform float gasWrap;
uniform vec3 gasTerminator;
uniform float gasAmbient;
uniform float gasRim;
uniform float gasGlow;
uniform float gasExposure;

const float GAS_NOISE_PERIOD = 32.0;   // lattice cells per tile (gas_giant.c)

// Four value noises at p (lattice units), one trilinear fetch.
vec4 gasNoise4(vec3 p)
{
	return textureLod(gasNoiseTex, p / GAS_NOISE_PERIOD, 0.0);
}

// FBM of the four noises, each in [0, 1]. fp: one pixel in noise units at
// the first octave; octaves finer than about a pixel fade to their mean
// instead of aliasing (and are skipped).
vec4 gasFbm4(vec3 p, int octaves, float fp)
{
	vec4 s = vec4(0.0);
	float a = 0.5, total = 0.0;
	for (int k = 0; k < octaves; k++) {
		float keep = 1.0 - smoothstep(0.25, 0.6, fp);
		if (keep <= 0.0) {
			// The rest of the octaves at their mean.
			float rest = 2.0 * a * (1.0 - pow(0.5, float(octaves - k)));
			s += 0.5 * rest;
			total += rest;
			break;
		}
		s += a * mix(vec4(0.5), gasNoise4(p), keep);
		total += a;
		p = p * 2.03 + vec3(1.7, 9.2, 3.1);
		fp *= 2.03;
		a *= 0.5;
	}
	return s / total;
}

// Storm i's offset from its centre at (lat, lon): east, north (rad).
vec2 gasStormOffset(int i, float lat, float lon)
{
	vec4 S = gasStorm[i];
	float dlon = mod(lon - S.y + GAS_PI, 2.0 * GAS_PI) - GAS_PI;
	return vec2(dlon * cos(lat), lat - S.x);
}

// Spins (lat, lon) around the storms, so the bands inside wind into a
// spiral; returns the strongest storm's colour (display) and cover, and
// in collar a pale ring around the spinning ones.
vec4 gasStorms(inout float lat, inout float lon, out float collar)
{
	vec4 cover = vec4(0.0);
	collar = 0.0;
	for (int i = 0; i < GAS_MAX_STORMS; i++) {
		if (i >= gasStormCount)
			break;
		vec4 S = gasStorm[i];
		if (S.z <= 0.0)
			continue;
		vec2 e = gasStormOffset(i, lat, lon);
		vec2 q = vec2(e.x, e.y * S.w) / S.z;    // the storm's edge at |q| = 1
		float r2 = dot(q, q);
		if (r2 > 9.0)
			continue;
		float ang = gasStormSpin[i] * exp(-1.5 * r2);
		float c = cos(ang), s = sin(ang);
		q = vec2(c * q.x - s * q.y, s * q.x + c * q.y);
		e = vec2(q.x, q.y / S.w) * S.z;
		lat = clamp(S.x + e.y, -0.5 * GAS_PI, 0.5 * GAS_PI);
		lon = S.y + e.x / max(cos(lat), 0.05);
		float r = sqrt(r2);
		float body = (1.0 - smoothstep(0.35, 1.05, r)) * gasStormColor[i].a;
		if (body > cover.a)
			cover = vec4(gasStormColor[i].rgb, body);
		float ring = (r - 1.2) / 0.22;
		collar = max(collar, exp(-ring * ring) * min(abs(gasStormSpin[i]) * 0.2, 0.5));
	}
	return cover;
}

// Polar cyclones: a ring of gasPolarCount around each pole plus one on the
// pole, in the pole's azimuthal-equidistant plane. Spins (lat, lon) like
// the storms; returns their core cover and, in collar, a pale ring.
float gasPolarCyclones(inout float lat, inout float lon, out float collar)
{
	collar = 0.0;
	float ringC = gasPolar.x, size = gasPolar.y;
	float colat = 0.5 * GAS_PI - abs(lat);
	if (gasPolarCount < 0 || colat > ringC + 2.5 * size)
		return 0.0;
	float hemi = lat >= 0.0 ? 1.0 : -1.0;
	vec2 P = colat * vec2(cos(lon), sin(lon));
	float cover = 0.0;
	for (int k = 0; k <= GAS_MAX_POLAR; k++) {
		if (k > gasPolarCount)
			break;
		// k = 0: the cyclone on the pole (larger); then the ring.
		// Jittered per cyclone (angle, radius, size), so the ring is not
		// a perfect polygon.
		float j = float(k) * 12.9898 + gasPolar.w * 7.0 + hemi * 3.1;
		float th = gasPolar.w + hemi * 0.7 + 2.0 * GAS_PI * float(k) / float(max(gasPolarCount, 1))
		         + 0.25 * sin(j);
		vec2 C = k == 0 ? vec2(0.0) : ringC * (1.0 + 0.12 * sin(j * 1.7)) * vec2(cos(th), sin(th));
		float sz = k == 0 ? size * 1.4 : size * (0.8 + 0.4 * fract(sin(j) * 43758.5));
		vec2 d = P - C;
		float r2 = dot(d, d) / (sz * sz);
		if (r2 > 6.0)
			continue;
		// Cyclones turn with the planet's spin in each hemisphere.
		float ang = hemi * gasPolar.z * exp(-1.5 * r2);
		float c = cos(ang), s = sin(ang);
		P = C + vec2(c * d.x - s * d.y, s * d.x + c * d.y);
		float r = sqrt(r2);
		cover = max(cover, 1.0 - smoothstep(0.2, 0.85, r));
		float ring = (r - 1.05) / 0.2;
		collar = max(collar, exp(-ring * ring) * 0.35);
	}
	colat = length(P);
	lat = hemi * (0.5 * GAS_PI - colat);
	lon = atan(P.y, P.x);
	return cover;
}

// Henyey-Greenstein, scaled to 1 at 90 degrees (cosT = dot(view ray, Sun)).
float gasPhase(float cosT)
{
	const float g = 0.6;
	float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5);
	float side = (1.0 - g * g) / pow(1.0 + g * g, 1.5);
	return hg / side;
}

// Light reaching the cloud tops at sun cosine mu0: wrapped past the
// terminator and reddened near it.
vec3 gasSunlight(float mu0, float wrap)
{
	float w = clamp((mu0 + wrap) / (1.0 + wrap), 0.0, 1.0);
	return w * mix(gasTerminator, vec3(1.0), smoothstep(0.0, 0.4, w));
}

// n, d, sunV: unit surface normal, view ray and Sun direction (view axes);
// nb, sunB: the normal and Sun direction in body-fixed axes (pole = +Z).
// pixelR: one pixel at the body, radii. sunVis: sunlight left after the
// ring's shadow. Returns the display colour.
vec3 gasGiantSurface(vec3 n, vec3 d, vec3 sunV, vec3 nb, vec3 sunB, float pixelR, float sunVis)
{
	float mu = max(dot(n, -d), 0.0);
	float mu0 = dot(n, sunV);
	float foot = pixelR / max(mu, 0.08);       // one pixel on the cloud tops, radii

	float lat = asin(clamp(nb.z, -1.0, 1.0));
	float lon = atan(nb.y, nb.x);
	float polarCollar;
	float polar = gasPolarCyclones(lat, lon, polarCollar);
	float collar;
	vec4 storm = gasStorms(lat, lon, collar);
	collar = max(collar, polarCollar);
	float cl = cos(lat);
	vec3 ps = vec3(cl * cos(lon), cl * sin(lon), sin(lat));

	// Noise space: long east-west, less so toward the poles.
	float st = mix(gasStreak, 1.0, smoothstep(0.75, 0.97, abs(ps.z)));
	vec3 q = vec3(ps.xy / st, ps.z) * gasFreq + gasSeedOffset;
	float fp = foot * gasFreq;

	float texel = GAS_PI / float(textureSize(gasProfile, 0));
	float lod = log2(max(foot / texel, 1.0));
	float turb = textureLod(gasProfile, lat / GAS_PI + 0.5, lod).a;

	// Two-level domain warp: long streamers, then curls where the profile
	// says the bands churn.
	vec2 w1 = gasFbm4(q, 4, fp).xy - 0.5;
	float amt = gasTurbulence * (0.25 + turb);
	vec3 q2 = q * 2.3 + vec3(w1.x, w1.y, w1.x + w1.y) * 4.0 * amt;
	vec4 n2 = gasFbm4(q2, 5, fp * 2.3);
	float w2 = n2.z - 0.5;
	float dlat = (w2 + 0.5 * w1.x) * 1.2 / gasFreq * amt;
	vec3 col = textureLod(gasProfile, (lat + dlat) / GAS_PI + 0.5, lod).rgb;

	// Cloud texture and its slope toward the Sun (sculpted relief).
	vec3 qd = q2 * 1.7 + vec3(11.0, 3.0, 7.0);
	float fpd = fp * 2.3 * 1.7;
	float h = gasFbm4(qd, 5, fpd).w;
	vec3 sunT = sunB - nb * dot(sunB, nb);
	float sunTl = length(sunT);
	float shade = 1.0;
	if (sunTl > 1e-4) {
		vec3 dq = vec3(sunT.xy / st, sunT.z);
		float h2 = gasFbm4(qd + normalize(dq) * 0.12, 5, fpd).w;
		shade = max(1.0 + gasRelief * (h2 - h) * 5.0 * sunTl, 0.2);
	}

	col = mix(col, gasPolarColor, polar * 0.75 * (0.8 + 0.4 * h));
	col = mix(col, storm.rgb, clamp(storm.a * (1.0 + 1.2 * (h - 0.5)), 0.0, 1.0));
	col = mix(col, max(col, vec3(0.95, 0.9, 0.82)), collar * (0.8 + 0.6 * (h - 0.5)));

	// Wisps: ridges of a noise stretched far east-west in the warped space,
	// so they come out as long thin strands that bend with the streamers
	// (not a cell network). They fade out before they would alias, and up
	// close once a strand is more than a few pixels wide (broad, they read
	// as a satin sheen, not cloud).
	float fpw = fp * 2.3 * 1.6;
	float strandPx = 0.05 / max(fpw, 1e-6);
	float wispFade = (1.0 - smoothstep(0.1, 0.4, fpw)) * (1.0 - smoothstep(4.0, 14.0, strandPx));
	if (gasWisps > 0.0 && wispFade > 0.0) {
		vec3 qw = vec3(q2.xy * 0.3, q2.z * 1.6) + vec3(5.0, 2.0, 9.0);
		float nw = gasFbm4(qw, 3, fpw).y;
		float ridge = 1.0 - abs(2.0 * nw - 1.0);
		float wisp = smoothstep(0.8, 0.97, ridge) * wispFade;
		col = mix(col, mix(col * 1.2, vec3(0.97, 0.95, 0.9), 0.45), clamp(wisp * gasWisps * (0.35 + turb), 0.0, 1.0));
	}

	col *= 1.0 + gasDetail * (h - 0.5) * 0.9;

	// Lighting, linear.
	vec3 alb = displayToLinear(clamp(col, 0.0, 1.0));
	vec3 haze = displayToLinear(gasHazeColor);
	float wrapL = clamp((mu0 + gasWrap) / (1.0 + gasWrap), 0.0, 1.0);
	float limb = pow(max(wrapL, 1e-4), gasLimbK - 1.0) * pow(max(mu, 0.02), gasLimbK - 1.0);
	vec3 sun = gasSunlight(mu0, gasWrap) * limb * sunVis;
	vec3 surface = alb * (sun * shade + gasAmbient * haze);

	// Haze veil: thicker toward the limb, lit with a wider wrap and a
	// forward-scattering rim when the Sun is behind the planet.
	float veil = 1.0 - exp(-gasHazeTau / max(mu, 0.03));
	vec3 hazeSun = gasSunlight(mu0, 2.0 * gasWrap) * sunVis;
	float phase = mix(1.0, gasPhase(dot(d, sunV)), clamp(0.25 * gasRim, 0.0, 1.0));
	vec3 inScatter = haze * (hazeSun * phase + gasAmbient);
	vec3 lin = mix(surface, inScatter, veil);
	return toneMap(lin * gasExposure);
}

// Glow beyond the limb. x: height through the haze shell (0 at the limb,
// 1 at its top); limbDir: the normal at the limb point below (view axes).
// Returns the display colour and its alpha.
vec4 gasGiantGlow(float x, vec3 limbDir, vec3 d, vec3 sunV)
{
	float mu0 = dot(limbDir, sunV);
	vec3 light = gasSunlight(mu0, 2.0 * gasWrap + 0.15);
	float phase = mix(1.0, gasPhase(dot(d, sunV)), clamp(0.25 * gasRim, 0.0, 1.0));
	float dens = exp(-4.0 * x) * (1.0 - x);
	float bright = max(max(light.r, light.g), light.b) * phase;
	float a = clamp(dens * gasGlow * bright, 0.0, 1.0);
	vec3 c = displayToLinear(gasHazeColor) * light / max(bright, 1e-3) * min(phase, 2.0);
	return vec4(toneMap(c * gasExposure), a);
}
