#version 410

in vec3 fSurfDir;
in vec3 fragPos;
in float fWave;

out vec4 FragColor;

uniform mat4 model;
uniform mat4 lightSpace;
uniform vec3 lightPosition;
uniform vec3 cameraPos;
// FFT ocean (Water.c): slopes and displacement derivatives per patch length.
uniform sampler2D waveSlope;     // dh/dx, dh/dz
uniform sampler2D waveJacobian;  // dDx/dx, dDz/dz, dDx/dz
uniform float waveAmp;
uniform float waveChoppy;
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
// Sea level + ocean depth from the baked, eroded height field (TerrainGen.c).
// The identical seaLevel quad.frag ramps with, so the discard edge lands on
// the drawn coastline; oceanDepth normalizes the depth tint (it replaces the
// old hardcoded 0.22 constant).
uniform float seaLevel;
uniform float oceanDepth;
// 1 when the camera is outside the water shell; gates the back-face early-out.
uniform float outsideShell;

const float PI = 3.14159265359;

// Camera-relative rendering: fragPos, lightPosition and bodyCenter (the
// planet centre) are all relative to the camera, which sits at the origin
// (cameraPos = lodCameraPos = 0). Planet-local positions are fragPos -
// bodyCenter.
uniform vec3 lodCameraPos;
uniform vec3 bodyCenter;

// Atmospheric extinction (OpenGLSolar planet.frag port): dims the surface by
// the transmittance along the surface-to-camera path through the dome's shell.
// The atmosphere dome pass adds the in-scattered light over both the limb and
// the planet disc; adding it here as well would count that radiance twice.
// applyAtmosphere takes planet-local eye/fragment positions (shell-centred).
uniform float hasAtmosphere;
uniform float fInnerRadius;
uniform float fOuterRadius;
uniform vec3 C_R;

// Keep the miss sentinel beyond the largest supported shell radius.
const float ATMO_MAX = 1e30;
const float K_R = 0.166;
const float K_M = 0.0025;
const int NUM_ATMO_STEPS = 5;

vec2 shellIntersection(vec3 p, vec3 dir, float radius)
{
	float b = dot(p, dir);
	float c = dot(p, p) - radius * radius;
	float d = b * b - c;
	if (d < 0.0) {
		return vec2(ATMO_MAX, -ATMO_MAX);
	}
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

float atmoOptic(vec3 p, vec3 q, float scaleH, float scaleL)
{
	vec3 step = (q - p) / float(NUM_ATMO_STEPS);
	vec3 v = p + step * 0.5;
	float sum = 0.0;
	for (int i = 0; i < NUM_ATMO_STEPS; i++) {
		sum += exp(-(length(v) - fInnerRadius) * scaleH);
		v += step;
	}
	return sum * length(step) * scaleL;
}

vec3 applyAtmosphere(vec3 color, vec3 eye, vec3 frag)
{
	float scaleH = 4.0 / (fOuterRadius - fInnerRadius);
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);

	// Ray in the shell-centred frame: eye = camera, fragment at distance tFrag.
	vec3 ray = frag - eye;
	float tFrag = length(ray);
	vec3 dir = ray / max(tFrag, 1e-6);

	vec2 e = shellIntersection(eye, dir, fOuterRadius);
	if (e.x > e.y) {
		return color; // view ray misses the shell entirely
	}
	vec2 f = shellIntersection(eye, dir, fInnerRadius);
	// Clip the path at the shell exit, the inner sphere, and the actual
	// fragment hit: the tFrag term handles displaced surfaces, where f.x
	// would overshoot or fall short of the visible surface.
	// camera inside the shell: the path starts at the camera
	e.x = max(e.x, 0.0);
	e.y = min(e.y, tFrag);
	if (f.x > e.x) {
		e.y = min(e.y, f.x);
	}
	if (e.y <= e.x) {
		return color;
	}

	float tau = atmoOptic(eye + dir * e.x, eye + dir * e.y, scaleH, scaleL) * (4.0 * PI);
	return color * exp(-(K_R * C_R + K_M) * tau);
}

float shadowCalculation(vec4 fLight) {
	// 2x2 PCF at half-texel offsets (was 3x3): bilinear filtering of the
	// depth map already blends neighbouring texels, so four taps keep the
	// soft edge at under half the fetches.
	float currentDepth = length(fragPos - lightPosition)/7000.0 - 0.02;
	vec2 base = fLight.xy * 0.5 + 0.5;
	vec2 h = 0.5 / vec2(textureSize(depthMap, 0));
	float shadow = 0.0;
	shadow += currentDepth > texture(depthMap, base + vec2(-h.x, -h.y)).r ? 1.0 : 0.0;
	shadow += currentDepth > texture(depthMap, base + vec2( h.x, -h.y)).r ? 1.0 : 0.0;
	shadow += currentDepth > texture(depthMap, base + vec2(-h.x,  h.y)).r ? 1.0 : 0.0;
	shadow += currentDepth > texture(depthMap, base + vec2( h.x,  h.y)).r ? 1.0 : 0.0;
	return shadow * 0.25;
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
	// Screen-space derivatives must be taken while the whole 2x2 quad is
	// still alive, i.e. before the discards below.
	vec3 dDirX = dFdx(dObj);
	vec3 dDirY = dFdy(dObj);

	// Cheap early-out: the shell is drawn double-sided (patch skirts need
	// culling off), so its back-facing far-hemisphere fragments would run
	// this whole shader only to lose the depth test against the opaque
	// planet. Discarding them first lets drivers recover early-Z. Gated off
	// while the camera is inside the shell (surface view).
	vec3 Vv = cameraPos - fragPos;
	vec3 localPos = fragPos - bodyCenter;
	if (outsideShell > 0.5 && dot(Vv, localPos) < 0.0)
		discard;

	float zc = clamp(dObj.z, -1.0, 1.0);
	vec2 huv = vec2((atan(dObj.y, dObj.x)/PI + 1.0)*0.5,
	                (asin(zc)/PI + 0.5));
	float cosLat = max(sqrt(max(1.0 - zc*zc, 1e-4)), 0.02);
	float graz = max(abs(dot(normalize(localPos), normalize(Vv))), 0.15);
	float lod = log2(max(length(Vv)*lodK, 1e-4)) - log2(graz) + log2(1.0/cosLat);
	float h = textureLod(heightMap, huv, clamp(lod, 0.0, 11.0)).r;
	float e = clamp(h, 0.0, 1.0);
	if (e > seaLevel)
		discard;

	// Water depth below sea level in height units, normalized by the 95th
	// percentile depth of the eroded field so the tint spans its real range.
	float depth = seaLevel - e;
	float depthT = clamp(depth / max(oceanDepth, 1e-4), 0.0, 1.0);

	vec2 duv = huv;
	vec2 uv = duv * waveTiling;

	// Long/lat UV gradients for the mipmapped wave/detail maps, chained
	// analytically from the direction's derivatives (taken before any
	// discard): no 1 -> 0 jump at the atan seam to pick the smallest mip.
	float rxy2 = max(dObj.x*dObj.x + dObj.y*dObj.y, 1e-6);
	vec2 dx = vec2((dObj.x*dDirX.y - dObj.y*dDirX.x) / (2.0*PI*rxy2),
	               dDirX.z / (PI*sqrt(rxy2)));
	vec2 dy = vec2((dObj.x*dDirY.y - dObj.y*dDirY.x) / (2.0*PI*rxy2),
	               dDirY.z / (PI*sqrt(rxy2)));

	// World size (unit-sphere radii) of one wave patch along east/north on
	// the long/lat map; turns the per-patch derivatives into world slopes.
	vec2 patchLen = vec2(2.0*PI*cosLat, PI) / waveTiling;
	vec2 sl = textureGrad(waveSlope, uv, dx*waveTiling, dy*waveTiling).rg * waveAmp / patchLen;
	vec3 jt = textureGrad(waveJacobian, uv, dx*waveTiling, dy*waveTiling).rgb * (waveAmp * waveChoppy);
	float jxx = jt.r / patchLen.x;
	float jzz = jt.g / patchLen.y;
	float jxz = jt.b / patchLen.y;   // dDx/dz
	float jzx = jt.b / patchLen.x;   // dDz/dx (same spectrum, east scale)
	// Jacobian of the choppy displacement: < 1 where crests pinch, < 0 folds.
	float J = (1.0 + jxx) * (1.0 + jzz) - jxz * jzx;

	// Normal of the displaced surface = cross of its tangents
	// (1+jxx, jzx, sl.x) x (jxz, 1+jzz, sl.y) in (east, north, up),
	// rescaled to unit up so it reads as a slope.
	vec2 fftSlope = -vec2(jzx*sl.y - sl.x*(1.0 + jzz),
	                      sl.x*jxz - (1.0 + jxx)*sl.y) / max(J, 0.05);

	// FFT swell slope + two scrolling tiled detail-normal layers for
	// high-frequency sparkle (the two-scale method).
	vec2 d1 = (textureGrad(detailNormal, duv*(waveTiling*2.0) + vec2(0.30, 0.17)*time,
	                       dx*(waveTiling*2.0), dy*(waveTiling*2.0)).rg - 0.5) * 2.0;
	vec2 d2 = (textureGrad(detailNormal, duv*(waveTiling*5.0) - vec2(0.19, 0.31)*time,
	                       dx*(waveTiling*5.0), dy*(waveTiling*5.0)).rg - 0.5) * 2.0;
	vec2 slope = waveNormalStrength * fftSlope + d1 * 0.35 + d2 * 0.2;

	// Tangent basis of the equirectangular (long/lat) wave map on the sphere.
	vec3 T = normalize(vec3(-dObj.y, dObj.x, 1e-4));
	vec3 B = normalize(cross(dObj, T));
	vec3 Nobj = normalize(dObj - (slope.x * T + slope.y * B));
	// model is uniform scale * rotation, so inverse(mat3(model)) * n is
	// n * mat3(model) up to a scale the normalize removes.
	vec3 N = normalize(Nobj * mat3(model));

	vec3 V = normalize(cameraPos - fragPos);
	vec3 eye = -V;
	vec3 L = normalize(lightPosition - fragPos);
	vec3 dist = fragPos - cameraPos;

	float shadow = shadowCalculation(vec4(fragPos, 1.0) * lightSpace);
	float sun = 1.0 - shadow * 0.85;

	// Fresnel sky reflection (Schlick, water F0 = 0.02). The reflectance is
	// gated by sky visibility: where the reflection ray misses the sky, the
	// water body must stay visible or grazing angles turn black.
	vec3 up = normalize(localPos);
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

	// Foam: a thin shore band where water is shallow + whitecaps where the
	// choppy displacement compresses the surface (low Jacobian).
	float shore = 1.0 - smoothstep(0.0, 0.02, depth);
	float whitecaps = (1.0 - smoothstep(0.3, 0.8, J)) * (0.25 + 0.75 * depthT);
	float foam = clamp(shore * 0.9 + whitecaps, 0.0, 1.0);
	vec3 foamCol = vec3(0.82, 0.87, 0.90) * (0.30 + 0.85 * max(dot(N, L), 0.0)) * mix(0.25, 1.0, sun);
	color = mix(color, foamCol, foam);

	// Surface extinction, matching quad.frag so the coastline stays seamless.
	if (hasAtmosphere > 0.5) {
		color = applyAtmosphere(color, lodCameraPos - bodyCenter, localPos);
	}

	// Seascape post (fragColor = pow(color, 0.65))
	FragColor = vec4(pow(clamp(color, 0.0, 1.0), vec3(0.65)), 1.0);
}
