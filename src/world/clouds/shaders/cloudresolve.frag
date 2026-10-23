#version 410

// Accumulates the cloud march over frames (cloud_volume.c): this frame's
// sample, blended with the history reprojected through the body's frame
// (camera motion and turning both), clamped to this frame's neighbourhood
// so moving edges don't ghost. Low resolution, in and out.

uniform mat4 projection;
uniform vec2 lowSize;
uniform mat3 viewToBody;       // this frame
uniform mat3 bodyToPrevView;   // last frame
uniform vec3 camDelta;         // camera now - camera then, body axes, m
uniform float historyWeight;   // 0: no usable history

uniform sampler2D curColor;
uniform sampler2D curDepth;
uniform sampler2D histColor;   // linear
uniform sampler2D histDepth;

layout(location = 0) out vec4 OutColor;
layout(location = 1) out float OutDepth;

const float NONE = 1e9;

void main()
{
	ivec2 px = ivec2(gl_FragCoord.xy);
	ivec2 hi = ivec2(lowSize) - 1;
	vec4 c = texelFetch(curColor, px, 0);
	float dc = texelFetch(curDepth, px, 0).r;
	if (historyWeight <= 0.0) {
		OutColor = c;
		OutDepth = dc;
		return;
	}

	// This frame's neighbourhood: the history may not leave its range.
	vec4 mn = c, mx = c;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++) {
			vec4 n = texelFetch(curColor, clamp(px + ivec2(x, y), ivec2(0), hi), 0);
			mn = min(mn, n);
			mx = max(mx, n);
		}

	// Where this pixel's clouds (or, without any, a far point) were on
	// screen last frame.
	vec2 ndc = gl_FragCoord.xy / lowSize * 2.0 - 1.0;
	vec3 d = normalize(vec3(ndc.x / projection[0][0], ndc.y / projection[1][1], -1.0));
	float dist = min(dc, 1.0e7);
	vec3 pv = bodyToPrevView * (viewToBody * (d * dist) + camDelta);
	vec4 clip = vec4(pv, 1.0) * projection;
	vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
	if (pv.z >= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		OutColor = c;
		OutDepth = dc;
		return;
	}
	vec4 h = clamp(texture(histColor, uv), mn, mx);
	OutColor = mix(c, h, historyWeight);
	OutDepth = dc < NONE ? dc : texture(histDepth, uv).r;
}
