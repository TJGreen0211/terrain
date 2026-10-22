#version 410

// Builds one weather map (clouds.c) for a planet at one simulation time:
// R = coverage field (signed, + 0.5), G = column thickness, in an equirect layout
// over the body-fixed frame (pole = +Z). The climate and wind maps it reads
// are cube maps, looked up by direction (planetmap.glsl).
//
// Coverage is the style's coverage potential (how cloudy this place tends to
// be) thresholded against domain-warped FBM (Schneider and Vos 2015's
// weather-map idea, made planet-wide). The noise is evaluated in 3D on the
// sphere, so there is no seam and no pinching at the poles, and moved along
// the planet's wind map by the two-phase flow map (cloudnoise.glsl).

const float PI = 3.14159265359;

out vec4 FragColor;

uniform vec2 mapSize;
uniform samplerCube climateMap; // R = moisture (terrain_gen.c)
uniform samplerCube windMap;    // the ocean map: wind speed and direction
uniform float windMax;
uniform float planetRadius;     // m
uniform float texelM;           // map texel at the equator, m

uniform int style;              // CloudStyle (clouds.h)
uniform float coverage;
uniform float climateWeight;
uniform float weatherScale;     // planet radii
uniform float bands;

// Gradient noise: the map thresholds the noise's tails (cloudnoise.glsl).
#define CLOUD_NOISE cloudGradientNoise
#include "cloudnoise.glsl"

#define STYLE_CLIMATE  1
#define STYLE_PATCHY   2
#define STYLE_BANDED   3
#define STYLE_OVERCAST 4

// Earth-like cloud belts: the wet ITCZ, the dry subtropical highs, the
// mid-latitude storm tracks, thinning toward the poles.
float latitudeBelts(float absLatDeg)
{
	float itcz    = exp(-pow(absLatDeg / 10.0, 2.0));
	float subtrop = exp(-pow((absLatDeg - 25.0) / 9.0, 2.0));
	float storm   = exp(-pow((absLatDeg - 55.0) / 13.0, 2.0));
	float polar   = smoothstep(70.0, 90.0, absLatDeg);
	return 0.8 + 0.5 * itcz - 0.6 * subtrop + 0.45 * storm - 0.3 * polar;
}

// Domain-warped FBM at planet-local p (m) for one flow phase.
float shapeNoise(vec3 p, vec3 wind, float tau, vec3 seed, float lambda0, int octaves)
{
	vec3 q = p - wind * tau + seed * lambda0;
	// Warp by low-frequency noise: swirls and fronts instead of round blobs.
	vec3 wq = q / (lambda0 * 0.7);
	vec3 warp = vec3(CLOUD_NOISE(wq + vec3(1.7, 9.2, 3.4)),
	                 CLOUD_NOISE(wq + vec3(8.3, 2.8, 7.1)),
	                 CLOUD_NOISE(wq + vec3(4.6, 6.5, 0.9))) - 0.5;
	q += warp * lambda0 * 0.9;
	return cloudFbm(q, lambda0, 2.0 * texelM, octaves);
}

void main()
{
	vec2 uv = gl_FragCoord.xy / mapSize;
	float lon = (uv.x * 2.0 - 1.0) * PI;   // long/lat of this weather-map texel
	float lat = (uv.y - 0.5) * PI;
	vec3 d = vec3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
	vec3 p = d * planetRadius;
	float absLatDeg = degrees(abs(lat));

	// Coverage potential: the sky fraction this place tends toward.
	float pot = coverage;
	if (style == STYLE_CLIMATE) {
		// Broad moisture (the climate map's R holds half-scale rain), from a
		// coarse mip (8 texels per face, ~11 deg): thresholding a sharp field
		// (it steps at every coast) would trace the map's texels as
		// stair-stepped cloud edges.
		float m = clamp(2.0 * textureLod(climateMap, d, log2(float(textureSize(climateMap, 0).x)) - 3.0).r, 0.0, 1.0);
		float climate = latitudeBelts(absLatDeg) * mix(0.6, 1.25, m);
		pot = coverage * mix(1.0, climate, climateWeight);
	} else if (style == STYLE_BANDED) {
		float wobble = CLOUD_NOISE(d * 3.0 + vec3(5.0)) - 0.5;
		pot = coverage + 0.4 * sin((lat + 0.12 * wobble) * bands);
	}
	pot = clamp(pot, 0.0, 1.0);

	// Shape noise, moved along the (smoothed) wind by both flow phases.
	vec3 wind = cloudWindAt(d, textureLod(windMap, d, log2(float(textureSize(windMap, 0).x)) - 6.0), windMax);
	float lambda0 = weatherScale * planetRadius;
	int octaves = int(clamp(ceil(log2(lambda0 / (2.0 * texelM))) + 1.0, 1.0, 9.0));
	float n0 = shapeNoise(p, wind, cloudFlowTau.x, cloudFlowSeed0, lambda0, octaves);
	float n1 = shapeNoise(p, wind, cloudFlowTau.y, cloudFlowSeed1, lambda0, octaves);
	// FBM bunches around its mean: stretch it to use the whole range.
	float n = clamp(0.5 + (cloudFlowBlend(n0, n1) - 0.5) * 2.6, 0.0, 1.0);

	// Coverage field: how far the noise rises above the potential's
	// threshold, signed (stored + 0.5). It is thresholded per pixel after
	// the detail noise is added (cloudlayer.glsl), so edges stay crisp and
	// fractal at any distance instead of being this map's blurred contour.
	// Thickness grows with the height above the threshold, so big systems
	// get dense cores; overcast decks stay thick throughout.
	float thresh = 1.0 - pot;
	float field = clamp(n - thresh, -0.5, 0.5);
	float above = clamp((n - thresh) / max(pot, 0.05), 0.0, 1.0);
	float thick = style == STYLE_OVERCAST ? 0.6 + 0.4 * n : 0.45 + 0.55 * above;
	FragColor = vec4(field + 0.5, thick, 0.0, 1.0);
}
