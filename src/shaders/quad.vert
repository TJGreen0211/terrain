#version 410

layout (location = 0) in vec4 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;
layout (location = 3) in vec3 vTangent;
layout (location = 4) in mat4 vInstanceMatrix;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 lightPosition;

out vec4 fLightSpace;
out vec3 fragPos;
out vec3 fE;
out vec3 fN;
out vec3 fL;
out vec2 fTexCoords;
out vec3 vColor;
out vec3 fSurfDir;
out mat3 fNormalMat;

void main()
{
	vec4 fPosition = vec4(vPosition.xyz, 1.0);

	vec3 transformed = (fPosition*vInstanceMatrix).xyz;
	fPosition.xyz = (length(transformed) > 0.0001) ? normalize(transformed) : vec3(0.0);
	//fPosition.xyz = (fPosition*vInstanceMatrix).xyz;

	// Boundary skirt: wall vertices (w = 1) sit just under the undisplaced
	// planet surface (the planet has no vertex displacement) so they hide
	// T-junction seams along LOD patch edges without z-fighting it.
	if(vPosition.w > 0.5)
		fPosition.xyz *= 0.995;

	vec3 n = fPosition.xyz;//normalize(vPosition).xyz;
	fSurfDir = fPosition.xyz;

	// Normal matrix: this pipeline transforms as row vectors (p * model * view
	// * projection) from row-major uploads, so the GLSL matrix is the
	// transposed model. Applying the standard transpose(inverse(M)) here would
	// rotate normals by the planet's INVERSE rotation (day/night terminator
	// drifting off the surface); inverse(M) alone yields the correct world
	// normal under this convention.
	mat3 normalMatrix = inverse(mat3(model));
	fNormalMat = normalMatrix;
	vec4 lightPos = vec4(lightPosition, 1.0);
	vec3 lightDir = normalize(fPosition*model - lightPos).xyz;
	fE = -normalize(fPosition*model).xyz;
	fN = normalize(n*normalMatrix);
	fL = -normalize(lightDir);

	// fPosition already carries the instance transform (normalized above), so
	// applying vInstanceMatrix again would double-transform the shadow lookup.
	fLightSpace = fPosition*model;
	fragPos = vec3(fPosition*model).xyz;
	fTexCoords = (fPosition.xy+1.0)/2.0;//vTexCoords*mat2(vInstanceMatrix);

	gl_Position = fPosition*model*view*projection;

	vColor = vTangent;//vec3(1.0, 0.5, 0.2);
}
