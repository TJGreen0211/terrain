#version 410

in vec2 fTexCoords;
in vec3 fragPos;
in vec3 fE;
in vec3 fN;
in vec3 fL;
in vec3 fH;
in vec3 vColor;
in vec4 fLightSpace;
in vec3 fSurfDir;
in mat3 fNormalMat;

out vec4 FragColor;

uniform vec3 lightPosition;
uniform vec3 cameraPos;
uniform sampler2D texture1;
uniform sampler2D depthMap;
// Height field baked once at startup from the original fbm3 (heightbake.frag).
// lodCameraPos is the true world camera (cameraPos intentionally carries the
// legacy sun-relative position); lodK folds the pixel/texel angular-size
// ratio into log2(dist*lodK), plus the equirect pole and grazing corrections,
// clamped to the bake's 11 mips (2048 wide).
uniform sampler2D heightMap;
uniform vec3 lodCameraPos;
uniform float lodK;
// Sea level from the baked + eroded height field (TerrainGen.c); the height
// ramp and the material bands all key off this single value, and water.frag
// gets the identical value so the discard edge sits on the coastline.
uniform float seaLevel;

const float PI = 3.14159265359;

// Atmospheric extinction (OpenGLSolar planet.frag port): dims the surface by
// the transmittance along the surface-to-camera path through the dome's shell.
// The atmosphere dome pass adds the in-scattered light over both the limb and
// the planet disc; adding it here as well would count that radiance twice.
// fragPos/lodCameraPos are planet-local: the planet sits at the world origin.
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

// Terrain height field + lighting controls (tweak here)
#define SURF_EPS        0.030   // angular step for the surface gradient
#define NORMAL_STRENGTH 0.62    // bump relief amount
#define BEACH_OFF       0.06    // shoreline width above seaLevel
#define TREE_OFF        0.20    // grass starts here above seaLevel
#define PEAK_OFF        0.35    // rock/snow transition above seaLevel
#define SNOW_BAND       0.10    // rock -> snow blend width

float heightAt(vec3 d, float lodBase) {
	float z = clamp(d.z, -1.0, 1.0);
	vec2 uv = vec2((atan(d.y, d.x)/PI + 1.0)*0.5,
	               (asin(z)/PI + 0.5));
	float cosLat = max(sqrt(max(1.0 - z*z, 1e-4)), 0.02);
	float lod = lodBase + log2(1.0/cosLat);
	return textureLod(heightMap, uv, clamp(lod, 0.0, 11.0)).r;
}

vec3 heightRamp(float e) {
	vec3 deep   = vec3(0.02, 0.09, 0.20);
	vec3 shall  = vec3(0.05, 0.22, 0.38);
	vec3 sand   = vec3(0.72, 0.64, 0.40);
	vec3 grass  = vec3(0.16, 0.38, 0.15);
	vec3 ridge  = vec3(0.42, 0.38, 0.34);
	vec3 snow   = vec3(0.92, 0.93, 0.95);
	float sea   = seaLevel;
	float beach = seaLevel + BEACH_OFF;
	float tree  = seaLevel + TREE_OFF;
	float peak  = seaLevel + PEAK_OFF;
	if (e < sea)   return mix(deep,  shall, smoothstep(0.0,   sea,   e));
	if (e < beach) return mix(shall, sand,  smoothstep(sea,   beach, e));
	if (e < tree)  return mix(sand,  grass, smoothstep(beach, tree,  e));
	if (e < peak)  return mix(grass, ridge, smoothstep(tree,  peak,  e));
	return mix(ridge, snow, smoothstep(peak, 1.0, e));
}

/*
fLight.xyz = fLight.xyz * 0.5 + 0.5;
float closestDepth = texture(depthMap, fLight.xy).r;
float currentDepth = fLight.z;
float shadow = 0.0;
vec2 texelSize = 1.0 / textureSize(depthMap, 0);
for(int x = -1; x <= 1; ++x) {
	for(int y = -1; y <= 1; ++y) {
		float pcfDepth = texture(depthMap, fLight.xy + vec2(x, y) * texelSize).r;
		shadow += currentDepth - 0.0001 > pcfDepth ? 1.0 : 0.0;
	}
}
shadow /= 9.0;

if(fLight.z > 1.0)
	shadow = 0.0;
return shadow;
*/

float shadowCalculation(vec4 fLight) {
	vec3 fragToLight = fragPos - lightPosition;
	//fragToLight = fLight.xyz * 0.5 + 0.5;
	//fragToLight = fragToLight/700.0;
	float closestDepth = texture(depthMap, vec2(fLight.xyz * 0.5 + 0.5)).r;
	float currentDepth = length(fragToLight)/7000.0;
	//closestDepth *= 700.0;

	float shadow = 0.0;
	vec2 texelSize = 1.0 / textureSize(depthMap, 0);
	for(int x = -1; x <= 1; ++x) {
		for(int y = -1; y <= 1; ++y) {
			float pcfDepth = texture(depthMap, vec2(fLight.xyz * 0.5 + 0.5) + vec2(x, y) * texelSize).r;
			shadow += currentDepth - 0.02 > pcfDepth ? 1.0 : 0.0;
		}
	}
	shadow /= 9.0;


	//shadow = currentDepth - 0.02 > closestDepth ? 1.0 : 0.0;
	//if(fragToLight.z > 1.0)
	//	shadow = 0.0;

	return shadow;
}

void main()
{
	vec3 ambientSky = vec3(0.10, 0.12, 0.16);   // cool skylight: keeps the night side off black
	vec3 sunRadiance = vec3(1.0, 0.97, 0.90) * 1.15;

	// Height field sampled from the startup-baked texture (rotates with the
	// planet: dObj lives in the planet-local frame). Every face samples this
	// one continuous field, so it is seamless across all six quadtree faces.
	vec3 dObj = normalize(fSurfDir);
	vec3 up = (abs(dObj.y) > 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 T = normalize(cross(up, dObj));
	vec3 B = cross(dObj, T);

	// lod base: pixel angular footprint on the sphere (dist*thetaPixel/r,
	// folded into lodK) plus grazing correction (footprint grows by 1/cos of
	// the view angle at the surface).
	vec3 camDir = lodCameraPos - fragPos;
	float graz = max(abs(dot(normalize(fragPos), normalize(camDir))), 0.15);
	float lodBase = log2(max(length(camDir)*lodK, 1e-4)) - log2(graz);
	float h  = heightAt(dObj, lodBase);
	float hT = heightAt(normalize(dObj + T * SURF_EPS), lodBase);
	float hB = heightAt(normalize(dObj + B * SURF_EPS), lodBase);

	float e = clamp(h, 0.0, 1.0);
	vec4 color = vec4(heightRamp(e), 1.0);
	vec3 albedo = color.rgb;

	// Height-driven microfacet material: ocean = smooth-ish/low-F0, land = rough,
	// snow = in between (same seaLevel-relative bands as heightRamp)
	float oceanMask = 1.0 - smoothstep(seaLevel, seaLevel + BEACH_OFF, e);
	float snowMask  = smoothstep(seaLevel + PEAK_OFF, seaLevel + PEAK_OFF + SNOW_BAND, e);
	float rough = mix(0.72, 0.15, oceanMask);   // ocean is no longer a perfect mirror
	rough = mix(rough, 0.45, snowMask);
	float a2 = max(rough * rough, 0.0004);
	vec3 F0 = mix(vec3(0.04), vec3(0.02), oceanMask);
	F0 = mix(F0, vec3(0.03), snowMask);

	// Bump relief scaled by material: a smooth ocean keeps a coherent sun glint
	// instead of scattering it into per-texel glitter; rough land keeps full relief.
	float bump = mix(NORMAL_STRENGTH, NORMAL_STRENGTH * 0.25, oceanMask);
	vec3 grad = T * ((hT - h) / SURF_EPS) + B * ((hB - h) / SURF_EPS);
	vec3 Nobj = normalize(dObj - bump * grad);
	vec3 N = normalize(fNormalMat * Nobj);

	vec3 V = normalize(cameraPos - fragPos);
	vec3 L = normalize(fL);
	vec3 H = normalize(L + V + vec3(1e-5));     // guard the L == -V (backscatter) case
	float NdotL = dot(N, L);
	float NdotV = max(dot(N, V), 1e-4);
	float NdotH = max(dot(N, H), 0.0);
	float VdotH = max(dot(V, H), 0.0);

	// GGX (Trowbridge-Reitz) normal distribution
	float g = NdotH * NdotH * (a2 - 1.0) + 1.0;
	float D = a2 / (PI * g * g + 1e-7);
	// Schlick fresnel
	vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

	vec3 lit = ambientSky * albedo;             // indirect sky/airlight term
	float wrap = 0.25;                           // wrap diffuse: soft terminator
	if (NdotL > -wrap) {
		float wl = clamp((NdotL + wrap) / (1.0 + wrap), 0.0, 1.0);
		vec3 kd = 1.0 - F;                       // energy conservation (dielectric)
		vec3 diff = albedo / PI * kd * wl;
		vec3 spec = vec3(0.0);
		if (NdotL > 0.0) {
			// Smith-Schlick geometry term. Unlike the Charlie visibility
			// approximation, G -> 0 as NdotL -> 0, so the specular cleanly vanishes
			// at grazing incidence instead of diverging to white. The NdotL
			// foreshortening is folded into the denominator (f·NdotL = D·G·F/(4·NdotV)),
			// so the lobe also fades correctly toward the terminator.
			float k = a2 * 0.5;
			float G1L = NdotL / (NdotL * (1.0 - k) + k);
			float G1V = NdotV / (NdotV * (1.0 - k) + k);
			spec = (D * G1L * G1V * F) / (4.0 * NdotV + 1e-4);
		}
		lit += (diff + spec) * sunRadiance;
	}

	// Surface extinction only (linear radiance, before the tone map); the
	// atmosphere dome owns all in-scattered radiance.
	if (hasAtmosphere > 0.5) {
		lit = applyAtmosphere(lit, lodCameraPos, fragPos);
	}

	// Filmic (ACES) tone map: compresses hot specular while preserving albedo
	// saturation, unlike Reinhard which trends everything toward white.
	float exposure = 1.05;
	vec3 x = lit * exposure;
	lit = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
	float gamma = 2.2;
	FragColor = vec4(pow(lit, vec3(1.0 / gamma)), color.a);
}
