#version 410

// MSAA resolve of the scene target (scene_target.c, linear radiance) into
// the window: each sample goes white balance -> exposure -> curve ->
// grading before averaging (averaging linear samples first would undo the
// anti-aliasing on bright edges), then the result is dithered to 8 bits.
// Alpha (the cloud cover) is resolved too (bench shots save RGBA).
#include "engine/gfx/tonemap.glsl"

uniform sampler2DMS scene;
uniform int samples;
out vec4 FragColor;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// ---- Curves: linear (exposed) in, display-encoded out ----------------------

// Khronos PBR Neutral (Khronos 3D Commerce, 2024): linear in [0,1] out.
vec3 pbrNeutral(vec3 c)
{
	const float startCompression = 0.8 - 0.04;
	const float desaturation = 0.15;
	float x = min(c.r, min(c.g, c.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	c -= offset;
	float peak = max(c.r, max(c.g, c.b));
	if (peak < startCompression)
		return c;
	const float d = 1.0 - startCompression;
	float newPeak = 1.0 - d * d / (peak + d - startCompression);
	c *= newPeak / peak;
	float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
	return mix(c, vec3(newPeak), g);
}

// AgX, minimal polynomial fit (B. Wrensch, 2023). Output is already
// display-encoded (its own power is part of the fit).
vec3 agx(vec3 c)
{
	const mat3 inset = mat3(
		0.842479062253094, 0.0423282422610123, 0.0423756549057051,
		0.0784335999999992, 0.878468636469772, 0.0784336,
		0.0792237451477643, 0.0791661274605434, 0.879142973793104);
	const mat3 outset = mat3(
		1.19687900512017, -0.0528968517574562, -0.0529716355144438,
		-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
		-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
	const float minEv = -12.47393, maxEv = 4.026069;
	c = inset * c;
	c = clamp(log2(max(c, 1e-10)), minEv, maxEv);
	c = (c - minEv) / (maxEv - minEv);
	vec3 c2 = c * c, c4 = c2 * c2;
	c = 15.5 * c4 * c2 - 40.14 * c4 * c + 31.96 * c4 - 6.868 * c2 * c
	  + 0.4298 * c2 + 0.1191 * c - 0.00232;
	return outset * c;
}

// Extended Reinhard per channel: `white` maps to 1.
vec3 reinhard(vec3 c, float white)
{
	return c * (1.0 + c / (white * white)) / (1.0 + c);
}

vec3 resolveCurve(vec3 lin)
{
	int curve = int(toneMapSettings.z + 0.5);
	if (curve == 0)
		return toneMapCurve(lin);   // the ACES fit (exposure inside)
	vec3 x = lin * toneMapExposure();
	vec3 display;
	if (curve == 1)
		display = pow(clamp(pbrNeutral(x), 0.0, 1.0), vec3(1.0 / GAMMA));
	else if (curve == 2)
		display = agx(x);
	else
		display = pow(clamp(reinhard(x, toneMapSettings.w), 0.0, 1.0), vec3(1.0 / GAMMA));
	return clamp(display, 0.0, 1.0);
}

// ---- Grading, in display space after the curve ------------------------------

vec3 grade(vec3 d)
{
	float y = dot(d, LUMA);
	d = mix(vec3(y), d, toneMapGrade.x);                    // saturation
	d = (d - 0.5) * toneMapGrade.y + 0.5;                    // contrast
	d = toneMapGain.rgb * (d + toneMapLift.rgb * (1.0 - d)); // lift, gain
	d = pow(clamp(d, 0.0, 1.0), 1.0 / max(toneMapGamma.rgb, 1e-3));
	return d;
}

vec3 hdrSample(vec3 lin)
{
	return grade(resolveCurve(max(lin, 0.0) * toneMapBalance.rgb));
}

// ---- Debug views -------------------------------------------------------------

// Invalid values: NaN/Inf magenta, within 10% of fp16 max cyan, negative
// yellow; 0 = fine.
vec3 invalidColor(vec3 c)
{
	if (any(isnan(c)) || any(isinf(c)))
		return vec3(1.0, 0.0, 1.0);
	if (max(c.r, max(c.g, c.b)) > 59000.0)
		return vec3(0.0, 1.0, 1.0);
	if (min(c.r, min(c.g, c.b)) < 0.0)
		return vec3(1.0, 1.0, 0.0);
	return vec3(0.0);
}

// Scene luminance (before exposure) in stops around 0.18, as bands:
// < -6 EV black, then blue, cyan, green, yellow, red, > +6 EV white.
vec3 evColor(vec3 lin)
{
	float ev = log2(max(dot(max(lin, 0.0), LUMA), 1e-6) / 0.18);
	const vec3 band[7] = vec3[7](
		vec3(0.0), vec3(0.1, 0.2, 1.0), vec3(0.0, 0.8, 1.0), vec3(0.1, 0.9, 0.2),
		vec3(1.0, 0.9, 0.0), vec3(1.0, 0.2, 0.0), vec3(1.0));
	float t = clamp((ev + 6.0) / 2.0, 0.0, 6.0);
	int i = int(min(t, 5.0));
	return mix(band[i], band[i + 1], fract(t) * step(t, 5.999));
}

// Triangular noise in (-1, 1) from the pixel position (fixed per pixel).
float triangularNoise(vec2 p)
{
	float a = fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
	float b = fract(sin(dot(p, vec2(39.3468, 11.1351))) * 24634.6345);
	return a + b - 1.0;
}

void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	int debugView = int(toneMapGrade.w + 0.5);
	vec4 sum = vec4(0.0);
	vec3 linSum = vec3(0.0);
	vec3 invalid = vec3(0.0);
	for (int i = 0; i < samples; i++) {
		vec4 s = texelFetch(scene, p, i);
		if (debugView == 1)
			invalid = max(invalid, invalidColor(s.rgb));
		linSum += s.rgb;
		s.rgb = hdrSample(s.rgb);
		sum += clamp(s, 0.0, 1.0);
	}
	FragColor = sum / float(samples);

	if (debugView == 1 && invalid != vec3(0.0))
		FragColor.rgb = invalid;
	else if (debugView == 2)
		FragColor.rgb = evColor(linSum / float(samples));

	if (toneMapGrade.z > 0.5)
		FragColor.rgb = clamp(FragColor.rgb + triangularNoise(gl_FragCoord.xy) / 255.0, 0.0, 1.0);
}
