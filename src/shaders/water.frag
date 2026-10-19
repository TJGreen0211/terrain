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
uniform float fInnerRadius;
uniform float fOuterRadius;
uniform vec3 C_R;

// Planet height field: must match quad.frag exactly (same noise, frequency,
// octaves, and the sea band at e = 0.50) so the water mask lines up with the
// coastline drawn by the planet shader.
#define NOISE_FREQ      2.6
#define NOISE_OCTAVES   4

const float PI = 3.14159265359;
const float ATMO_MAX = 1e30;
const float ATMO_K_R = 0.166;
const float ATMO_K_M = 0.0025;
const int ATMO_STEPS = 3;

vec2 atmosphereIntersection(vec3 p, vec3 dir, float radius)
{
	float b = dot(p, dir);
	float c = dot(p, p) - radius * radius;
	float d = b * b - c;
	if (d < 0.0)
		return vec2(ATMO_MAX, -ATMO_MAX);
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

float atmosphereOptic(vec3 p, vec3 q, float scaleH, float scaleL)
{
	vec3 step = (q - p) / float(ATMO_STEPS);
	vec3 v = p + step * 0.5;
	float sum = 0.0;
	for (int i = 0; i < ATMO_STEPS; i++) {
		sum += exp(-(length(v) - fInnerRadius) * scaleH);
		v += step;
	}
	return sum * length(step) * scaleL;
}

vec3 applyAtmosphereExtinction(vec3 radiance)
{
	if (fOuterRadius <= fInnerRadius)
		return radiance;

	float scaleH = 4.0 / (fOuterRadius - fInnerRadius);
	float scaleL = 1.0 / (fOuterRadius - fInnerRadius);
	vec3 eye = cameraPos;
	vec3 ray = fragPos - eye;
	float fragmentDistance = length(ray);
	vec3 dir = ray / max(fragmentDistance, 1e-6);

	vec2 e = atmosphereIntersection(eye, dir, fOuterRadius);
	if (e.x > e.y)
		return radiance;
	vec2 f = atmosphereIntersection(eye, dir, fInnerRadius);
	e.x = max(e.x, 0.0);
	e.y = min(e.y, fragmentDistance);
	if (f.x > e.x)
		e.y = min(e.y, f.x);
	if (e.y <= e.x)
		return radiance;

	float tau = atmosphereOptic(eye + dir * e.x, eye + dir * e.y,
	                            scaleH, scaleL) * (4.0 * PI);
	return radiance * exp(-(ATMO_K_R * C_R + ATMO_K_M) * tau);
}

vec3 mod289v(vec3 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 mod289v(vec4 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289v(((x*34.0)+1.0)*x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

float snoise(vec3 v) {
	const vec2 C = vec2(1.0/6.0, 1.0/3.0);
	const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);
	vec3 i  = floor(v + dot(v, C.yyy));
	vec3 x0 = v - i + dot(i, C.xxx);
	vec3 g = step(x0.yzx, x0.xyz);
	vec3 l = 1.0 - g;
	vec3 i1 = min(g.xyz, l.zxy);
	vec3 i2 = max(g.xyz, l.zxy);
	vec3 x1 = x0 - i1 + C.xxx;
	vec3 x2 = x0 - i2 + C.yyy;
	vec3 x3 = x0 - D.yyy;
	i = mod289v(i);
	vec4 p = permute(permute(permute(
			i.z + vec4(0.0, i1.z, i2.z, 1.0))
			+ i.y + vec4(0.0, i1.y, i2.y, 1.0))
			+ i.x + vec4(0.0, i1.x, i2.x, 1.0));
	float n_ = 0.142857142857;
	vec3 ns = n_ * D.wyz - D.xzx;
	vec4 j = p - 49.0 * floor(p * ns.z * ns.z);
	vec4 x_ = floor(j * ns.z);
	vec4 y_ = floor(j - 7.0 * x_);
	vec4 x = x_ * ns.x + ns.yyyy;
	vec4 y = y_ * ns.x + ns.yyyy;
	vec4 h = 1.0 - abs(x) - abs(y);
	vec4 b0 = vec4(x.xy, y.xy);
	vec4 b1 = vec4(x.zw, y.zw);
	vec4 s0 = floor(b0)*2.0 + 1.0;
	vec4 s1 = floor(b1)*2.0 + 1.0;
	vec4 sh = -step(h, vec4(0.0));
	vec4 a0 = b0.xzyw + s0.xzyw*sh.xxyy;
	vec4 a1 = b1.xzyw + s1.xzyw*sh.zzww;
	vec3 p0 = vec3(a0.xy, h.x);
	vec3 p1 = vec3(a0.zw, h.y);
	vec3 p2 = vec3(a1.xy, h.z);
	vec3 p3 = vec3(a1.zw, h.w);
	vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));
	p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
	vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
	m = m*m;
	return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
}

float fbm3(vec3 p) {
	float v = 0.0;
	float amp = 0.5;
	float frq = NOISE_FREQ;
	for (int i = 0; i < NOISE_OCTAVES; i++) {
		v += amp * snoise(p * frq);
		frq *= 2.0;
		amp *= 0.5;
	}
	return v;
}

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
	float h = fbm3(dObj);
	float e = clamp(h * 0.5 + 0.5, 0.0, 1.0);
	if (e > 0.5)
		discard;

	// Water depth below sea level in height units. The planet fbm keeps its
	// ocean mostly in a narrow band just under e = 0.5, so normalize the
	// effective range (~0.22) rather than the theoretical half-open 0.5.
	float depth = 0.5 - e;
	float depthT = clamp(depth / 0.22, 0.0, 1.0);

	vec2 duv = vec2((atan(dObj.y, dObj.x) / 3.1415926 + 1.0) * 0.5,
	                (asin(dObj.z) / 3.1415926 + 0.5));
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

	color = applyAtmosphereExtinction(color);

	// Seascape post (fragColor = pow(color, 0.65))
	FragColor = vec4(pow(clamp(color, 0.0, 1.0), vec3(0.65)), 1.0);
}
