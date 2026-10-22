#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
// Quadtree patch instance (locations 4-11) + patchDir/patchSphere.
#include "world/terrain/patch.glsl"

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 lightPosition;
// 1 = body drawn as a quadtree LOD instance (moon): the patch vertex is
// placed on the unit sphere exactly like the planet (patch.glsl), then
// reuses the plain-mesh shading with that direction. The equirectangular
// UVs reproduce the CPU-baked quadCube mapping, so moon.jpg frames the
// sphere identically. 0 = sun billboard / ship paths.
uniform int instanced;

out vec3 fragPos;
out vec3 fE;
out vec3 fN;
out vec3 fL;
out vec2 fTexCoords;

void main()
{
	vec3 p = vPosition.xyz;
	vec3 world;
	if(instanced == 1) {
		vec2 uv = vPosition.xy;
		p = patchDir(uv);
		world = patchSphere(uv, p, model);
	}
	else {
		world = (vec4(p, 1.0)*model).xyz;
	}
	gl_Position = vec4(world, 1.0)*view*projection;

	mat3 normalMatrix = transpose(inverse(mat3(model)));
	vec3 lightDir = normalize(world - lightPosition);
	fE = -normalize(world);
	fN = (instanced == 1) ? normalize(p*normalMatrix) : normalize(vNormal*normalMatrix);
	fL = -lightDir;

	fragPos = world;
	fTexCoords = (instanced == 1)
		? vec2((atan(p.y, p.x)/3.1415926 + 1.0)*0.5,
		       (asin(clamp(p.z, -1.0, 1.0))/3.1415926 + 0.5))
		: vTexCoords;
}
