#version 410

// A body drawn as a stand-in (Planets.c): an exact ray-traced sphere with
// an atmosphere glow and a ring sheet, or a soft dot when it is below
// ~1.5 px.
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
uniform vec3 poleView;      // unit, the body's spin axis, view axes
uniform mat3 viewToBody;    // view axes -> body-fixed axes (mottling)
uniform vec3 color;
uniform int banded;         // gas giant latitude bands
uniform float mottle;       // rocky surface variation (0 = none)
uniform float seed;
uniform float atmoThick;    // shell height / radius; 0 = no atmosphere
uniform vec3 atmoColor;     // Rayleigh tint (C_R): the sky's hue
uniform int ringOn;
uniform vec2 ringRadii;     // inner, outer, body radii
uniform vec3 ringNormalView;
uniform vec3 ringColor;
uniform float pixelR;       // one pixel at the body, in body radii
uniform int dotMode;
uniform float dotFlux;      // dot: total light (summed over pixels)
uniform float dotSigma;     // dot: gaussian width, px
// The cloud layer (Clouds_BindStandIn), once the planet is baked: the same
// weather maps the full-detail slab draws, at the map's own resolution.
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

// Cloud cover over body-fixed direction nb seen at cosine mu: alpha and
// reflectance, as clouds.frag shades a slab from above (map coverage with
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
	float reflectance = 1.0 - 1.0 / (1.0 + 0.1125 * tau);   // clouds.frag's two-stream slab
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

// Ring optical density at radius r (body radii): seeded bands, soft edges.
float ringDensity(float r)
{
	float t = (r - ringRadii.x) / (ringRadii.y - ringRadii.x);
	if (t <= 0.0 || t >= 1.0)
		return 0.0;
	float bands = 0.55 + 0.25 * sin(t * 41.0 + seed) + 0.2 * sin(t * 97.0 + seed * 2.3);
	float gap = smoothstep(0.02, 0.05, abs(t - fract(seed * 0.37) * 0.6 - 0.2));
	float edge = smoothstep(0.0, 0.06, t) * smoothstep(1.0, 0.9, t);
	return clamp(bands, 0.0, 1.0) * gap * edge;
}

// Lit ring colour at plane point p (body radii, view axes) and its alpha.
vec4 ringSample(vec3 p)
{
	float dens = ringDensity(length(p));
	if (dens <= 0.0)
		return vec4(0.0);
	float lit = 0.35 + 0.65 * abs(dot(sunView, ringNormalView));
	// The planet's shadow on the ring.
	if (dot(p, sunView) < 0.0 && length(cross(p, sunView)) < 1.0)
		lit *= 0.12;
	return vec4(ringColor * lit, dens * 0.8);
}

void main()
{
	if (dotMode == 1) {
		vec4 cc = vec4(centerView, 1.0) * projection;
		vec2 centerPx = (cc.xy / cc.w * 0.5 + 0.5) * viewportSize;
		vec2 dp = gl_FragCoord.xy - centerPx;
		float s2 = dotSigma * dotSigma;
		float g = exp(-dot(dp, dp) / (2.0 * s2)) / (6.2831853 * s2);
		FragColor = vec4(color * dotFlux * g, 1.0);
		gl_FragDepth = depthOf(centerView);
		return;
	}

	// View ray through this pixel (symmetric perspective).
	vec2 ndc = gl_FragCoord.xy / viewportSize * 2.0 - 1.0;
	vec3 d = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
	vec3 o = camBody;

	vec3 c = cross(o, d);
	float h2 = dot(c, c);
	float h = sqrt(h2);
	float front = -dot(o, d);           // distance to the closest approach
	// Sphere: coverage over about one pixel at the limb.
	float cover = front > 0.0 ? clamp((1.0 - h) / pixelR + 0.5, 0.0, 1.0) : 0.0;
	float tS = front - sqrt(max(1.0 - h2, 0.0));

	vec4 col = vec4(0.0);
	float depthT = -1.0;                // ray distance written to depth (radii)

	if (cover > 0.0) {
		vec3 n = normalize(cross(d, c) - sqrt(max(1.0 - h2, 0.0)) * d);
		vec3 albedo = color;
		if (banded == 1) {
			float lat = dot(n, poleView);
			float band = sin(lat * 22.0 + 1.3 * sin(lat * 7.0) + seed);
			albedo *= mix(0.82, 1.08, 0.5 + 0.5 * band);
			albedo *= 0.65 + 0.35 * max(dot(n, -d), 0.0);   // limb darkening
		}
		if (mottle > 0.0) {
			vec3 nb = viewToBody * n;
			float m = fbm(nb * 3.0);
			albedo *= mottle * (0.65 + 0.7 * m) + (1.0 - mottle);
		}
		float ndl = dot(n, sunView);
		float light = max(ndl, 0.0);
		// Ring shadow on the planet.
		if (ringOn == 1) {
			float dn = dot(sunView, ringNormalView);
			if (abs(dn) > 1e-4) {
				float tq = -dot(n, ringNormalView) / dn;
				if (tq > 0.0)
					light *= 1.0 - 0.75 * ringDensity(length(n + tq * sunView));
			}
		}
		vec3 lit = albedo * (light + 0.02);
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
		col = vec4(lit, cover);
		depthT = tS;
	}
	// Glow beyond the limb, through the shell.
	if (cover < 1.0 && atmoThick > 0.0 && front > 0.0 && h < 1.0 + atmoThick) {
		float x = max(h - 1.0, 0.0) / atmoThick;
		vec3 limbDir = normalize(cross(d, c));
		float day = clamp(dot(limbDir, sunView) + 0.4, 0.0, 1.0);
		float glow = exp(-4.0 * x) * (1.0 - x) * day;
		col.rgb = mix(atmoColor * 0.9, col.rgb, col.a);
		col.a = max(col.a, glow);
		if (depthT < 0.0)
			depthT = front;
	}
	// Ring sheet, in front of the planet or around it.
	if (ringOn == 1) {
		float dn = dot(d, ringNormalView);
		if (abs(dn) > 1e-6) {
			float tR = -dot(o, ringNormalView) / dn;
			if (tR > 0.0 && (cover <= 0.0 || tR < tS)) {
				vec4 rs = ringSample(o + tR * d);
				if (rs.a > 0.0) {
					// "Over", premultiplied, back to straight alpha.
					vec3 pre = rs.rgb * rs.a + col.rgb * col.a * (1.0 - rs.a);
					col.a = rs.a + col.a * (1.0 - rs.a);
					col.rgb = pre / max(col.a, 1e-4);
					if (rs.a > 0.1)
						depthT = tR;
				}
			}
		}
	}

	if (col.a < 0.003)
		discard;
	FragColor = col;
	// Depth of the nearest surface hit (or the closest approach for glow);
	// the camera is the origin, so the hit is just along the ray.
	gl_FragDepth = depthOf(radius * max(depthT, 0.0) * d);
}
