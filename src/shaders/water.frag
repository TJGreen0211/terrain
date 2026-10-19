#version 410

in vec3 fSurfDir;
in vec3 fragPos;
in mat3 fNormalMat;
in float fWave;

out vec4 FragColor;

uniform mat4 lightSpace;
uniform vec3 lightPosition;
uniform vec3 cameraPos;
uniform sampler2D waveDx;
uniform sampler2D waveDz;
uniform sampler2D detailNormal;
uniform samplerCube skybox;
uniform sampler2D depthMap;
uniform float waveNormalStrength;
uniform float waveTiling;
uniform float time;
// Baked planet height field (heightbake.frag), the same texture quad.frag
// shades from, so the land/ocean mask lines up with the coastline the planet
// shader draws. lodK folds the pixel/texel angular-size ratio into the lod;
// it must be the same formula quad.frag uses or the discard edge drifts off
// the drawn coastline.
uniform sampler2D heightMap;
uniform float lodK;
// 1 when the camera is outside the water shell; gates the back-face early-out.
uniform float outsideShell;

const float PI = 3.14159265359;

float shadowCalculation(vec4 fLight) {
	vec3 fragToLight = fragPos - lightPosition;
	float shadow = 0.0;
	vec2 texelSize = 1.0 / textureSize(depthMap, 0);
	for(int x = -1; x <= 1; ++x) {
		for(int y = -1; y <= 1; ++y) {
			float pcfDepth = texture(depthMap, vec2(fLight.xyz * 0.5 + 0.5) + vec2(x, y) * texelSize).r;
			float currentDepth = length(fragToLight)/7000.0;
			shadow += currentDepth - 0.02 > pcfDepth ? 1.0 : 0.0;
		}
	}
	shadow /= 9.0;
	return shadow;
}

// Seascape (Alexander Alekseev aka TDM) sea palette and lighting terms.
const vec3 SEA_BASE = vec3(0.0, 0.09, 0.18);
const vec3 SEA_WATER_COLOR = vec3(0.8, 0.9, 0.6) * 0.6;

float diffuse(vec3 n, vec3 l, float p) {
	return pow(dot(n, l) * 0.4 + 0.6, p);
}

float specular(vec3 n, vec3 l, vec3 e, float s) {
	float nrm = (s + 8.0) / (PI * 8.0);
	return pow(max(dot(reflect(e, n), l), 0.0), s) * nrm;
}

void main()
{
	// Land mask: the planet renders its height field only in the fragment
	// (pure unit-sphere geometry), so the water shell discards where the
	// planet shows land, letting continents stand through the ocean.
	vec3 dObj = normalize(fSurfDir);

	// Cheap early-out: the shell is drawn double-sided (patch skirts need
	// culling off), so its back-facing far-hemisphere fragments would run
	// this whole shader only to lose the depth test against the opaque
	// planet. Discarding them first lets drivers recover early-Z. Gated off
	// while the camera is inside the shell (surface view).
	vec3 Vv = cameraPos - fragPos;
	if (outsideShell > 0.5 && dot(Vv, fragPos) < 0.0)
		discard;

	float zc = clamp(dObj.z, -1.0, 1.0);
	vec2 huv = vec2((atan(dObj.y, dObj.x)/PI + 1.0)*0.5,
	                (asin(zc)/PI + 0.5));
	float cosLat = max(sqrt(max(1.0 - zc*zc, 1e-4)), 0.02);
	float graz = max(abs(dot(normalize(fragPos), normalize(Vv))), 0.15);
	float lod = log2(max(length(Vv)*lodK, 1e-4)) - log2(graz) + log2(1.0/cosLat);
	float h = textureLod(heightMap, huv, clamp(lod, 0.0, 11.0)).r;
	float e = clamp(h, 0.0, 1.0);
	if (e > 0.5)
		discard;

	// Water depth below sea level in height units. The planet fbm keeps its
	// ocean mostly in a narrow band just under e = 0.5, so normalize the
	// effective range (~0.22) rather than the theoretical half-open 0.5.
	float depth = 0.5 - e;
	float depthT = clamp(depth / 0.22, 0.0, 1.0);

	vec2 duv = huv;
	vec2 uv = duv * waveTiling;
	float gx = texture(waveDx, uv).r;
	float gz = texture(waveDz, uv).r;

	// FFT swell slope + two scrolling tiled detail-normal layers for
	// high-frequency sparkle (the two-scale method).
	vec2 d1 = (texture(detailNormal, duv*(waveTiling*2.0) + vec2(0.30, 0.17)*time).rg - 0.5) * 2.0;
	vec2 d2 = (texture(detailNormal, duv*(waveTiling*5.0) - vec2(0.19, 0.31)*time).rg - 0.5) * 2.0;
	vec2 slope = waveNormalStrength * vec2(gx, gz) + d1 * 0.35 + d2 * 0.2;

	// Tangent basis of the equirectangular (long/lat) wave map on the sphere.
	vec3 T = normalize(vec3(-dObj.y, dObj.x, 1e-4));
	vec3 B = normalize(cross(dObj, T));
	vec3 Nobj = normalize(dObj - (slope.x * T + slope.y * B));
	vec3 N = normalize(fNormalMat * Nobj);

	vec3 V = normalize(cameraPos - fragPos);
	vec3 eye = -V;
	vec3 L = normalize(lightPosition - fragPos);
	vec3 dist = fragPos - cameraPos;

	float shadow = shadowCalculation(vec4(fragPos, 1.0) * lightSpace);
	float sun = 1.0 - shadow * 0.85;

	// Fresnel sky reflection (Schlick, water F0 = 0.02). The reflectance is
	// gated by sky visibility: where the reflection ray misses the sky, the
	// water body must stay visible or grazing angles turn black.
	vec3 up = normalize(fragPos);
	vec3 R = reflect(eye, N);
	float skyVis = smoothstep(-0.1, 0.4, dot(R, up));
	float nDotV = clamp(dot(N, V), 0.0, 1.0);
	float fresnel = min((0.02 + 0.98 * pow(1.0 - nDotV, 5.0)) * skyVis, 0.6);
	vec3 reflected = texture(skybox, R).rgb * skyVis * 1.3;

	// Beer's-law style depth tinting: teal shallows to deep navy.
	vec3 deepCol = vec3(0.008, 0.05, 0.13);
	vec3 shallowCol = vec3(0.04, 0.26, 0.36);
	vec3 waterCol = mix(shallowCol, deepCol, depthT);

	// Hemispheric sky irradiance: the night side of an ocean is lit by the
	// whole sky dome, not just the sun. Weighted toward shadow so the lit
	// side keeps its sun-driven contrast.
	float upDotN = clamp(dot(N, up), 0.0, 1.0);
	vec3 skyAmbient = mix(vec3(0.010, 0.024, 0.048), vec3(0.030, 0.058, 0.095), upDotN)
	                * (0.25 + 0.75 * (1.0 - sun));
	vec3 baseCol = waterCol + skyAmbient;

	vec3 refracted = baseCol + diffuse(N, L, 80.0) * SEA_WATER_COLOR * 0.12 * sun;
	vec3 color = mix(refracted, reflected, fresnel);

	// Seascape crest brightening.
	float atten = 1.0 / (1.0 + dot(dist, dist) * 0.0000002);
	color += SEA_WATER_COLOR * (fWave * 30.0) * 0.18 * atten * sun;

	// Fast subsurface scattering: sun light through wave crests toward
	// the viewer (Jensen/Uncharted approximation).
	float crest = clamp(fWave / 0.008, 0.0, 1.0) * (0.4 + 0.6 * depthT);
	vec3 sssDir = normalize(L + N * 0.3);
	float sssSun = pow(clamp(dot(V, sssDir), 0.0, 1.0), 3.0) * sun;
	float sssSky = pow(clamp(dot(V, up), 0.0, 1.0), 2.0) * (1.0 - sun);
	color += SEA_WATER_COLOR * crest * 1.5 * (sssSun + 0.15 * sssSky);

	// Sun glint (Seascape specular, softened with distance).
	color += specular(N, L, eye, 6000.0 * inversesqrt(dot(dist, dist))) * sun;

	// Foam: a thin shore band where water is shallow + whitecaps on the
	// steepest wave faces.
	float shore = 1.0 - smoothstep(0.0, 0.02, depth);
	float steep = (abs(gx) + abs(gz)) * 60.0;
	float whitecaps = smoothstep(1.2, 2.4, steep) * (0.25 + 0.75 * depthT);
	float foam = clamp(shore * 0.9 + whitecaps, 0.0, 1.0);
	vec3 foamCol = vec3(0.82, 0.87, 0.90) * (0.30 + 0.85 * max(dot(N, L), 0.0)) * mix(0.25, 1.0, sun);
	color = mix(color, foamCol, foam);

	// Seascape post (fragColor = pow(color, 0.65))
	FragColor = vec4(pow(clamp(color, 0.0, 1.0), vec3(0.65)), 1.0);
}
