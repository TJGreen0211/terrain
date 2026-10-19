#version 410

layout (location = 0) in vec3 vPosition;
layout (location = 1) in vec3 vNormal;
layout (location = 2) in vec2 vTexCoords;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform sampler2D waveHeight;
uniform float testAmp;

out float fH;

void main()
{
	vec3 p = vPosition;
	float h = texture(waveHeight, vTexCoords).r;
	p.z += h * testAmp;
	fH = h;
	gl_Position = vec4(p, 1.0)*model*view*projection;
}
