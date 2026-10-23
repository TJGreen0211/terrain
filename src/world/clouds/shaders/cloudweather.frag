#version 410

// A planet's weather for the cloud volume (clouds.c, cloud_volume.h): one
// face of a body-fixed cube map, from the planet's weather maps
// (cloudlayer.glsl's equirect field, moved along the wind by the flow map)
// and their detail noise down to two texels (wound round the same
// cyclones), so the systems have fractal, windblown outlines at every
// scale the shapes don't cover.
//
// - r, cover: full in the weather systems (where the field is well above
//   its threshold), and sparse thin patches between them.
// - g, how high the clouds build (0..1 of the layer): the column
//   thickness, highest in the systems' cores, varied by the detail.
// - b, optical thickness (0..1): the same, more strongly.
// - a, wispiness: 1 away from the systems (the shapes erode more there).

const float PI = 3.14159265359;

uniform int face;              // 0..5: +X, -X, +Y, -Y, +Z, -Z
uniform float faceSize;        // texels a side
uniform float texelM;          // m: a texel at the layer
// The detail noise (from the same lattice, flow phases and wind as the
// weather maps).
uniform samplerCube cloudWind;
uniform float windMax;
uniform float detailLambda0;   // m
uniform int detailOctaves;
uniform int cyclonesOn;        // the climate style (cloudmap.frag's cyclones)

out vec4 Weather;

#include "cloudnoise.glsl"
#include "cloudlayer.glsl"

// The direction through texel coordinate st (each in -1..1) of a face, as
// GL's cube map lookup picks it.
vec3 faceDir(int f, vec2 st)
{
	float s = st.x, t = st.y;
	if (f == 0) return vec3(1.0, -t, -s);
	if (f == 1) return vec3(-1.0, -t, s);
	if (f == 2) return vec3(s, 1.0, t);
	if (f == 3) return vec3(s, -1.0, -t);
	if (f == 4) return vec3(s, -t, 1.0);
	return vec3(-s, -t, -1.0);
}

// The detail noise at body-fixed direction d, its octaves fading out below
// foot (m).
float detailNoise(vec3 d, float foot)
{
	vec3 pBody = d * cloudRadius;
	vec3 wind = cloudWindAt(d, textureLod(cloudWind, d, log2(float(textureSize(cloudWind, 0).x)) - 5.0), windMax);
	// Wound round the same cyclones as the maps' noise.
	vec3 c0 = cyclonesOn != 0 ? cloudCyclones(pBody, cloudFlowSeed0) : pBody;
	vec3 c1 = cyclonesOn != 0 ? cloudCyclones(pBody, cloudFlowSeed1) : pBody;
	vec3 p0 = c0 - wind * cloudFlowTau.x + cloudFlowSeed0 * detailLambda0;
	vec3 p1 = c1 - wind * cloudFlowTau.y + cloudFlowSeed1 * detailLambda0;
	float n0 = cloudFbm(p0, detailLambda0, foot, detailOctaves);
	float n1 = cloudFbm(p1, detailLambda0, foot, detailOctaves);
	return clamp(0.5 + (cloudFlowBlend(n0, n1) - 0.5) * 2.0, 0.0, 1.0);
}

void main()
{
	vec3 d = normalize(faceDir(face, gl_FragCoord.xy / faceSize * 2.0 - 1.0));
	vec2 w = cloudWeather(d, texelM / cloudRadius);
	float n = detailNoise(d, 2.0 * texelM);
	// Half the detail's strength: the systems and storms lead.
	float field = w.x - 0.5 + 0.5 * cloudDetail * (n - 0.5);

	// Weather systems only where the field is well above its threshold
	// (fronts, storms, the wet belts); elsewhere sparse, thin patches where
	// the detail peaks, fading out where the air is dry, eroded to wisps.
	float system = smoothstep(0.08, 0.4, field);
	float patches = 0.75 * smoothstep(0.5, 0.8, n) * smoothstep(-0.4, 0.05, field);
	float cover = max(system, patches);
	float build = clamp((0.2 + 0.8 * smoothstep(0.4, 1.0, w.y) * system) * mix(0.6, 1.15, n), 0.0, 1.0);
	// Thin toward a system's edge and in the patches: translucent veils.
	float thick = clamp(w.y * (0.35 + 1.3 * n) * mix(0.3, 1.0, system), 0.0, 1.0);
	Weather = vec4(cover, build, thick, 1.0 - system);
}
