#version 410

in vec4 vPosition;
in vec3 vNormal;

out vec4 fPosition;
out vec3 fLocalPos;
out mat4 m;
out mat4 v;
out vec3 fNormal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{	
	fPosition = vPosition;
	fNormal = vNormal;
	m = model;
	v = view;
	fLocalPos = vec3(vPosition * model) - vec3(model[3][0], model[3][1], model[3][2]);
	gl_Position = vPosition*model*view*projection;
}
