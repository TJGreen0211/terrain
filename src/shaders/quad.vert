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

void main()
{
	vec4 fPosition = vPosition;

	fPosition.xyz = normalize((fPosition*vInstanceMatrix).xyz);
	//fPosition.xyz = (fPosition*vInstanceMatrix).xyz;

	vec3 n = fPosition.xyz;//normalize(vPosition).xyz;

	mat3 normalMatrix = transpose(inverse(mat3(model)));
	vec4 lightPos = vec4(lightPosition, 1.0);
	vec3 lightDir = normalize(fPosition*model - lightPos).xyz;
	fE = -normalize(fPosition*model).xyz;
	fN = normalize(n*normalMatrix);
	fL = -normalize(lightDir);

	fLightSpace = fPosition*model*vInstanceMatrix;
	fragPos = vec3(fPosition*model).xyz;
	fTexCoords = (fPosition.xy+1.0)/2.0;//vTexCoords*mat2(vInstanceMatrix);

	gl_Position = fPosition*model*view*projection;

	vColor = vTangent;//vec3(1.0, 0.5, 0.2);
}
