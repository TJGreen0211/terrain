#version 410

// A body drawn as a stand-in (planets.c): an exact ray-traced sphere with
// an atmosphere glow and a ring sheet, or a soft dot when it is below
// ~1.5 px. Gas giants are shaded by gasgiant.glsl (bands, storms, haze). The rings are the full passes' (rings.c): the body's baked
// profile, shaded by the same slab model (ringshade.glsl), shadowing the
// sphere through the profile's optical depth.
//
// Ray-sphere in body radii relative to the body: o = camera, d = unit ray.
// At thousands of radii the textbook quadratic (b^2 - (|o|^2 - 1)) cancels
// catastrophically in float; the miss distance |o x d| and the hit point
// d x (o x d) - sqrt(1 - h^2) d keep full relative precision.

uniform mat4 projection;
uniform vec2 viewportSize;
uniform vec3 centerView;    // body centre, view space (m)
uniform float radius;       // m
uniform vec3 camBody;       // camera relative to the body, view axes, body radii
uniform vec3 sunView;       // unit, body -> Sun, view axes
uniform mat3 viewToBody;    // view axes -> body-fixed axes (mottling)
uniform vec3 color;
uniform float mottle;       // rocky surface variation (0 = none)
uniform float seed;
uniform float atmoThick;    // shell height / radius; 0 = no atmosphere
uniform vec3 atmoColor;     // Rayleigh tint (C_R): the sky's hue
uniform int ringOn;
uniform int ringSheet;      // 0: the full ring passes draw this body's sheet
uniform vec2 ringRadii;     // inner, outer, body radii
uniform vec3 ringNormalView;
uniform sampler1D ringProfile;   // Rings_BindStandIn
uniform float ringSunAngle;
uniform float pixelR;       // one pixel at the body, in body radii
uniform int dotMode;
// 0: everything; 1: all but the glow beyond the limb; 2: that glow alone.
// The body whose rings the full passes draw splits in two around them
// (planets.c), so its glow lies over the sheet behind it.
uniform int glowPass;
uniform float dotFlux;      // dot: total light (summed over pixels)
uniform float dotSigma;     // dot: gaussian width, px
// The cloud layer (Clouds_BindStandIn), once the planet is baked: the same
// weather maps the full-detail clouds are made from, at the map's own resolution.
uniform float cloudsOn;
uniform sampler2D cloudMapA;
uniform sampler2D cloudMapB;
uniform float cloudMapBlend;
uniform float cloudMapMaxLod;
uniform float cloudTexelAngle;
uniform float cloudOpacity;
uniform float cloudDetail;
uniform float cloudEdge;
uniform vec3 cloudColor;

out vec4 FragColor;

#include "engine/gfx/tonemap.glsl"
uniform float hdrGain;   // Planets_HdrGain

// A display-authored colour as linear radiance (tonemap.glsl), scaled.
vec3 fromDisplay(vec3 d)
{
	return sceneFromDisplay(d) * hdrGain;
}
#include "world/rings/ringshade.glsl"
#include "world/gasgiant/gasgiant.glsl"

// Cloud cover over body-fixed direction nb seen at cosine mu: alpha and
// reflectance of a plane-parallel cloud seen from above (map coverage with
// the detail at its mean: it is far below a pixel here).
vec2 standInCloud(vec3 nb, float mu)
{
	float z = clamp(nb.z, -1.0, 1.0);
	float cosLat = max(sqrt(max(1.0 - z * z, 1e-4)), 0.02);
	float lod = clamp(log2(max(pixelR / max(mu, 0.2) / cloudTexelAngle, 1e-4) / cosLat), 0.0, cloudMapMaxLod);
	vec2 uv = vec2((atan(nb.y, nb.x) / 3.14159265 + 1.0) * 0.5, asin(z) / 3.14159265 + 0.5);
	vec2 w = mix(textureLod(cloudMapA, uv, lod).rg, textureLod(cloudMapB, uv, lod).rg, cloudMapBlend);
	float cov = smoothstep(-cloudEdge, cloudEdge, w.x - 0.5);
	float tau = cloudOpacity * w.y;
	float reflectance = 1.0 - 1.0 / (1.0 + 0.1125 * tau);   // two-stream (cloudlayer.glsl's cloudDiffuseT)
	return vec2(cov * (1.0 - exp(-tau / max(mu, 0.1))), reflectance);
}

float depthOf(vec3 p)
{
	vec4 c = vec4(p, 1.0) * projection;
	return c.z / c.w;
}

float hash(vec3 p)
{
	p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3) + seed * 0.0123);
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float vnoise(vec3 p)
{
	vec3 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(hash(i), hash(i + vec3(1, 0, 0)), f.x),
	               mix(hash(i + vec3(0, 1, 0)), hash(i + vec3(1, 1, 0)), f.x), f.y),
	           mix(mix(hash(i + vec3(0, 0, 1)), hash(i + vec3(1, 0, 1)), f.x),
	               mix(hash(i + vec3(0, 1, 1)), hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

float fbm(vec3 p)
{
	float s = 0.0, a = 0.5;
	for (int k = 0; k < 5; k++) {
		s += a * vnoise(p);
		p = p * 2.03 + 1.7;
		a *= 0.5;
	}
	return s;
}

// Radius r (body radii) -> profile coordinate.
float ringR01(float r)
{
	return (r - ringRadii.x) / (ringRadii.y - ringRadii.x);
}

// Sunlight through the ring onto sphere point n (as ringshadow.glsl): the
// optical depth where the sun ray crosses the plane, blurred over the sun's
// footprint there or the pixel, whichever is wider.
float ringShadowOnSphere(vec3 n)
{
	float dn = dot(sunView, ringNormalView);
	if (abs(dn) < 1e-4)
		return 1.0;
	float tq = -dot(n, ringNormalView) / dn;
	if (tq <= 0.0)
		return 1.0;
	float q01 = ringR01(length(n + tq * sunView));
	if (q01 <= 0.0 || q01 >= 1.0)
		return 1.0;
	float texelW = (ringRadii.y - ringRadii.x) / float(textureSize(ringProfile, 0));
	float tau = textureLod(ringProfile, q01, log2(max(max(tq * ringSunAngle, pixelR) / texelW, 1.0))).a;
	return exp(-tau / abs(dn));
}

void main()
{
	if (dotMode == 1) {
		vec4 cc = vec4(centerView, 1.0) * projection;
		vec2 centerPx = (cc.xy / cc.w * 0.5 + 0.5) * viewportSize;
		vec2 dp = gl_FragCoord.xy - centerPx;
		float s2 = dotSigma * dotSigma;
		float g = exp(-dot(dp, dp) / (2.0 * s2)) / (6.2831853 * s2);
		FragColor = vec4(fromDisplay(color * dotFlux * g), 1.0);
		gl_FragDepth = depthOf(centerView);
		return;
	}

	// View ray through this pixel (symmetric perspective).
	vec2 ndc = gl_FragCoord.xy / viewportSize * 2.0 - 1.0;
	vec3 d = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
	vec3 o = camBody;

	// Ring-plane hit, ahead of any branch so the profile's mip (its
	// derivatives) is defined in every pixel.
	float ringDn = dot(d, ringNormalView);
	float tR = -dot(o, ringNormalView) / (abs(ringDn) > 1e-6 ? ringDn : 1e-6);
	vec3 pR = o + tR * d;
	float r01 = ringR01(length(pR));
	float r01dx = dFdx(r01), r01dy = dFdy(r01);

	vec3 c = cross(o, d);
	float h2 = dot(c, c);
	float h = sqrt(h2);
	float front = -dot(o, d);           // distance to the closest approach
	// Sphere: coverage over about one pixel at the limb, measured where
	// the ray passes it (pixelR is a pixel at the centre's distance: from
	// close up, far wider than the limb's).
	float pixLimb = pixelR * max(front, 0.0) / length(o);
	float cover = front > 0.0 ? clamp((1.0 - h) / max(pixLimb, 1e-9) + 0.5, 0.0, 1.0) : 0.0;
	float tS = front - sqrt(max(1.0 - h2, 0.0));

	vec4 col = vec4(0.0);
	float depthT = -1.0;                // ray distance written to depth (radii)

	if (cover > 0.0) {
		vec3 n = normalize(cross(d, c) - sqrt(max(1.0 - h2, 0.0)) * d);
		float ndl = dot(n, sunView);
		// Ring shadow on the planet.
		float sunVis = ringOn == 1 ? ringShadowOnSphere(n) : 1.0;
		vec3 lit;
		if (gasOn == 1) {
			// One pixel at the hit point, not at the centre's distance
			// (they differ a lot up close).
			float pixHit = pixelR * tS / length(o);
			lit = gasGiantSurface(n, d, sunView, viewToBody * n, viewToBody * d, viewToBody * sunView, pixHit, sunVis, radius * tS);
		} else {
			vec3 albedo = color;
			if (mottle > 0.0) {
				vec3 nb = viewToBody * n;
				float m = fbm(nb * 3.0);
				albedo *= mottle * (0.65 + 0.7 * m) + (1.0 - mottle);
			}
			float light = max(ndl, 0.0) * sunVis;
			lit = albedo * (light + 0.02);
			if (cloudsOn > 0.5) {
				vec2 cl = standInCloud(viewToBody * n, max(dot(n, -d), 0.0));
				lit = mix(lit, cloudColor * cl.y * (light + 0.02), cl.x);
			}
			// Air at the limb: a tinted rim on the day side.
			if (atmoThick > 0.0) {
				float mu = max(dot(n, -d), 0.0);
				float day = clamp(ndl + 0.3, 0.0, 1.0);
				lit += atmoColor * pow(1.0 - mu, 3.0) * 0.8 * day;
			}
			lit = fromDisplay(lit);
		}
		col = vec4(lit, cover);
		depthT = tS;
	}
	// Glow beyond the limb, through the shell (straight alpha).
	vec4 glow = vec4(0.0);
	if (glowPass != 1 && cover < 1.0 && atmoThick > 0.0 && front > 0.0 && h < 1.0 + atmoThick) {
		float x = max(h - 1.0, 0.0) / atmoThick;
		vec3 limbDir = normalize(cross(d, c));
		if (gasOn == 1) {
			glow = gasGiantGlow(x, limbDir, d, sunView);
		} else {
			float day = clamp(dot(limbDir, sunView) + 0.4, 0.0, 1.0);
			glow = vec4(fromDisplay(atmoColor * 0.9), exp(-4.0 * x) * (1.0 - x) * day);
		}
	}
	// A gas giant's aurora curtains beyond the limb join the glow. They are
	// emission, which straight alpha can't simply add: they take a little
	// alpha (dimming what's behind them slightly) and carry their light
	// over it.
	if (glowPass != 1 && gasOn == 1 && cover < 1.0 && front > 0.0) {
		vec3 A = gasAuroraBeyond(viewToBody * o, viewToBody * d, h, front, viewToBody * sunView,
		                         pixelR * front / length(o));
		float aL = dot(A, vec3(0.3, 0.5, 0.2));
		if (aL > 1e-5) {
			float aA = 1.0 - exp(-0.25 * aL);
			vec3 pre = glow.rgb * glow.a + A;
			glow.a = glow.a + (1.0 - glow.a) * aA;
			glow.rgb = pre / glow.a;
		}
	}
	if (glowPass == 2) {
		// Over the scene so far, rings included: under the sphere's own
		// coverage and under the sheet where it crosses in front.
		float a = glow.a * (1.0 - cover);
		if (ringOn == 1 && abs(ringDn) > 1e-6 && tR > 0.0 && tR < front && r01 > 0.0 && r01 < 1.0)
			a *= exp(-textureGrad(ringProfile, r01, r01dx, r01dy).a / abs(ringDn));
		if (a < 0.003)
			discard;
		FragColor = vec4(max(glow.rgb, 0.0), clamp(a, 0.0, 1.0));
		gl_FragDepth = depthOf(radius * (cover > 0.0 ? tS : front) * d);
		return;
	}
	if (glow.a > 0.0) {
		col.rgb = mix(glow.rgb, col.rgb, col.a);
		col.a = max(col.a, glow.a);
		if (depthT < 0.0)
			depthT = front;
	}
	// Ring sheet, in front of the planet or around it: ring.frag's
	// shading, tone mapped as it is.
	if (ringOn == 1 && ringSheet == 1 && abs(ringDn) > 1e-6 && tR > 0.0 &&
	    (cover <= 0.0 || tR < tS) && r01 > 0.0 && r01 < 1.0) {
		vec4 prof = textureGrad(ringProfile, r01, r01dx, r01dy);
		if (prof.a > 1e-4) {
			vec4 ring = ringShade(prof, -d, sunView, ringNormalView,
			                      ringPlanetShadow(pR, sunView, 1.0, ringSunAngle));
			vec3 rc = toneMap(ring.rgb);
			// "Over", premultiplied, back to straight alpha; behind the
			// glow (only off the sphere) the glow goes over the ring.
			vec3 pre;
			if (cover <= 0.0 && tR > front) {
				pre = col.rgb * col.a + rc * ring.a * (1.0 - col.a);
			} else {
				pre = rc * ring.a + col.rgb * col.a * (1.0 - ring.a);
			}
			col.a = ring.a + col.a * (1.0 - ring.a);
			col.rgb = pre / max(col.a, 1e-4);
			if (ring.a > 0.1)
				depthT = tR;
		}
	}

	if (col.a < 0.003)
		discard;
	FragColor = vec4(max(col.rgb, 0.0), clamp(col.a, 0.0, 1.0));
	// Depth of the nearest surface hit (or the closest approach for glow);
	// the camera is the origin, so the hit is just along the ray.
	gl_FragDepth = depthOf(radius * max(depthT, 0.0) * d);
}
