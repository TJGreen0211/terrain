#version 410

// The accumulated clouds over the scene (cloud_volume.c), upsampled from
// the low-resolution target: premultiplied, depth-tested against the
// scene at the clouds' distance (nothing written).

uniform mat4 projection;
uniform vec2 fullSize;
uniform sampler2D cloudColor;   // linear
uniform sampler2D cloudDepth;   // nearest

out vec4 FragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy / fullSize;
	vec4 c = texture(cloudColor, uv);
	if (c.a < 0.004)
		discard;
	float dist = texture(cloudDepth, uv).r;
	vec2 ndc = uv * 2.0 - 1.0;
	vec3 d = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
	vec4 cc = vec4(d * dist, 1.0) * projection;
	gl_FragDepth = cc.z / cc.w;
	FragColor = c;
}
