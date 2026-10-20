#version 410

// Ring dust (Rings.c): GL_POINTS generated from gl_VertexID, perCell points
// per cell of the same hashed ring-plane grid the rocks use (see rock.vert),
// finer and closer to the camera. Lit per vertex; ringdust.frag only shapes
// the sprite.
uniform mat4 view;
uniform mat4 projection;
uniform vec3 lightPosition;   // camera-relative
uniform vec3 ringCenter;      // camera-relative planet centre
uniform vec3 ringNormal;
uniform vec3 ringU;
uniform vec3 ringV;
uniform vec2 ringRadii;
uniform sampler1D ringProfile;
uniform float planetRadius;
uniform float ringSunAngle;
uniform vec2 camLocal;
uniform float camHeight;
uniform ivec2 baseCell;
uniform int gridN;
uniform int perCell;
uniform float cellSize;
uniform float range;
uniform float halfThickness;
uniform uint seed;
uniform float density;
uniform float projScale;      // pixels per world unit at unit distance
uniform float time;

out vec4 fColor;   // tone-mapped colour, coverage

const float PI = 3.14159265359;

uint pcg(uint v)
{
	uint s = v * 747796405u + 2891336453u;
	uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
	return (w >> 22u) ^ w;
}

float u01(inout uint h)
{
	h = pcg(h);
	return float(h) * (1.0 / 4294967296.0);
}

float hg(float g, float c)
{
	float gg = g * g;
	return (1.0 - gg) / (4.0 * PI * pow(1.0 + gg - 2.0 * g * c, 1.5));
}

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
	int cellId = gl_VertexID / perCell;
	int k = gl_VertexID - cellId * perCell;
	ivec2 cell = baseCell + ivec2(cellId % gridN, cellId / gridN) - ivec2(gridN / 2);
	uint h = pcg(uint(cell.x) * 73856093u ^ pcg(uint(cell.y) * 19349663u ^ (seed + uint(k) * 2654435769u)));

	vec2 local = (vec2(cell) + vec2(u01(h), u01(h))) * cellSize;
	float r01 = (length(local) - ringRadii.x) / (ringRadii.y - ringRadii.x);
	vec4 prof = textureLod(ringProfile, clamp(r01, 0.0, 1.0), 4.0);
	float tau = (r01 > 0.0 && r01 < 1.0) ? prof.a : 0.0;
	float present = step(u01(h), min(tau * density, 1.0));

	float height = (u01(h) + u01(h) - 1.0) * halfThickness;
	// Slow drift so the field is not frozen in place.
	float ph = u01(h) * 6.2831853;
	local += 0.15 * vec2(sin(time * 0.20 + ph), cos(time * 0.17 + ph * 1.3));
	vec3 p = ringU * (local.x - camLocal.x) + ringV * (local.y - camLocal.y)
	       + ringNormal * (height - camHeight);
	float dist = length(p);

	// Sub-pixel grains keep a minimum sprite size and lose coverage by area
	// instead.
	float size = 0.015 + 0.06 * pow(u01(h), 3.0);
	float pix = size * projScale / max(dist, 1e-3);
	float sub = clamp(pix / 1.5, 0.0, 1.0);
	float coverage = (1.0 - smoothstep(range * 0.6, range, dist)) * sub * sub;

	if (present < 0.5 || coverage < 1.0 / 255.0) {
		fColor = vec4(0.0);
		gl_PointSize = 1.0;
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}

	// Grains forward-scatter strongly: looking toward the sun the field
	// lights up. Same slab self-shadowing as rock.frag.
	vec3 V = normalize(-p);
	vec3 L = normalize(lightPosition - p);
	float ln = dot(L, ringNormal);
	float depth = clamp((halfThickness - height * sign(ln)) / (2.0 * halfThickness), 0.0, 1.0);
	float planetSun = planetShadow(p - ringCenter, L);
	float sun = planetSun * exp(-tau * depth * 0.25 / max(abs(ln), 0.05));
	float phase = 4.0 * PI * mix(hg(0.0, dot(-L, V)), hg(0.7, dot(-L, V)), 0.6);
	vec3 sunRadiance = vec3(1.0, 0.97, 0.90) * 1.15;
	// + ringshine, as in rock.frag.
	vec3 lit = prof.rgb * (0.02 + sunRadiance * (0.25 * phase * sun + 0.06 * planetSun * min(tau, 1.0)));

	float exposure = 1.05;
	vec3 x = lit * exposure;
	lit = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
	fColor = vec4(pow(lit, vec3(1.0 / 2.2)), coverage);

	gl_PointSize = clamp(pix, 1.5, 8.0);
	gl_Position = vec4(p, 1.0) * view * projection;
}
