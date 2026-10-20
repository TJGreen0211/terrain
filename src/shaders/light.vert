#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 4) in mat4 vInstanceMatrix;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 lightPosition;
// 1 = body drawn as a quadtree LOD instance (moon): transform the flat
// face-grid vertex by the instance matrix and project it onto the unit
// sphere, then reuse the plain-mesh shading with that direction. The
// equirectangular UVs reproduce the CPU-baked quadCube mapping, so
// moon.jpg frames the sphere identically. 0 = sun billboard / ship paths.
uniform int instanced;

out vec3 fragPos;
out vec3 fE;
out vec3 fN;
out vec3 fL;
out vec2 fTexCoords;

void main()
{
	vec3 p = vPosition.xyz;
	if(instanced == 1) {
		vec3 transformed = (vec4(vPosition.xyz, 1.0)*vInstanceMatrix).xyz;
		p = (length(transformed) > 0.0001) ? normalize(transformed) : vec3(0.0);
	}

	vec4 world = vec4(p, 1.0)*model;
	gl_Position = world*view*projection;

	mat3 normalMatrix = transpose(inverse(mat3(model)));
	vec4 lightPos = vec4(lightPosition, 1.0);
	vec3 lightDir = normalize(world - lightPos).xyz;
	fE = -normalize(world).xyz;
	fN = (instanced == 1) ? normalize(p*normalMatrix) : normalize(vNormal*normalMatrix);
	fL = -normalize(lightDir);

	fragPos = world.xyz;
	fTexCoords = (instanced == 1)
		? vec2((atan(p.y, p.x)/3.1415926 + 1.0)*0.5,
		       (asin(clamp(p.z, -1.0, 1.0))/3.1415926 + 0.5))
		: vTexCoords;
}
