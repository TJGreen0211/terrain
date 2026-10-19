#version 410

in vec4 vPosition;

out vec3 fLocalPos;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{	
	fLocalPos = vec3(vPosition * model) - vec3(model[3][0], model[3][1], model[3][2]);
	gl_Position = vPosition*model*view*projection;
}
