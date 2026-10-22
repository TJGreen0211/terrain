#version 410

// Instanced ring rocks (rings.c). There is no instance buffer: gl_InstanceID
// picks one cell of a gridN x gridN window of ring-plane cells around the
// camera, and the cell's integer coordinates seed a hash for everything
// else (presence, position, height, size, shape, spin). Cells are anchored
// in the ring frame, so a cell grows the same rock every frame however the
// camera moves. Rejected instances are sent outside the clip volume.
layout (location = 0) in vec3 vPosition;
layout (location = 1) in vec3 vNormal;

uniform mat4 view;
uniform mat4 projection;
uniform vec3 ringNormal;
uniform vec3 ringU;
uniform vec3 ringV;
uniform vec2 ringRadii;       // inner, outer (world units)
uniform sampler1D ringProfile;
uniform vec2 camLocal;        // camera in ring-plane coords about the planet centre
uniform float camHeight;      // camera height above the ring plane
uniform ivec2 baseCell;       // floor(camLocal / cellSize), floored in double on the CPU
uniform int gridN;
uniform float cellSize;
uniform float range;          // spawn radius around the camera
uniform float halfThickness;  // ring slab half-thickness
uniform uint seed;            // differs per rock mesh
uniform float density;        // spawn probability per unit optical depth
uniform vec2 sizeRange;       // power-law min / max rock radius
uniform float meshScale;      // obj extents -> unit radius
uniform float time;

out vec3 fragPos;
out vec3 fN;
out vec3 fAlbedo;
out float fHeight;   // height in the slab, for the light's path through the ring
out float fTau;

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

vec3 rotateAxis(vec3 v, vec3 k, float a)
{
	float c = cos(a), s = sin(a);
	return v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
}

void main()
{
	ivec2 cell = baseCell + ivec2(gl_InstanceID % gridN, gl_InstanceID / gridN) - ivec2(gridN / 2);
	uint h = pcg(uint(cell.x) * 73856093u ^ pcg(uint(cell.y) * 19349663u ^ seed));

	vec2 local = (vec2(cell) + vec2(u01(h), u01(h))) * cellSize;
	float r01 = (length(local) - ringRadii.x) / (ringRadii.y - ringRadii.x);
	// Four mips down (~0.3 world units): spawn density follows the bands, not
	// single-texel ringlets.
	vec4 prof = textureLod(ringProfile, clamp(r01, 0.0, 1.0), 4.0);
	float tau = (r01 > 0.0 && r01 < 1.0) ? prof.a : 0.0;
	float present = step(u01(h), min(tau * density, 0.9));

	// Triangular distribution: concentrated toward the midplane.
	float height = (u01(h) + u01(h) - 1.0) * halfThickness;
	vec3 center = ringU * (local.x - camLocal.x) + ringV * (local.y - camLocal.y)
	            + ringNormal * (height - camHeight);

	// Power-law sizes: mostly pebbles, the odd boulder. Rocks shrink to
	// nothing toward the spawn radius instead of popping in.
	float size = min(sizeRange.x * pow(max(1.0 - u01(h), 1e-4), -1.0 / 2.2), sizeRange.y);
	size *= 1.0 - smoothstep(range * 0.7, range, length(center));
	vec3 stretch = vec3(0.6) + 0.4 * vec3(u01(h), u01(h), u01(h));
	vec3 axis = normalize(vec3(u01(h), u01(h), u01(h)) - 0.5 + 1e-4);
	float spin = u01(h) * 6.2831853 + time * (u01(h) - 0.5) * 0.6 / sqrt(size + 0.1);

	fAlbedo = prof.rgb * (0.65 + 0.7 * u01(h));
	fHeight = height;
	fTau = tau;

	if (present < 0.5 || size <= 1e-3) {
		fragPos = vec3(0.0);
		fN = ringNormal;
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}

	vec3 p = rotateAxis(vPosition * stretch, axis, spin) * (size * meshScale);
	fN = rotateAxis(normalize(vNormal / stretch), axis, spin);
	fragPos = center + p;
	gl_Position = vec4(fragPos, 1.0) * view * projection;
}
