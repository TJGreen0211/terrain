#version 410

#include "engine/gfx/tonemap.glsl"

in vec3 TexCoords;
out vec4 FragColor;

uniform samplerCube skybox;
uniform float hdrGain;   // Skybox_HdrGain

void main()
{
	// The cube holds display values (baked through the curve).
	vec4 c = texture(skybox, TexCoords);
	FragColor = vec4(sceneFromDisplay(c.rgb) * hdrGain, c.a);
}
