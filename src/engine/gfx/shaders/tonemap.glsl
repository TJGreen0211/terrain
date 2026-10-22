// The scene's tone map, shared by every lit shader. The scene target
// (RGBA16F) holds linear radiance; the resolve (resolve.frag) applies the
// curve, grading and dither per sample. Settings come from one uniform block
// (engine/gfx/tone_map.c). See docs/hdr.md.
// A shader that bakes display values (the nebula sky) defines
// TONEMAP_DISPLAY before including this: toneMap() then applies the ACES
// fit at the default exposure, whatever the runtime settings.
// Layout: tone_map.c ToneMap_Update. Only the resolve reads past the first.
layout(std140) uniform ToneMapBlock {
	vec4 toneMapSettings;   // x exposure, y unused, z curve, w Reinhard white
	vec4 toneMapGrade;      // x saturation, y contrast, z dither, w debug view
	vec4 toneMapBalance;    // rgb white balance scales (linear)
	vec4 toneMapLift;       // rgb
	vec4 toneMapGamma;      // rgb
	vec4 toneMapGain;       // rgb
};

const float GAMMA = 2.2;

float toneMapExposure()
{
#ifdef TONEMAP_DISPLAY
	return 1.05;
#else
	return toneMapSettings.x;
#endif
}

// The ACES fit (Narkowicz) with exposure and gamma: linear radiance ->
// display value.
vec3 toneMapCurve(vec3 c)
{
	vec3 x = c * toneMapExposure();
	x = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
	return pow(x, vec3(1.0 / GAMMA));
}

// What a lit shader writes to the scene target: linear radiance (curved at
// the resolve), or a display value in a TONEMAP_DISPLAY bake.
vec3 toneMap(vec3 c)
{
#ifdef TONEMAP_DISPLAY
	return toneMapCurve(c);
#else
	return max(c, 0.0);
#endif
}

// Inverse of the ACES fit (before exposure), for y = curve output in [0,1].
// Finite at y = 1 (x ~ 7.2).
vec3 acesInverse(vec3 y)
{
	vec3 a = 2.51 - 2.43 * y;
	vec3 b = 0.03 - 0.59 * y;
	return (-b + sqrt(b * b + 4.0 * a * 0.14 * y)) / (2.0 * a);
}

// Inverse of toneMapCurve, for display-encoded colours lit in linear (the
// gas giants' albedo profile, water's reflection of the display sky).
vec3 displayToLinear(vec3 d)
{
	return acesInverse(pow(clamp(d, 0.0, 0.95), vec3(GAMMA))) / toneMapExposure();
}

// A display value, from a pass still authored in display values (the
// skybox, the dome, the sun, rocky stand-ins, ring dust), as linear
// radiance: what the ACES fit at the default exposure maps back onto it,
// so over black it looks as it was authored, and the exposure slider
// scales it like everything else.
vec3 sceneFromDisplay(vec3 d)
{
	return acesInverse(pow(clamp(d, 0.0, 1.0), vec3(GAMMA))) / 1.05;
}
