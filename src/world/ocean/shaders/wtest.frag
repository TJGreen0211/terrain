#version 410

in float fH;

out vec4 FragColor;

uniform float testAmp;

void main()
{
	float v = clamp(0.5 + fH*testAmp, 0.0, 1.0);
	FragColor = vec4(v, 0.15, 1.0 - v, 1.0);
}
