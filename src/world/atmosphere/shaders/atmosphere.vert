#version 410

in vec4 vPosition;
in vec3 vNormal;

out vec3 fLocalPos;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{	
	// Planet-local vertex position (dome centred on the origin, radius
	// fOuterRadius). The planet centre is the matrix translation column:
	// model[0][3]..model[2][3] in this codebase's row-vector convention
	// (model[3][0..2] is always (0,0,0) -- subtracting it is a silent no-op).
	vec4 worldPos = vPosition * model;
	fLocalPos = vec3(worldPos) - vec3(model[0][3], model[1][3], model[2][3]);
	gl_Position = vPosition*model*view*projection;
}
