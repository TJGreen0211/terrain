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
// High clouds: a sparse layer of bright streaks and plumes in a few
// latitude bands, above the deck. They sit above most of the haze, darken
// less toward the limb, and cast soft shadows on the deck, offset away
// from the Sun by their height.
//
// Lightning (night side): flashes glow under the cloud tops, spread by the
// cloud, brighter where it is thick; white-blue at the core, the cloud's
// tint at the edge. Placed and timed on the CPU (gas_giant.c).
//
// Aurora: round each magnetic pole, above the haze, four thin strands one
// behind another toward the pole. Each wanders on its own (so they cross,
// merge, kink and part), lights up and fades along its length, and splits
// into wisps by noise stretched along it. Sharp on the equatorward side
// (the curtains' feet), feathered poleward, all drifting on the scene
// clock. They are curtains in a thin glowing layer above the cloud tops,
// leaning poleward with height, green at their feet and magenta at the
// top: marched through on the disc (gasGiantSurface) and beyond the limb
// (gasAuroraBeyond, with the glow), where they stand off the edge.
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
#define GAS_MAX_HIGH   4    // GASGIANT_MAX_HIGH
#define GAS_MAX_FLASHES 8   // GASGIANT_MAX_FLASHES

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
uniform float gasHigh;          // high cloud amount
uniform float gasHighHeight;    // their altitude, radii
uniform vec3 gasHighColor;      // display
uniform int gasHighCount;
uniform vec3 gasHighBand[GAS_MAX_HIGH];   // lat, half-width (rad), strength
uniform int gasFlashCount;
uniform vec4 gasFlash[GAS_MAX_FLASHES];      // body-fixed direction (unit), glow radius (radii)
uniform float gasFlashGain[GAS_MAX_FLASHES]; // x full sunlight
uniform float gasAurora;        // brightness (0 = none), x full sunlight
uniform vec3 gasAuroraPole[2];  // magnetic poles, body-fixed (unit)
uniform vec2 gasAuroraOval;     // the oval's radius from its pole, its edge's width (rad)
uniform float gasAuroraDay;     // share left on the day side
uniform float gasAuroraHeight;  // the curtains' top above the cloud tops, radii
uniform float gasAuroraLean;    // how far poleward their tops lean, oval widths
uniform float gasAuroraPhase;   // shimmer, noise cells (scene clock)
uniform float gasAuroraGreen;   // share of the low colour at the edge
uniform vec3 gasAuroraLow;      // display colours: edge (green), bulk (violet), upper (magenta)
uniform vec3 gasAuroraMid;
uniform vec3 gasAuroraHigh;
uniform float gasLimbK;
uniform float gasWrap;
uniform vec3 gasTerminator;
uniform float gasAmbient;
uniform float gasRim;
uniform float gasGlow;
uniform float gasExposure;
uniform float gasInside;        // share of the haze the interior pass draws (gas_interior.c)
uniform int gasOctaves;         // detail octaves: more close to the cloud tops
uniform float gasGlowFade;      // the glow beyond the limb (0 once the camera is in the shell)
uniform float gasDeep;          // 1: the deck lies under the interior's cloud layers (darker)
uniform float gasDeepReach;     // m: how far those layers reach (fading from 35% of it)

const float GAS_NOISE_PERIOD = 32.0;   // lattice cells per tile (gas_giant.c)

// Four value noises at p (lattice units), one trilinear fetch.
vec4 gasNoise4(vec3 p)
{
	return textureLod(gasNoiseTex, p / GAS_NOISE_PERIOD, 0.0);
}

// The same, smooth between texels (the texel position eased, so one fetch
// is C1): for low frequencies drawn large, where plain trilinear shows
// as facets.
vec4 gasNoise4Smooth(vec3 p)
{
	vec3 t = p * (float(textureSize(gasNoiseTex, 0).x) / GAS_NOISE_PERIOD) + 0.5;
	vec3 i = floor(t), f = t - i;
	f = f * f * (3.0 - 2.0 * f);
	return textureLod(gasNoiseTex, (i + f - 0.5) / float(textureSize(gasNoiseTex, 0).x), 0.0);
}

// gasFbm4 with gasNoise4Smooth.
vec4 gasFbm4Smooth(vec3 p, int octaves, float fp)
{
	vec4 s = vec4(0.0);
	float a = 0.5, total = 0.0;
	for (int k = 0; k < octaves; k++) {
		float keep = 1.0 - smoothstep(0.25, 0.6, fp);
		s += a * mix(vec4(0.5), gasNoise4Smooth(p), keep);
		total += a;
		p = p * 2.03 + vec3(1.7, 9.2, 3.1);
		fp *= 2.03;
		a *= 0.5;
	}
	return s / total;
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

// High cloud cover at p (unit, body-fixed, storm-spun). st: the deck's
// east-west stretch; fp: one pixel in deck noise units. Long streaks,
// stretched further east-west than the deck, gathered into the latitude
// bands; far away they fade to their mean cover instead of vanishing.
float gasHighCover(vec3 p, float st, float fp)
{
	float lat = asin(clamp(p.z, -1.0, 1.0));
	float band = 0.0;
	for (int i = 0; i < GAS_MAX_HIGH; i++) {
		if (i >= gasHighCount)
			break;
		float t = (lat - gasHighBand[i].x) / gasHighBand[i].y;
		band = max(band, gasHighBand[i].z * exp(-t * t));
	}
	band = min(band * gasHigh, 1.0);
	if (band < 0.01)
		return 0.0;
	const float freq = 2.5, stretch = 1.5;
	vec3 qh = vec3(p.xy / (st * stretch), p.z) * gasFreq * freq + gasSeedOffset + vec3(17.0, 5.0, 23.0);
	float fph = fp * freq;
	// A gentle north-south wobble, so the streaks bend; no isotropic warp
	// (that reads as marbling).
	vec2 w = gasFbm4(qh * 0.5, 2, fph * 0.5).zw - 0.5;
	qh.z += w.x * 0.8;
	qh.xy += w.y * 1.5;
	float n = gasFbm4(qh, 4, fph).x;
	float lo = 0.62 - 0.1 * band;
	float near = smoothstep(lo, lo + 0.15, n);
	float far = smoothstep(0.08, 0.5, fph);
	return band * mix(near, 0.15, far);
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

// The cloud field at body-fixed direction nb (pole = +Z), one footprint
// foot (radii) across: what the deck's look is made of, before lighting.
// gasGiantSurface lights it; the interior's weather bake
// (gasweather.frag) turns it into cloud cover.
struct GasCloud {
	vec3 col;      // albedo, display
	float turb;    // the profile's turbulence weight here
	float h;       // the cloud texture, [0, 1]
	float hi;      // high cloud cover
	float storm;   // storm and cyclone cover, cores and collars
	float wall;    // the collars alone: the rings around spinning storms
	vec3 ps;       // nb spun around the storms (unit)
	vec3 qd;       // cloud texture coordinates
	float st;      // east-west stretch here
	float fp;      // one footprint in noise units
	float fpd;     // the same for the cloud texture
};

GasCloud gasCloudField(vec3 nb, float foot)
{
	GasCloud c;
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
	vec4 n2 = gasFbm4(q2, gasOctaves, fp * 2.3);
	float w2 = n2.z - 0.5;
	float dlat = (w2 + 0.5 * w1.x) * 1.2 / gasFreq * amt;
	vec3 col = textureLod(gasProfile, (lat + dlat) / GAS_PI + 0.5, lod).rgb;

	// Cloud texture (its slope toward the Sun is the relief:
	// gasGiantSurface).
	vec3 qd = q2 * 1.7 + vec3(11.0, 3.0, 7.0);
	float fpd = fp * 2.3 * 1.7;
	float h = gasFbm4(qd, gasOctaves, fpd).w;

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

	c.col = col;
	c.turb = turb;
	c.h = h;
	c.hi = gasHigh > 0.0 && gasHighCount > 0 ? gasHighCover(ps, st, fp) : 0.0;
	c.storm = max(max(storm.a, polar), collar);
	c.wall = collar;
	c.ps = ps;
	c.qd = qd;
	c.st = st;
	c.fp = fp;
	c.fpd = fpd;
	return c;
}

// The flashes' light at nb (linear), under cloud c with linear albedo alb.
// pixelR: one pixel there, radii: a glow is never narrower than ~2.5
// pixels, so far off it reads as a flicker, not a shimmering point.
vec3 gasLightning(vec3 nb, GasCloud c, vec3 alb, float pixelR)
{
	vec3 E = vec3(0.0);
	// Thick cloud glows; gaps hardly: the light picks out the cloud.
	float thick = mix(0.1, 1.8, smoothstep(0.25, 0.85, c.h));
	vec3 tint = alb / max(max(alb.r, alb.g), max(alb.b, 1e-4));
	for (int i = 0; i < GAS_MAX_FLASHES; i++) {
		if (i >= gasFlashCount)
			break;
		vec3 f = gasFlash[i].xyz;
		float r = max(gasFlash[i].w, 2.5 * pixelR);
		vec3 dv = nb - f;
		if (dot(dv, dv) > 144.0 * r * r)
			continue;
		// Spread twice as far east-west, along the bands; the halo ends
		// smoothly by x = 6 (no visible edge on the dark night side).
		vec3 east = normalize(cross(vec3(0.0, 0.0, 1.0), f) + vec3(1e-6, 0.0, 0.0));
		vec3 north = cross(f, east);
		float x = length(vec2(dot(dv, east) * 0.5, dot(dv, north))) / r;
		if (x > 6.0)
			continue;
		float core = exp(-3.0 * x * x);
		float glow = 0.6 * core + 0.4 * exp(-1.5 * x) * (1.0 - smoothstep(3.0, 6.0, x));
		vec3 col = mix(tint * 0.9, vec3(0.75, 0.85, 1.0), core);
		E += gasFlashGain[i] * glow * thick * col;
	}
	return E;
}

// The aurora's strands over body-fixed direction nb (unit), shifted
// equatorward by lean widths (the curtains lean poleward with height, so
// higher up the pattern sits farther toward the pole): their colour times
// brightness, linear, before the layer's height profile and gain. pixelR:
// one pixel there, radii (detail finer than a pixel fades to its mean).
vec3 gasAuroraStrands(vec3 nb, float lean, float pixelR)
{
	vec3 E = vec3(0.0);
	float th0 = gasAuroraOval.x, w = gasAuroraOval.y;
	for (int k = 0; k < 2; k++) {
		vec3 p = gasAuroraPole[k];
		float th = acos(clamp(dot(nb, p), -1.0, 1.0)) + lean * w;
		// Equatorward of the oval: nothing (its folds and edge end within
		// ~3 widths). Poleward the tail and cap glow reach the pole.
		if (th > th0 + 4.0 * w)
			continue;
		// Position on the plane round the magnetic pole, 1 at the oval: the
		// noise is read there (not by angle alone, which would pinch into
		// a pinwheel at the pole), so round the oval it has no seam.
		vec3 a = normalize(cross(p, abs(p.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
		vec3 b = cross(p, a);
		float ring = max(sin(th0), 0.05);   // radii per radian round the oval
		vec2 cs = vec2(dot(nb, a), dot(nb, b)) / ring;
		float side = 37.0 * float(k);        // the other hemisphere's own noise
		float d0 = (th0 - th) / w;           // widths poleward of the oval

		// Slow noise: each strand's wander (one channel each), and where
		// each one is lit (so the arcs break and fade in and out). The
		// wander also depends on d0 itself, so a strand's course can kink,
		// fold back and swirl instead of running as a clean ring.
		// Read through a gentle warp: at these low frequencies the value
		// noise's lattice otherwise shows as a polygonal outline.
		const float SLOW = 1.5, FAST = 30.0;
		float fpS = SLOW * pixelR / ring;
		vec2 wp = gasFbm4Smooth(vec3(cs * 0.8, side + 5.0 + 0.05 * gasAuroraPhase), 2, 0.8 * pixelR / ring).xy - 0.5;
		vec2 cw = cs + wp * 0.4;
		vec4 wander = gasFbm4Smooth(vec3(cw * SLOW, 0.35 * d0 + 0.12 * gasAuroraPhase + side), 3, fpS);
		vec4 lit = gasFbm4Smooth(vec3(cw * SLOW * 1.7, 0.2 * gasAuroraPhase + side + 13.0), 3, fpS * 1.7);
		// Filaments: noise stretched along the strands (slow round the
		// oval, fast across it), so each strand splits into wisps.
		float fpF = max(3.0 * pixelR / ring, 5.0 * pixelR / w);
		vec4 fil = gasFbm4Smooth(vec3(cs * 3.0, d0 * 5.0 + 0.3 * gasAuroraPhase + side + 29.0), 3, fpF);
		// Faint rays across, by angle (they only show near the strands,
		// well before the angle pinches at the pole).
		vec2 dir = normalize(cs + vec2(1e-6, 0.0));
		float ray = gasFbm4(vec3(dir * FAST, gasAuroraPhase + 0.5 * d0 + side), 3, FAST * pixelR / ring).y;
		float rays = mix(0.9, 1.1, smoothstep(0.3, 0.75, ray));

		vec3 mid = displayToLinear(gasAuroraMid), high = displayToLinear(gasAuroraHigh);
		float pxW = pixelR / w;               // one pixel, in widths
		// Strands: thin arcs one behind another toward the pole, each
		// wandering on its own, so they cross, merge and part.
		for (int i = 0; i < 4; i++) {
			float off = 0.7 * float(i) + (wander[i] - 0.5) * 3.2;
			float sw = max(mix(0.15, 0.5, lit[(i + 2) & 3]), 1.5 * pxW);
			float x = (d0 - off) / sw;
			// Feathered poleward.
			float line = x < 0.0 ? exp(-2.5 * x * x) : exp(-0.3 * x * x);
			float on = smoothstep(0.35, 0.7, lit[i]);
			float streak = mix(0.12, 1.3, smoothstep(0.35, 0.85, fil[i]));
			float amt = line * on * streak * (1.0 - 0.18 * float(i));
			vec3 col = mix(mid, high, clamp(0.25 * float(i) + 0.6 * (fil[(i + 1) & 3] - 0.5), 0.0, 1.0));
			E += col * amt * rays;
		}
		// A faint veil tying the strands together, and a fainter one over
		// the cap (on the black night side anything more fills it).
		E += mid * 0.025 * exp(-pow((d0 - 1.4) / 1.6, 2.0)) * (0.4 + lit.x);
		E += mid * 0.01 * smoothstep(2.0, 5.0, d0);
	}
	return E;
}

// How much the aurora shows at body-fixed direction u (Sun direction sunB):
// full on the night side, gasAuroraDay by day, none from inside the shell.
float gasAuroraShow(vec3 u, vec3 sunB)
{
	return mix(1.0, gasAuroraDay, smoothstep(-0.1, 0.2, dot(u, sunB))) * (1.0 - gasInside);
}

// The aurora's light (linear) along the ray q0 + dq s, s in [s0, s1] (body
// axes, radii): a thin emitting layer from the cloud tops up to
// gasAuroraHeight. Its curtains lean poleward with height (gasAuroraLean
// widths at the top), are brightest just above their feet, and colour
// with height: a green hint at the bottom, violet, magenta at the top.
// The density is scaled so a column straight down gives the strands' own
// brightness; seen edge-on the path through it is long and they glow.
vec3 gasAuroraMarch(vec3 q0, vec3 dq, float s0, float s1, vec3 sunB, float pixelR)
{
	// Steps by the path's length through the layer (4 straight down, up to
	// 16 edge-on), jittered per pixel: even steps through the leaning
	// curtains show as stair-stepped slices.
	float H = gasAuroraHeight;
	int N = int(clamp(ceil((s1 - s0) / H * 4.0), 4.0, 16.0));
	float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	vec3 low = displayToLinear(gasAuroraLow), mid = displayToLinear(gasAuroraMid);
	vec3 high = displayToLinear(gasAuroraHigh);
	const vec3 LUM = vec3(0.3, 0.5, 0.2);
	float lowL = max(dot(low, LUM), 1e-3), highL = max(dot(high, LUM), 1e-3);
	float ds = (s1 - s0) / float(N);
	vec3 E = vec3(0.0);
	for (int i = 0; i < N; i++) {
		vec3 q = q0 + dq * (s0 + (float(i) + jit) * ds);
		float r = length(q);
		float hgt = (r - 1.0) / H;
		if (hgt < 0.0 || hgt > 1.0)
			continue;
		// Sharp feet, fading upward (its integral over the height ~0.23).
		float vp = smoothstep(0.0, 0.05, hgt) * exp(-4.0 * hgt) * (1.0 - smoothstep(0.75, 1.0, hgt));
		vec3 u = q / r;
		vec3 S = gasAuroraStrands(u, hgt * gasAuroraLean, pixelR);
		float I = dot(S, LUM);
		S = mix(S, low * (I / lowL), gasAuroraGreen * (1.0 - smoothstep(0.03, 0.2, hgt)));
		S = mix(S, high * (I / highL), 0.6 * smoothstep(0.35, 1.0, hgt));
		E += S * vp * gasAuroraShow(u, sunB);
	}
	E *= ds / (0.23 * H);
	// Edge-on the path is many times the layer's height: compress so it
	// glows instead of clipping.
	float L = dot(E, LUM);
	return E * gasAurora / (1.0 + 0.35 * L * gasAurora);
}

// The aurora on a ray that misses the sphere: ob, db the ray's origin and
// direction in body axes (radii, db unit), passing the centre at h radii
// (its closest approach at distance front). Linear; 0 if it misses the
// layer.
vec3 gasAuroraBeyond(vec3 ob, vec3 db, float h, float front, vec3 sunB, float pixelR)
{
	float R = 1.0 + gasAuroraHeight;
	if (gasAurora <= 0.0 || gasInside >= 1.0 || h >= R)
		return vec3(0.0);
	float half_ = sqrt(R * R - h * h);
	// A ray grazing the sphere (an edge pixel) ends where it meets it.
	float s1 = h < 1.0 ? front - sqrt(1.0 - h * h) : front + half_;
	return gasAuroraMarch(ob, db, max(front - half_, 0.0), s1, sunB, pixelR);
}

// n, d, sunV: unit surface normal, view ray and Sun direction (view axes);
// nb, sunB: the normal and Sun direction in body-fixed axes (pole = +Z);
// db: the view ray in body axes. pixelR: one pixel at the body, radii.
// sunVis: sunlight left after the ring's shadow. hitM: the camera's
// distance to this point (m). Returns the display colour.
vec3 gasGiantSurface(vec3 n, vec3 d, vec3 sunV, vec3 nb, vec3 db, vec3 sunB, float pixelR, float sunVis, float hitM)
{
	// Under the interior's cloud layers, as far as they reach: there the
	// deck is the deep layer below them (darker, no high clouds); beyond,
	// it keeps the look from outside, so the clouds fade into it.
	float deep = gasDeep * (1.0 - smoothstep(0.35 * gasDeepReach, gasDeepReach, hitM));
	float mu = max(dot(n, -d), 0.0);
	float mu0 = dot(n, sunV);
	float foot = pixelR / max(mu, 0.08);       // one pixel on the cloud tops, radii

	GasCloud c = gasCloudField(nb, foot);
	vec3 col = c.col;
	float h = c.h;

	// Sculpted relief: the cloud texture's slope toward the Sun.
	vec3 sunT = sunB - nb * dot(sunB, nb);
	float sunTl = length(sunT);
	float shade = 1.0;
	if (sunTl > 1e-4) {
		vec3 dq = vec3(sunT.xy / c.st, sunT.z);
		float h2 = gasFbm4(c.qd + normalize(dq) * 0.12, gasOctaves, c.fpd).w;
		shade = max(1.0 + gasRelief * (h2 - h) * 5.0 * sunTl, 0.2);
	}

	// The high clouds' shadow: their cover sunward by their height.
	float hi = c.hi, hiShadow = 0.0;
	if (hi > 0.0 || (gasHigh > 0.0 && gasHighCount > 0)) {
		float mu0b = dot(sunB, nb);
		if (sunTl > 1e-4 && mu0b > -0.1) {
			float off = min(gasHighHeight * sunTl / max(mu0b, 0.15), 0.08);
			hiShadow = gasHighCover(normalize(c.ps + sunT / sunTl * off), c.st, c.fp);
		}
	}

	// Lighting, linear. From inside the giant the deck lies under the
	// cloud layers: gasDeep darkens it.
	vec3 alb = displayToLinear(clamp(col, 0.0, 1.0)) * mix(1.0, 0.35, deep);
	vec3 haze = displayToLinear(gasHazeColor);
	float wrapL = clamp((mu0 + gasWrap) / (1.0 + gasWrap), 0.0, 1.0);
	// From inside the haze the darkening toward the horizon is the
	// interior pass's haze, not the limb's.
	float muL = mix(mu, 1.0, gasInside);
	float limb = pow(max(wrapL, 1e-4), gasLimbK - 1.0) * pow(max(muL, 0.02), gasLimbK - 1.0);
	vec3 sun = gasSunlight(mu0, gasWrap) * limb * sunVis;
	vec3 surface = alb * (sun * shade * (1.0 - 0.6 * hiShadow) + gasAmbient * haze);
	// Lightning, under the haze veil: lost in daylight, and from inside
	// the shell the interior's own flashes take over.
	if (gasFlashCount > 0) {
		float night = (1.0 - smoothstep(-0.1, 0.15, mu0)) * (1.0 - gasInside);
		if (night > 0.0)
			surface += gasLightning(nb, c, alb, pixelR) * night;
	}

	// Haze veil: thicker toward the limb, lit with a wider wrap and a
	// forward-scattering rim when the Sun is behind the planet.
	float veil = (1.0 - exp(-gasHazeTau / max(mu, 0.03))) * (1.0 - gasInside);
	vec3 hazeSun = gasSunlight(mu0, 2.0 * gasWrap) * sunVis;
	float phase = mix(1.0, gasPhase(dot(d, sunV)), clamp(0.25 * gasRim, 0.0, 1.0));
	vec3 inScatter = haze * (hazeSun * phase + gasAmbient);
	vec3 lin = mix(surface, inScatter, veil);

	// High clouds: above most of the haze, so a thinner veil and a
	// milder limb darkening; dimmer where more cloud lies sunward (their
	// shaded side). From inside, the volume's top layer replaces them.
	hi *= 1.0 - deep;
	if (hi > 0.0) {
		float kH = mix(1.0, gasLimbK, 0.5);
		float limbH = pow(max(wrapL, 1e-4), kH - 1.0) * pow(max(mu, 0.02), kH - 1.0);
		float selfSh = 1.0 - 0.35 * clamp(hiShadow - hi, 0.0, 1.0);
		vec3 hiAlb = displayToLinear(gasHighColor) * (0.82 + 0.36 * h);
		vec3 hiLit = hiAlb * (gasSunlight(mu0, gasWrap) * limbH * sunVis * selfSh + gasAmbient * haze);
		lin = mix(lin, mix(hiLit, inScatter, veil * 0.4), clamp(hi, 0.0, 1.0));
	}

	// Aurora, above the haze and the high clouds: the layer between the
	// camera and this point, back from it to the layer's top.
	if (gasAurora > 0.0 && gasInside < 1.0) {
		float b = dot(nb, db);
		float R = 1.0 + gasAuroraHeight;
		float sTop = b + sqrt(max(b * b + R * R - 1.0, 0.0));
		lin += gasAuroraMarch(nb, -db, 0.0, sTop, sunB, pixelR);
	}
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
	float a = clamp(dens * gasGlow * bright, 0.0, 1.0) * gasGlowFade;
	vec3 c = displayToLinear(gasHazeColor) * light / max(bright, 1e-3) * min(phase, 2.0);
	return vec4(toneMap(c * gasExposure), a);
}
