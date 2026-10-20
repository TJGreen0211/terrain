#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat4 lightSpace;
// Geometric relief (see quad.vert): the planet's shadow caster must be
// displaced by the same height field or the terminator shadows drift off
// the terrain. The shadow pass uses the fixed 30-division grid, whose
// ~1/30 rad cells correspond to lod ~4, so a fixed lod is correct here.
uniform sampler2D heightMap;
uniform float seaLevel;
uniform float landRelief;
uniform float oceanRelief;
uniform float oceanDepth;
uniform float displace;   // 1 for the planet grid, 0 for moon/ring

out vec4 fragPos;

const float PI = 3.14159265359;

void main()
{	
	vec3 p = vPosition.xyz;
	if (displace > 0.5) {
		vec2 uv = vec2((atan(p.y, p.x)/PI + 1.0)*0.5,
		               (asin(clamp(p.z, -1.0, 1.0))/PI + 0.5));
		float h = textureLod(heightMap, uv, 4.0).r;
		float s = clamp((seaLevel - h) / max(oceanDepth, 1e-4), 0.0, 1.0);
		float relief = landRelief * max(h - seaLevel, 0.0) / max(1.0 - seaLevel, 1e-3)
		             - oceanRelief * s * s;
		p *= 1.0 + relief;
	}
	gl_Position = vec4(p, 1.0)*model*lightSpace;
	fragPos = vec4(p, 1.0)*model;
}
