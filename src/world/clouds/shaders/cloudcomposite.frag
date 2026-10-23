#version 410

// The accumulated clouds over the scene (cloud_volume.c), upsampled from
// the low-resolution target: premultiplied, depth-tested against the
// scene at the clouds' distance (nothing written), times the provider's
// hand-off (opacity).

uniform mat4 projection;
uniform vec2 fullSize;
uniform sampler2D cloudColor;   // linear
uniform sampler2D cloudDepth;   // nearest
uniform float opacity;
// Debug view (cloud_volume.c): the march's counts (r steps, g density
// calls) as a heat map, red at debugMax, over everything.
uniform int debugView;
uniform float debugMax;

out vec4 FragColor;

vec3 heat(float x)
{
	const vec3 c0 = vec3(0.0, 0.0, 0.0), c1 = vec3(0.0, 0.2, 1.0), c2 = vec3(0.0, 0.9, 0.3);
	const vec3 c3 = vec3(1.0, 0.9, 0.0), c4 = vec3(1.0, 0.0, 0.0), c5 = vec3(1.0, 1.0, 1.0);
	x = clamp(x, 0.0, 1.25) * 4.0;
	return x < 1.0 ? mix(c0, c1, x) : x < 2.0 ? mix(c1, c2, x - 1.0) : x < 3.0 ? mix(c2, c3, x - 2.0)
	     : x < 4.0 ? mix(c3, c4, x - 3.0) : mix(c4, c5, x - 4.0);
}

void main()
{
	vec2 uv = gl_FragCoord.xy / fullSize;
	if (debugView != 0) {
		vec4 n = texture(cloudColor, uv);
		// (As linear radiance: the tone map bends it, the order stays.)
		FragColor = vec4(heat((debugView == 1 ? n.r : n.g) / debugMax), 1.0);
		gl_FragDepth = 1.0;   // reversed-Z: in front of everything
		return;
	}
	vec4 c = texture(cloudColor, uv) * opacity;
	if (c.a < 0.004)
		discard;
	float dist = texture(cloudDepth, uv).r;
	vec2 ndc = uv * 2.0 - 1.0;
	vec3 d = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
	vec4 cc = vec4(d * dist, 1.0) * projection;
	gl_FragDepth = cc.z / cc.w;
	FragColor = c;
}
