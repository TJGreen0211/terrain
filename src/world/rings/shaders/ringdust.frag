#version 410

in vec4 fColor;

out vec4 FragColor;

// Soft round sprite, premultiplied alpha.
void main()
{
	vec2 c = gl_PointCoord * 2.0 - 1.0;
	float a = fColor.a * (1.0 - smoothstep(0.25, 1.0, dot(c, c)));
	if (a < 1.0 / 255.0)
		discard;
	FragColor = vec4(fColor.rgb * a, a);
}
