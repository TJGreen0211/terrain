#version 410

// The sky bake's glow pass (plan Phase 1b step 5; Nebula_BakeSky), one
// cubemap face per draw: the HDR bake (nebula_sky.frag, linear radiance)
// plus bloom, then the scene's tone map and a contrast curve, into the
// display-valued sky the skybox draws.
// - Bloom: a weighted sum of the HDR cube's mips (seamless cube filtering
//   on, so the blur crosses face edges). Bright gas and the light cores
//   bleed over the dust in front of them: light pouring through the gaps.
// - Contrast: an S-curve on the display value (blacks stay black, the lit
//   gas stays bright).

#include "tonemap.glsl"

out vec4 FragColor;

uniform samplerCube hdr;
uniform int face;
uniform float faceSize;
uniform int axisDebug;       // pass the orientation test through untouched
uniform float bloomStrength;
uniform int bloomLevels;     // mips summed, from level 2
uniform float exposure;
uniform float contrast;      // 1 = none

// The GL cube-map face basis, as nebula_sky.frag.
vec3 faceDir(vec2 st)
{
	float s = st.x, t = st.y;
	if (face == 0) return vec3( 1.0, -t, -s);
	if (face == 1) return vec3(-1.0, -t,  s);
	if (face == 2) return vec3( s,  1.0,  t);
	if (face == 3) return vec3( s, -1.0, -t);
	if (face == 4) return vec3( s, -t,  1.0);
	return vec3(-s, -t, -1.0);
}

void main()
{
	vec2 st = gl_FragCoord.xy / faceSize * 2.0 - 1.0;
	vec3 d = normalize(faceDir(st));
	vec3 L = textureLod(hdr, d, 0.0).rgb;
	if (axisDebug != 0) {
		FragColor = vec4(L, 1.0);
		return;
	}
	// Wider levels weigh a little less: a tight glow with a long tail.
	vec3 glow = vec3(0.0);
	float wsum = 0.0, w = 1.0;
	for (int k = 0; k < bloomLevels; k++) {
		glow += w * textureLod(hdr, d, float(k + 2)).rgb;
		wsum += w;
		w *= 0.8;
	}
	if (wsum > 0.0)
		L += bloomStrength * glow / wsum;
	vec3 x = toneMap(L * exposure);
	vec3 xc = pow(x, vec3(contrast));
	FragColor = vec4(xc / (xc + pow(1.0 - x, vec3(contrast)) + 1e-6), 1.0);
}
