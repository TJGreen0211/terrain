#version 410

layout (location = 0) in vec2 vUV;
out vec2 fUV;

void main()
{
	fUV = vUV;
	gl_Position = vec4(vUV*2.0 - 1.0, 0.0, 1.0);
}
